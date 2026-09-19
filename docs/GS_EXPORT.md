# MCAP → 3DGS 数据集导出

`scripts/export_gs_dataset.py` 把一条 ROVER MCAP 记录导出成 3D Gaussian Splatting
训练所需的「图片 + 外参 + 主车位姿」三部分数据。

## 数据来源

| 导出内容 | MCAP 来源 | 说明 |
| --- | --- | --- |
| 图片 | `/drivers/camera/*_image_hevc`（`jdx.rover.driver.proto.Image`） | `data`(字段 8) 为 HEVC 裸流，用 FFmpeg（PyAV）解码为 JPG/PNG |
| 图片观测时间 | `Image.sensing_header.measurement_time` | 10Hz 采样时刻，作为文件名与位姿对齐的时间基准 |
| 相机内参 | attachment `sensor_calib_param.conf` → `camera_calibration_intrinsic_map` | `intrisic_param` 3x3 K、`distortion_param` 畸变、分辨率/FOV |
| 相机外参 | 同上 → `connected_frame_trans_container` | `target_frame/source_frame/trans(x,y,z,roll,pitch,yaw)` 链路，链式合成到 `FRAME_BASE_LINK` |
| 主车位姿 | `/localization/odometry_location`（`OdomFusion`，100Hz） | `odometry.pose.position/orientation/attitude`、`twist`、`global_position` |

坐标约定：

- 标定矩阵沿用 xmonitor 的行向量约定 `M = Rx·Ry·Rz·T`，满足 `v_target = v_source · M`；
  导出到 JSON 时转成列向量约定 `T_base_link_camera`（`p_base_link = T · p_camera`）。
- 相机坐标系为光学系（x 右 / y 下 / z 前，OpenCV）。
- `transforms.json` 的 `transform_matrix` 为 camera-to-world，相机系已转成
  Blender/NeRF 约定（x 右 / y 上 / z 后），世界系为 `FRAME_ODOM`，单位米。

## 使用

```bash
python3 -m venv .venv-gs
.venv-gs/bin/pip install -r scripts/requirements-gs-export.txt

.venv-gs/bin/python scripts/export_gs_dataset.py \
  --mcap /path/to/record.mcap \
  --out  /path/to/gs_dataset \
  --cameras all
```

常用参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--cameras` | `all` | 逗号分隔相机名：`camera360_front`、`camera360_front_left`、`camera360_rear`、`monitor_front` … |
| `--max-width` | `0` | 大于该宽度时等比缩小（3DGS 常用 1600/1920） |
| `--stride` | `1` | 每 N 帧保留一张，用于抽帧 |
| `--max-frames` | `0` | 每相机最多导出帧数，用于快速试跑 |
| `--format` / `--quality` | `jpg` / `92` | 图片格式与 JPEG 质量 |
| `--time-format` | `ns` | 文件名为纳秒时间戳；`iso` 输出 `20260901T162423.010522000Z` |
| `--time-source` | `sensor` | `sensor` 用传感器时间戳；`log` 用 MCAP log_time |
| `--calib-ip` | 空 | 多份标定附件时按来源 IP 前缀选择（如 `10.10.1.103`） |
| `--dry-run` | 关 | 只检查相机/标定匹配情况 |

## 输出结构

```
<out>/
  manifest.json                  # 导出统计与参数
  calibration.json               # 各相机内外参（全量）
  ego_pose.csv / ego_pose.json   # 主车轨迹（时间戳/位置/姿态/速度）
  <camera>/
    transforms.json              # 单相机 3DGS 数据集（含逐帧 c2w）
    extrinsics.json              # 该相机静态外参
    images/<t_ns>.jpg            # 文件名含观测时间
  all/transforms.json            # 全相机合并数据集（逐帧内参）
```

`calibration.json` 中每个相机包含：

- `T_base_link_camera` / `T_camera_base_link`：4x4 行主序矩阵；
- `position_base_link_m`、`orientation_quat_xyzw`、`optical_axis_base_link`；
- `intrinsic`：`fx/fy/cx/cy`、`k_matrix_row_major`、`distortion_coefficients`
  （假设 OpenCV 顺序 `k1,k2,p1,p2[,k3,k4,k5,k6]`，宽视场相机建议自行校验）。

## 训练

单相机 3DGS（以 nerfstudio 为例）：

```bash
ns-train splatfacto \
  --data <out>/camera360_front \
  --orientation-method none --center-method none --auto-scale-poses False
```

`transforms.json` 已写入 `orientation_override: none`；显式传 `--orientation-method none
--center-method none --auto-scale-poses False` 可以保留 `FRAME_ODOM` 的米制尺度，便于多相机
对齐以及与点云/地图叠加。

多相机合并训练用 `<out>/all/transforms.json`（逐帧内参与分辨率），同样建议关闭自动定向与
缩放，保证各相机处于同一个 ODOM 世界系。

## 实现要点

- **解码**：`av.CodecContext('hevc')` 逐帧喂入 Annex-B 裸流；用 `packet.pts` 显式标记
  MCAP 消息序号，读回 `frame.pts` 精确对齐「解码帧 ↔ 观测时间」，避免丢帧或 B 帧重排导致
  时间戳错位。
- **效率**：`mcap` 读取按 topic 过滤走 ChunkIndex，跳过无关 chunk；所有相机在一次
  `log_time` 遍历中并行解码，`--max-frames` 达标后该相机不再喂帧。
- **位姿**：主车轨迹独立遍历读取（100Hz 全量），按图片观测时间做位置线性插值 + 四元数
  slerp；超出轨迹范围时钳制到最近位姿（`manifest.json` 会提示缺位姿的图片数）。
