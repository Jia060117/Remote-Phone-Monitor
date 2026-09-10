#!/usr/bin/env python3
"""Assemble verified v1.0.0 release assets from tracked sources."""

import argparse
import hashlib
import os
import shutil
import stat
import struct
import zipfile
from pathlib import Path


VERSION = "1.0.0"
SERVER_FILES = (
    "main.py",
    "requirements.txt",
    "requirements-dev.txt",
    "remote-monitor.service",
    "server.json.example",
    "tokens.json.example",
    "test_config_sync.py",
)


def parse_args():
    parser = argparse.ArgumentParser(description="Build Remote Phone Monitor release assets")
    parser.add_argument("--agent", required=True, help="compiled Android arm64 agent")
    parser.add_argument("--output", default="dist", help="release output directory")
    return parser.parse_args()


def verify_android_agent(path):
    data = path.read_bytes()
    if len(data) < 1024 or data[:4] != b"\x7fELF":
        raise SystemExit("agent is not a valid ELF binary")
    if data[4] != 2 or data[5] != 1:
        raise SystemExit("agent must be a 64-bit little-endian ELF binary")
    machine = struct.unpack_from("<H", data, 18)[0]
    if machine != 183:
        raise SystemExit("agent is not AArch64 (ELF machine must be 183)")


def copy_server(source, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name in SERVER_FILES:
        shutil.copy2(source / name, destination / name)
    shutil.copytree(source / "static", destination / "static")


def copy_module(source, destination, agent, repo):
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns("pack.sh", "*.zip", "bin"))
    (destination / "bin").mkdir(parents=True, exist_ok=True)
    shutil.copy2(agent, destination / "bin" / "remote-agent")
    shutil.copy2(repo / "LICENSE", destination / "LICENSE")
    shutil.copy2(repo / "THIRD_PARTY_NOTICES.md", destination / "THIRD_PARTY_NOTICES.md")
    licenses = destination / "licenses"
    licenses.mkdir()
    dependency_root = agent.parent / "_deps"
    curl_license = dependency_root / "curl_src-src" / "COPYING"
    mbedtls_license = dependency_root / "mbedtls_src-src" / "LICENSE"
    if not curl_license.is_file() or not mbedtls_license.is_file():
        raise SystemExit("dependency license files are missing beside the agent build")
    shutil.copy2(curl_license, licenses / "curl-COPYING")
    shutil.copy2(mbedtls_license, licenses / "mbedtls-LICENSE")


def archive_tree(source, output, archive_root=None):
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(source)
            target = relative if archive_root is None else Path(archive_root) / relative
            info = zipfile.ZipInfo(target.as_posix())
            info.create_system = 3
            executable = (
                relative.parts[0] == "bin"
                or relative.suffix in (".sh", ".py")
                or relative.name == "update-binary"
            )
            mode = 0o755 if executable else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    args = parse_args()
    repo = Path(__file__).resolve().parent.parent
    agent = Path(args.agent).resolve()
    output = (repo / args.output).resolve()
    verify_android_agent(agent)

    if output == repo or repo not in output.parents:
        raise SystemExit("output directory must be inside the repository")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    bundle_name = "Remote-Phone-Monitor-v%s" % VERSION
    staging = output / "staging" / bundle_name
    staging.mkdir(parents=True)
    for name in ("initialize.py", "start-server.ps1", "start-server.sh", "README.md"):
        shutil.copy2(repo / "release" / name, staging / name)
    for name in ("LICENSE", "SECURITY.md", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(repo / name, staging / name)
    copy_server(repo / "server", staging / "server")
    copy_module(repo / "remote-monitor", staging / "module-template", agent, repo)

    bundle_zip = output / (bundle_name + "-bundle.zip")
    server_zip = output / (bundle_name + "-server.zip")
    module_zip = output / (bundle_name + "-module-template.zip")
    archive_tree(staging, bundle_zip, bundle_name)

    server_stage = output / "staging" / (bundle_name + "-server")
    server_stage.mkdir(parents=True)
    copy_server(repo / "server", server_stage / "server")
    for name in ("initialize.py", "start-server.ps1", "start-server.sh", "README.md"):
        shutil.copy2(repo / "release" / name, server_stage / name)
    for name in ("LICENSE", "SECURITY.md"):
        shutil.copy2(repo / name, server_stage / name)
    archive_tree(server_stage, server_zip, bundle_name + "-server")
    archive_tree(staging / "module-template", module_zip)

    assets = (bundle_zip, server_zip, module_zip)
    checksum_path = output / "SHA256SUMS.txt"
    checksum_path.write_text(
        "".join("%s  %s\n" % (sha256(path), path.name) for path in assets),
        encoding="utf-8",
    )
    shutil.rmtree(output / "staging")
    for path in assets + (checksum_path,):
        print("%s  %d bytes" % (path, path.stat().st_size))


if __name__ == "__main__":
    main()
