#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 ROVER MCAP 导出 3D Gaussian Splatting 数据集。

一条记录导出的三类数据（对应 3DGS 重建所需的三要素）：

1. 图片：按相机分目录导出 HEVC 帧为 JPG/PNG，文件名含观测时间
   （``sensing_header.measurement_time``，单位 ns）。
2. 外参：来自 MCAP attachment ``sensor_calib_param.conf``（标定 TextFormat）：
   静态外参 ``T_base_link<-camera`` / ``T_camera<-base_link``，以及每张图随时间变化的
   世界位姿 ``T_world<-camera``（写进各相机 ``transforms.json`` 的 ``transform_matrix``）。
3. 主车位姿：``/localization/odometry_location``（OdomFusion）按观测时间插值后的
   位置/姿态/速度，导出为 CSV/JSON。

输出目录结构::

    <out>/
      manifest.json
      calibration.json              # 各相机内外参（静态）
      ego_pose.csv / ego_pose.json  # 主车位姿轨迹
      <camera>/images/<t_ns>.jpg    # 该相机图片
      <camera>/transforms.json      # 单相机可直接训练的 3DGS 数据集
      <camera>/extrinsics.json      # 该相机静态外参
      all/transforms.json           # 全相机合并数据集（逐帧内参）

依赖见 ``scripts/requirements-gs-export.txt``，建议用独立 venv::

    python3 -m venv .venv-gs
    .venv-gs/bin/pip install -r scripts/requirements-gs-export.txt
    .venv-gs/bin/python scripts/export_gs_dataset.py \
        --mcap /path/to/record.mcap --out /path/to/out
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple

import numpy as np


# ---------------------------------------------------------------------------
# 相机 topic 与标定 frame 的对应关系。
# 说明：Image.sensing_header.frame_id 与 FrameId 枚举并不一致（实测 300/320/...），
# 因此用 topic 名直接映射到标定文件里的 frame 名，最稳。
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class CameraSpec:
    name: str            # 输出目录名
    frame: str           # 标定文件中的 frame 名
    topic: str           # MCAP topic


CAMERA_SPECS: Tuple[CameraSpec, ...] = (
    CameraSpec("camera360_front", "FRAME_CAMERA360_FRONT",
               "/drivers/camera/camera360_front_image_hevc"),
    CameraSpec("camera360_front_tele", "FRAME_CAMERA360_FRONT_TELE",
               "/drivers/camera/camera360_front_tele_image_hevc"),
    CameraSpec("camera360_front_left", "FRAME_CAMERA360_FRONT_LEFT",
               "/drivers/camera/camera360_front_left_image_hevc"),
    CameraSpec("camera360_front_right", "FRAME_CAMERA360_FRONT_RIGHT",
               "/drivers/camera/camera360_front_right_image_hevc"),
    CameraSpec("camera360_left", "FRAME_CAMERA360_LEFT",
               "/drivers/camera/camera360_left_image_hevc"),
    CameraSpec("camera360_right", "FRAME_CAMERA360_RIGHT",
               "/drivers/camera/camera360_right_image_hevc"),
    CameraSpec("camera360_rear", "FRAME_CAMERA360_REAR",
               "/drivers/camera/camera360_rear_image_hevc"),
    CameraSpec("camera360_rear_left", "FRAME_CAMERA360_REAR_LEFT",
               "/drivers/camera/camera360_rear_left_image_hevc"),
    CameraSpec("camera360_rear_right", "FRAME_CAMERA360_REAR_RIGHT",
               "/drivers/camera/camera360_rear_right_image_hevc"),
    CameraSpec("monitor_front", "FRAME_CAMERA_MONITOR_FRONT",
               "/drivers/camera/monitor_front_image_hevc"),
    CameraSpec("monitor_left", "FRAME_CAMERA_MONITOR_LEFT",
               "/drivers/camera/monitor_left_image_hevc"),
    CameraSpec("monitor_right", "FRAME_CAMERA_MONITOR_RIGHT",
               "/drivers/camera/monitor_right_image_hevc"),
)

DEFAULT_EGO_TOPIC = "/localization/odometry_location"
FALLBACK_EGO_TOPIC = "/localization/ldmap_location"
FRAME_BASE_LINK = "FRAME_BASE_LINK"
CALIB_ATTACHMENT_KEY = "sensor_calib_param.conf"


# ---------------------------------------------------------------------------
# 最小 protobuf 解析（只按需取标量/子消息，避免依赖生成的 pb2）
# ---------------------------------------------------------------------------
def _read_varint(buf: bytes, i: int) -> Tuple[int, int]:
    result = 0
    shift = 0
    while True:
        b = buf[i]
        i += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, i
        shift += 7
        if shift > 70:
            raise ValueError("varint 过长")


def pb_fields(buf: bytes) -> Iterator[Tuple[int, int, object]]:
    """迭代 (field_number, wire_type, value)。

    wire_type: 0=int, 1=double, 2=bytes, 5=float；其它类型原样跳过。
    """
    i = 0
    n = len(buf)
    while i < n:
        try:
            key, i = _read_varint(buf, i)
        except (IndexError, ValueError):
            return
        number = key >> 3
        wire = key & 7
        if wire == 0:
            value, i = _read_varint(buf, i)
            yield number, wire, value
        elif wire == 1:
            if i + 8 > n:
                return
            yield number, wire, struct.unpack_from("<d", buf, i)[0]
            i += 8
        elif wire == 2:
            length, i = _read_varint(buf, i)
            if i + length > n:
                return
            yield number, wire, buf[i:i + length]
            i += length
        elif wire == 5:
            if i + 4 > n:
                return
            yield number, wire, struct.unpack_from("<f", buf, i)[0]
            i += 4
        else:
            return


def pb_get(buf: bytes, number: int) -> Optional[object]:
    """取第一个匹配字段的值。"""
    for fn, _wire, value in pb_fields(buf):
        if fn == number:
            return value
    return None


def pb_get_all(buf: bytes, number: int) -> List[object]:
    return [value for fn, _wire, value in pb_fields(buf) if fn == number]


def _vec3(buf: bytes) -> np.ndarray:
    """Point3D/Vector3d：x=1, y=2, z=3 (double)。"""
    out = np.zeros(3, dtype=np.float64)
    for fn, wire, value in pb_fields(buf):
        if wire == 1 and 1 <= fn <= 3:
            out[fn - 1] = float(value)
    return out


def quat_to_matrix(q: Sequence[float]) -> np.ndarray:
    """四元数 (qx, qy, qz, qw) -> 3x3 旋转矩阵。"""
    x, y, z, w = (float(v) for v in q)
    n = math.sqrt(x * x + y * y + z * z + w * w)
    if n == 0.0:
        return np.eye(3)
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def make_transform(rotation: np.ndarray, translation: Sequence[float]) -> np.ndarray:
    t = np.eye(4, dtype=np.float64)
    t[:3, :3] = rotation
    t[:3, 3] = np.asarray(translation, dtype=np.float64)
    return t


def normalize_quat(q: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(q)
    return q / n if n > 0 else np.array([0.0, 0.0, 0.0, 1.0])


def quat_slerp(q0: np.ndarray, q1: np.ndarray, t: float) -> np.ndarray:
    q0 = normalize_quat(np.asarray(q0, dtype=np.float64))
    q1 = normalize_quat(np.asarray(q1, dtype=np.float64))
    dot = float(np.dot(q0, q1))
    if dot < 0.0:
        q1 = -q1
        dot = -dot
    if dot > 0.9995:
        return normalize_quat(q0 + t * (q1 - q0))
    theta0 = math.acos(max(-1.0, min(1.0, dot)))
    theta = theta0 * t
    q2 = normalize_quat(q1 - q0 * dot)
    return q0 * math.cos(theta) + q2 * math.sin(theta)


# ---------------------------------------------------------------------------
# 消息解析
# ---------------------------------------------------------------------------
@dataclass
class ImageMessageInfo:
    """Image proto 里与导出相关的最少字段。"""
    payload: bytes
    width: int = 0
    height: int = 0
    channels: int = 0
    measurement_time_ns: int = 0   # sensing_header.measurement_time
    header_time_ns: int = 0        # header.timestamp
    compress_type: int = 0
    encode_frame_type: int = 0
    encode_frame_seq: int = 0


def parse_image_message(data: bytes) -> ImageMessageInfo:
    info = ImageMessageInfo(payload=b"")
    for fn, wire, value in pb_fields(data):
        if fn == 1 and wire == 2:            # header
            ts = pb_get(value, 3)
            if isinstance(ts, int):
                info.header_time_ns = ts
        elif fn == 2 and wire == 2:          # sensing_header
            mt = pb_get(value, 2)
            if isinstance(mt, int):
                info.measurement_time_ns = mt
        elif fn == 3 and wire == 0:
            info.width = int(value)
        elif fn == 4 and wire == 0:
            info.height = int(value)
        elif fn == 5 and wire == 0:
            info.channels = int(value)
        elif fn == 8 and wire == 2:
            info.payload = value
        elif fn == 10 and wire == 0:
            info.compress_type = int(value)
        elif fn == 14 and wire == 0:
            info.encode_frame_type = int(value)
        elif fn == 15 and wire == 0:
            info.encode_frame_seq = int(value)
    return info


@dataclass
class EgoPose:
    time_ns: int
    position: np.ndarray          # (3,) 世界/ODOM 系，米
    quaternion: np.ndarray        # (4,) qx,qy,qz,qw
    attitude: np.ndarray          # (3,) roll,pitch,yaw（rad）
    linear_velocity: np.ndarray   # (3,) m/s
    angular_velocity: np.ndarray  # (3,) rad/s
    global_position: Optional[np.ndarray] = None

    @property
    def yaw(self) -> float:
        return float(self.attitude[2])


def parse_odometry_message(data: bytes, time_ns: int) -> Optional[EgoPose]:
    """解析 OdomFusion / Localization 的 odometry.pose（两者结构一致）。

    路径：odometry(2).pose(3){position(1), orientation(2), attitude(3)}，
    twist(4){linear(1), angular(2)}，global_position(5)。
    """
    odometry = pb_get(data, 2)
    if not isinstance(odometry, bytes):
        return None
    position = np.zeros(3)
    quaternion = np.array([0.0, 0.0, 0.0, 1.0])
    attitude = np.zeros(3)
    pose = pb_get(odometry, 3)
    if not isinstance(pose, bytes):
        return None
    pos_msg = pb_get(pose, 1)
    if isinstance(pos_msg, bytes):
        position = _vec3(pos_msg)
    quat_msg = pb_get(pose, 2)
    if isinstance(quat_msg, bytes):
        q = np.array([0.0, 0.0, 0.0, 0.0])
        for fn, wire, value in pb_fields(quat_msg):
            if wire == 1 and 1 <= fn <= 4:
                q[fn - 1] = float(value)
        if np.linalg.norm(q) > 0:
            quaternion = normalize_quat(q)
    att_msg = pb_get(pose, 3)
    if isinstance(att_msg, bytes):
        attitude = _vec3(att_msg)
    linear = np.zeros(3)
    angular = np.zeros(3)
    twist = pb_get(odometry, 4)
    if isinstance(twist, bytes):
        lin = pb_get(twist, 1)
        if isinstance(lin, bytes):
            linear = _vec3(lin)
        ang = pb_get(twist, 2)
        if isinstance(ang, bytes):
            angular = _vec3(ang)
    global_position = None
    gp = pb_get(odometry, 5)
    if isinstance(gp, bytes):
        global_position = _vec3(gp)
    return EgoPose(time_ns, position, quaternion, attitude, linear, angular,
                   global_position)


# ---------------------------------------------------------------------------
# 标定附件解析（TextFormat 的 CalibrationParam）
# ---------------------------------------------------------------------------
def _iter_brace_blocks(text: str, keyword: str) -> Iterator[str]:
    """逐个产出 ``keyword { ... }`` 的内部文本（支持嵌套花括号）。"""
    pattern = re.compile(re.escape(keyword) + r"\s*\{")
    for match in pattern.finditer(text):
        depth = 1
        i = match.end()
        start = i
        while i < len(text) and depth > 0:
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if depth == 0:
            yield text[start:i]


def _token_after(body: str, key: str) -> str:
    m = re.search(re.escape(key) + r"\s*:\s*([A-Za-z0-9_]+)", body)
    return m.group(1) if m else ""


def _numbers_after(body: str, key: str) -> List[float]:
    m = re.search(re.escape(key) + r"\s*:\s*\[([^\]]*)\]", body)
    if not m:
        return []
    return [float(v) for v in re.findall(r"[-+0-9.eE]+", m.group(1))]


def _scalar_after(body: str, key: str) -> Optional[float]:
    m = re.search(re.escape(key) + r"\s*:\s*([-+0-9.eE]+)", body)
    return float(m.group(1)) if m else None


def _osg_rot_x(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    m = np.eye(4)
    m[1, 1], m[1, 2], m[2, 1], m[2, 2] = c, s, -s, c
    return m


def _osg_rot_y(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    m = np.eye(4)
    m[0, 0], m[0, 2], m[2, 0], m[2, 2] = c, -s, s, c
    return m


def _osg_rot_z(a: float) -> np.ndarray:
    c, s = math.cos(a), math.sin(a)
    m = np.eye(4)
    m[0, 0], m[0, 1], m[1, 0], m[1, 1] = c, s, -s, c
    return m


def calibration_matrix(trans: Sequence[float]) -> np.ndarray:
    """复刻 xmonitor VehicleMatrixTransForm 的行向量约定矩阵。

    trans = (x, y, z, roll, pitch, yaw)，M = Rx(roll)·Ry(pitch)·Rz(yaw)·T，
    行向量约定下满足 ``v_target = v_source · M``。
    """
    x, y, z, roll, pitch, yaw = (float(v) for v in trans[:6])
    t = np.eye(4)
    t[3, 0], t[3, 1], t[3, 2] = x, y, z
    return _osg_rot_x(roll) @ _osg_rot_y(pitch) @ _osg_rot_z(yaw) @ t


@dataclass
class CameraIntrinsic:
    frame: str
    matrix: np.ndarray            # 3x3 K，行主序（来自 intrisic_param）
    distortion: List[float]
    image_width: int
    image_height: int
    focal_length_mm: Optional[float] = None
    pixel_size_mm: Optional[float] = None
    hfov_deg: Optional[float] = None
    vfov_deg: Optional[float] = None

    def to_json(self) -> dict:
        k = self.matrix
        return {
            "frame": self.frame,
            "fx": float(k[0, 0]),
            "fy": float(k[1, 1]),
            "cx": float(k[0, 2]),
            "cy": float(k[1, 2]),
            "k_matrix_row_major": [float(v) for v in k.reshape(-1)],
            "distortion_model": "opencv",
            "distortion_coefficients": [float(v) for v in self.distortion],
            "distortion_names": ["k1", "k2", "p1", "p2", "k3", "k4", "k5", "k6"][
                : len(self.distortion)],
            "image_width": self.image_width,
            "image_height": self.image_height,
            "focal_length_mm": self.focal_length_mm,
            "pixel_size_mm": self.pixel_size_mm,
            "hfov_deg": self.hfov_deg,
            "vfov_deg": self.vfov_deg,
        }


@dataclass
class Calibration:
    # (target, source) -> 行向量约定矩阵 M，满足 v_target = v_source · M
    edges: Dict[Tuple[str, str], np.ndarray] = field(default_factory=dict)
    intrinsics: Dict[str, CameraIntrinsic] = field(default_factory=dict)
    effective_frames: List[str] = field(default_factory=list)

    def col_transform(self, target: str, source: str) -> Optional[np.ndarray]:
        """列向量约定的 ``T_target<-source``（p_target = T · p_source）。"""
        m = self.edge_on_path(target, source)
        return None if m is None else m.T

    def edge_on_path(self, target: str, source: str) -> Optional[np.ndarray]:
        """沿标定图搜索 target<-source 的链路并合成行向量约定矩阵。"""
        if target == source:
            return np.eye(4)
        # 邻接表：frame -> [(neighbor, M)]，M 满足 v_neighbor = v_frame · M
        adjacency: Dict[str, List[Tuple[str, np.ndarray]]] = {}
        for (tgt, src), m in self.edges.items():
            adjacency.setdefault(src, []).append((tgt, m))
            adjacency.setdefault(tgt, []).append((src, np.linalg.inv(m)))
        queue = deque([(source, np.eye(4))])
        seen = {source}
        while queue:
            node, acc = queue.popleft()
            for nxt, m in adjacency.get(node, ()):
                if nxt in seen:
                    continue
                combined = acc @ m
                if nxt == target:
                    return combined
                seen.add(nxt)
                queue.append((nxt, combined))
        return None


def parse_calibration_text(text: str) -> Calibration:
    calib = Calibration()
    eff = re.search(r"effective_frame\s*:\s*\[([^\]]*)\]", text)
    if eff:
        calib.effective_frames = [t.strip() for t in eff.group(1).split(",")
                                  if t.strip()]
    for body in _iter_brace_blocks(text, "connected_frame_trans"):
        target = _token_after(body, "target_frame")
        source = _token_after(body, "source_frame")
        trans = _numbers_after(body, "trans")
        if not target or not source or len(trans) < 6:
            continue
        calib.edges[(target, source)] = calibration_matrix(trans)
    for body in _iter_brace_blocks(text, "camera_param_map"):
        key_match = re.search(r'key\s*:\s*"([^"]+)"', body)
        if not key_match:
            continue
        frame = key_match.group(1)
        intrinsic = _numbers_after(body, "intrisic_param")
        if len(intrinsic) < 9:
            continue
        k = np.array(intrinsic[:9], dtype=np.float64).reshape(3, 3)
        width = _scalar_after(body, "horizontal_resolution")
        height = _scalar_after(body, "vertical_resolution")
        calib.intrinsics[frame] = CameraIntrinsic(
            frame=frame,
            matrix=k,
            distortion=_numbers_after(body, "distortion_param"),
            image_width=int(width) if width else 0,
            image_height=int(height) if height else 0,
            focal_length_mm=_scalar_after(body, "focal_length"),
            pixel_size_mm=_scalar_after(body, "pixel_size"),
            hfov_deg=_scalar_after(body, "HFOV"),
            vfov_deg=_scalar_after(body, "VFOV"),
        )
    return calib


# ---------------------------------------------------------------------------
# MCAP 读取辅助
# ---------------------------------------------------------------------------
def read_attachment_payload(mcap_path: Path, name_key: str,
                            prefer_prefix: str = "") -> Optional[Tuple[str, bytes]]:
    """按 Summary 里的 AttachmentIndex 直接 seek 读附件，不扫描整文件。"""
    from mcap.reader import make_reader

    with open(mcap_path, "rb") as f:
        reader = make_reader(f)
        summary = reader.get_summary()
        if summary is None:
            return None
        candidates = [a for a in summary.attachment_indexes if name_key in a.name]
        if not candidates:
            return None
        if prefer_prefix:
            preferred = [a for a in candidates if a.name.startswith(prefer_prefix)]
            if preferred:
                candidates = preferred
        index = candidates[0]
        f.seek(index.offset)
        header = f.read(9)
        if len(header) != 9 or header[0] != 0x09:
            return None
        length = struct.unpack_from("<Q", header, 1)[0]
        body = f.read(length)
    offset = 0
    offset += 8  # log_time
    offset += 8  # create_time
    name_len = struct.unpack_from("<I", body, offset)[0]
    offset += 4
    name = body[offset:offset + name_len].decode("utf-8", "replace")
    offset += name_len
    media_len = struct.unpack_from("<I", body, offset)[0]
    offset += 4
    offset += media_len
    data_len = struct.unpack_from("<Q", body, offset)[0]
    offset += 8
    return name, body[offset:offset + data_len]


def list_topics(mcap_path: Path) -> List[str]:
    from mcap.reader import make_reader

    with open(mcap_path, "rb") as f:
        reader = make_reader(f)
        summary = reader.get_summary()
        if summary is None:
            return []
        return sorted({ch.topic for ch in summary.channels.values()})


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="从 ROVER MCAP 导出 3DGS 数据集（图片 + 外参 + 主车位姿）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--mcap", required=True, help="输入 MCAP 路径")
    parser.add_argument("--out", required=True, help="输出目录")
    parser.add_argument("--cameras", default="all",
                        help="逗号分隔的相机名（或 all）：" +
                             ", ".join(c.name for c in CAMERA_SPECS))
    parser.add_argument("--ego-topic", default=DEFAULT_EGO_TOPIC,
                        help="主车位姿 topic")
    parser.add_argument("--calib-ip", default="",
                        help="标定附件来源 IP 前缀（多份标定附件时用于选择）")
    parser.add_argument("--format", default="jpg", choices=("jpg", "png"),
                        help="图片格式")
    parser.add_argument("--quality", type=int, default=92,
                        help="JPEG 质量（png 忽略）")
    parser.add_argument("--max-width", type=int, default=0,
                        help="超过该宽度则等比缩放（0 = 保持原分辨率）")
    parser.add_argument("--stride", type=int, default=1,
                        help="每隔 N 帧保留一张（1 = 全保留）")
    parser.add_argument("--max-frames", type=int, default=0,
                        help="每相机最多导出帧数（0 = 不限，用于快速试跑）")
    parser.add_argument("--time-format", default="ns", choices=("ns", "iso"),
                        help="文件名里的观测时间格式")
    parser.add_argument("--time-source", default="sensor",
                        choices=("sensor", "log"),
                        help="sensor=measurement_time/header.timestamp；log=MCAP log_time")
    parser.add_argument("--no-ego-interp", action="store_true",
                        help="不做时间插值，直接用最近的主车位姿")
    parser.add_argument("--dry-run", action="store_true",
                        help="只打印将导出的相机与统计，不写文件")
    return parser.parse_args(argv)


def select_cameras(spec: str, available: Sequence[str]) -> List[CameraSpec]:
    available_set = set(available)
    ordered = [c for c in CAMERA_SPECS if c.topic in available_set]
    if spec.strip().lower() == "all":
        return ordered
    wanted = [s.strip() for s in spec.split(",") if s.strip()]
    by_name = {c.name: c for c in ordered}
    unknown = [w for w in wanted if w not in by_name]
    if unknown:
        raise SystemExit("未知相机: %s\n可选: %s" %
                         (", ".join(unknown), ", ".join(by_name)))
    return [by_name[w] for w in wanted]


def ns_to_iso(ns: int) -> str:
    """纳秒时间戳 -> 文件名友好的 UTC 字符串。"""
    import datetime
    seconds, frac = divmod(ns, 1_000_000_000)
    dt = datetime.datetime.fromtimestamp(seconds, tz=datetime.timezone.utc)
    return dt.strftime("%Y%m%dT%H%M%S") + ".%09dZ" % frac


class EgoTrajectory:
    """主车位姿序列 + 时间插值。"""

    def __init__(self, poses: List[EgoPose], interpolate: bool = True):
        self.poses = sorted(poses, key=lambda p: p.time_ns)
        self.interpolate = interpolate
        self.times = np.array([p.time_ns for p in self.poses], dtype=np.int64)

    def __bool__(self) -> bool:
        return bool(self.poses)

    def sample(self, time_ns: int) -> Optional[EgoPose]:
        if not self.poses:
            return None
        idx = int(np.searchsorted(self.times, time_ns))
        if idx <= 0:
            return self.poses[0]
        if idx >= len(self.poses):
            return self.poses[-1]
        a = self.poses[idx - 1]
        b = self.poses[idx]
        if not self.interpolate or b.time_ns == a.time_ns:
            return a if (time_ns - a.time_ns) <= (b.time_ns - time_ns) else b
        t = (time_ns - a.time_ns) / float(b.time_ns - a.time_ns)
        position = a.position + t * (b.position - a.position)
        quaternion = quat_slerp(a.quaternion, b.quaternion, t)
        attitude = a.attitude + t * (b.attitude - a.attitude)
        linear = a.linear_velocity + t * (b.linear_velocity - a.linear_velocity)
        angular = a.angular_velocity + t * (b.angular_velocity - a.angular_velocity)
        return EgoPose(int(time_ns), position, quaternion, attitude, linear,
                       angular, None)


@dataclass
class ExportedImage:
    camera: str
    frame: str
    file_path: str          # 相对入口目录
    measurement_time_ns: int
    header_time_ns: int
    log_time_ns: int
    width: int
    height: int
    # 由后续步骤填充
    c2w_world: Optional[np.ndarray] = None
    ego_pose: Optional[EgoPose] = None


def build_image_saver(fmt: str, quality: int, max_width: int) -> Callable:
    from PIL import Image

    def save(frame, path: Path):
        image = frame.to_image()
        if max_width and image.width > max_width:
            ratio = max_width / float(image.width)
            image = image.resize(
                (max_width, max(1, int(round(image.height * ratio)))),
                Image.LANCZOS,
            )
        path.parent.mkdir(parents=True, exist_ok=True)
        if fmt == "jpg":
            image.convert("RGB").save(path, quality=quality, subsampling=0)
        else:
            image.save(path)
        return image.size  # (w, h)

    return save


def load_ego_poses(mcap_path: Path, topic: str,
                   use_log_time: bool = False) -> List[EgoPose]:
    """独立遍历主车定位 topic，读全量位姿（与相机导出解耦，试跑也完整）。"""
    from mcap.reader import make_reader

    poses: List[EgoPose] = []
    if not topic:
        return poses
    with open(mcap_path, "rb") as f:
        reader = make_reader(f)
        for _schema, _channel, message in reader.iter_messages(
                topics=[topic], log_time_order=True):
            if use_log_time:
                time_ns = message.log_time
            else:
                header = pb_get(message.data, 1)
                header_ts = pb_get(header, 3) if isinstance(header, bytes) else None
                time_ns = int(header_ts) if isinstance(header_ts, int) else \
                    message.log_time
            pose = parse_odometry_message(message.data, time_ns)
            if pose is not None:
                poses.append(pose)
    return poses


def export_images(mcap_path: Path, out_dir: Path, cameras: Sequence[CameraSpec],
                  args: argparse.Namespace
                  ) -> Tuple[Dict[str, List[ExportedImage]], dict]:
    """单遍扫描 MCAP：按相机解码 HEVC 并存图。"""
    import av
    from mcap.reader import make_reader

    save_image = build_image_saver(args.format, args.quality, args.max_width)
    topics = [c.topic for c in cameras]
    topic_to_camera = {c.topic: c for c in cameras}
    decoders: Dict[str, "av.CodecContext"] = {}
    pending: Dict[str, deque] = {}
    message_index: Dict[str, int] = {}
    message_meta: Dict[str, Dict[int, ImageMessageInfo]] = {}
    message_log_time: Dict[str, Dict[int, int]] = {}
    exported: Dict[str, List[ExportedImage]] = {c.name: [] for c in cameras}
    saved_count: Dict[str, int] = {c.name: 0 for c in cameras}
    use_log_time = args.time_source == "log"
    last_report = time.time()
    processed = 0

    for camera in cameras:
        ctx = av.CodecContext.create("hevc", "r")
        try:
            ctx.flags |= av.codec.context.Flags.LOW_DELAY
        except Exception:
            pass
        decoders[camera.name] = ctx
        pending[camera.name] = deque()
        message_index[camera.name] = 0
        message_meta[camera.name] = {}
        message_log_time[camera.name] = {}

    scan_topics = list(topics)

    def camera_done(name: str) -> bool:
        return bool(args.max_frames) and saved_count[name] >= args.max_frames

    with open(mcap_path, "rb") as f:
        reader = make_reader(f)
        for _schema, channel, message in reader.iter_messages(
                topics=scan_topics, log_time_order=True):
            processed += 1
            if all(camera_done(c.name) for c in cameras):
                break
            if processed % 200 == 0 and time.time() - last_report > 15:
                done = ", ".join("%s=%d" % (k, v) for k, v in saved_count.items() if v)
                print("  ... 已处理 %d 条消息（%s）" % (processed, done), flush=True)
                last_report = time.time()

            camera = topic_to_camera.get(channel.topic)
            if camera is None:
                continue
            # 达到 --max-frames 后该相机不再喂帧，省掉无谓的解码开销。
            if camera_done(camera.name):
                continue
            info = parse_image_message(message.data)
            if not info.payload:
                continue
            index = message_index[camera.name]
            message_index[camera.name] += 1
            message_meta[camera.name][index] = info
            message_log_time[camera.name][index] = message.log_time
            pending[camera.name].append(index)

            ctx = decoders[camera.name]
            packets = ctx.parse(info.payload)
            for packet in packets:
                if not pending[camera.name]:
                    continue
                idx = pending[camera.name].popleft()
                packet.pts = idx
                packet.dts = idx
                for frame in ctx.decode(packet):
                    pts = frame.pts if frame.pts is not None else idx
                    meta = message_meta[camera.name].get(pts)
                    if meta is None:
                        continue
                    keep = (pts % args.stride == 0)
                    if args.max_frames and saved_count[camera.name] >= args.max_frames:
                        keep = False
                    if not keep:
                        continue
                    observed_ns = meta.measurement_time_ns or meta.header_time_ns
                    if use_log_time:
                        observed_ns = message_log_time[camera.name].get(
                            pts, message.log_time)
                    name = ns_to_iso(observed_ns) if args.time_format == "iso" \
                        else str(observed_ns)
                    rel_path = Path(camera.name) / "images" / \
                        ("%s.%s" % (name, args.format))
                    width, height = save_image(frame, out_dir / rel_path)
                    exported[camera.name].append(ExportedImage(
                        camera=camera.name,
                        frame=camera.frame,
                        file_path=rel_path.as_posix(),
                        measurement_time_ns=observed_ns,
                        header_time_ns=meta.header_time_ns,
                        log_time_ns=message_log_time[camera.name].get(
                            pts, message.log_time),
                        width=width,
                        height=height,
                    ))
                    saved_count[camera.name] += 1

    # flush（LOW_DELAY 下一般无剩余帧）
    for camera in cameras:
        ctx = decoders[camera.name]
        try:
            remaining = ctx.decode(None)
        except Exception:
            remaining = []
        for frame in remaining or []:
            pts = frame.pts
            meta = message_meta[camera.name].get(pts) if pts is not None else None
            if meta is None:
                continue
            observed_ns = meta.measurement_time_ns or meta.header_time_ns
            name = ns_to_iso(observed_ns) if args.time_format == "iso" \
                else str(observed_ns)
            rel_path = Path(camera.name) / "images" / \
                ("%s.%s" % (name, args.format))
            width, height = save_image(frame, out_dir / rel_path)
            exported[camera.name].append(ExportedImage(
                camera=camera.name,
                frame=camera.frame,
                file_path=rel_path.as_posix(),
                measurement_time_ns=observed_ns,
                header_time_ns=meta.header_time_ns,
                log_time_ns=message_log_time[camera.name].get(pts, 0),
                width=width,
                height=height,
            ))
            saved_count[camera.name] += 1

    stats = {
        "camera_message_counts": {c.name: message_index[c.name] for c in cameras},
        "exported_counts": saved_count,
    }
    return exported, stats


def write_calibration(out_dir: Path, calib: Calibration,
                      cameras: Sequence[CameraSpec]) -> dict:
    payload: Dict[str, dict] = {"frames": {}, "cameras": {}}
    for camera in cameras:
        t_base_cam = calib.col_transform(FRAME_BASE_LINK, camera.frame)
        t_cam_base = calib.col_transform(camera.frame, FRAME_BASE_LINK)
        intrinsic = calib.intrinsics.get(camera.frame)
        entry = {
            "camera": camera.name,
            "frame": camera.frame,
            "topic": camera.topic,
            "parent_frame": FRAME_BASE_LINK,
            "has_extrinsic": t_base_cam is not None,
            "T_base_link_camera": None if t_base_cam is None
            else [float(v) for v in t_base_cam.reshape(-1)],
            "T_camera_base_link": None if t_cam_base is None
            else [float(v) for v in t_cam_base.reshape(-1)],
            "intrinsic": intrinsic.to_json() if intrinsic else None,
        }
        if t_base_cam is not None:
            rotation = t_base_cam[:3, :3]
            translation = t_base_cam[:3, 3]
            entry["position_base_link_m"] = [float(v) for v in translation]
            entry["orientation_quat_xyzw"] = quaternion_from_matrix(rotation)
            entry["optical_axis_base_link"] = [
                float(v) for v in (rotation.T @ np.array([0.0, 0.0, 1.0]))]
        payload["cameras"][camera.name] = entry
    payload["calibration_frames"] = sorted(calib.edges.keys())
    payload["effective_frames"] = calib.effective_frames
    payload["convention"] = {
        "matrix_layout": "row-major 4x4",
        "T_base_link_camera": "p_base_link = T · p_camera（列向量约定）",
        "camera_axis": "光学系 x 右 / y 下 / z 前（OpenCV）",
        "distortion_order": "按 OpenCV 约定假设为 k1,k2,p1,p2[,k3,k4,k5,k6]；"
                            "宽视场/360 相机建议用 distortion_coefficients 自行校验",
        "source": "MCAP attachment %s" % CALIB_ATTACHMENT_KEY,
    }
    (out_dir / "calibration.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    for camera in cameras:
        entry = payload["cameras"][camera.name]
        camera_dir = out_dir / camera.name
        camera_dir.mkdir(parents=True, exist_ok=True)
        (camera_dir / "extrinsics.json").write_text(
            json.dumps(entry, ensure_ascii=False, indent=2), encoding="utf-8")
    return payload


def quaternion_from_matrix(rotation: np.ndarray) -> List[float]:
    """3x3 旋转矩阵 -> 四元数 (qx, qy, qz, qw)。"""
    m = rotation
    trace = m[0, 0] + m[1, 1] + m[2, 2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        qw = 0.25 * s
        qx = (m[2, 1] - m[1, 2]) / s
        qy = (m[0, 2] - m[2, 0]) / s
        qz = (m[1, 0] - m[0, 1]) / s
    elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
        s = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
        qw = (m[2, 1] - m[1, 2]) / s
        qx = 0.25 * s
        qy = (m[0, 1] + m[1, 0]) / s
        qz = (m[0, 2] + m[2, 0]) / s
    elif m[1, 1] > m[2, 2]:
        s = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
        qw = (m[0, 2] - m[2, 0]) / s
        qx = (m[0, 1] + m[1, 0]) / s
        qy = 0.25 * s
        qz = (m[1, 2] + m[2, 1]) / s
    else:
        s = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
        qw = (m[1, 0] - m[0, 1]) / s
        qx = (m[0, 2] + m[2, 0]) / s
        qy = (m[1, 2] + m[2, 1]) / s
        qz = 0.25 * s
    return [qx, qy, qz, qw]


def write_ego_pose(out_dir: Path, poses: Sequence[EgoPose]) -> None:
    lines = ["timestamp_ns,x,y,z,qx,qy,qz,qw,roll,pitch,yaw,"
             "linear_vx,linear_vy,linear_vz,angular_vx,angular_vy,angular_vz"]
    for p in sorted(poses, key=lambda x: x.time_ns):
        q = normalize_quat(p.quaternion)
        lines.append("%d,%.6f,%.6f,%.6f,%.9f,%.9f,%.9f,%.9f,"
                     "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f" % (
                         p.time_ns, p.position[0], p.position[1], p.position[2],
                         q[0], q[1], q[2], q[3],
                         p.attitude[0], p.attitude[1], p.attitude[2],
                         p.linear_velocity[0], p.linear_velocity[1],
                         p.linear_velocity[2], p.angular_velocity[0],
                         p.angular_velocity[1], p.angular_velocity[2]))
    (out_dir / "ego_pose.csv").write_text("\n".join(lines) + "\n",
                                          encoding="utf-8")
    payload = {
        "frame": "FRAME_ODOM",
        "unit": {"position": "m", "angle": "rad", "velocity": "m/s"},
        "columns": ["timestamp_ns", "x", "y", "z", "qx", "qy", "qz", "qw",
                    "roll", "pitch", "yaw",
                    "linear_vx", "linear_vy", "linear_vz",
                    "angular_vx", "angular_vy", "angular_vz"],
        "poses": [{
            "timestamp_ns": p.time_ns,
            "position": [float(v) for v in p.position],
            "orientation_quat_xyzw": [float(v) for v in normalize_quat(p.quaternion)],
            "attitude_rpy_rad": [float(v) for v in p.attitude],
            "linear_velocity": [float(v) for v in p.linear_velocity],
            "angular_velocity": [float(v) for v in p.angular_velocity],
            "global_position": None if p.global_position is None
            else [float(v) for v in p.global_position],
        } for p in sorted(poses, key=lambda x: x.time_ns)],
    }
    (out_dir / "ego_pose.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


# OpenCV 光学系 (x 右, y 下, z 前) -> Blender/NeRF 相机系 (x 右, y 上, z 后)
OPTICAL_TO_BLENDER = np.diag([1.0, -1.0, -1.0, 1.0])


def resolve_world_poses(images: List[ExportedImage], trajectory: EgoTrajectory,
                        calib: Calibration) -> int:
    missing = 0
    for image in images:
        pose = trajectory.sample(image.measurement_time_ns)
        if pose is None:
            missing += 1
            continue
        t_world_base = make_transform(quat_to_matrix(pose.quaternion), pose.position)
        t_base_cam = calib.col_transform(FRAME_BASE_LINK, image.frame)
        if t_base_cam is None:
            missing += 1
            continue
        image.ego_pose = pose
        image.c2w_world = t_world_base @ t_base_cam @ OPTICAL_TO_BLENDER
    return missing


def build_transforms_entry(image: ExportedImage, calib: Calibration) -> dict:
    intrinsic = calib.intrinsics.get(image.frame)
    entry = {
        "file_path": image.file_path,
        "transform_matrix": [[float(v) for v in row]
                             for row in image.c2w_world],
        "timestamp_ns": image.measurement_time_ns,
        "timestamp": image.measurement_time_ns / 1e9,
        "camera": image.camera,
        "camera_frame": image.frame,
        "width": image.width,
        "height": image.height,
    }
    if intrinsic is not None:
        scale_x = image.width / float(intrinsic.image_width) \
            if intrinsic.image_width else 1.0
        scale_y = image.height / float(intrinsic.image_height) \
            if intrinsic.image_height else 1.0
        entry["fl_x"] = float(intrinsic.matrix[0, 0] * scale_x)
        entry["fl_y"] = float(intrinsic.matrix[1, 1] * scale_y)
        entry["cx"] = float(intrinsic.matrix[0, 2] * scale_x)
        entry["cy"] = float(intrinsic.matrix[1, 2] * scale_y)
        entry["w"] = image.width
        entry["h"] = image.height
        if len(intrinsic.distortion) >= 4:
            entry["k1"], entry["k2"], entry["p1"], entry["p2"] = \
                [float(v) for v in intrinsic.distortion[:4]]
    return entry


def write_transforms(out_dir: Path, images: Sequence[ExportedImage],
                     calib: Calibration, camera: Optional[CameraSpec],
                     name: str) -> Optional[Path]:
    usable = [im for im in images if im.c2w_world is not None]
    if not usable:
        return None
    directory = out_dir / name
    directory.mkdir(parents=True, exist_ok=True)
    intrinsic = calib.intrinsics.get(camera.frame) if camera else None
    payload: Dict[str, object] = {
        "camera_model": "OPENCV",
        "orientation_override": "none",
        "notes": [
            "transform_matrix 为 camera-to-world（相机系 x 右 / y 上 / z 后，"
            "Blender/NeRF 约定），世界系为 FRAME_ODOM，单位为米。",
            "训练建议：--orientation-method none --center-method none "
            "--auto-scale-poses False 保留 ODOM 米制尺度。",
        ],
        "frames": [
            build_transforms_entry(im, calib) for im in usable
        ],
    }
    if intrinsic is not None:
        payload["fl_x"] = float(intrinsic.matrix[0, 0])
        payload["fl_y"] = float(intrinsic.matrix[1, 1])
        payload["cx"] = float(intrinsic.matrix[0, 2])
        payload["cy"] = float(intrinsic.matrix[1, 2])
        payload["w"] = intrinsic.image_width
        payload["h"] = intrinsic.image_height
        payload["k1"], payload["k2"], payload["p1"], payload["p2"] = \
            (list(intrinsic.distortion) + [0.0, 0.0, 0.0, 0.0])[:4]
    path = directory / "transforms.json"
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    return path


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    mcap_path = Path(args.mcap).expanduser()
    if not mcap_path.is_file():
        raise SystemExit("MCAP 不存在: %s" % mcap_path)
    out_dir = Path(args.out).expanduser()
    out_dir.mkdir(parents=True, exist_ok=True)

    topics = list_topics(mcap_path)
    cameras = select_cameras(args.cameras, topics)
    if not cameras:
        raise SystemExit("MCAP 中没有可用相机 topic")
    ego_topic = args.ego_topic
    if ego_topic not in topics:
        if FALLBACK_EGO_TOPIC in topics:
            print("[warn] %s 不存在，改用 %s" % (ego_topic, FALLBACK_EGO_TOPIC))
            ego_topic = FALLBACK_EGO_TOPIC
        else:
            ego_topic = ""
    args.ego_topic = ego_topic

    print("MCAP: %s" % mcap_path)
    print("相机: %s" % ", ".join(c.name for c in cameras))
    print("主车定位 topic: %s" % (ego_topic or "<无>"))

    attachment = read_attachment_payload(mcap_path, CALIB_ATTACHMENT_KEY,
                                         args.calib_ip)
    if attachment is None:
        raise SystemExit("未找到标定附件 %s" % CALIB_ATTACHMENT_KEY)
    calib_name, calib_blob = attachment
    calib = parse_calibration_text(calib_blob.decode("utf-8", "replace"))
    print("标定: %s（%d 条外参，%d 个相机内参）" %
          (calib_name, len(calib.edges), len(calib.intrinsics)))

    if args.dry_run:
        for camera in cameras:
            t = calib.col_transform(FRAME_BASE_LINK, camera.frame)
            k = calib.intrinsics.get(camera.frame)
            print("  %-24s 外参=%-5s 内参=%-5s" %
                  (camera.name, t is not None, k is not None))
        return 0

    started = time.time()
    ego_poses = load_ego_poses(mcap_path, ego_topic,
                               use_log_time=args.time_source == "log")
    print("主车位姿: %d 条" % len(ego_poses))
    exported, stats = export_images(mcap_path, out_dir, cameras, args)
    if not ego_poses:
        print("[warn] 未解出主车位姿，图片外参只有静态部分")
    trajectory = EgoTrajectory(ego_poses, interpolate=not args.no_ego_interp)

    total_images = 0
    missing_pose = 0
    results = {}
    for camera in cameras:
        images = exported[camera.name]
        total_images += len(images)
        missing_pose += resolve_world_poses(images, trajectory, calib)
        results[camera.name] = len(images)

    write_calibration(out_dir, calib, cameras)
    write_ego_pose(out_dir, ego_poses)

    written = {}
    for camera in cameras:
        path = write_transforms(out_dir, exported[camera.name], calib, camera,
                                camera.name)
        if path:
            written[camera.name] = path.relative_to(out_dir).as_posix()
    all_images = [im for camera in cameras for im in exported[camera.name]]
    all_path = write_transforms(out_dir, all_images, calib, None, "all")

    manifest = {
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "mcap": str(mcap_path),
        "mcap_size_bytes": mcap_path.stat().st_size,
        "elapsed_sec": round(time.time() - started, 2),
        "calibration_attachment": calib_name,
        "ego_topic": ego_topic or None,
        "ego_pose_count": len(ego_poses),
        "time_source": args.time_source,
        "image_format": args.format,
        "image_quality": args.quality if args.format == "jpg" else None,
        "max_width": args.max_width or None,
        "stride": args.stride,
        "frames_per_camera": results,
        "total_images": total_images,
        "images_without_pose": missing_pose,
        "transforms": written,
        "all_transforms": all_path.relative_to(out_dir).as_posix()
        if all_path else None,
        "stats": stats,
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    print("\n导出完成：%d 张图片，耗时 %.1fs" % (total_images, time.time() - started))
    for camera in cameras:
        print("  %-24s %d 张" % (camera.name, results[camera.name]))
    if missing_pose:
        print("  [warn] %d 张图片缺少外参/位姿" % missing_pose)
    print("输出目录: %s" % out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
