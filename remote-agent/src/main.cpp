#include "config.h"
#include "capture.h"
#include "scheduler.h"
#include "encoder.h"
#include "uploader.h"
#include "queue.h"
#include "changer.h"
#include "netutil.h"
#include "logger.h"
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

static std::atomic<bool> running{true};

// ============================================================================
// 屏幕点亮状态检测（用于锁屏自动暂停采集）
// ============================================================================
// 优先读 sysfs 背光亮度（零 fork、零解析，微秒级）；
// 节点不可读时兜底 dumpsys power（百毫秒级，fork+全量序列化）。
// 解析失败时按亮屏处理（fail-open），避免监控被误停。
static bool screen_is_on() {
    // 优先读背光亮度：零 fork、零解析，绝大多数 ROM 可用
    static const char* const kPaths[] = {
        "/sys/class/leds/lcd-backlight/brightness",
        "/sys/class/backlight/panel0-backlight/brightness",
        "/sys/class/backlight/panel0-backlight/bl_brightness",
    };
    for (const char* p : kPaths) {
        if (FILE* fp = std::fopen(p, "rb")) {
            int v = 0;
            const bool ok = std::fscanf(fp, "%d", &v) == 1;
            std::fclose(fp);
            if (ok) return v > 0;  // 亮度 > 0 即亮屏
        }
    }

    // 兜底：dumpsys power（加 3 秒超时，防止 system_server 繁忙时卡死）
    // 首次走兜底路径时打日志，方便排查
    static bool fallback_warned = false;
    if (!fallback_warned) {
        LOGW("screen_is_on: sysfs backlight not readable, falling back to dumpsys");
        fallback_warned = true;
    }
    std::string out;
    if (FILE* fp = popen("/system/bin/timeout -s KILL 3 /system/bin/dumpsys power 2>/dev/null", "r")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
            out.append(buf, buf + n);
        pclose(fp);
    } else {
        return true;
    }

    size_t pos = out.find("mWakefulness=");
    if (pos != std::string::npos) {
        // 提取完整值：Awake / Asleep / Dozing / Dreaming
        size_t eol = out.find_first_of("\r\n", pos);
        std::string wake = (eol == std::string::npos)
            ? out.substr(pos + 13)
            : out.substr(pos + 13, eol - pos - 13);
        // Dreaming（屏保/AOD）屏幕实际亮着，按亮屏处理
        return wake.rfind("Awake", 0) == 0 || wake.rfind("Dreaming", 0) == 0;
    }
    pos = out.find("Display Power: state=");
    if (pos != std::string::npos) {
        return out.compare(pos + 22, 2, "ON") == 0;
    }
    return true;  // 解析失败，按亮屏处理
}

// ============================================================================
// 服务端下发配置的接收与热应用
// ============================================================================
// 配置同步有两条通道：
//   1) 上传响应捎带（主通道）：每次上传随请求头上报本地版本号，服务端版本
//      不一致时在响应体附一份完整配置，零额外请求。
//   2) 心跳轮询（兜底）：长时间无上传时按 server.config_poll_sec 间隔主动
//      GET /api/config，命中 304 时代价约 100 字节。
// 两条通道最终都走 apply_remote_config()，但响应字段名不同，见该函数注释。
//
// 兜底通道什么时候真正生效，取决于 last_config_check 是否推进：
// 只有「服务端确实确认过我们的版本号」才算一次有效校验，才刷新这个时间戳。
// 如果主通道因为字段名对不上/响应解析失败而一直拿不到版本，时间戳就不会推进，
// 心跳照常触发、设备自行恢复，而不是永远卡在启动时的那份配置上。
//
// 注意：主循环是单线程的，配置只在「上传之后」这一个时机被拉取和应用，
// 因此新间隔从下一个采样点起生效，最长延迟 = 一个旧周期。

// 极简 JSON 取值：只需从响应体根对象取 version / config_version / config，
// 不引入完整 JSON 库。
static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
}

// 定位顶层 key 对应值的起始下标，未找到返回 false。
//
// 必须只匹配根对象里的 key（depth == 1）：响应体里的 config 对象内部可能也有
// 同名字段，用朴素 find() 会误命中嵌套值。字符串值内部的同名字样同样要跳过，
// 否则 {"policy":"含 version 的策略"} 会被当成 version 字段。
static bool json_find_value(const std::string& s, const std::string& key, size_t& out) {
    const std::string needle = "\"" + key + "\"";
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (in_str) {
            if (c == '\\' && i + 1 < s.size()) i++;   // 跳过转义字符
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            // 先判断是不是要找的顶层 key（key 本身也是字符串），不是则当作
            // 字符串值整段跳过
            if (depth == 1 && s.compare(i, needle.size(), needle) == 0) {
                size_t j = i + needle.size();
                json_skip_ws(s, j);
                if (j < s.size() && s[j] == ':') {
                    j++;
                    json_skip_ws(s, j);
                    out = j;
                    return true;
                }
            }
            in_str = true;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return false;
}

static bool json_get_int(const std::string& s, const std::string& key, int64_t& out) {
    size_t i = 0;
    if (!json_find_value(s, key, i)) return false;
    const char* begin = s.c_str() + i;
    char* end = nullptr;
    const long long v = std::strtoll(begin, &end, 10);
    if (end == begin) return false;
    out = static_cast<int64_t>(v);
    return true;
}

// 提取 key 对应的对象原文（含外层花括号），整体交给配置解析器处理
static bool json_get_object(const std::string& s, const std::string& key, std::string& out) {
    size_t i = 0;
    if (!json_find_value(s, key, i)) return false;
    if (i >= s.size() || s[i] != '{') return false;

    int depth = 0;
    bool in_str = false, esc = false;
    const size_t start = i;
    for (; i < s.size(); i++) {
        const char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) {
                out = s.substr(start, i - start + 1);
                return true;
            }
        }
    }
    return false;
}

// 响应体里是否带版本字段（config_version 或 version）。
//
// 两个通道的字段名不同，缺一不可：
//   上传响应  {"ok":true, ..., "config_version":N, "config":{...}}   <- 主通道
//   心跳响应  {"version":N, "config":{...}}                          <- 兜底通道
// 历史上这里只认 "version"，而上传响应里的字段叫 "config_version"，
// 结果主通道每次都静默失败、设备永远停在启动时的那份配置上（详见 BUILD.md 故障案例）。
// 现在两个名字都接受，并且对外暴露这个判断用于"配置校验成功"的判定。
static bool json_has_version(const std::string& payload) {
    int64_t v = 0;
    return json_get_int(payload, "config_version", v) ||
           json_get_int(payload, "version", v);
}

// 检查响应中是否有 "live_mode": true
static bool json_get_bool(const std::string& s, const std::string& key, bool& out) {
    size_t i = 0;
    if (!json_find_value(s, key, i)) return false;
    if (i + 4 <= s.size() && s.compare(i, 4, "true") == 0) { out = true; return true; }
    if (i + 5 <= s.size() && s.compare(i, 5, "false") == 0) { out = false; return true; }
    return false;
}

// 应用服务端下发的配置：合并进 cfg 并推送给各模块。
// 返回是否成功应用。返回 false 不一定代表出错——
// 版本一致时服务端不附带 config，也属于正常情况，由调用方用
// json_has_version() 区分"没问题"和"响应看不懂"。
static bool apply_remote_config(Config& cfg, const std::string& payload,
                                Scheduler* scheduler, CaptureBackend* cap,
                                ChangeDetector* changer) {
    int64_t ver = 0;
    std::string cfg_json;
    // 两个字段名都要认：上传响应叫 config_version，心跳响应叫 version。
    // 只认其中一个会让另一条通道永久静默失效（踩过，见 BUILD.md 故障案例）。
    if (!json_get_int(payload, "config_version", ver) &&
        !json_get_int(payload, "version", ver)) {
        LOGW("Config: response has no version field (len=%zu), ignored", payload.size());
        return false;
    }
    // 版本一致时服务端不附带 config，静默跳过即可
    if (!json_get_object(payload, "config", cfg_json)) return false;

    Config next = cfg;
    if (!Config::merge_json(next, cfg_json)) return false;   // 解析失败，cfg 保持不变
    next.active_version = ver;

    // 结构性字段无法热应用，记录提示（仍需重启进程）
    const bool need_restart =
        next.capture.backend     != cfg.capture.backend   ||
        next.upload.queue_dir    != cfg.upload.queue_dir  ||
        next.server.url          != cfg.server.url;

    const int old_interval = cfg.capture.interval_sec;
    cfg = next;

    if (scheduler && cfg.capture.interval_sec != old_interval) {
        scheduler->set_interval(cfg.capture.interval_sec);
        LOGI("Config: capture interval -> %d sec", cfg.capture.interval_sec);
    }
    if (cap)     cap->apply_config(cfg.capture);
    if (changer) changer->apply_config(cfg.change_detection);

    LOGI("Config: server config v%lld applied%s",
         static_cast<long long>(ver),
         cfg.capture.paused ? " (capture paused remotely)" : "");
    if (need_restart) {
        LOGW("Config: structural change (backend/queue_dir/server) requires agent restart");
    }
    return true;
}

// 主动向服务端拉取配置（心跳通道）。
// 返回是否完成了一次有效校验：通信失败算失败；304（版本一致）算成功；
// 200 且能解析出版本字段也算成功。拿不到版本字段才算失败——
// 此时调用方不会刷新 last_config_check，下一轮会继续重试。
static bool sync_config_from_server(Config& cfg, Uploader& uploader,
                                    Scheduler* scheduler, CaptureBackend* cap,
                                    ChangeDetector* changer) {
    std::string payload;
    if (!uploader.fetch_config(cfg.active_version, payload)) return false;  // 通信失败
    if (payload.empty()) return true;                    // 304：版本一致，配置未变
    if (!json_has_version(payload)) {
        LOGW("Config: heartbeat response has no version field (len=%zu)", payload.size());
        return false;
    }
    // 应用与否都算校验成功：服务端没附带 config 只是说明版本已经是最新的
    apply_remote_config(cfg, payload, scheduler, cap, changer);
    return true;
}

// 判断当前是否允许发起上传：
// 总开关关闭，或开启了"仅 Wi-Fi 上传"且当前走蜂窝网络时，均不允许上传
// （帧仍会入离线队列，等条件满足后补传，不丢数据）。
static bool upload_allowed(const Config& cfg, bool cellular) {
    if (!cfg.upload.enabled) return false;
    if (cfg.upload.wifi_only && cellular) return false;
    return true;
}

static void signal_handler(int sig) {
    // 仅写原子变量，保持异步信号安全
    (void)sig;
    running = false;
}

static void install_signal_handlers() {
#if defined(_WIN32)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    // 使用 sigaction 且不带 SA_RESTART，
    // 保证 sleep_until 能被信号打断、进程能及时退出。
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 故意不设 SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s --config <path>\n", prog);
}

int main(int argc, char** argv) {
    // 解析命令行参数
    const char* config_path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (!config_path) {
        print_usage(argv[0]);
        return 1;
    }

    // 加载配置
    Config cfg;
    try {
        cfg = Config::load(config_path);
    } catch (const std::exception& e) {
        fprintf(stderr, "Failed to load config: %s\n", e.what());
        return 1;
    }

    // 初始化日志
    Logger::init(cfg.logging.path, cfg.logging.level);
    LOGI("remote-agent starting, device=%s", cfg.device_id.c_str());
    LOGI("Capture interval: %d sec, format: %s, quality: %d",
         cfg.capture.interval_sec, cfg.capture.format.c_str(), cfg.capture.quality);

    // 注册信号处理
    install_signal_handlers();

    // 初始化各模块
    Scheduler scheduler(cfg.capture.interval_sec);
    std::unique_ptr<CaptureBackend> cap(create_capture_backend(cfg));
    if (!cap) {
        LOGE("Unknown capture backend: %s", cfg.capture.backend.c_str());
        Logger::shutdown();
        return 1;
    }
    Encoder encoder(cfg.capture);
    Uploader uploader(cfg.device_id, cfg.server, cfg.upload);
    OfflineQueue queue(cfg.upload.queue_dir, cfg.upload.max_queue_mb);
    ChangeDetector changer(cfg.change_detection);

    LOGI("Server: %s%s (urls=%s, url=%s)",
         uploader.effective_url().c_str(), cfg.server.upload_path.c_str(),
         cfg.server.urls.c_str(), cfg.server.url.c_str());

    // 启动采集后端
    if (!cap->start()) {
        LOGE("Capture backend start failed");
        Logger::shutdown();
        return 1;
    }

    LOGI("Capture backend started, entering main loop");
    if (cfg.capture.pause_on_screen_off) {
        LOGI("Screen-off pause: enabled (capture suspends while display is off)");
    }
    if (cfg.capture.cellular_enabled) {
        LOGI("Cellular low-quality tier: enabled (%dx%d q=%d)",
             cfg.capture.cellular_width, cfg.capture.cellular_height,
             cfg.capture.cellular_quality);
    }
    if (cfg.upload.wifi_only) {
        LOGI("Wi-Fi only upload: enabled (frames queue up on cellular)");
    }
    if (cfg.server.config_poll_sec > 0) {
        LOGI("Config heartbeat: every %d sec (server can change or disable it)",
             cfg.server.config_poll_sec);
    } else {
        LOGI("Config heartbeat: disabled (config syncs via upload responses only)");
    }

    // 启动时先拉一次服务端配置；失败则用本地 config.json 兜底，不阻塞启动
    if (sync_config_from_server(cfg, uploader, &scheduler, cap.get(), &changer)) {
        LOGI("Config: initial sync from server completed");
    } else {
        LOGW("Config: initial sync failed, running on local config (will retry)");
    }
    LOGI("Config: effective version=%lld, interval=%d sec, poll=%d sec",
         static_cast<long long>(cfg.active_version),
         cfg.capture.interval_sec, cfg.server.config_poll_sec);

    // 主循环
    bool screen_paused = false;

    // 配置心跳相关
    auto last_config_check = std::chrono::steady_clock::now();

    // 自看门狗：记录上次成功完成一轮的时刻
    auto last_alive = std::chrono::steady_clock::now();
    const auto watchdog_timeout = std::chrono::seconds(
        std::max(cfg.capture.interval_sec * 10, 300));  // 至少 300 秒

    while (running) {
    try {
        // 自看门狗：太久没完成一轮，主动 abort 让 service.sh 重启
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_alive).count();
            if (elapsed > watchdog_timeout.count()) {
                LOGE("Watchdog: no completed cycle for %lld sec, aborting",
                     static_cast<long long>(elapsed));
                std::abort();
            }
        }

        // 实时查看模式：强制 1 秒间隔，关闭时恢复原配置
        {
            static bool was_live = false;
            const bool is_live = cfg.capture.live_mode;
            if (is_live && !was_live) {
                scheduler.set_interval(1);
                LOGI("Live mode: ON (interval=1s)");
            } else if (!is_live && was_live) {
                scheduler.set_interval(cfg.capture.interval_sec);
                LOGI("Live mode: OFF (interval=%ds)", cfg.capture.interval_sec);
            }
            was_live = is_live;
        }

        scheduler.wait();                 // 阻塞到下一次采样点
        if (!running) break;

        // ---- 锁屏（息屏）自动暂停：跳过采集/编码/上传，降低 CPU/耗电 ----
        // 恢复由下一周期的检测自动完成；离线队列补传不受影响。
        if (!cfg.capture.pause_on_screen_off) {
            // 开关已关，清除残留状态（防止"息屏时远程关闭开关导致永久暂停"）
            screen_paused = false;
        } else {
            const bool on = screen_is_on();
            if (!on && !screen_paused) {
                screen_paused = true;
                LOGI("Screen off: capture paused (waiting for unlock)");
            } else if (on && screen_paused) {
                screen_paused = false;
                LOGI("Screen on: capture resumed");
            }
        }

        if (!screen_paused && !cfg.capture.paused) {
            // 拍摄时刻（epoch 毫秒）：实时上传与离线入队共用，
            // 确保断网补传后服务端能还原到真实拍摄时间。
            const int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto frame = cap->capture();      // 取一帧
            if (!frame) {
                LOGW("Capture returned empty frame");
                continue;
            }

            // 预编码帧（screencap 输出的 PNG）直接进上传链路，跳过 encoder
            std::optional<std::vector<uint8_t>> image;
            if (!frame->encoded.empty()) {
                image = std::move(frame->encoded);  // move，避免 460KB 拷贝
            } else {
                image = encoder.encode(*frame);   // 压缩为 JPEG/WebP
            }
            if (!image) {
                LOGW("Encode failed");
                continue;
            }

            if (changer.should_upload(*image, frame->hash)) {   // 变化检测（复用捕获阶段哈希）
                const bool cellular = net_is_cellular();
                if (!upload_allowed(cfg, cellular)) {
                    // 远程关闭了上传 / 蜂窝下仅 Wi-Fi 上传：不发包，但入离线队列，
                    // 等开关恢复或回到 Wi-Fi 后由补传逻辑慢慢传完（不丢数据）
                    if (cfg.upload.offline_queue) {
                        queue.save(*image, ts_ms);
                        LOGD("Upload held back (%s), queued (size=%zu)",
                             cfg.upload.enabled ? "cellular network" : "upload disabled",
                             queue.size());
                    }
                } else {
                    // 随请求上报本地配置版本；服务端版本不一致时会在响应体附带新配置
                    std::string resp;
                    if (!uploader.upload(*image, ts_ms, cfg.active_version, &resp)) {
                        if (cfg.upload.offline_queue) {
                            queue.save(*image, ts_ms);   // 离线入队（带拍摄时刻）
                            LOGW("Upload failed, saved to offline queue (size=%zu)", queue.size());
                        }
                    } else {
                        LOGD("Upload ok, size=%zu KB", image->size() / 1024);
                        // 服务端可能捎带了新配置；响应里没有 config 字段时静默跳过
                        if (resp.empty()) {
                            LOGW("Config: upload response empty, config not verified");
                        } else if (json_has_version(resp)) {
                            apply_remote_config(cfg, resp, &scheduler, cap.get(), &changer);
                            last_config_check = std::chrono::steady_clock::now();
                        } else {
                            LOGW("Config: upload response has no version field (len=%zu), "
                                 "will fall back to heartbeat", resp.size());
                        }
                        // 实时查看指令：上传响应捎带，即时生效
                        {
                            bool live = false;
                            if (json_get_bool(resp, "live_mode", live) && live != cfg.capture.live_mode) {
                                cfg.capture.live_mode = live;
                                scheduler.set_interval(live ? 1 : cfg.capture.interval_sec);
                                LOGI("Live mode: %s", live ? "ON" : "OFF");
                            }
                        }
                    }
                }
            } else {
                LOGD("Frame unchanged, skipping upload");
            }
        }  // end !screen_paused && !paused

        // ---- 配置心跳：配置长时间没被校验过时主动拉取 ----
        // 触发条件是「距上次成功校验已超过 poll_sec」，而不是「距上次上传」——
        // 上传一直成功但响应读不出版本字段时，这个时间戳不推进，心跳就会顶上来。
        // 息屏时跳过（反正也不采集，亮屏后第一帧上传即会捎带新配置）。
        //
        // 安全兜底：远程暂停采集期间，即使心跳被设为 0（关闭）也强制保留
        // 最低 60 秒一次的轮询。否则"关闭心跳 + 暂停采集"组合会让设备既
        // 不上传、也不轮询，永远收不到恢复指令，只能靠 adb 手工救回。
        int poll_sec = cfg.server.config_poll_sec;
        if (cfg.capture.paused && poll_sec < 60) poll_sec = 60;

        if (poll_sec > 0 && !screen_paused) {
            const auto now = std::chrono::steady_clock::now();
            const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_config_check).count();
            if (idle >= poll_sec) {
                if (sync_config_from_server(cfg, uploader, &scheduler, cap.get(), &changer)) {
                    last_config_check = now;
                } else {
                    // 失败时 30 秒后重试，避免断网期间每个周期都做无效连接
                    last_config_check = now - std::chrono::seconds(poll_sec)
                                            + std::chrono::seconds(30);
                }
            }
        }

        // 网络恢复后，补传离线队列（每周期最多 backlog_batch 张，加速追平积压）
        // 同样受上传开关与"仅 Wi-Fi"约束：蜂窝下不会消耗流量补传。
        if (upload_allowed(cfg, net_is_cellular()) && uploader.online() && queue.size() > 0) {
            const int batch = cfg.upload.backlog_batch > 0 ? cfg.upload.backlog_batch : 1;
            int uploaded = 0;
            std::string resp;
            for (int k = 0; k < batch && queue.size() > 0; k++) {
                auto pending = queue.pop();
                if (!pending) break;
                if (uploader.upload(pending->data, pending->ts, cfg.active_version, &resp)) {
                    queue.confirm(*pending);  // 上传成功，确认删除
                    uploaded++;
                    if (!resp.empty()) {
                        if (json_has_version(resp)) {
                            apply_remote_config(cfg, resp, &scheduler, cap.get(), &changer);
                            last_config_check = std::chrono::steady_clock::now();
                        }
                        resp.clear();
                    }
                } else {
                    // 上传失败，放回队列（保留原始时间戳），等下周期再试
                    queue.save(pending->data, pending->ts);
                    break;
                }
            }
            if (uploaded > 0) {
                LOGI("Offline queue: uploaded %d pending frame%s (%zu remaining)",
                     uploaded, uploaded > 1 ? "s" : "", queue.size());
            }
        }
        last_alive = std::chrono::steady_clock::now();  // 看门狗：本轮完成
    } catch (const std::exception& e) {
        LOGE("Main loop exception: %s, skipping this cycle", e.what());
    } catch (...) {
        LOGE("Main loop: unknown exception, skipping this cycle");
    }
    } // end while (running)

    LOGI("Shutting down...");
    cap->stop();
    Logger::shutdown();
    return 0;
}
