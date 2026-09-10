"""远程配置下发链路的回归测试。

覆盖 2026-08-31 那个"改了 30 秒间隔，设备却一直按旧间隔跑"的故障：

  根因  设备端 agent 只认响应里的 "version" 字段，而上传响应里的字段叫
        "config_version"（"version" 是后来才加的兼容字段）。主通道（上传捎带）
        因此每次都静默失败，又因为上传一直成功、心跳时间戳一直被刷新，
        兜底的心跳轮询永远不会触发，设备就永远停在启动时那份配置上。

  诱因  服务端 GET /api/config 把「服务端版本」当成「设备已生效版本」写进
        device_state，管理后台于是显示"已同步"，把故障掩盖了。

运行：python test_config_sync.py
"""
import os
import re
import sys
import json
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
tmp = tempfile.mkdtemp(prefix="rpm-cfg-test-")
os.environ["DB_PATH"] = os.path.join(tmp, "t.db")
os.environ["STORE_DIR"] = os.path.join(tmp, "store")
token_file = os.path.join(tmp, "tokens.json")
with open(token_file, "w", encoding="utf-8") as f:
    json.dump({"test-device-token": "DEVICE_A"}, f)
os.environ["TOKENS_FILE"] = token_file

import importlib.util
spec = importlib.util.spec_from_file_location("srv", os.path.join(HERE, "main.py"))
srv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(srv)

from fastapi.testclient import TestClient
client = TestClient(srv.app)
DEVICE_ID = "DEVICE_A"
HDR = {"X-Device-ID": DEVICE_ID, "X-Device-Token": "test-device-token"}

PNG = bytes([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) + b"\x00" * 64

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}  {detail}")


# ---------------------------------------------------------------------------
# 设备端手写 JSON 解析器的忠实复刻（对应 remote-agent/src/main.cpp）
# ---------------------------------------------------------------------------

def json_skip_ws(s, i):
    while i < len(s) and s[i] in " \n\r\t":
        i += 1
    return i


def json_find_value(s, key):
    needle = '"' + key + '"'
    depth = 0
    in_str = False
    i = 0
    while i < len(s):
        c = s[i]
        if in_str:
            if c == "\\" and i + 1 < len(s):
                i += 1
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            if depth == 1 and s[i:i + len(needle)] == needle:
                j = json_skip_ws(s, i + len(needle))
                if j < len(s) and s[j] == ":":
                    j = json_skip_ws(s, j + 1)
                    return j
            in_str = True
            i += 1
            continue
        if c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
        i += 1
    return None


def json_get_int(s, key):
    i = json_find_value(s, key)
    if i is None:
        return None
    m = re.match(r"-?\d+", s[i:])
    return int(m.group(0)) if m else None


def json_get_object(s, key):
    i = json_find_value(s, key)
    if i is None or i >= len(s) or s[i] != "{":
        return None
    depth = 0
    in_str = False
    esc = False
    start = i
    while i < len(s):
        c = s[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return s[start:i + 1]
        i += 1
    return None


def device_parse(payload, accept_config_version=True):
    """复刻 apply_remote_config 的版本判定。

    accept_config_version=False 时模拟故障版本的固件（只认 "version"）。
    返回 (version, config_json)，拿不到版本返回 (None, None)。
    """
    ver = None
    if accept_config_version:
        ver = json_get_int(payload, "config_version")
    if ver is None:
        ver = json_get_int(payload, "version")
    if ver is None:
        return None, None
    return ver, json_get_object(payload, "config")


def device_state():
    conn = srv.get_db()
    row = conn.execute(
        "SELECT applied_version FROM device_state WHERE device='DEVICE_A'"
    ).fetchone()
    conn.close()
    return row["applied_version"] if row else None


# ---------------------------------------------------------------------------

def main():
    srv.init_db()
    srv.save_remote_config(srv.DEFAULT_REMOTE_CONFIG, bump=False)
    _, base_ver = srv.load_remote_config()
    base_eff_ver, _, _ = srv.effective_config()

    print("\n[0] 设备鉴权")
    r = client.get("/api/config", headers={"X-Device-ID": DEVICE_ID})
    check("缺少设备令牌返回 401", r.status_code == 401, str(r.status_code))
    r = client.get("/api/config", headers={**HDR, "X-Device-ID": "OTHER_DEVICE"})
    check("设备 ID 与令牌映射不符返回 403", r.status_code == 403, str(r.status_code))

    print("\n[1] 设备启动：心跳拉取配置")
    r = client.get("/api/config", headers=HDR)
    check("心跳返回 200", r.status_code == 200, str(r.status_code))
    ver, cfg = device_parse(r.text)
    check("设备解析出内容版本号", ver == base_eff_ver, f"ver={ver}")
    check("设备解析出 config", cfg is not None)
    check("配置里带 interval_sec", cfg and '"interval_sec"' in cfg)
    # 设备此时还没应用任何版本，上报 0 -> 服务端的 COALESCE 不应写入伪版本
    check("device_state 不把服务端版本当成设备版本",
          device_state() in (None, 0), f"applied_version={device_state()}")

    print("\n[2] 管理端把采集间隔改成 30 秒")
    stored, _ = srv.load_remote_config()
    stored["capture"]["interval_sec"] = 30
    srv.clamp_config(stored)
    new_ver = srv.save_remote_config(stored, bump=True)
    eff_ver, eff_cfg, _ = srv.effective_config()
    check("版本号自增", new_ver == base_ver + 1, f"{base_ver} -> {new_ver}")
    check("配置变化后内容版本号变化", eff_ver != base_eff_ver, str(eff_ver))

    print("\n[3] 主通道：上传响应捎带新配置")
    r = client.post("/api/upload", files={"image": ("shot.png", PNG, "image/png")},
                    headers={**HDR, "X-Config-Version": str(base_eff_ver)})
    check("上传返回 200", r.status_code == 200, str(r.status_code))
    body = r.json()
    check("响应带 config_version（新固件依赖它）", "config_version" in body)
    check("响应带兼容字段 version（老固件依赖它）", "version" in body)
    check("版本不一致时附带 config", "config" in body)

    ver, cfg = device_parse(r.text, accept_config_version=True)
    check("新固件能解析上传响应", ver == eff_ver and cfg is not None, f"ver={ver}")
    check("新配置里 interval_sec=30",
          cfg and json.loads(cfg)["capture"]["interval_sec"] == 30)

    print("\n[4] 回归：模拟故障固件（只认 version）")
    stripped = json.dumps({k: v for k, v in body.items() if k != "version"})
    old_ver, old_cfg = device_parse(stripped, accept_config_version=False)
    check("故障固件在缺少兼容字段时拿不到版本（这正是当时的症状）",
          old_ver is None, f"old_ver={old_ver}")
    ok_ver, ok_cfg = device_parse(stripped, accept_config_version=True)
    check("新固件在同样响应下正常", ok_ver == eff_ver and ok_cfg is not None)

    print("\n[5] 心跳通道：版本号上报与 304")
    r = client.get("/api/config", headers={**HDR, "If-None-Match": str(eff_ver)})
    check("版本一致返回 304", r.status_code == 304, str(r.status_code))
    check("device_state 记录设备上报的版本", device_state() == eff_ver,
          f"applied_version={device_state()}")

    print("\n[6] 版本落后时心跳应返回完整配置")
    r = client.get("/api/config", headers={**HDR, "If-None-Match": "1000"})
    check("版本不一致返回 200", r.status_code == 200, str(r.status_code))
    ver, cfg = device_parse(r.text)
    check("心跳响应可解析", ver == eff_ver and cfg is not None, f"ver={ver}")

    print("\n[7] 主循环行为模拟：故障固件 vs 修复固件")
    # 忠实复刻 src/main.cpp 主循环里跟配置同步有关的部分，用真实服务端驱动。
    #   mode='old'：last_sync 由「上传成功」刷新（8-31 故障版的行为）
    #   mode='new'：last_config_check 仅在「服务端确认过版本号」时刷新（修复版）
    # strip 用来从响应里抹掉字段，模拟服务端还没加兼容字段 / 主通道坏掉。

    def simulate(mode, strip=(), max_t=700, poll_sec=300, local_interval=9):
        t, interval, active_version, last_check = 0, local_interval, 0, 0
        events = []
        while t < max_t:
            t += interval
            # --- 上传（主通道，配置捎带在响应里）---
            resp = client.post(
                "/api/upload", files={"image": ("shot.png", PNG, "image/png")},
                headers={**HDR, **({"X-Config-Version": str(active_version)}
                                   if active_version else {})}).json()
            for k in strip:
                resp.pop(k, None)
            ver, cfg = device_parse(json.dumps(resp),
                                    accept_config_version=(mode == "new"))
            if ver is not None and cfg is not None:
                ni = json.loads(cfg)["capture"]["interval_sec"]
                if ni != interval:
                    events.append(f"t={t}s 上传捎带: 间隔 {interval}->{ni}")
                    interval = ni
                active_version = ver
                last_check = t
            elif mode == "old":
                last_check = t          # 故障版：只要上传成功就刷新
            # 修复版：响应读不出版本就不刷新，让心跳兜底顶上来

            # --- 心跳（兜底通道）---
            if t - last_check >= poll_sec:
                h = {**HDR, **({"If-None-Match": str(active_version)}
                               if active_version else {})}
                r = client.get("/api/config", headers=h)
                if r.status_code == 200:
                    ver, cfg = device_parse(r.text,
                                            accept_config_version=(mode == "new"))
                    if ver and cfg:
                        ni = json.loads(cfg)["capture"]["interval_sec"]
                        if ni != interval:
                            events.append(f"t={t}s 心跳兜底: 间隔 {interval}->{ni}")
                            interval = ni
                        active_version = ver
                last_check = t
        return interval, events

    # 场景 A：复现 8-31 的现场 —— 服务端还没加 "version" 兼容字段
    old_iv, old_ev = simulate("old", strip=("version",))
    new_iv, new_ev = simulate("new", strip=("version",))
    check("场景A 故障固件：间隔永远停在 9 秒（卡死）",
          old_iv == 9 and not old_ev, f"interval={old_iv} events={old_ev}")
    check("场景A 修复固件：第一时间就生效",
          new_iv == 30 and new_ev and "上传捎带" in new_ev[0],
          f"interval={new_iv} events={new_ev}")

    # 场景 B：主通道彻底坏掉（两个版本字段都读不到），验证自愈
    old_iv2, old_ev2 = simulate("old", strip=("config_version", "version"))
    new_iv2, new_ev2 = simulate("new", strip=("config_version", "version"))
    check("场景B 故障固件：主通道坏了就再也不会恢复",
          old_iv2 == 9 and not old_ev2, f"interval={old_iv2} events={old_ev2}")
    check("场景B 修复固件：心跳兜底在 poll_sec 内自愈",
          new_iv2 == 30 and new_ev2 and "心跳兜底" in new_ev2[0],
          f"interval={new_iv2} events={new_ev2}")

    print("\n    故障固件（8-31 现场）:")
    print(f"      场景A 主通道字段不匹配 -> 间隔停在 {old_iv}s，事件 {old_ev or '无（永久卡死）'}")
    print(f"      场景B 主通道彻底坏掉     -> 间隔停在 {old_iv2}s，事件 {old_ev2 or '无（永久卡死）'}")
    print("    修复固件（本次改动）:")
    for e in new_ev:
        print(f"      场景A {e}")
    for e in new_ev2:
        print(f"      场景B {e}")

    print(f"\n{'=' * 60}\n通过 {passed} 项，失败 {failed} 项\n{'=' * 60}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
