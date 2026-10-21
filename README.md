<div align="center">

# Android 远程屏幕采样系统

**让设备画面，回到自己的服务里。**

Remote-Phone-Monitor · 面向自有或已获明确授权 Android 设备的自托管屏幕采样工具。

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/平台-Android%20arm64--v8a-3DDC84)](#运行环境与兼容说明)
[![Release](https://img.shields.io/github/v/release/Jia060117/Remote-Phone-Monitor?label=下载)](https://github.com/Jia060117/Remote-Phone-Monitor/releases)
[![CI](https://github.com/Jia060117/Remote-Phone-Monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/Jia060117/Remote-Phone-Monitor/actions/workflows/ci.yml)

由 **[情绪零碎の佳 · Jia060117](https://github.com/Jia060117)** 发布与维护

[下载发行版](https://github.com/Jia060117/Remote-Phone-Monitor/releases) · [运行环境](#运行环境与兼容说明) · [使用方法](#开始使用) · [源码构建](BUILD.md) · [反馈问题](https://github.com/Jia060117/Remote-Phone-Monitor/issues)

**简体中文（默认）** | [English](README_EN.md)

</div>

在自己的设备与服务之间，建立一条可以回看的画面时间轴。

Remote-Phone-Monitor 由 **Android 原生采集端、KernelSU/Magisk 模块和自托管服务端**组成。采集端按设定间隔获取画面，经 HTTPS 上传到 FastAPI 服务；你可以在浏览器里查看最新截图、回看时间轴、了解存储占用，并按设备调整配置与定时策略。

它面向需要管理自有或明确授权设备的用户，也适合对 Android 原生开发、设备通信和自托管服务感兴趣的开发者了解与实践。

> [!WARNING]
> 屏幕截图可能包含密码、消息、金融信息等高度敏感内容。仅可在设备所有者和所有受影响用户明确知情并同意的前提下使用。请遵守当地法律；本项目不得用于隐蔽监控。

## 能做什么

| 功能 | 具体表现 |
| --- | --- |
| 定时采集画面 | C++17 原生采集端配合 KernelSU/Magisk 模块运行，按配置间隔进行屏幕采样。 |
| 上传与断网补传 | 支持 HTTPS 上传、画面变化检测、离线队列和网络恢复后的补传。 |
| 节能与流量策略 | 支持息屏暂停、蜂窝网络低画质和仅 Wi-Fi 上传，可按需要调整。 |
| 浏览器查看 | 登录后查看最新截图、时间轴、存储统计与实时预览。 |
| 远程配置 | 按设备下发配置与定时策略，减少反复修改本地配置的操作。 |
| 存储自动清理 | 按保留时长或容量限制清理截图，服务端使用 FastAPI 与 SQLite。 |
| 独立部署凭据 | 初始化时生成独立设备令牌、随机管理密码与会话密钥。 |

## 运行环境与兼容说明

- **Android 端：** 预编译模块面向 **arm64-v8a** 设备，需要已安装 **KernelSU 或 Magisk**。这不是普通的免 Root APK。
- **服务端：** 需要 **Python 3.9+**，发行包提供 Windows PowerShell 与 Linux/macOS 启动脚本。
- **网络：** 手机必须能够访问配置的服务地址。公网部署应使用 **HTTPS 反向代理与防火墙**，SQLite 服务建议保持单个工作进程。
- **自行构建：** Android 交叉编译需要 **Android NDK r25+**；完整环境与命令见 [构建与部署说明](BUILD.md)。

👉 **[下载统一发行包](https://github.com/Jia060117/Remote-Phone-Monitor/releases/latest)**，选择文件名以 `-bundle.zip` 结尾的附件。

当前 v1.0.0 的统一包为 `Remote-Phone-Monitor-v1.0.0-bundle.zip`。**Source code** 压缩包是源码，不是可直接安装的模块。

## 开始使用

### 1. 下载并初始化

从 [Releases](https://github.com/Jia060117/Remote-Phone-Monitor/releases) 下载统一包并解压，在**解压目录**中运行：

```bash
python initialize.py --server-url https://你的服务地址
```

Linux/macOS 可按本机环境将 `python` 替换为 `python3`。这里填写的是 **Android 设备实际能够访问的服务地址**，并需要将该地址反向代理到后续启动的服务端。

初始化会生成匹配的服务端配置与可安装模块：

| 文件 | 用途 |
| --- | --- |
| `server/tokens.json` | 设备令牌与匿名设备 ID 的映射。 |
| `server/server.json` | 随机管理密码、会话密钥与存储保留策略。 |
| `generated/credentials.txt` | 私密凭据备份，请妥善保管。 |
| `generated/Remote-Phone-Monitor-module-v1.0.0.zip` | 初始化后生成的最终 Android 模块。 |

### 2. 启动服务端

Windows PowerShell：

```powershell
.\start-server.ps1
```

Linux/macOS：

```bash
./start-server.sh
```

发行包启动脚本默认监听 `0.0.0.0:8000`。对外部署前，请先配置 HTTPS 反向代理和防火墙；手动部署与单进程设置见 [BUILD.md](BUILD.md)。

### 3. 安装模块并查看画面

1. 将 `generated/` 下生成的模块 ZIP 传到已授权的 Android 设备。
2. 通过 **KernelSU 或 Magisk 管理器**安装该 ZIP，并重启设备。
3. 在浏览器访问你的服务地址，使用 `generated/credentials.txt` 中的管理员密码登录。
4. 查看设备是否已上传画面，再根据需要调整采集间隔、流量策略、保留时长与远程配置。

**不要直接安装 `module-template` 模板包。** 其中的占位配置会拒绝启动；应使用初始化程序生成的最终模块。

## 使用前了解这些边界

- 本项目按间隔采集截图，不等同于持续的视频直播或远程桌面。
- 上传与配置请求默认需要有效设备令牌；网页控制台、截图、设备信息和管理 API 默认要求管理员登录。
- 设备令牌、截图和管理凭据都可能包含敏感信息。不要分享私密凭据文件、真实配置、数据库、截图、日志或离线队列。
- 无法验证 TLS 证书时，客户端会终止 HTTPS 请求，不会静默关闭证书校验。
- 屏幕采样会占用电量、流量与存储。建议按需求调大采集间隔，启用合适的息屏、Wi-Fi 与保留策略。

完整的部署要求见 **[安全与隐私说明](SECURITY.md)**。

## 关键配置

| 字段 | 含义 | 建议 |
| --- | --- | --- |
| `device_id` | 匿名设备标识 | 与服务端令牌映射一致，不包含姓名、型号或序列号等信息。 |
| `server.urls` | 服务端地址 | 填写设备可访问的 HTTPS 地址。 |
| `server.token` | 设备令牌 | 每台设备使用独立随机值，与服务端配置匹配。 |
| `capture.interval_sec` | 采样间隔 | 按需要适当调大，平衡画面密度与资源占用。 |
| `capture.pause_on_screen_off` | 息屏暂停 | 建议开启。 |
| `upload.wifi_only` | 仅 Wi-Fi 上传 | 对移动流量敏感时开启。 |
| `upload.max_queue_mb` | 离线队列上限 | 根据设备可用存储设置。 |
| `server.config_poll_sec` | 配置轮询间隔 | 默认 300 秒。 |

运行态配置优先级：**内置默认值 → 本地配置 → 服务端配置 → 命中的定时策略**，后者覆盖前者。

## 常见问题

**上传返回 401 或 403**

401 通常需要检查采集端的 `server.token` 是否存在于服务端 `tokens.json`；403 则检查 `device_id` 是否与该令牌映射的设备 ID 一致。统一包的初始化程序会为服务端与模块写入匹配配置。

**管理页面返回 503**

检查 `server/server.json` 中的管理密码，或 `ADMIN_PASSWORD` 环境变量是否已设置。统一包用户应先完成初始化，再启动服务。

**HTTPS 请求失败**

检查服务地址、证书链、设备系统信任证书与设备时间。客户端不会自动跳过证书校验；更多排查项见 [构建与部署说明](BUILD.md#5-常见问题)。

**远程配置没有立即生效**

检查设备令牌、`server.config_poll_sec`、服务端配置版本与采集端日志；同时确认是否有定时策略覆盖了当前设置。

**运行文件放在哪里？**

模块安装后的常用目录如下，均属于运行态数据，不应提交到 Git：

```text
/data/adb/modules/remote-monitor/config/config.json
/data/adb/modules/remote-monitor/logs/agent.log
/data/adb/modules/remote-monitor/queue/
```

## 开发与构建

完整步骤见 **[构建与部署说明](BUILD.md)**，实现结构见 **[架构设计](docs/design.md)**。

```bash
git clone https://github.com/Jia060117/Remote-Phone-Monitor.git
cd Remote-Phone-Monitor
```

<details>
<summary><strong>展开：服务端开发、采集端测试与 Android 构建入口</strong></summary>

### 服务端开发与测试

```bash
cd server
python -m venv .venv

# Linux/macOS
.venv/bin/pip install -r requirements.txt -r requirements-dev.txt
.venv/bin/python test_config_sync.py

# Windows PowerShell
# .venv\Scripts\pip install -r requirements.txt -r requirements-dev.txt
# .venv\Scripts\python test_config_sync.py
```

手动启动服务前，复制 `tokens.json.example` 与 `server.json.example`，分别设置独立设备令牌、强管理员密码与随机会话密钥，并保持采集端配置一致。公网部署与启动命令见 [服务端部署说明](BUILD.md#1-fastapi-服务端)。

### 电脑端采集程序测试

在仓库根目录运行：

```bash
cd remote-agent
cp config.example.json config.json
cmake -S . -B build
cmake --build build -j
./build/remote-agent --config config.json
```

默认示例使用**合成测试画面**，不读取电脑真实屏幕。CMake 会下载并校验固定版本的 curl 与 Mbed TLS；离线依赖、JPEG 编解码和目标平台支持见 [BUILD.md](BUILD.md)。

### Android 交叉编译与打包

按 [Android 交叉编译说明](BUILD.md#22-android-交叉编译)构建后，将产物复制为 `remote-monitor/bin/remote-agent`，设置模块配置中的 HTTPS 地址、匿名设备 ID 与独立令牌，再按 [模块打包说明](BUILD.md#3-kernelsumagisk-模块)打包。

</details>

### 项目结构

```text
remote-agent/           C++17 采集端、CMake 构建与示例配置
remote-monitor/         KernelSU/Magisk 模块模板
server/                 FastAPI 服务、网页界面与回归测试
release/                发行包构建、初始化与启动脚本
docs/design.md          架构设计
BUILD.md                完整构建与部署说明
SECURITY.md             安全与隐私部署要求
THIRD_PARTY_NOTICES.md  第三方依赖许可证说明
```

构建产物、依赖缓存、虚拟环境、数据库、截图、日志、令牌与部署配置均由 `.gitignore` 排除，不属于源码发布内容。

## 开源许可与致谢

本项目由 **情绪零碎の佳（[Jia060117](https://github.com/Jia060117)）** 发布与维护，采用 **[MIT 许可证](LICENSE)**。版权归 Remote Phone Monitor contributors 所有。

感谢项目贡献者，以及 FastAPI、SQLite、curl、Mbed TLS 等开源项目。预编译二进制所含依赖的版本与许可证见 **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**。

欢迎通过 [Issue](https://github.com/Jia060117/Remote-Phone-Monitor/issues)反馈复现条件，或通过 Pull Request 改善功能与文档。参与前请阅读 [贡献指南](CONTRIBUTING.md)，提交材料前请移除真实截图、设备标识、IP、域名、凭据与个人信息。
