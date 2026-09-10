# Remote Phone Monitor — 构建与部署

本文档补充 README 中的快速开始。请先阅读 `SECURITY.md`，并仅在自有或明确授权
的设备上部署。

## 1. FastAPI 服务端

### 1.1 安装

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

生成三个互不相同的随机值：

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

- 将第一个值作为 `tokens.json` 的 key，值使用不含个人信息的设备 ID。
- 将第二、第三个值写入 `server.json` 的 `password` 与 `secret`。
- 将第一个值同时写入 Agent 配置的 `server.token`。

`tokens.json` 示例：

```json
{
  "随机设备令牌": "DEVICE_A"
}
```

启动：

```bash
# Linux/macOS
.venv/bin/uvicorn main:app --host 127.0.0.1 --port 8000 --workers 1

# Windows PowerShell
# .venv\Scripts\uvicorn main:app --host 127.0.0.1 --port 8000 --workers 1
```

服务端使用 SQLite，因此建议保持单 worker。公网部署必须置于 HTTPS 反向代理后，
并通过防火墙限制访问。

### 1.2 环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `DB_PATH` | `shots.db` | SQLite 文件路径 |
| `STORE_DIR` | `store` | 截图目录 |
| `TOKENS_FILE` | `tokens.json` | 设备令牌映射 |
| `SERVER_CONFIG` | `server.json` | 管理员与保留策略配置 |
| `ADMIN_PASSWORD` | 空 | 覆盖 `server.json` 管理密码 |
| `SESSION_SECRET` | 进程内随机值 | 覆盖会话签名密钥 |
| `ADMIN_TOKEN` | 空 | 管理 API 自动化令牌（可选） |

### 1.3 systemd

仓库提供 `server/remote-monitor.service`。复制前修改路径，并通过环境文件或受限配置
文件提供凭据：

```bash
sudo cp server/remote-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now remote-monitor
sudo journalctl -u remote-monitor -f
```

## 2. Native Agent

### 2.1 PC 验证

```bash
cd remote-agent
cp config.example.json config.json
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/remote-agent --config config.json
```

`config.example.json` 使用 `testpattern` 合成画面，不读取真实屏幕。CMake 会下载并
校验固定版本的 curl 与 Mbed TLS；系统有 libjpeg 时会启用 JPEG 编解码，否则相关
功能使用桩实现。

离线构建可把官方发布包解压为：

```text
deps/
├── curl-8.22.0/
└── mbedtls-3.6.4/
```

也可传入 `-DRPM_DEPS_DIR=/path/to/deps`。

### 2.2 Android 交叉编译

要求 Android NDK r25+：

```bash
cmake -S remote-agent -B remote-agent/build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DRPM_WITH_JPEG=OFF
cmake --build remote-agent/build-android -j
```

Windows Git Bash 也可使用 `remote-agent/build-win.sh`，但必须先设置 `NDK`、
`ANDROID_NDK_HOME` 或 `ANDROID_NDK_ROOT`。脚本不包含任何用户专属路径。

发布构建会自动剥离符号。需要调试符号时使用
`-DCMAKE_BUILD_TYPE=Debug`。

### 2.3 部署配置

复制 `remote-agent/config.example.json` 后至少修改：

```json
{
  "device_id": "DEVICE_A",
  "server": {
    "url": "",
    "urls": "https://monitor.example.com",
    "token": "随机设备令牌"
  },
  "capture": {
    "backend": "screencap"
  }
}
```

设备 ID 必须与 `tokens.json` 中的映射一致，且不得包含姓名、手机号、设备型号、序列
号等信息。

## 3. KernelSU/Magisk 模块

```bash
New-Item -ItemType Directory -Force remote-monitor/bin  # PowerShell 示例
Copy-Item remote-agent/build-android/remote-agent remote-monitor/bin/remote-agent
```

在 `remote-monitor/config/config.json` 中设置 HTTPS 地址、匿名设备 ID 和设备令牌，
然后在 Git Bash、Linux 或 macOS 下打包：

```bash
cd remote-monitor
./pack.sh
```

安装后常用路径：

```text
/data/adb/modules/remote-monitor/config/config.json
/data/adb/modules/remote-monitor/logs/agent.log
/data/adb/modules/remote-monitor/queue/
```

这些运行态文件不得提交到 Git。

## 4. 测试

```bash
cd server
python -m pip install -r requirements.txt -r requirements-dev.txt
python test_config_sync.py
```

回归测试使用临时目录、合成图片和测试令牌，不包含真实设备或服务信息。

## 5. 常见问题

| 问题 | 检查项 |
|---|---|
| 上传返回 401 | Agent 的 `server.token` 是否存在于 `tokens.json` |
| 上传返回 403 | `device_id` 是否与令牌映射的设备 ID 一致 |
| 管理页面返回 503 | 是否设置了 `ADMIN_PASSWORD` 或 `server.json.password` |
| HTTPS 失败 | 域名证书链、设备系统 CA 与时间是否正确；客户端不会关闭证书校验 |
| 编码为桩 | 安装 libjpeg，或为目标平台提供 libjpeg-turbo |
| SQLite 锁冲突 | 保持 uvicorn `--workers 1` |
| 配置未生效 | 检查 token、`config_poll_sec`、Agent 日志与服务端配置版本 |
