import os
import re
import time
import copy
import hmac
import sqlite3
import hashlib
import json
import secrets
from pathlib import Path
from datetime import datetime
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File, Header, HTTPException, Query, Request, Depends, Cookie
from fastapi.responses import FileResponse, HTMLResponse, Response, RedirectResponse
from fastapi.staticfiles import StaticFiles

# ============================================================================
# 配置
# ============================================================================

DB_PATH = os.environ.get("DB_PATH", "shots.db")
STORE_DIR = os.environ.get("STORE_DIR", "store")
SERVER_CONFIG_FILE = os.environ.get("SERVER_CONFIG", "server.json")
TOKENS_FILE = os.environ.get("TOKENS_FILE", "tokens.json")

# 存储限制（可通过 server.json 覆盖）
DEFAULT_MAX_STORAGE_MB = 10240   # 10GB
DEFAULT_MAX_DAYS = 30            # 30天，0=不限制

# 清理节流：最多每 10 分钟跑一次
_last_cleanup_ts = 0

# ============================================================================
# 服务端配置（密码等）
# ============================================================================
# server.json 示例：
#   { "password": "你的密码", "secret": "可选的签名密钥" }
# 密码为空或未配置时，管理页面不需要登录（与旧行为一致）。

_server_config_cache = {}
_server_config_mtime = 0
_ephemeral_session_secret = secrets.token_urlsafe(32)

def load_server_config() -> dict:
    """加载服务端配置（带文件修改时间缓存，改配置不用重启）"""
    global _server_config_cache, _server_config_mtime
    try:
        mtime = os.path.getmtime(SERVER_CONFIG_FILE)
    except OSError:
        return {}
    if mtime == _server_config_mtime and _server_config_cache:
        return _server_config_cache
    try:
        with open(SERVER_CONFIG_FILE, "r", encoding="utf-8") as f:
            _server_config_cache = json.load(f)
    except (json.JSONDecodeError, OSError):
        _server_config_cache = {}
    _server_config_mtime = mtime
    return _server_config_cache

def get_admin_password() -> str:
    """获取管理密码，优先环境变量，其次配置文件"""
    password = os.environ.get("ADMIN_PASSWORD") or load_server_config().get("password", "")
    return "" if str(password).startswith("CHANGE_ME_") else str(password)

def get_session_secret() -> str:
    """获取 HMAC 签名密钥（未配置时用随机密钥，重启后旧 cookie 失效）"""
    env_secret = os.environ.get("SESSION_SECRET")
    if env_secret:
        return env_secret
    cfg_secret = load_server_config().get("secret")
    if cfg_secret and not str(cfg_secret).startswith("CHANGE_ME_"):
        return cfg_secret
    return _ephemeral_session_secret

# ============================================================================
# 管理后台鉴权（可选）
# ============================================================================
# 管理 API 接受已登录的浏览器会话；也可额外设置 ADMIN_TOKEN 供自动化调用。
def require_admin(request: Request, x_admin_token: str = None):
    """管理 API 支持独立令牌或已登录的同源会话，默认拒绝匿名访问。"""
    admin_token = os.environ.get("ADMIN_TOKEN")
    if admin_token and x_admin_token and hmac.compare_digest(x_admin_token, admin_token):
        return
    if get_admin_password() and check_web_auth(request):
        return
    if not admin_token and not get_admin_password():
        raise HTTPException(status_code=503, detail="Admin authentication is not configured")
    raise HTTPException(status_code=403, detail="Admin authentication required")

def require_viewer(request: Request):
    """截图、设备与实时画面 API 只允许已登录的浏览器会话访问。"""
    if not get_admin_password():
        raise HTTPException(status_code=503, detail="ADMIN_PASSWORD is not configured")
    if not check_web_auth(request):
        raise HTTPException(status_code=401, detail="Authentication required")

def authenticate_device(token: str, claimed_id: str = None) -> str:
    """验证设备令牌，并返回服务端登记的设备 ID。"""
    try:
        with open(TOKENS_FILE, "r", encoding="utf-8") as f:
            tokens = json.load(f)
    except (OSError, json.JSONDecodeError):
        raise HTTPException(status_code=503, detail="Device authentication is not configured")
    if not isinstance(tokens, dict) or not token or token.startswith("CHANGE_ME_"):
        raise HTTPException(status_code=401, detail="Valid device token required")
    registered_id = tokens.get(token)
    if not isinstance(registered_id, str) or not registered_id:
        raise HTTPException(status_code=401, detail="Valid device token required")
    if claimed_id and not hmac.compare_digest(claimed_id, registered_id):
        raise HTTPException(status_code=403, detail="Device ID does not match token")
    return registered_id

# ============================================================================
# Web 页面登录鉴权（基于 HMAC 签名 cookie）
# ============================================================================
# 原理：密码正确时，用 HMAC-SHA256(password, secret) 生成签名 cookie，
# 验证时重新计算签名比对，无需服务端 session 存储。
# cookie 有效期 24 小时。

COOKIE_NAME = "rpm_session"

def _sign_session(password: str) -> str:
    """生成不包含明文密码的会话签名。"""
    secret = get_session_secret()
    return hmac.new(secret.encode(), password.encode(), hashlib.sha256).hexdigest()

def _verify_session(token: str) -> bool:
    """验证 session cookie 是否有效"""
    password = get_admin_password()
    if not password or not token:
        return False
    return hmac.compare_digest(token, _sign_session(password))

def check_web_auth(request: Request) -> bool:
    """检查请求是否已登录（用于页面路由）"""
    password = get_admin_password()
    if not password:
        return False
    token = request.cookies.get(COOKIE_NAME)
    return _verify_session(token)

# ============================================================================
# 存储清理
# ============================================================================
# server.json 中可配置：
#   "max_storage_mb": 10240   存储上限（MB），超出时删最旧的
#   "max_days": 30            保留天数，0=不限制

def get_storage_limits():
    cfg = load_server_config()
    return (
        cfg.get("max_storage_mb", DEFAULT_MAX_STORAGE_MB),
        cfg.get("max_days", DEFAULT_MAX_DAYS),
    )

def cleanup_storage():
    """清理超出限制的截图（最旧优先）。返回 (deleted_files, freed_bytes)。"""
    global _last_cleanup_ts
    now = time.time()
    if now - _last_cleanup_ts < 600:  # 10分钟节流
        return 0, 0
    _last_cleanup_ts = now

    max_mb, max_days = get_storage_limits()
    if max_mb <= 0 and max_days <= 0:
        return 0, 0

    max_bytes = max_mb * 1024 * 1024 if max_mb > 0 else 0
    cutoff_ts = int((now - max_days * 86400) * 1000) if max_days > 0 else 0

    conn = None
    deleted = 0
    freed = 0

    try:
        conn = get_db()

        # 1. 按天数清理（分批，避免全表物化）
        if cutoff_ts > 0:
            while True:
                rows = conn.execute(
                    "SELECT id, path, size_kb FROM shots WHERE ts < ? ORDER BY ts ASC LIMIT 500",
                    (cutoff_ts,)).fetchall()
                if not rows:
                    break
                ids_to_delete = []
                for r in rows:
                    full = os.path.join(STORE_DIR, r["path"])
                    if os.path.isfile(full):
                        try:
                            os.remove(full)
                            freed += int((r["size_kb"] or 0) * 1024)
                        except OSError:
                            pass
                    ids_to_delete.append(r["id"])
                    deleted += 1
                if ids_to_delete:
                    placeholders = ",".join("?" * len(ids_to_delete))
                    conn.execute(f"DELETE FROM shots WHERE id IN ({placeholders})", ids_to_delete)

        # 2. 按总大小清理（分批，删最旧的直到低于限额）
        if max_bytes > 0:
            total = conn.execute("SELECT COALESCE(SUM(size_kb), 0) * 1024 FROM shots").fetchone()[0]
            while total > max_bytes:
                rows = conn.execute(
                    "SELECT id, path, size_kb FROM shots ORDER BY ts ASC LIMIT 500"
                ).fetchall()
                if not rows:
                    break
                ids_to_delete = []
                for r in rows:
                    if total <= max_bytes:
                        break
                    fsize = int((r["size_kb"] or 0) * 1024)
                    full = os.path.join(STORE_DIR, r["path"])
                    if os.path.isfile(full):
                        try:
                            os.remove(full)
                        except OSError:
                            pass
                    ids_to_delete.append(r["id"])
                    total -= fsize
                    freed += fsize
                    deleted += 1
                if ids_to_delete:
                    placeholders = ",".join("?" * len(ids_to_delete))
                    conn.execute(f"DELETE FROM shots WHERE id IN ({placeholders})", ids_to_delete)

        conn.commit()
    except Exception as e:
        print(f"[cleanup] error: {e}")
    finally:
        if conn:
            conn.close()

    # 清理空的日期目录
    if deleted > 0:
        for root, dirs, files in os.walk(STORE_DIR, topdown=False):
            if root == STORE_DIR:
                continue
            if not files and not dirs:
                try:
                    os.rmdir(root)
                except OSError:
                    pass

    return deleted, freed

# ============================================================================
# 数据库
# ============================================================================

def init_db():
    """初始化数据库"""
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS shots (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            device    TEXT NOT NULL,
            ts        INTEGER NOT NULL,
            path      TEXT NOT NULL,
            width     INTEGER,
            height    INTEGER,
            size_kb   REAL,
            hash      TEXT,
            changed   INTEGER DEFAULT 1
        )
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS idx_device_ts ON shots(device, ts)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_ts ON shots(ts)")

    # 远程配置（全局一份，带自增版本号）
    conn.execute("""
        CREATE TABLE IF NOT EXISTS app_config (
            id         INTEGER PRIMARY KEY CHECK (id = 1),
            config     TEXT NOT NULL,
            version    INTEGER NOT NULL DEFAULT 1,
            updated_at INTEGER NOT NULL
        )
    """)
    # 设备心跳状态：最近通信时间、设备已生效的配置版本
    conn.execute("""
        CREATE TABLE IF NOT EXISTS device_state (
            device          TEXT PRIMARY KEY,
            last_seen       INTEGER,
            applied_version INTEGER,
            last_ip         TEXT
        )
    """)
    # 设备独立配置：每台设备可覆盖全局配置中的任意字段
    conn.execute("""
        CREATE TABLE IF NOT EXISTS device_config (
            device      TEXT PRIMARY KEY,
            config      TEXT NOT NULL,
            version     INTEGER NOT NULL DEFAULT 1,
            updated_at  INTEGER NOT NULL
        )
    """)
    conn.commit()
    conn.close()

from contextlib import contextmanager

def _configure_conn(conn):
    """配置连接：WAL 模式 + busy_timeout + 同步级别"""
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA synchronous=NORMAL")
    conn.execute("PRAGMA busy_timeout=15000")
    conn.row_factory = sqlite3.Row

@contextmanager
def db():
    """推荐用法：with db() as conn: ... 自动 commit/rollback/close"""
    conn = sqlite3.connect(DB_PATH, timeout=15)
    _configure_conn(conn)
    try:
        yield conn
        conn.commit()
    except:
        conn.rollback()
        raise
    finally:
        conn.close()

def get_db():
    """获取数据库连接（已配置 WAL + busy_timeout）"""
    conn = sqlite3.connect(DB_PATH, timeout=15)
    _configure_conn(conn)
    return conn

# ============================================================================
# 远程配置（服务端保存，设备拉取，支持定时策略）
# ============================================================================
# 配置只保存一份（全局），所有设备共用。
# 设备通过 GET /api/config 拉取，或在上传响应里被捎带。
# 每条配置带一个自增 version，设备上报自己的版本号，
# 一致时服务端返回 304（约 100 字节），避免重复传输。
# ============================================================================

# 默认配置：新建数据库时写入，也是"恢复默认"的基准。
# schedule 为定时策略列表，默认含一条"夜间省流"但处于关闭状态。
DEFAULT_REMOTE_CONFIG = {
    "capture": {
        "interval_sec": 9,
        "quality": 30,
        "width": 720,
        "height": 1600,
        "pause_on_screen_off": True,
        "paused": False,
        "cellular_enabled": True,
        "cellular_width": 480,
        "cellular_height": 1066,
        "cellular_quality": 20,
    },
    "change_detection": {
        "enabled": True,
        "threshold": 0.92,
    },
    "upload": {
        "offline_queue": True,
        "backlog_batch": 5,
        "enabled": True,
        "wifi_only": False,
    },
    "server": {
        "config_poll_sec": 300,
    },
    "schedule": [
        {
            "name": "夜间省流",
            "enabled": False,
            "priority": 10,
            "days": [1, 2, 3, 4, 5, 6, 7],
            "start": "23:00",
            "end": "07:00",
            "overrides": {
                "capture.interval_sec": 30,
                "capture.quality": 20,
                "capture.width": 480,
                "capture.height": 1066,
            },
        }
    ],
}

def _ensure_config_row():
    """确保配置行存在（首次启动自动写入默认配置）"""
    with db() as conn:
        conn.execute(
            "INSERT OR IGNORE INTO app_config (id, config, version, updated_at) "
            "VALUES (1, ?, 1, ?)",
            (json.dumps(DEFAULT_REMOTE_CONFIG, ensure_ascii=False), int(time.time() * 1000))
        )

def load_remote_config():
    """读取服务端保存的配置与版本号"""
    _ensure_config_row()
    with db() as conn:
        row = conn.execute("SELECT config, version FROM app_config WHERE id = 1").fetchone()
    try:
        cfg = json.loads(row["config"])
    except (ValueError, TypeError):
        cfg = copy.deepcopy(DEFAULT_REMOTE_CONFIG)
    return cfg, int(row["version"])

def save_remote_config(cfg: dict, bump: bool = True) -> int:
    """写入配置；bump=True 时版本号 +1（触发设备重新拉取）。返回新版本号。"""
    _ensure_config_row()
    now = int(time.time() * 1000)
    with db() as conn:
        if bump:
            conn.execute(
                "UPDATE app_config SET config = ?, version = version + 1, updated_at = ? "
                "WHERE id = 1", (json.dumps(cfg, ensure_ascii=False), now))
        else:
            conn.execute(
                "UPDATE app_config SET config = ?, updated_at = ? WHERE id = 1",
                (json.dumps(cfg, ensure_ascii=False), now))
        ver = conn.execute("SELECT version FROM app_config WHERE id = 1").fetchone()["version"]
    return int(ver)

def load_device_config(device_id: str):
    """读取设备独立配置（覆盖字段），无覆盖返回 (None, 0)"""
    with db() as conn:
        row = conn.execute(
            "SELECT config, version FROM device_config WHERE device = ?",
            (device_id,)).fetchone()
    if not row:
        return None, 0
    try:
        return json.loads(row["config"]), int(row["version"])
    except (ValueError, TypeError):
        return None, 0

def save_device_config(device_id: str, cfg: dict, bump: bool = True) -> int:
    """写入设备独立配置"""
    now = int(time.time() * 1000)
    with db() as conn:
        existing = conn.execute(
            "SELECT version FROM device_config WHERE device = ?",
            (device_id,)).fetchone()
        if existing:
            if bump:
                conn.execute(
                    "UPDATE device_config SET config=?, version=version+1, updated_at=? WHERE device=?",
                    (json.dumps(cfg, ensure_ascii=False), now, device_id))
            else:
                conn.execute(
                    "UPDATE device_config SET config=?, updated_at=? WHERE device=?",
                    (json.dumps(cfg, ensure_ascii=False), now, device_id))
        else:
            conn.execute(
                "INSERT INTO device_config(device, config, version, updated_at) VALUES(?,?,1,?)",
                (device_id, json.dumps(cfg, ensure_ascii=False), now))
        ver = conn.execute(
            "SELECT version FROM device_config WHERE device=?",
            (device_id,)).fetchone()["version"]
    return int(ver)

def delete_device_config(device_id: str):
    """删除设备独立配置（恢复为全局配置）"""
    with db() as conn:
        conn.execute("DELETE FROM device_config WHERE device = ?", (device_id,))

def merge_device_overrides(base: dict, overrides: dict) -> dict:
    """将设备覆盖字段深度合并到基础配置上"""
    result = copy.deepcopy(base)
    for k, v in overrides.items():
        if isinstance(v, dict) and isinstance(result.get(k), dict):
            result[k] = merge_device_overrides(result[k], v)
        else:
            result[k] = copy.deepcopy(v)
    return result

def _set_dotted(d: dict, key: str, value):
    """按点分路径写入嵌套字典，如 capture.interval_sec -> d['capture']['interval_sec']"""
    parts = key.split(".")
    cur = d
    for p in parts[:-1]:
        nxt = cur.get(p)
        if not isinstance(nxt, dict):
            nxt = {}
            cur[p] = nxt
        cur = nxt
    cur[parts[-1]] = value

def _clamp(value, lo, hi, default):
    """把数值钳制到 [lo, hi]，非法值回退到 default"""
    try:
        v = float(value)
    except (TypeError, ValueError):
        return default
    v = min(hi, max(lo, v))
    return float(v) if isinstance(default, float) else int(v)

def clamp_config(cfg: dict) -> dict:
    """服务端侧数值钳制：防止界面误填（如 interval=0）导致设备端异常"""
    cap = cfg.setdefault("capture", {})
    cap["interval_sec"]      = _clamp(cap.get("interval_sec"), 1, 3600, 9)
    cap["quality"]           = _clamp(cap.get("quality"), 1, 100, 30)
    cap["width"]             = _clamp(cap.get("width"), 64, 4096, 720)
    cap["height"]            = _clamp(cap.get("height"), 64, 4096, 1600)
    cap["cellular_quality"]  = _clamp(cap.get("cellular_quality"), 1, 100, 20)
    cap["cellular_width"]    = _clamp(cap.get("cellular_width"), 64, 4096, 480)
    cap["cellular_height"]   = _clamp(cap.get("cellular_height"), 64, 4096, 1066)
    cap["pause_on_screen_off"] = bool(cap.get("pause_on_screen_off", True))
    cap["paused"]              = bool(cap.get("paused", False))
    cap["cellular_enabled"]    = bool(cap.get("cellular_enabled", True))
    cap["live_mode"]           = bool(cap.get("live_mode", False))

    cd = cfg.setdefault("change_detection", {})
    cd["enabled"]   = bool(cd.get("enabled", True))
    cd["threshold"] = _clamp(cd.get("threshold"), 0.0, 1.0, 0.92)

    up = cfg.setdefault("upload", {})
    up["offline_queue"]  = bool(up.get("offline_queue", True))
    up["enabled"]        = bool(up.get("enabled", True))
    up["wifi_only"]      = bool(up.get("wifi_only", False))
    up["backlog_batch"]  = _clamp(up.get("backlog_batch"), 1, 50, 5)
    up["retry"]          = _clamp(up.get("retry"), 0, 5, 3)
    up["retry_delay_sec"] = _clamp(up.get("retry_delay_sec"), 0, 30, 5)
    up["max_queue_mb"]   = _clamp(up.get("max_queue_mb"), 1, 1024, 200)

    sv = cfg.setdefault("server", {})
    sv["config_poll_sec"] = _clamp(sv.get("config_poll_sec"), 0, 3600, 300)
    sv["timeout_ms"]     = _clamp(sv.get("timeout_ms"), 1000, 60000, 10000)

    # 枚举白名单：非法值回退默认
    if cap.get("backend") not in ("screencap", "mediaprojection", "testpattern"):
        cap["backend"] = "screencap"
    if cap.get("format") not in ("jpeg", "jpg", "png"):
        cap["format"] = "jpeg"

    # 定时策略：清洗为规范结构，剔除非法条目，避免脏数据进入匹配引擎
    cleaned = []
    sched = cfg.get("schedule")
    if isinstance(sched, list):
        for p in sched:
            if not isinstance(p, dict):
                continue
            raw_days = p.get("days")
            days = [d for d in raw_days if isinstance(d, int) and 1 <= d <= 7] \
                   if isinstance(raw_days, list) else [1, 2, 3, 4, 5, 6, 7]
            if not days:
                days = [1, 2, 3, 4, 5, 6, 7]
            st, en = str(p.get("start") or "00:00"), str(p.get("end") or "00:00")
            if not _valid_hhmm(st): st = "00:00"
            if not _valid_hhmm(en): en = "00:00"
            try:
                prio = int(p.get("priority", 10))
            except (TypeError, ValueError):
                prio = 10
            ov = p.get("overrides")
            cleaned.append({
                "name":    str(p.get("name") or "未命名策略"),
                "enabled": bool(p.get("enabled", False)),
                "priority": prio,
                "days":    sorted(set(days)),
                "start":   st,
                "end":     en,
                "overrides": ov if isinstance(ov, dict) else {},
            })
    cfg["schedule"] = cleaned
    return cfg

def _valid_hhmm(s: str) -> bool:
    """校验 "HH:MM" 是否合法"""
    try:
        h, m = str(s).split(":")[:2]
        return 0 <= int(h) <= 23 and 0 <= int(m) <= 59
    except (ValueError, AttributeError):
        return False

def _hhmm(s: str) -> int:
    """把 "23:00" 转成当天分钟数"""
    h, m = str(s).split(":")[:2]
    return int(h) * 60 + int(m)

def match_policy(schedule, now: datetime = None):
    """找出当前时间命中的定时策略。

    返回 (policy_index, policy)；未命中返回 (0, None)。
    policy_index 从 1 开始（0 表示无策略，只应用基础配置）。
    start > end 表示跨午夜（如 23:00-07:00）。
    多条同时命中时取 priority 最大者。
    """
    now = now or datetime.now()
    weekday = now.isoweekday()          # 周一=1 ... 周日=7
    minutes = now.hour * 60 + now.minute

    best = None
    for idx, p in enumerate(schedule or []):
        if not isinstance(p, dict) or not p.get("enabled"):
            continue
        days = p.get("days") or [1, 2, 3, 4, 5, 6, 7]
        if weekday not in days:
            continue
        try:
            start, end = _hhmm(p["start"]), _hhmm(p["end"])
        except (ValueError, KeyError):
            continue
        if start <= end:
            if not (start <= minutes < end):
                continue
        else:  # 跨午夜
            if not (minutes >= start or minutes < end):
                continue
        prio = p.get("priority", 0)
        try:
            prio = int(prio)
        except (TypeError, ValueError):
            prio = 0
        if best is None or prio > best[0]:
            best = (prio, idx, p)

    if best is None:
        return 0, None
    return best[1] + 1, best[2]

def effective_config(device_id: str = None, now: datetime = None):
    """计算当前应下发给设备的配置。

    device_id 非空时，叠加该设备的独立覆盖字段。
    定时策略在服务端算好再下发，设备端不需要规则引擎。
    返回 (version, config, policy)。
    """
    base, ver = load_remote_config()
    idx, policy = match_policy(base.get("schedule"), now)

    cfg = copy.deepcopy(base)

    if policy:
        for k, v in (policy.get("overrides") or {}).items():
            _set_dotted(cfg, k, v)

    # 叠加设备独立配置（优先级最高）
    device_ver = 0
    if device_id:
        dev_cfg, device_ver = load_device_config(device_id)
        if dev_cfg:
            cfg = merge_device_overrides(cfg, dev_cfg)

    clamp_config(cfg)
    cfg.pop("schedule", None)

    # 内容寻址版本号：配置内容一变版本必然变，无碰撞风险
    payload = json.dumps(cfg, sort_keys=True, ensure_ascii=False).encode()
    return int(hashlib.sha256(payload).hexdigest()[:12], 16), cfg, policy

def touch_device(device_id: str, applied_version=None, ip: str = None):
    """记录设备心跳（最近一次通信时间、已生效版本、来源 IP）

    applied_version 为非正数时视为"未知"，保留原有记录不覆盖——
    否则未携带版本号的上报（旧版本 agent、首次启动）会把状态冲成 0，
    管理端就会误判为"待同步"。
    """
    try:
        applied_version = int(applied_version) if applied_version is not None else None
    except (TypeError, ValueError):
        applied_version = None
    if applied_version is not None and applied_version <= 0:
        applied_version = None

    now = int(time.time() * 1000)
    with db() as conn:
        conn.execute(
            "INSERT INTO device_state(device, last_seen, applied_version, last_ip) "
            "VALUES (?, ?, ?, ?) ON CONFLICT(device) DO UPDATE SET "
            "last_seen = excluded.last_seen, "
            "applied_version = COALESCE(excluded.applied_version, device_state.applied_version), "
            "last_ip = COALESCE(excluded.last_ip, device_state.last_ip)",
            (device_id, now, applied_version, ip))

# ============================================================================
# 图片尺寸解析（不依赖 Pillow，支持 JPEG/PNG）
# ============================================================================

def jpeg_size(data: bytes):
    """从 JPEG 字节流解析 (width, height)，失败返回 (None, None)"""
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        return None, None

    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        # SOF0-SOF15（跳过 DHT/JPG/DAC 保留段）
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            height = (data[i + 5] << 8) | data[i + 6]
            width = (data[i + 7] << 8) | data[i + 8]
            return width, height
        # 跳过本段
        seg_len = (data[i + 2] << 8) | data[i + 3]
        if seg_len <= 0:
            return None, None
        i += 2 + seg_len
    return None, None

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

def png_size(data: bytes):
    """从 PNG 字节流解析 (width, height)（IHDR 大端，偏移 16/20）"""
    if len(data) < 24 or data[:8] != PNG_MAGIC:
        return None, None
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    return width, height

def image_size(data: bytes):
    """根据魔数自动识别图片格式并解析尺寸"""
    return png_size(data) or jpeg_size(data)

# ============================================================================
# FastAPI 应用
# ============================================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    os.makedirs(STORE_DIR, exist_ok=True)
    init_db()
    yield

app = FastAPI(title="Remote Phone Monitor", version="1.0.0", lifespan=lifespan)

# ============================================================================
# 实时查看：内存帧缓冲 + MJPEG 流
# ============================================================================
# 每台设备缓存最新一帧（内存中，不落盘），供 /api/live/stream 消费。
# 上传接口写入，MJPEG 端点读取，用 threading.Lock 保护并发访问。
import threading

_live_frames = {}       # {device_id: bytes}  最新一帧 JPEG
_live_frames_ts = {}    # {device_id: float}  最后更新时间
_live_frames_lock = threading.Lock()
_live_subscribers = {}  # {device_id: threading.Event}  新帧通知
_live_mode_devices = {}  # {device_id: last_active_ts} 实时查看设备+最后活跃时间
_live_mode_lock = threading.Lock()
_LIVE_MODE_TTL = 60  # 60秒无活跃自动过期

def _push_live_frame(device_id: str, jpeg_data: bytes):
    """上传接口调用：更新最新帧并通知等待的流"""
    with _live_frames_lock:
        _live_frames[device_id] = jpeg_data
        _live_frames_ts[device_id] = time.time()
        ev = _live_subscribers.get(device_id)
        if ev:
            ev.set()

def _get_live_frame(device_id: str, timeout: float = 2.0):
    """MJPEG 流调用：阻塞等待新帧，返回 JPEG bytes 或 None"""
    with _live_frames_lock:
        if device_id not in _live_subscribers:
            _live_subscribers[device_id] = threading.Event()
        ev = _live_subscribers[device_id]
    ev.wait(timeout=timeout)
    ev.clear()
    with _live_frames_lock:
        return _live_frames.get(device_id)

def _cleanup_live_frames():
    """清理超过30秒未更新的帧（设备下线后释放内存）"""
    now = time.time()
    with _live_frames_lock:
        stale = [did for did, ts in _live_frames_ts.items() if now - ts > 30]
        for did in stale:
            _live_frames.pop(did, None)
            _live_frames_ts.pop(did, None)
            _live_subscribers.pop(did, None)

def _clear_live_frame(device_id: str):
    """实时查看结束时立即释放该设备的帧缓冲"""
    with _live_frames_lock:
        _live_frames.pop(device_id, None)
        _live_frames_ts.pop(device_id, None)
        ev = _live_subscribers.pop(device_id, None)
    # 唤醒可能阻塞的生成器，让它退出
    if ev:
        ev.set()

# ============================================================================
# API 端点
# ============================================================================

@app.post("/api/upload")
def upload(
    request: Request,
    image: UploadFile = File(...),
    x_device_id: str = Header(None, alias="X-Device-ID"),
    x_device_token: str = Header(None, alias="X-Device-Token"),
    x_capture_ts: str = Header(None, alias="X-Capture-Ts"),
    x_config_version: str = Header(None, alias="X-Config-Version")
):
    """接收设备上传的截图。

    设备通过 X-Device-Token 鉴权，X-Device-ID 必须与服务端映射一致。
    顺带承担配置下发职责：设备随请求上报自己已生效的版本号，
    服务端版本不一致时在同一份响应里附一份配置，不产生额外请求。
    """
    device_id = authenticate_device(x_device_token, x_device_id)

    # 读取图片数据（sync 端点用 image.file.read()）
    data = image.file.read()
    if not data:
        raise HTTPException(status_code=400, detail="Empty image")

    # 解析图片尺寸（自动识别 PNG/JPEG）
    width, height = image_size(data)

    # 实时查看模式：只进内存缓冲，不落盘、不写 DB
    if _is_live_mode(device_id):
        ext = ".jpg" if data[:3] == b'\xff\xd8\xff' else ".png"
        if ext == ".jpg":
            _push_live_frame(device_id, data)
        client_ip = request.client.host if request.client else None
        touch_device(device_id, 0, client_ip)
        return {"ok": True, "live_mode": True, "width": width, "height": height}

    # 生成时间戳和路径：
    # 优先采用 agent 传来的拍摄时间（X-Capture-Ts，离线补传时与上传时间相差很大），
    # 缺失/非法/超前超过容差（手机时钟漂移）时回退到服务端时间。
    ts = int(time.time() * 1000)
    if x_capture_ts:
        try:
            cts = int(x_capture_ts)
            if 0 < cts <= ts + 120_000:  # 容差 2 分钟
                ts = cts
        except ValueError:
            pass  # 非法头，按无拍摄时间处理
    day = datetime.fromtimestamp(ts / 1000).strftime("%Y/%m/%d")
    day_dir = os.path.join(STORE_DIR, day)
    os.makedirs(day_dir, exist_ok=True)

    ext = ".png" if data[:8] == PNG_MAGIC else ".jpg"
    # 文件名加设备 ID 和随机片段，防止多设备同毫秒碰撞
    rel_path = f"{day}/{device_id}-{ts}-{secrets.token_hex(4)}{ext}"
    full_path = os.path.join(STORE_DIR, rel_path)

    # 保存图片
    with open(full_path, "wb") as f:
        f.write(data)

    # 计算 hash
    file_hash = hashlib.blake2b(data, digest_size=16).hexdigest()

    # 写入数据库（失败时回滚文件，避免孤儿）
    try:
        with db() as conn:
            conn.execute(
                "INSERT INTO shots(device, ts, path, width, height, size_kb, hash) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (device_id, ts, rel_path, width, height, len(data) / 1024, file_hash)
            )
    except Exception:
        os.remove(full_path)  # DB 失败，回滚文件
        raise

    # 更新实时查看帧缓冲（仅 JPEG，PNG 太大不适合实时流）
    if ext == ".jpg":
        _push_live_frame(device_id, data)

    # 配置捎带：版本号不一致时才附带完整配置（约 600-800 字节）
    eff_version, eff_cfg, policy = effective_config(device_id)
    try:
        device_version = int(x_config_version) if x_config_version else 0
    except ValueError:
        device_version = 0

    out = {"ok": True, "ts": ts, "path": rel_path,
           "width": width, "height": height,
           "config_version": eff_version}
    if device_version != eff_version:
        out["config"] = eff_cfg
        out["policy"] = (policy or {}).get("name")   # 只下发策略名，省字节
        # 兼容旧版 agent：早期固件只认 "version" 字段，读不到就整包丢弃，
        # 表现为"改了配置不生效"。版本一致时不下发，稳态零额外字节。
        out["version"] = eff_version

    # 实时查看指令：通过上传响应即时通知 agent（不走配置心跳）
    out["live_mode"] = _is_live_mode(device_id)

    client_ip = request.client.host if request.client else None
    touch_device(device_id, device_version, client_ip)
    cleanup_storage()
    _cleanup_live_frames()  # 清理超过30秒未更新的帧缓冲
    return out

@app.get("/api/config")
def get_config(
    request: Request,
    x_device_id: str = Header(None, alias="X-Device-ID"),
    x_device_token: str = Header(None, alias="X-Device-Token"),
    if_none_match: str = Header(None, alias="If-None-Match")
):
    """设备拉取配置（心跳轮询通道）。

    设备通过 X-Device-Token 鉴权。
    把本地已生效版本放在 If-None-Match 里，一致时返回 304 且响应体为空。
    """
    device_id = authenticate_device(x_device_token, x_device_id)
    version, cfg, policy = effective_config(device_id)
    client_ip = request.client.host if request.client else None

    # 记录的是「设备自称已生效的版本」，不是服务端版本。
    # 早先这里传的是 version（服务端版本），于是只要设备来问过一次，
    # 管理后台就显示成"已同步"——哪怕设备那边其实一次都没应用成功，
    # 正好把"改了配置不生效"这个故障掩盖掉了。
    device_version = 0
    if if_none_match:
        try:
            device_version = int(if_none_match.strip('"'))
        except ValueError:
            device_version = 0
    touch_device(device_id, device_version, client_ip)

    if if_none_match and if_none_match.strip('"') == str(version):
        return Response(status_code=304)

    return {"version": version, "config": cfg,
            "policy": (policy or {}).get("name")}   # 设备端只需策略名

@app.get("/api/admin/config")
def admin_get_config(request: Request, x_admin_token: str = Header(None, alias="X-Admin-Token")):
    """管理端读取配置（含 schedule 定义）与当前生效状态"""
    require_admin(request, x_admin_token)
    cfg, version = load_remote_config()
    eff_version, _eff_cfg, policy = effective_config()
    with db() as conn:
        rows = conn.execute("SELECT * FROM device_state ORDER BY last_seen DESC").fetchall()
    # 附加每个设备的独立配置
    devices = []
    for r in rows:
        d = dict(r)
        dev_cfg, dev_ver = load_device_config(d["device"])
        d["custom_config"] = dev_cfg
        d["custom_version"] = dev_ver
        devices.append(d)
    return {
        "version": version,
        "config": cfg,
        "effective_version": eff_version,
        "active_policy": policy,
        "devices": devices,
    }

@app.put("/api/admin/config")
def admin_put_config(
    request: Request,
    payload: dict,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """管理端写入配置，版本号 +1（设备下次通信即生效）。

    body 可以是完整配置对象，也可以是 {"config": {...}} 形式。
    服务端会做数值钳制，非法值就地修正而不是拒绝整个请求。
    """
    require_admin(request, x_admin_token)
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400, detail="config object required")

    cfg = payload.get("config") if isinstance(payload.get("config"), dict) else payload

    # 与已保存的配置按顶层键合并：本次未提交的分组（典型如 schedule）
    # 保持原样，避免局部更新把定时策略悄悄清空。
    stored, _ = load_remote_config()
    merged = copy.deepcopy(stored)
    for k, v in cfg.items():
        merged[k] = copy.deepcopy(v)

    clamp_config(merged)
    version = save_remote_config(merged, bump=True)
    return {"ok": True, "version": version}

@app.post("/api/admin/config/reset")
def admin_reset_config(request: Request, x_admin_token: str = Header(None, alias="X-Admin-Token")):
    """恢复默认配置（相当于一键回滚）"""
    require_admin(request, x_admin_token)
    version = save_remote_config(copy.deepcopy(DEFAULT_REMOTE_CONFIG), bump=True)
    return {"ok": True, "version": version}

# --------------- 设备独立配置管理 ---------------

@app.get("/api/admin/devices/{device_id}/config")
def admin_get_device_config(
    request: Request,
    device_id: str,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """读取设备独立配置"""
    require_admin(request, x_admin_token)
    dev_cfg, dev_ver = load_device_config(device_id)
    eff_ver, eff_cfg, _ = effective_config(device_id)
    return {
        "device": device_id,
        "custom_config": dev_cfg,
        "custom_version": dev_ver,
        "effective_config": eff_cfg,
        "effective_version": eff_ver,
    }

@app.put("/api/admin/devices/{device_id}/config")
def admin_put_device_config(
    request: Request,
    device_id: str,
    payload: dict,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """写入设备独立配置（只存覆盖字段，未提及的字段保持全局默认）"""
    require_admin(request, x_admin_token)
    if not isinstance(payload, dict):
        raise HTTPException(status_code=400, detail="config object required")
    cfg = payload.get("config") if isinstance(payload.get("config"), dict) else payload
    version = save_device_config(device_id, cfg, bump=True)
    return {"ok": True, "device": device_id, "version": version}

@app.delete("/api/admin/devices/{device_id}/config")
def admin_delete_device_config(
    request: Request,
    device_id: str,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """删除设备独立配置（恢复为全局配置）"""
    require_admin(request, x_admin_token)
    delete_device_config(device_id)
    return {"ok": True, "device": device_id}

@app.get("/api/latest")
def latest(request: Request, device: str = Query("DEVICE_A", description="设备 ID")):
    """获取最新截图"""
    require_viewer(request)
    with db() as conn:
        row = conn.execute(
            "SELECT * FROM shots WHERE device = ? ORDER BY ts DESC LIMIT 1",
            (device,)
        ).fetchone()
    if not row:
        return {}
    return dict(row)

@app.get("/api/history")
def history(
    request: Request,
    device: str = Query("DEVICE_A", description="设备 ID"),
    limit: int = Query(200, ge=1, le=1000, description="返回数量"),
    since: int = Query(0, description="起始时间戳（毫秒）")
):
    """获取历史截图列表"""
    require_viewer(request)
    with db() as conn:
        rows = conn.execute(
            "SELECT * FROM shots WHERE device = ? AND ts >= ? ORDER BY ts DESC LIMIT ?",
            (device, since, limit)
        ).fetchall()
    return [dict(r) for r in rows]

@app.get("/api/devices")
def devices(request: Request):
    """获取所有设备列表（附带心跳时间与设备已生效的配置版本）"""
    require_viewer(request)
    with db() as conn:
        rows = conn.execute(
            "SELECT DISTINCT device, COUNT(*) as shot_count, MAX(ts) as last_ts "
            "FROM shots GROUP BY device"
        ).fetchall()
        states = {r["device"]: dict(r)
                  for r in conn.execute("SELECT * FROM device_state").fetchall()}

    out = []
    for r in rows:
        d = dict(r)
        st = states.get(d["device"], {})
        d["last_seen"] = st.get("last_seen")
        d["applied_version"] = st.get("applied_version")
        out.append(d)
    # 已登记但还没有截图的设备也要列出
    known = {d["device"] for d in out}
    for dev, st in states.items():
        if dev not in known:
            out.append({"device": dev, "shot_count": 0, "last_ts": None,
                        "last_seen": st.get("last_seen"),
                        "applied_version": st.get("applied_version")})
    return out

@app.get("/api/stats")
def stats(request: Request, device: str = Query("DEVICE_A", description="设备 ID")):
    """获取设备统计信息"""
    require_viewer(request)
    with db() as conn:
        row = conn.execute("""
            SELECT
                COUNT(*) as total_shots,
                SUM(size_kb) as total_size_kb,
                MIN(ts) as first_ts,
                MAX(ts) as last_ts
            FROM shots WHERE device = ?
        """, (device,)).fetchone()
    if not row:
        return {}
    return dict(row)

@app.get("/api/shots")
def list_shots(
    request: Request,
    device: str = Query(None, description="设备 ID，不传则返回全部"),
    date:   str = Query(None, description="按存储日期筛选，格式 YYYY-MM-DD"),
    sort:   str = Query("desc", pattern="^(asc|desc)$", description="按 ts 排序方向"),
    limit:  int = Query(20, ge=1, le=100, description="每页显示数量（1-100）"),
    page:   int = Query(1, ge=1, description="页码（从 1 开始）")
):
    """列出截图（管理后台用），支持设备/日期筛选、时间排序与分页。

    分页基于当前选定的 limit 计算：offset = (page-1) * limit，
    total_pages = ceil(total / limit)。
    """
    require_viewer(request)
    where, params = [], []
    if device:
        where.append("device = ?"); params.append(device)
    if date:
        # path 形如 YYYY/MM/DD/<ts>.<ext>，按日期前缀匹配
        where.append("path LIKE ?"); params.append(date.replace("-", "/") + "%")
    sql_where = ("WHERE " + " AND ".join(where)) if where else ""

    with db() as conn:
        total = conn.execute(f"SELECT COUNT(*) FROM shots {sql_where}", params).fetchone()[0]
        total_pages = (total + limit - 1) // limit if limit > 0 else 1
        if page > total_pages:
            page = max(1, total_pages)
        offset = (page - 1) * limit
        rows = conn.execute(
            f"SELECT * FROM shots {sql_where} "
            f"ORDER BY ts {'DESC' if sort == 'desc' else 'ASC'} LIMIT ? OFFSET ?",
            params + [limit, offset]
        ).fetchall()

    items = []
    for r in rows:
        d = dict(r)
        d["url"] = f"/img/{d['path']}"
        items.append(d)
    return {
        "total": total,
        "items": items,
        "sort": sort,
        "limit": limit,
        "page": page,
        "total_pages": total_pages,
    }

def _safe_remove(path_rel: str) -> bool:
    """在 STORE_DIR 内安全删除一个文件（防路径穿越），返回是否实际删除。"""
    base = os.path.realpath(STORE_DIR)
    full = os.path.realpath(os.path.join(STORE_DIR, path_rel))
    if full != base and not full.startswith(base + os.sep):
        return False
    try:
        if os.path.isfile(full):
            os.remove(full)
            return True
    except OSError:
        pass
    return False

@app.delete("/api/shots/{shot_id}")
def delete_shot(
    request: Request,
    shot_id: int,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """删除单张截图（DB 记录 + 文件）。"""
    require_admin(request, x_admin_token)
    with db() as conn:
        row = conn.execute("SELECT path FROM shots WHERE id = ?", (shot_id,)).fetchone()
        if not row:
            raise HTTPException(status_code=404, detail="Shot not found")
        _safe_remove(row["path"])
        conn.execute("DELETE FROM shots WHERE id = ?", (shot_id,))
    return {"ok": True, "id": shot_id}

@app.post("/api/shots/batch-delete")
def batch_delete(
    request: Request,
    payload: dict,
    x_admin_token: str = Header(None, alias="X-Admin-Token")
):
    """批量删除截图，body: {"ids": [1, 2, 3]}"""
    require_admin(request, x_admin_token)
    ids = payload.get("ids") if isinstance(payload, dict) else None
    if not isinstance(ids, list) or not ids:
        raise HTTPException(status_code=400, detail="ids (list) required")

    placeholders = ",".join("?" * len(ids))
    with db() as conn:
        rows = conn.execute(
            f"SELECT id, path FROM shots WHERE id IN ({placeholders})", ids
        ).fetchall()
        deleted = 0
        for r in rows:
            if _safe_remove(r["path"]):
                deleted += 1
        conn.execute(f"DELETE FROM shots WHERE id IN ({placeholders})", ids)
    return {"ok": True, "requested": len(ids), "deleted": deleted}

# --------------- 实时查看控制 ---------------

@app.post("/api/live/start")
def live_start(request: Request, device: str = Query(...)):
    """开启设备实时查看模式（通过上传响应即时通知 agent）"""
    require_admin(request)
    with _live_mode_lock:
        _live_mode_devices[device] = time.time()
    return {"ok": True, "device": device}

@app.post("/api/live/stop")
def live_stop(request: Request, device: str = Query(...)):
    """关闭设备实时查看模式，立即释放帧缓冲"""
    require_admin(request)
    with _live_mode_lock:
        _live_mode_devices.pop(device, None)
    _clear_live_frame(device)
    return {"ok": True, "device": device}

def _is_live_mode(device: str) -> bool:
    with _live_mode_lock:
        ts = _live_mode_devices.get(device)
        if ts is None:
            return False
        if time.time() - ts > _LIVE_MODE_TTL:
            # 超时自动过期，清理资源
            _live_mode_devices.pop(device, None)
            _clear_live_frame(device)
            return False
        return True

def _touch_live_mode(device: str):
    """MJPEG 流 / 上传时刷新活跃时间戳"""
    with _live_mode_lock:
        if device in _live_mode_devices:
            _live_mode_devices[device] = time.time()

# --------------- 实时查看 MJPEG 流 ---------------

@app.get("/api/live/stream")
async def live_stream(
    request: Request,
    device: str = Query("DEVICE_A", description="设备 ID"),
    fps: int = Query(1, ge=1, le=10, description="目标帧率")
):
    """MJPEG 实时流（异步生成器，不占线程池）。
    单条流最长 10 分钟自动断开，防止半开连接泄漏。
    """
    require_viewer(request)
    import asyncio
    from starlette.responses import StreamingResponse

    boundary = "frame"
    interval = max(1.0 / fps, 0.2)
    max_duration = 600  # 10 分钟强制断开

    async def generate():
        start = asyncio.get_event_loop().time()
        try:
            while (asyncio.get_event_loop().time() - start) < max_duration:
                _touch_live_mode(device)  # 刷新活跃时间，防止 TTL 过期
                frame = await asyncio.to_thread(_get_live_frame, device, interval)
                if frame:
                    yield (f"--{boundary}\r\n"
                           f"Content-Type: image/jpeg\r\n"
                           f"Content-Length: {len(frame)}\r\n\r\n").encode()
                    yield frame
                    yield b"\r\n"
                    del frame
                else:
                    await asyncio.sleep(0.3)
        finally:
            with _live_frames_lock:
                _live_subscribers.pop(device, None)

    return StreamingResponse(
        generate(),
        media_type=f"multipart/x-mixed-replace; boundary={boundary}",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )

@app.get("/api/live/snapshot")
def live_snapshot(request: Request, device: str = Query("DEVICE_A")):
    """获取设备最新一帧（单张 JPEG），无帧时返回404"""
    require_viewer(request)
    with _live_frames_lock:
        frame = _live_frames.get(device)
    if not frame:
        return Response(status_code=404, content="No frame available")
    return Response(content=frame, media_type="image/jpeg",
                    headers={"Cache-Control": "no-store"})

@app.get("/img/{path:path}")
async def img(request: Request, path: str):
    """提供截图访问（带路径穿越防护）"""
    require_viewer(request)
    # 规范化并确保最终路径仍位于 STORE_DIR 内，防止 ../ 路径穿越
    base = os.path.realpath(STORE_DIR)
    full_path = os.path.realpath(os.path.join(STORE_DIR, path))

    if not full_path.startswith(base + os.sep) and full_path != base:
        raise HTTPException(status_code=403, detail="Forbidden")

    if not os.path.isfile(full_path):
        raise HTTPException(status_code=404, detail="Image not found")

    media_type = "image/png" if full_path.lower().endswith(".png") \
        else "image/jpeg" if full_path.lower().endswith((".jpg", ".jpeg")) \
        else "application/octet-stream"
    return FileResponse(full_path, media_type=media_type)

# ============================================================================
# Dashboard & Admin（需要登录）
# ============================================================================

LOGIN_HTML = """<!DOCTYPE html>
<html lang="zh">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>登录 · Remote Phone Monitor</title>
    <style>
        :root {
            --primary: #3b82f6;
            --primary-hover: #2563eb;
            --primary-glow: rgba(59, 130, 246, 0.35);
            --bg-base: #090d16;
            --card-bg: rgba(18, 24, 38, 0.7);
            --card-border: rgba(255, 255, 255, 0.08);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --input-bg: rgba(11, 15, 25, 0.6);
            --input-border: rgba(255, 255, 255, 0.12);
        }

        * { margin: 0; padding: 0; box-sizing: border-box; }

        body {
            background-color: var(--bg-base);
            color: var(--text-main);
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Hiragino Sans GB", sans-serif;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            position: relative;
            overflow: hidden;
        }

        /* 背景氛围微光渐变 */
        body::before, body::after {
            content: "";
            position: absolute;
            width: 320px;
            height: 320px;
            border-radius: 50%;
            filter: blur(100px);
            opacity: 0.25;
            pointer-events: none;
            z-index: 0;
        }

        body::before {
            background: #3b82f6;
            top: 20%;
            left: 50%;
            transform: translateX(-120%);
        }

        body::after {
            background: #8b5cf6;
            bottom: 20%;
            right: 50%;
            transform: translateX(120%);
        }

        /* 磨砂玻璃卡片 */
        .login-box {
            position: relative;
            z-index: 1;
            background: var(--card-bg);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border: 1px solid var(--card-border);
            border-radius: 20px;
            padding: 44px 36px;
            width: 380px;
            max-width: 90vw;
            box-shadow: 0 20px 50px rgba(0, 0, 0, 0.5),
                        inset 0 1px 0 rgba(255, 255, 255, 0.1);
        }

        /* 图标徽章 */
        .badge-icon {
            width: 52px;
            height: 52px;
            margin: 0 auto 18px;
            background: rgba(59, 130, 246, 0.1);
            border: 1px solid rgba(59, 130, 246, 0.2);
            border-radius: 14px;
            display: flex;
            align-items: center;
            justify-content: center;
            color: #60a5fa;
            box-shadow: 0 0 20px rgba(59, 130, 246, 0.15);
        }

        .badge-icon svg {
            width: 24px;
            height: 24px;
            stroke-width: 2;
        }

        .login-box h1 {
            font-size: 20px;
            font-weight: 600;
            color: var(--text-main);
            letter-spacing: -0.01em;
            margin-bottom: 6px;
            text-align: center;
        }

        .login-box p {
            font-size: 13.5px;
            color: var(--text-muted);
            text-align: center;
            margin-bottom: 28px;
        }

        /* 输入框样式 */
        .input-group {
            margin-bottom: 18px;
        }

        .login-box input[type="password"] {
            width: 100%;
            padding: 13px 16px;
            background: var(--input-bg);
            color: var(--text-main);
            border: 1px solid var(--input-border);
            border-radius: 10px;
            font-size: 14.5px;
            outline: none;
            transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
        }

        .login-box input[type="password"]::placeholder {
            color: #475569;
        }

        .login-box input[type="password"]:focus {
            border-color: var(--primary);
            background: rgba(15, 23, 42, 0.8);
            box-shadow: 0 0 0 3px var(--primary-glow);
        }

        /* 登录按钮 */
        .login-box button {
            width: 100%;
            padding: 13px;
            background: var(--primary);
            color: #ffffff;
            border: none;
            border-radius: 10px;
            font-size: 14.5px;
            font-weight: 500;
            letter-spacing: 0.05em;
            cursor: pointer;
            transition: all 0.2s ease;
            box-shadow: 0 4px 14px var(--primary-glow);
        }

        .login-box button:hover {
            background: var(--primary-hover);
            box-shadow: 0 6px 20px rgba(37, 99, 235, 0.45);
            transform: translateY(-1px);
        }

        .login-box button:active {
            transform: translateY(0);
        }

        /* 错误提示框 */
        .error {
            display: flex;
            align-items: center;
            justify-content: center;
            background: rgba(239, 68, 68, 0.1);
            border: 1px solid rgba(239, 68, 68, 0.2);
            color: #f87171;
            font-size: 13px;
            padding: 9px 12px;
            border-radius: 8px;
            margin-top: 16px;
            animation: shake 0.3s ease-in-out;
        }

        @keyframes shake {
            0%, 100% { transform: translateX(0); }
            25% { transform: translateX(-4px); }
            75% { transform: translateX(4px); }
        }
    </style>
</head>
<body>
    <div class="login-box">
        <div class="badge-icon">
            <!-- 矢量锁图标代替 Emoji -->
            <svg fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <rect x="3" y="11" width="18" height="11" rx="2" ry="2"></rect>
                <path d="M7 11V7a5 5 0 0 1 10 0v4"></path>
            </svg>
        </div>
        <h1>MT</h1>
        <p>输入管理密码以继续访问控制台</p>
        <form method="POST" action="/login">
            <div class="input-group">
                <input type="password" name="password" placeholder="请输入访问密钥" autofocus autocomplete="current-password" required>
            </div>
            <button type="submit">进入控制台</button>
        </form>
        {{error}}
    </div>
</body>
</html>"""

@app.get("/login", response_class=HTMLResponse)
async def login_page(request: Request):
    """登录页面"""
    if not get_admin_password():
        return HTMLResponse(
            content="ADMIN_PASSWORD is not configured. Set it before starting the server.",
            status_code=503,
        )
    if check_web_auth(request):
        return RedirectResponse(url="/admin", status_code=302)
    error_msg = request.query_params.get("error")
    error_html = f'<div class="error">❌ 密码错误</div>' if error_msg else ""
    return HTMLResponse(content=LOGIN_HTML.replace("{{error}}", error_html))

@app.post("/login")
async def login_submit(request: Request):
    """处理登录表单"""
    form = await request.form()
    password = form.get("password", "")
    correct = get_admin_password()
    if correct and hmac.compare_digest(str(password), str(correct)):
        token = _sign_session(correct)
        resp = RedirectResponse(url="/admin", status_code=302)
        resp.set_cookie(
            COOKIE_NAME, token,
            httponly=True, samesite="lax", secure=request.url.scheme == "https",
            max_age=86400
        )
        return resp
    return RedirectResponse(url="/login?error=1", status_code=302)

@app.get("/logout")
async def logout():
    """退出登录"""
    resp = RedirectResponse(url="/login", status_code=302)
    resp.delete_cookie(COOKIE_NAME)
    return resp

@app.get("/", response_class=HTMLResponse)
async def index(request: Request):
    """Dashboard 主页"""
    if not check_web_auth(request):
        return RedirectResponse(url="/login", status_code=302)
    dashboard_path = os.path.join(os.path.dirname(__file__), "static", "dashboard.html")
    if os.path.exists(dashboard_path):
        with open(dashboard_path, "r", encoding="utf-8") as f:
            return HTMLResponse(content=f.read())
    return HTMLResponse(content="<h1>Dashboard not found</h1>")

@app.get("/admin", response_class=HTMLResponse)
async def admin_page(request: Request):
    """截图管理后台"""
    if not check_web_auth(request):
        return RedirectResponse(url="/login", status_code=302)
    admin_path = os.path.join(os.path.dirname(__file__), "static", "admin.html")
    if os.path.exists(admin_path):
        with open(admin_path, "r", encoding="utf-8") as f:
            return HTMLResponse(content=f.read())
    return HTMLResponse(content="<h1>Admin not found</h1>")

# 挂载静态文件
static_dir = os.path.join(os.path.dirname(__file__), "static")
if os.path.exists(static_dir):
    app.mount("/static", StaticFiles(directory=static_dir), name="static")

# ============================================================================
# 主入口
# ============================================================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
