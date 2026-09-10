# Remote Phone Monitor v1.0.0 Release Bundle

[简体中文](#中文快速开始) | [English](#english-quick-start)

> [!WARNING]
> Use this software only on devices you own or are explicitly authorized to
> administer, with the informed consent of every affected user. Screen captures
> may contain highly sensitive information. Never use it for covert monitoring.

## 中文快速开始

要求：服务端安装 Python 3.9+；Android 设备使用 arm64-v8a，并已安装 Magisk 或
KernelSU。

1. 解压统一包，在解压目录中运行：

   ```bash
   python initialize.py --server-url https://你的服务地址
   ```

   Windows 也可使用 `python`；Linux/macOS 通常使用 `python3`。手机必须能访问该
   地址。局域网测试允许 HTTP，但截图和 token 将不会被加密传输。

2. 初始化程序会在本机生成：

   - `server/tokens.json`：唯一设备 token 映射；
   - `server/server.json`：随机管理密码和会话密钥；
   - `generated/credentials.txt`：私密凭据备份，请勿分享；
   - `generated/Remote-Phone-Monitor-module-v1.0.0.zip`：已写入相同 token 与服务地址
     的最终可安装模块。

3. 启动服务端：

   ```powershell
   .\start-server.ps1
   ```

   或：

   ```bash
   ./start-server.sh
   ```

4. 通过 Magisk/KernelSU 管理器安装 `generated/` 下的模块 ZIP，并重启设备。

服务端默认监听 `0.0.0.0:8000`。公网部署必须使用 HTTPS 反向代理和防火墙。不要
直接安装名为 `module-template` 的模板包；其中的占位配置会拒绝启动。

## English quick start

Requirements: Python 3.9+ on the server, and an arm64-v8a Android device with
Magisk or KernelSU.

1. Extract the all-in-one bundle and run:

   ```bash
   python3 initialize.py --server-url https://your-server.example
   ```

   Use `python` on Windows when appropriate. The Android device must be able to
   reach this URL. HTTP is accepted for isolated LAN testing, but it does not
   encrypt captures or tokens in transit.

2. The initializer creates unique local credentials, matching server and agent
   authentication configuration, and the final installable module under
   `generated/`.
3. Start the server with `start-server.ps1` or `./start-server.sh`.
4. Install `generated/Remote-Phone-Monitor-module-v1.0.0.zip` through Magisk or
   KernelSU Manager and reboot the device.

Keep `generated/credentials.txt` private. For an Internet-facing deployment,
use an HTTPS reverse proxy and a firewall.
