# Remote Phone Monitor v1.0.0

首个公开发行版，适用于自有或已获明确授权的 Android 设备。

## 下载选择

- `Remote-Phone-Monitor-v1.0.0-bundle.zip`：推荐。包含服务端、初始化程序、模块模板
  和已编译的 arm64-v8a Agent。
- `Remote-Phone-Monitor-v1.0.0-server.zip`：仅服务端。
- `Remote-Phone-Monitor-v1.0.0-module-template.zip`：包含已编译 Agent 的模块模板；
  请勿直接安装，需通过统一包中的初始化程序写入私有服务地址和 token。
- `SHA256SUMS.txt`：下载文件完整性校验值。

## 快速开始

1. 解压 `Remote-Phone-Monitor-v1.0.0-bundle.zip`。
2. 运行 `python initialize.py --server-url https://你的服务地址`。
3. 使用 `start-server.ps1`（Windows）或 `./start-server.sh`（Linux/macOS）启动服务端。
4. 通过 Magisk/KernelSU 安装 `generated/Remote-Phone-Monitor-module-v1.0.0.zip`，
   然后重启 Android 设备。

初始化过程会在下载者本机生成唯一设备 token、随机管理密码和会话密钥，并将相同
token 写入服务端与最终模块。公开资产不包含可被所有人复用的固定密钥。

## Build and security notes

- Android Agent: AArch64 ELF, Android API 29+, C++17.
- HTTPS: statically linked curl 8.22.0 with Mbed TLS 3.6.4.
- Device uploads and configuration polling require a matching per-install token.
- Dashboard and administration APIs require the generated administrator password.
- TLS certificate verification fails closed.

> Screen captures can contain highly sensitive information. Use this software
> only with the informed, explicit consent of the device owner and every
> affected user. Never use it for covert monitoring.
