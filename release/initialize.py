#!/usr/bin/env python3
"""Create private runtime credentials and a configured installable module."""

import argparse
import getpass
import json
import os
import re
import secrets
import stat
import sys
import zipfile
from pathlib import Path
from urllib.parse import urlparse


VERSION = "1.0.0"
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Initialize Remote Phone Monitor with unique local credentials."
    )
    parser.add_argument("--server-url", help="URL reachable by the Android device")
    parser.add_argument("--device-id", default="DEVICE_A", help="anonymous device ID")
    parser.add_argument("--token", help="device token; generated when omitted")
    parser.add_argument("--admin-password", help="administrator password; generated when omitted")
    parser.add_argument("--server-only", action="store_true", help="configure only the server")
    parser.add_argument("--force", action="store_true", help="replace generated configuration")
    return parser.parse_args()


def prompt_server_url(value):
    if value:
        return value.strip()
    if not sys.stdin.isatty():
        raise SystemExit("--server-url is required in non-interactive mode")
    return input("Server URL reachable by the Android device (https://...): ").strip()


def validate_server_url(value):
    value = value.rstrip("/")
    parsed = urlparse(value)
    if parsed.scheme not in ("http", "https") or not parsed.netloc:
        raise SystemExit("server URL must be an absolute http:// or https:// URL")
    if parsed.scheme != "https":
        print("WARNING: HTTP does not protect screenshots or tokens in transit.", file=sys.stderr)
        print("Use HTTP only for an isolated local test network.", file=sys.stderr)
    return value


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def private_write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    try:
        path.chmod(stat.S_IRUSR | stat.S_IWUSR)
    except OSError:
        pass


def add_module_file(archive, source, relative):
    data = source.read_bytes()
    info = zipfile.ZipInfo(relative.as_posix())
    info.create_system = 3
    executable = (
        relative.parts[0] == "bin"
        or relative.suffix == ".sh"
        or relative.name == "update-binary"
    )
    mode = 0o755 if executable else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, data)


def build_module(module_dir, output):
    required = module_dir / "bin" / "remote-agent"
    if not required.is_file() or required.stat().st_size < 1024:
        raise SystemExit("compiled Android agent is missing from the module template")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w") as archive:
        for source in sorted(module_dir.rglob("*")):
            if source.is_file():
                add_module_file(archive, source, source.relative_to(module_dir))


def main():
    args = parse_args()
    root = Path(__file__).resolve().parent
    server_dir = root / "server"
    module_dir = root / "module-template"
    generated_dir = root / "generated"
    credentials_path = generated_dir / "credentials.txt"

    if not DEVICE_ID_RE.fullmatch(args.device_id):
        raise SystemExit("device ID may contain only letters, numbers, dot, underscore, or hyphen")
    if credentials_path.exists() and not args.force:
        raise SystemExit("configuration already exists; use --force to replace it")
    if not server_dir.is_dir():
        raise SystemExit("server directory is missing from this release package")

    server_url = validate_server_url(prompt_server_url(args.server_url))
    token = args.token or secrets.token_urlsafe(32)
    admin_password = args.admin_password or secrets.token_urlsafe(24)
    session_secret = secrets.token_urlsafe(48)

    write_json(server_dir / "tokens.json", {token: args.device_id})
    write_json(
        server_dir / "server.json",
        {
            "password": admin_password,
            "secret": session_secret,
            "max_storage_mb": 10240,
            "max_days": 30,
        },
    )

    module_output = None
    server_only = args.server_only or not module_dir.is_dir()
    if not server_only:
        config_path = module_dir / "config" / "config.json"
        config = json.loads(config_path.read_text(encoding="utf-8"))
        config["device_id"] = args.device_id
        config["server"]["url"] = ""
        config["server"]["urls"] = server_url
        config["server"]["token"] = token
        write_json(config_path, config)
        module_output = generated_dir / ("Remote-Phone-Monitor-module-v%s.zip" % VERSION)
        build_module(module_dir, module_output)

    credential_lines = [
        "Remote Phone Monitor v%s - PRIVATE CREDENTIALS" % VERSION,
        "Do not publish or share this file.",
        "",
        "Server URL: %s" % server_url,
        "Device ID: %s" % args.device_id,
        "Device token: %s" % token,
        "Admin password: %s" % admin_password,
        "Session secret: %s" % session_secret,
        "",
    ]
    private_write(credentials_path, "\n".join(credential_lines))

    print("Initialization complete.")
    print("Private credentials: %s" % credentials_path)
    if module_output:
        print("Installable Android module: %s" % module_output)
    else:
        print("Server-only package initialized; copy the device token into your agent config.")
    print("Start the server with start-server.ps1 or ./start-server.sh")
    print("Keep generated/credentials.txt private.")


if __name__ == "__main__":
    main()
