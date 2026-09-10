# Remote Phone Monitor — 项目设计文档

> **文档定位**：既是一份架构设计文档，也是可落地的实现基准（base code）。agent 可依据本文件分模块实现，无需再从头讨论架构。
>
> **范围声明**：本方案不包含绕过 Android 14 屏幕采集授权、隐私指示器（privacy indicator）或任何系统安全机制的实现。采集后端仅采用 Android / 设备所允许的屏幕采集接口（如用户一次性授予的 `MediaProjection`）。设计重点放在**后台常驻、低功耗、异步处理、网络传输与历史归档**上。

---

## 0. 一句话定位

在用户明确授权且拥有管理权的 Android 设备上，通过 **KernelSU/Magisk 模块**启动一个 **Native Agent**，按低频（默认 5s）定时采样屏幕 → `JPEG` 压缩 → 经 `HTTPS` 上传到自托管服务器 → 服务器端保存历史截图并提供 **Web 页面**（最新截图 + 时间轴）。

---

## 1. 项目目标

| 项目 | 内容 |
|---|---|
| 暂定名 | Remote Phone Monitor |
| 客户端 | 已获授权、具备 KernelSU/Magisk 的 Android 设备 |
| 部署方式 | KernelSU 模块（开机自启） |
| 采集方式 | 后台定时采样屏幕（默认每 5 秒 1 张） |
| 编码 | JPEG / WebP 压缩 |
| 上传 | HTTPS 上传到自托管服务器 |
| 服务端 | 保存历史截图，Web 页查看最新截图 + 时间轴 |
| 可靠性 | 网络断开自动重连；手机重启自动恢复 |
| 扩展 | 后续可增加 WebRTC 实时查看 / 远程操作 |

**核心思路**：常态不是"一直传视频"，而是"低频采样 + 历史归档"；仅当用户主动需要实时画面时，才拉起 WebRTC。

---

## 2. 整体架构

### 2.1 常态数据流（定时采样）

```
Android 设备 (KernelSU/Magisk + remote-agent)
        │  每 5s 采样
        ▼
   JPEG / WebP (压缩)
        │  HTTPS
        ▼
   自托管服务器 (HTTPS API)
        │
        ▼
   图片存储 + SQLite 元数据
        │
        ▼
   Web Dashboard (最新截图 + 时间轴)
```

### 2.2 实时数据流（按需）

```
浏览器 (你的电脑/手机)
        │  WebRTC offer
        ▼
   自托管服务器 (信令 / 必要时 TURN)
        │
        ▼
   Android 设备 (remote-agent Live Mode)
        │  H.264 (MediaCodec 硬件编码)
        ▼
   浏览器播放
```

### 2.3 组件清单与职责

| 组件 | 位置 | 职责 |
|---|---|---|
| `remote-monitor` KernelSU 模块 | 手机 | 安装、开机启动、配置、升级/卸载 |
| `remote-agent` (native) | 手机 | 采样、编码、上传、离线队列、可选 WebRTC |
| HTTPS API | 自托管服务器 | 接收上传、鉴权、写库、提供查询接口 |
| 图片存储 | 自托管服务器 | 落盘历史截图（按日期分目录） |
| SQLite / 关系库 | 自托管服务器 | 截图元数据（时间戳、设备、hash、尺寸、大小） |
| Web Dashboard | 自托管服务器 | 最新截图 + 时间轴浏览 |
| 信令 / TURN | 自托管服务器 | 实时模式下的 WebRTC 连接建立（可选） |

---

## 3. 客户端：KernelSU 模块

**为什么做成模块而非普通 App**：模块在 `post-fs-data` / `service.sh` 阶段即可获得 root 上下文、随系统启动、不受后台杀进程限制，适合"常驻 + 自启 + 低依赖"。业务逻辑全部放在 Native Agent，模块本身只做"拉起 + 配置 + 文件布局"。

### 3.1 目录结构

```
remote-monitor/
├── module.prop          # KernelSU 模块元数据
├── service.sh           # 开机启动脚本（推荐入口）
├── uninstall.sh         # 卸载清理
│
├── bin/
│   └── remote-agent     # 编译好的 native 可执行文件
│
├── config/
│   └── config.json      # 运行时配置
│
├── lib/                 # 依赖的 so（curl / jpeg-turbo / sqlite 等，视编译方式）
│
└── logs/                # agent 日志（运行时生成）
```

---

## 4. 开机自启（service.sh）

**推荐用 `service.sh`，不要将大量逻辑写进 `post-fs-data.sh`**。`post-fs-data` 阶段文件系统尚未完全就绪、某些系统属性不可用；`service.sh` 由 KernelSU 在更靠后的阶段调用，更适合拉起长驻进程。

### 4.1 启动流程

```
Android Boot
    ▼
KernelSU
    ▼
service.sh
    ▼
等待 sys.boot_completed = 1
    ▼
sleep 5（确保图形栈/服务就绪）
    ▼
exec remote-agent --config config.json
```

---

## 5. Native Agent（C/C++ + CMake）

Agent 用 **C++17 + CMake**，交叉编译为 Android 可执行文件（NDK，当前目标架构为 `arm64-v8a`）。

### 5.1 模块图

```
remote-agent
├── Config Manager     读取/校验 config.json
├── Scheduler          定时触发（可动态改 interval）
├── Capture Backend    取一帧（抽象接口）
├── Image Encoder      JPEG/WebP 压缩
├── Change Detector    与上帧比对，决定是否上传
├── Upload Manager     HTTPS 上传 + 重试
├── Offline Queue      上传失败落盘，网络恢复后补传
├── Device Status      电量/网络/存储状态
└── Logger             android log + 文件日志
```

---

## 6. 屏幕采集层（CaptureBackend 抽象接口）

把"怎么拿帧"抽象出来，主程序不关心底层实现。以后研究 `Surface / BufferQueue / HardwareBuffer / MediaCodec` 时，只需新增一个 `CaptureBackend` 实现。

### 6.1 合法采集实现（规范）

Android 14 上"设备允许的屏幕采集接口"主要是 **`MediaProjection`**。它要求用户在 UI 上**一次性授权**（系统弹窗 + 隐私指示器）。落地方式有两种合规路径：

1. **Companion 前台服务 + MediaProjection（推荐，最稳）**
   - 一个极轻量的 Android App / 前台服务持有 `MediaProjection` 权限（用户授权一次后，在合法范围内保持）。
   - 它通过 `ImageReader` / `HardwareBuffer` 拿到帧，经 **共享内存（Ashmem）/ Binder** 交给 native `remote-agent`。

2. **抽象接口预留其它实现**
   - 以后若研究 `SurfaceFlinger` / `BufferQueue` 的合法取帧路径，只需新增 `class XxxCapture : public CaptureBackend`，替换工厂返回。

---

## 7. 图片格式与大小

| 项目 | 默认 |
|---|---|
| 格式 | JPEG（WebP 可选） |
| 质量 | `quality = 55 ~ 60`（UI 小字多时提到 70） |
| 典型分辨率 | 1080 × 2400 |
| 单张大小 | 约 50 ~ 200 KB（取决于画面内容） |

---

## 8. 变化检测（Change Detection）

**为什么需要**：手机停在同一页面时，05:00 ~ 05:20 的 240 张截图几乎一样，没必要全存。只保留"首张 + 变化点"，数据库仍记录状态时间线。

**算法**：`ahash` 平均哈希，阈值 `0.90 ~ 0.95`。

---

## 9. 上传与离线队列

- HTTPS 上传（libcurl）
- 离线队列：上传失败落盘，网络恢复后补传
- 队列上限 `max_queue_mb`：超过则丢弃最旧帧

---

## 10. 服务器端设计

- **技术选型**：Python + FastAPI
- **存储**：磁盘落图（按 `YYYY/MM/DD/` 分目录） + SQLite
- **鉴权**：设备 `X-Device-Token` 头
- **Dashboard**：最新截图 + 时间轴浏览

---

## 11. 实现路线图

| 阶段 | 交付物 | 验收 |
|---|---|---|
| **P0 骨架** | KernelSU 模块 + agent 主循环桩 + config.json | 模块可刷入，agent 能启动并写日志 |
| **P1 采集** | 合规 `MediaProjectionCapture`（companion 前台服务） | 能稳定每 5s 取一帧 |
| **P2 编码+上传** | encoder(jpeg) + uploader(HTTPS) + 离线队列 | 服务器收到图、落盘、DB 有记录 |
| **P3 服务端** | FastAPI API + SQLite + Dashboard | 网页看到最新截图 + 时间轴 |
| **P4 优化** | 变化检测 + 动态 interval + 重连/重试 | 静止页面不再重复存储 |
| **P5 WebRTC（可选）** | 信令服务 + agent Live Mode + 浏览器播放 | 点击实时查看可出画面 |
| **P6 加固** | token 管理、Dashboard 鉴权、日志轮转 | 安全与可运维达标 |
