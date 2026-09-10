# Remote Phone Monitor

一个面向自有或已获明确授权 Android 设备的自托管屏幕采样系统。Native Agent
定时采集画面，经 HTTPS 上传到 FastAPI 服务；浏览器端可查看最新截图、时间轴、
存储统计和远程配置。

> [!WARNING]
> 屏幕截图可能包含密码、消息、金融信息等高度敏感内容。仅可在设备所有者和所有
> 受影响用户明确知情并同意的前提下使用。请遵守当地法律；本项目不得用于隐蔽监控。

## 功能

- Android Native Agent（C++17）与 KernelSU/Magisk 模块
- HTTPS 上传、变化检测、离线队列和网络恢复补传
- 息屏暂停、蜂窝低画质、仅 Wi-Fi 上传等节能/流量策略
- FastAPI + SQLite 服务端
- 登录保护的 Dashboard、管理后台和实时预览
- 按设备下发配置与定时策略
- 自动清理超期或超出容量限制的截图

## 项目结构

```text
remote-agent/        C++ Agent 源码、CMake 构建与示例配置
remote-monitor/      KernelSU/Magisk 模块模板
server/              FastAPI 服务、Web UI 与回归测试
docs/design.md       架构设计
BUILD.md             完整构建与部署说明
SECURITY.md          安全与隐私部署要求
```

构建产物、依赖缓存、虚拟环境、数据库、截图、日志、令牌和部署配置均被
`.gitignore` 排除，不属于源码发布内容。

## 快速开始

### 1. 启动服务端

要求 Python 3.9+：

```bash
cd server
python -m venv .venv

# Linux/macOS
.venv/bin/pip install -r requirements.txt
cp tokens.json.example tokens.json
cp server.json.example server.json

# Windows PowerShell
# .venv\Scripts\pip install -r requirements.txt
# Copy-Item tokens.json.example tokens.json
# Copy-Item server.json.example server.json
```

编辑两个不会被 Git 跟踪的文件：

- `tokens.json`：把 `CHANGE_ME_DEVICE_TOKEN` 换成高强度随机令牌；值是匿名设备 ID。
- `server.json`：设置强管理员密码和随机会话密钥。

生成随机值示例：

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

启动服务：

```bash
# Linux/macOS
.venv/bin/uvicorn main:app --host 127.0.0.1 --port 8000

# Windows PowerShell
# .venv\Scripts\uvicorn main:app --host 127.0.0.1 --port 8000
```

访问 `http://127.0.0.1:8000`。面向公网部署时，请使用 HTTPS 反向代理，不要直接
暴露开发服务器。

### 2. 配置并构建 Agent

复制并编辑示例配置：

```bash
cd remote-agent
cp config.example.json config.json
```

至少设置：

- `device_id`：与 `tokens.json` 中该令牌映射的值一致；不要使用真实姓名或设备型号。
- `server.urls`：你的 HTTPS 服务地址。
- `server.token`：对应的设备令牌。

PC 测试构建默认使用合成画面，不读取真实屏幕：

```bash
cmake -S . -B build
cmake --build build -j
./build/remote-agent --config config.json
```

CMake 会从官方发布源下载并校验固定版本的 curl 与 Mbed TLS。离线构建时，可将
解压后的 `curl-8.22.0` 和 `mbedtls-3.6.4` 放到 `remote-agent/deps/`，或通过
`-DRPM_DEPS_DIR=/path/to/deps` 指定位置。

### 3. Android 交叉编译

要求 Android NDK r25+：

```bash
cmake -S remote-agent -B remote-agent/build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build remote-agent/build-android -j
```

将产物复制为 `remote-monitor/bin/remote-agent`，编辑已匿名化的
`remote-monitor/config/config.json`，再执行：

```bash
cd remote-monitor
./pack.sh
```

通过 KernelSU 或 Magisk 管理器安装生成的 zip。完整说明见 [BUILD.md](BUILD.md)。

## 关键配置

| 字段 | 含义 | 建议 |
|---|---|---|
| `device_id` | 匿名设备标识 | 不包含姓名、型号、序列号等信息 |
| `server.urls` | 服务端基址 | 生产环境仅使用 HTTPS |
| `server.token` | 设备令牌 | 每台设备使用独立随机值 |
| `capture.interval_sec` | 采样间隔 | 按需求适当调大 |
| `capture.pause_on_screen_off` | 息屏暂停 | 建议开启 |
| `upload.wifi_only` | 仅 Wi-Fi 上传 | 移动流量敏感时开启 |
| `upload.max_queue_mb` | 离线队列上限 | 根据设备存储空间设置 |
| `server.config_poll_sec` | 配置轮询间隔 | 默认 300 秒 |

运行态配置优先级为：硬编码默认值 < 本地配置 < 服务端配置 < 命中的定时策略。

## 安全与隐私

- 服务端默认拒绝未携带有效设备令牌的上传和配置请求。
- Dashboard、截图、设备信息和管理 API 默认要求管理员登录。
- TLS 证书不可验证时客户端会终止 HTTPS 请求，不会静默降级。
- 真实配置、截图、数据库、日志、队列和二进制不会被 Git 跟踪。
- 建议使用防火墙、HTTPS 反向代理、最小权限账户与短期数据保留策略。

发布或部署前请完整阅读 [SECURITY.md](SECURITY.md)。

## 测试

```bash
cd server
python -m pip install -r requirements.txt -r requirements-dev.txt
python test_config_sync.py
```

## 贡献

请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。提交 issue 或日志时不得包含真实截图、
设备标识、IP、域名、凭据或个人信息。

## 许可证

[MIT](LICENSE)
