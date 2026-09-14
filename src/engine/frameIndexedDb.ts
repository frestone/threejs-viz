const DB_NAME = "viz-frame-cache";
const DB_VERSION = 1;
const STORE_NAME = "frames";

interface StoredFrame {
  source: string;
  t: number;
  seq: number;
  bytes: ArrayBuffer;
}

export interface CachedFrameBytes {
  t: number;
  seq: number;
  bytes: Uint8Array;
}

interface PendingFrame extends StoredFrame {
  resolve: (t: number) => void;
  reject: (error: unknown) => void;
}

function requestResult<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("IndexedDB 请求失败"));
  });
}

export class FrameIndexedDb {
  private dbPromise: Promise<IDBDatabase> | null = null;
  private queue: PendingFrame[] = [];
  private flushTimer: number | null = null;
  private flushing = false;
  private disposed = false;
  // 连续失败批计数与熔断标志：WebKitGTK 等环境的 origin 配额（约 1GB）耗尽后，
  // 事务会 abort 且重试事务可能长时间不回调（oncomplete/onabort 都不触发）。
  // 不熔断会让 flushing 永锁、数千个 put 永久 pending、UI 缓存进度冻结。
  private failedBatches = 0;
  private broken = false;
  // 累计已落盘字节：写到上限后主动停写，避免写满配额后事务挂死、以及满库残留拖慢
  // 后续 clear/打开。上限提高到 5GB 以支持全量预取落盘大文件（需浏览器 origin 配额足够，
  // Chromium 通常按可用磁盘的一定比例授予，远超 5GB）。
  private totalBytes = 0;
  private static readonly MAX_TOTAL_BYTES = 5 * 1024 * 1024 * 1024;
  // 【修复 webview OOM】写事务挂起期间允许积压的队列水位（帧数/字节），
  // 超限丢新帧而非堆积内存（见 put）。
  private queuedBytes = 0;

  private open(): Promise<IDBDatabase> {
    if (this.dbPromise) return this.dbPromise;
    if (typeof indexedDB === "undefined") {
      return Promise.reject(new Error("当前环境不支持 IndexedDB"));
    }
    this.dbPromise = new Promise((resolve, reject) => {
      const request = indexedDB.open(DB_NAME, DB_VERSION);
      request.onupgradeneeded = () => {
        const db = request.result;
        if (!db.objectStoreNames.contains(STORE_NAME)) {
          db.createObjectStore(STORE_NAME, { keyPath: ["source", "t"] });
        }
      };
      request.onsuccess = () => {
        request.result.onversionchange = () => request.result.close();
        resolve(request.result);
      };
      request.onerror = () => reject(request.error ?? new Error("IndexedDB 打开失败"));
    });
    return this.dbPromise;
  }

  put(source: string, t: number, seq: number, bytes: Uint8Array): Promise<number | undefined> {
    if (this.disposed || !Number.isFinite(t)) return Promise.reject(new Error("帧缓存不可用"));
    if (this.broken) return Promise.reject(new Error("IndexedDB 持久化不可用（可能超出存储配额），本次播放不再写入磁盘缓存"));
    // 【修复 webview OOM 崩溃】写事务挂起期间（60s 超时内）后端全速推帧会使
    // queue 无限堆积，实测约 110s 堆积数百 MB 导致 WebKitWebProcess 被 OOM
    // 杀死（WebSocket 断连、页面黑屏）。超过积压上限时丢弃新帧：resolve
    // (undefined) 让 persist() 跳过计数，仅损失缓存完整性（洞由 seek 后的
    // seekBackend 兜底），不会虚假推进 persistedMaxTime。
    if (this.queue.length >= 300 || this.queuedBytes + bytes.byteLength > 128 * 1024 * 1024) {
      return Promise.resolve(undefined);
    }
    // 调用方已为持久化保留独立 Uint8Array；覆盖完整 ArrayBuffer 时直接移交，避免再次整帧复制。
    const copy = bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength
      ? bytes.buffer as ArrayBuffer
      : bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
    return new Promise((resolve, reject) => {
      this.queue.push({ source, t, seq, bytes: copy, resolve, reject });
      this.queuedBytes += copy.byteLength;
      // 【修复播放后进度长期不变 + IndexedDB 帧写入失败】
      // 上一版阈值降到 1 帧立即 flush,导致每帧一个事务,IndexedDB 在高频
      // put 下频繁 abort(Quota/事务调度冲突),并出现"flushing 期间新帧入队
      // 永远捡不起来"的死锁。本版改回"攒批 + 短间隔兜底":
      //   - 16 帧立即 flush:与 maxBatchFrames 对齐,确保一次事务能消化
      //   - 30ms 定时器兜底:低帧率时仍能及时落盘,persistedMaxTime 几乎实时推进
      // 高频帧仍会合并(30ms 内可攒到 3~5 帧),事务开销可控。
      if (this.queue.length >= 16) void this.flush();
      else if (this.flushTimer === null) {
        this.flushTimer = window.setTimeout(() => void this.flush(), 30);
      }
    });
  }

  private async flush(): Promise<void> {
    if (this.flushing || this.queue.length === 0 || this.disposed) return;
    this.flushing = true;
    if (this.flushTimer !== null) {
      clearTimeout(this.flushTimer);
      this.flushTimer = null;
    }
    // 每个事务同时限制帧数和总字节。不能一次 splice 整个积压队列：当磁盘短暂变慢时，
    // 队列可能积累成数百 MB 的单事务，浏览器会长时间提交甚至直接 abort，表现为进度卡死。
    // 【修复播放后进度长期不变】批上限从 64 → 16 帧,16MiB 不变。
    // 降低单事务延迟,让 persistedMaxTime 的推进更接近帧到达节奏。
    const maxBatchFrames = 16;
    const maxBatchBytes = 16 * 1024 * 1024;
    let batchBytes = 0;
    let batchCount = 0;
    while (batchCount < this.queue.length && batchCount < maxBatchFrames) {
      const nextBytes = this.queue[batchCount].bytes.byteLength;
      if (batchCount > 0 && batchBytes + nextBytes > maxBatchBytes) break;
      batchBytes += nextBytes;
      batchCount++;
    }
    const batch = this.queue.splice(0, Math.max(1, batchCount));
    this.queuedBytes -= batchBytes;
    // 【修复 IndexedDB 帧写入失败】保留被回退的 batch,在 abort 时重新入队,
    // 避免一次失败把整批 persist 的 resolve 永久挂起或永久 reject 导致
    // persistedMaxTime 不再推进。retry 计数防止无限重试拖死 UI。
    const MAX_RETRY = 2;
    let attempt = 0;
    let lastError: unknown = null;
    while (attempt <= MAX_RETRY) {
      try {
        const db = await this.open();
        // 【修复缓存进度冻结】事务挂死兜底：WebKitGTK 配额耗尽后重试事务可能
        // oncomplete/onabort 都不回调，flushing 永锁导致数千 put 永久 pending。
        // 超时后主动 abort 并 reject，保证 flush 循环能退出。
        await new Promise<void>((resolve, reject) => {
          const tx = db.transaction(
            STORE_NAME,
            "readwrite",
            { durability: "relaxed" },
          );
          const store = tx.objectStore(STORE_NAME);
          for (const frame of batch) {
            store.put({ source: frame.source, t: frame.t, seq: frame.seq, bytes: frame.bytes });
          }
          let settled = false;
          // 【修复缓存 0% + webview 崩溃】磁盘 IO 竞争（后端全速读盘推帧）可使
          // WebKitGTK 写事务挂起数十秒后自愈。实测：10s 超时 abort 是安全的
          // （run17 两批超时后自愈、ok 持续推进）；而 60s 超时对长时间挂起事务
          // abort 会触发 WebKitWebProcess 崩溃（run18/18b/19 三次复现：启动后
          // ~90-110s webview 死亡、WebSocket EOF 黑屏）。因此超时保持 10s，
          // "误判丢帧→熔断 0%"问题改由熔断阈值 3→6 批 + put 侧积压上限解决。
          const timer = window.setTimeout(() => {
            if (settled) return;
            settled = true;
            try { tx.abort(); } catch { /* 已结束 */ }
            reject(new Error("IndexedDB 事务超时（写入挂起）"));
          }, 10000);
          tx.oncomplete = () => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            resolve();
          };
          const fail = () => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            reject(tx.error ?? new Error("IndexedDB 帧写入失败"));
          };
          tx.onerror = fail;
          tx.onabort = fail;
        });
        for (const frame of batch) frame.resolve(frame.t);
        lastError = null;
        break;
      } catch (error) {
        lastError = error;
        attempt++;
        if (attempt > MAX_RETRY) break;
        // 短暂退避再试一次(磁盘/调度瞬时问题通常 50ms 内恢复)
        await new Promise((r) => setTimeout(r, 50));
      }
    }
    if (lastError) {
      // 【修复缓存进度冻结】重试用尽后不再把 batch 回插队首无限重试：
      // 配额耗尽等持续失败场景会形成"flush→失败→回队→flush"死循环，
      // 且挂起事务让 flushing 永锁。改为丢批 reject + 连续失败计数熔断。
      this.failedBatches++;
      // 【修复缓存 0%】熔断阈值 3→6：IO 竞争导致的写事务挂起是瞬态的（run17
      // 实测连续 2 批超时后自愈，ok 计数从 9 恢复到 665+），过敏感的熔断
      // 会让 persistedMaxTime 永久停在 0。QuotaExceeded 类持续失败仍会触发。
      if (this.failedBatches >= 6 && !this.broken) {
        this.broken = true;
        for (const frame of this.queue) {
          frame.reject(new Error("IndexedDB 持久化不可用（可能超出存储配额），已停止写入磁盘缓存"));
        }
        this.queue = [];
        this.queuedBytes = 0;
      }
      for (const frame of batch) frame.reject(lastError);
    }
    // 成功批清零连续失败计数（瞬态冲突可自愈）
    if (!lastError) {
      this.failedBatches = 0;
      // 【修复上限过早触发】totalBytes 改为按"落盘成功"累计而非入队累计：
      // 帧到达速度远快于写盘时，入队计数会让队列瞬时超限、实际只落盘少量帧就熔断。
      this.totalBytes += batchBytes;
      if (this.totalBytes > FrameIndexedDb.MAX_TOTAL_BYTES && !this.broken) {
        this.broken = true;
        const limitErr = new Error("已达到单次播放磁盘缓存上限（约 5GB），超出部分仅保留在内存中");
        for (const frame of this.queue) frame.reject(limitErr);
        this.queue = [];
        this.queuedBytes = 0;
      }
    }
    this.flushing = false;
    if (this.queue.length > 0) void this.flush();
  }

  async frameAt(
    source: string,
    target: number,
    maxDistance = Number.POSITIVE_INFINITY,
  ): Promise<CachedFrameBytes | null> {
    const db = await this.open();
    const cursorValue = async (
      range: IDBKeyRange,
      direction: IDBCursorDirection = "next",
    ): Promise<StoredFrame | null> => {
      const tx = db.transaction(STORE_NAME, "readonly");
      const cursor = await requestResult(
        tx.objectStore(STORE_NAME).openCursor(range, direction),
      );
      return cursor ? cursor.value as StoredFrame : null;
    };

    const [before, after] = await Promise.all([
      cursorValue(
        IDBKeyRange.bound(
          [source, -Number.MAX_VALUE],
          [source, target],
        ),
        "prev",
      ),
      cursorValue(
        IDBKeyRange.bound(
          [source, target],
          [source, Number.MAX_VALUE],
        ),
      ),
    ]);
    const frame = !before
      ? after
      : !after
        ? before
        : target - before.t <= after.t - target
          ? before
          : after;
    if (!frame || Math.abs(frame.t - target) > maxDistance) return null;
    return {
      t: frame.t,
      seq: frame.seq,
      bytes: new Uint8Array(frame.bytes),
    };
  }

  async framesBetween(
    source: string,
    start: number,
    end: number,
  ): Promise<CachedFrameBytes[]> {
    const db = await this.open();
    const tx = db.transaction(STORE_NAME, "readonly");
    const request = tx.objectStore(STORE_NAME).openCursor(
      IDBKeyRange.bound([source, start], [source, end]),
    );
    return new Promise((resolve, reject) => {
      const frames: CachedFrameBytes[] = [];
      request.onsuccess = () => {
        const cursor = request.result;
        if (!cursor) {
          resolve(frames);
          return;
        }
        const frame = cursor.value as StoredFrame;
        frames.push({
          t: frame.t,
          seq: frame.seq,
          bytes: new Uint8Array(frame.bytes),
        });
        cursor.continue();
      };
      request.onerror = () => reject(request.error ?? new Error("IndexedDB 帧区间读取失败"));
    });
  }

  async maxTime(source: string): Promise<number> {
    const db = await this.open();
    const tx = db.transaction(STORE_NAME, "readonly");
    const cursor = await requestResult(
      tx.objectStore(STORE_NAME).openCursor(
        IDBKeyRange.bound(
          [source, -Number.MAX_VALUE],
          [source, Number.MAX_VALUE],
        ),
        "prev",
      ),
    );
    return cursor ? Number((cursor.value as StoredFrame).t) : 0;
  }

  // 【修复打开后初始 42%】换包时主动清掉旧 source 的持久化帧,避免
  // persistedMaxTime 读到上一次打开留下的尾部时间。开 indexed 区间删除,
  // 写事务 + relaxed,失败不影响后续写入。
  async clear(source: string): Promise<void> {
    if (this.disposed) return;
    const db = await this.open();
    const tx = db.transaction(STORE_NAME, "readwrite");
    const store = tx.objectStore(STORE_NAME);
    const range = IDBKeyRange.bound(
      [source, -Number.MAX_VALUE],
      [source, Number.MAX_VALUE],
    );
    await new Promise<void>((resolve, reject) => {
      const req = store.delete(range);
      req.onsuccess = () => resolve();
      req.onerror = () => reject(req.error ?? new Error("IndexedDB 帧清理失败"));
    });
  }

  async requestPersistence(): Promise<void> {
    try {
      await navigator.storage?.persist?.();
    } catch {
      // 浏览器可拒绝持久化授权；IndexedDB 仍可在默认配额内工作。
    }
  }

  // 换数据包时重置熔断：上一个包写满配额不代表新包（更小/清理后）也写不进。
  reset(): void {
    this.failedBatches = 0;
    this.broken = false;
    this.totalBytes = 0;
    this.queuedBytes = 0;
  }

  dispose(): void {
    this.disposed = true;
    if (this.flushTimer !== null) clearTimeout(this.flushTimer);
    for (const frame of this.queue) frame.reject(new Error("帧缓存已关闭"));
    this.queue = [];
    this.queuedBytes = 0;
    void this.dbPromise?.then((db) => db.close()).catch(() => {});
    this.dbPromise = null;
  }
}