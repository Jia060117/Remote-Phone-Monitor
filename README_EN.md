# Remote Phone Monitor

**English** | [简体中文](README.md)

A self-hosted screen sampling system for Android devices that you own or are
explicitly authorized to administer. A native agent captures the screen at
configured intervals and uploads images over HTTPS to a FastAPI server. The web
interface provides recent captures, a timeline, storage statistics, and remote
configuration.

> [!WARNING]
> Screen captures may contain passwords, messages, financial information, and
> other highly sensitive data. Use this software only with the informed,
> explicit consent of the device owner and every affected user. Follow all
> applicable laws. This project must not be used for covert surveillance.

## Features

- Android native agent written in C++17, plus a KernelSU/Magisk module
- HTTPS upload, change detection, offline queueing, and retry after reconnection
- Screen-off pause, reduced cellular quality, and Wi-Fi-only upload policies
- FastAPI and SQLite server
- Login-protected dashboard, administration interface, and live preview
- Per-device remote configuration and scheduling policies
- Automatic cleanup by retention period or storage limit

## Repository layout

```text
remote-agent/        C++ agent source, CMake build, and example configuration
remote-monitor/      KernelSU/Magisk module template
server/              FastAPI server, web UI, and regression tests
docs/design.md       Architecture and design notes
BUILD.md             Complete build and deployment guide
SECURITY.md          Security and privacy requirements
THIRD_PARTY_NOTICES.md  Third-party dependency licenses
```

Build artifacts, dependency caches, virtual environments, databases, captures,
logs, tokens, and deployment configuration are excluded through `.gitignore`
and are not part of the source release.

## Quick start

For a prebuilt release, download `Remote-Phone-Monitor-v1.0.0-bundle.zip` from
GitHub Releases, extract it, and run:

```bash
python3 initialize.py --server-url https://your-server.example
```

The initializer generates a unique device token, a random administrator
password, and a session secret locally. It then creates the final installable
module with matching authentication settings. The public release does not ship
one fixed secret shared by every user.

### 1. Start the server

Python 3.9 or later is required:

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

Edit the two files that are intentionally excluded from Git:

- In `tokens.json`, replace `CHANGE_ME_DEVICE_TOKEN` with a strong random token.
  Its value is an anonymous device ID.
- In `server.json`, configure a strong administrator password and a random
  session secret.

Generate a suitable random value with:

```bash
python -c "import secrets; print(secrets.token_urlsafe(32))"
```

Start the service:

```bash
# Linux/macOS
.venv/bin/uvicorn main:app --host 127.0.0.1 --port 8000

# Windows PowerShell
# .venv\Scripts\uvicorn main:app --host 127.0.0.1 --port 8000
```

Open `http://127.0.0.1:8000`. For an Internet-facing deployment, place the
service behind an HTTPS reverse proxy. Do not expose the development server
directly.

### 2. Configure and build the agent

Copy and edit the example configuration:

```bash
cd remote-agent
cp config.example.json config.json
```

At minimum, set:

- `device_id`: the value mapped to this token in `tokens.json`; do not use a
  real name or device model.
- `server.urls`: the HTTPS base URL of your server.
- `server.token`: the corresponding device token.

The desktop test build uses a synthetic test pattern and does not capture the
real screen:

```bash
cmake -S . -B build
cmake --build build -j
./build/remote-agent --config config.json
```

CMake downloads pinned versions of curl and Mbed TLS from their official
release sources and verifies their checksums. For offline builds, place the
extracted `curl-8.22.0` and `mbedtls-3.6.4` directories under
`remote-agent/deps/`, or provide `-DRPM_DEPS_DIR=/path/to/deps`.

### 3. Cross-compile for Android

Android NDK r25 or later is required:

```bash
cmake -S remote-agent -B remote-agent/build-android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
cmake --build remote-agent/build-android -j
```

Copy the result to `remote-monitor/bin/remote-agent`, edit the anonymized
`remote-monitor/config/config.json`, and package the module:

```bash
cd remote-monitor
./pack.sh
```

Install the generated ZIP through KernelSU or Magisk Manager. See [BUILD.md](BUILD.md)
for the complete procedure.

## Key configuration

| Field | Purpose | Recommendation |
|---|---|---|
| `device_id` | Anonymous device identifier | Do not include names, models, or serial numbers |
| `server.urls` | Server base URL | Use HTTPS in production |
| `server.token` | Device authentication token | Use a unique random value per device |
| `capture.interval_sec` | Capture interval | Increase it when frequent captures are unnecessary |
| `capture.pause_on_screen_off` | Pause while the screen is off | Keep enabled |
| `upload.wifi_only` | Upload only over Wi-Fi | Enable when cellular data is limited |
| `upload.max_queue_mb` | Offline queue size limit | Size according to available device storage |
| `server.config_poll_sec` | Configuration polling interval | Defaults to 300 seconds |

Runtime configuration precedence is: built-in defaults < local configuration <
server configuration < matching scheduled policy.

## Security and privacy

- The server rejects upload and configuration requests without a valid device
  token by default.
- The dashboard, captures, device information, and administration APIs require
  administrator authentication by default.
- The client fails closed when it cannot validate the TLS certificate; it does
  not silently disable verification.
- Real configuration, captures, databases, logs, queues, and binaries are not
  tracked by Git.
- Use a firewall, an HTTPS reverse proxy, a least-privilege service account, and
  a short data-retention policy.

Read [SECURITY.md](SECURITY.md) completely before publishing or deploying an
instance.

## Tests

```bash
cd server
python -m pip install -r requirements.txt -r requirements-dev.txt
python test_config_sync.py
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Never include real captures, device
identifiers, IP addresses, domains, credentials, or personal information in an
issue or log.

## License

[MIT](LICENSE)

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for licenses covering
dependencies included in prebuilt binaries.
