#include "config.h"
#include "logger.h"
#include <fstream>
#include <stdexcept>
#include <utility>

// 简易 JSON 解析（不依赖外部库）
// 生产环境建议使用 nlohmann/json 或 rapidjson
namespace {

// 跳过空白
static void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t'))
        i++;
}

// 解析字符串值（带引号）
static std::string parse_string(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"') throw std::runtime_error("Expected '\"'");
    i++; // skip opening quote
    std::string result;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') {
            i++;
            if (i < s.size()) {
                switch (s[i]) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case '\\': result += '\\'; break;
                    case '"': result += '"'; break;
                    default: result += s[i]; break;
                }
            }
        } else {
            result += s[i];
        }
        i++;
    }
    if (i >= s.size()) throw std::runtime_error("Unterminated string");
    i++; // skip closing quote
    return result;
}

// 解析数字
static double parse_number(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && s[i] == '-') i++;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-'))
        i++;
    return std::stod(s.substr(start, i - start));
}

// 解析布尔值
static bool parse_bool(const std::string& s, size_t& i) {
    if (s.compare(i, 4, "true") == 0) { i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { i += 5; return false; }
    throw std::runtime_error("Expected boolean");
}

// 前向声明
static void parse_value(const std::string& s, size_t& i, Config& cfg, const std::string& full_key);

// 解析对象
static void parse_object(const std::string& s, size_t& i, Config& cfg, const std::string& parent_key) {
    if (i >= s.size() || s[i] != '{') throw std::runtime_error("Expected '{'");
    i++; // skip '{'
    skip_ws(s, i);

    while (i < s.size() && s[i] != '}') {
        skip_ws(s, i);
        std::string key = parse_string(s, i);
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') throw std::runtime_error("Expected ':'");
        i++; // skip ':'
        skip_ws(s, i);

        std::string full_key = parent_key.empty() ? key : parent_key + "." + key;
        parse_value(s, i, cfg, full_key);

        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            i++; // skip ','
            skip_ws(s, i);
        }
    }

    if (i >= s.size() || s[i] != '}') throw std::runtime_error("Expected '}'");
    i++; // skip '}'
}

// 解析值
static void parse_value(const std::string& s, size_t& i, Config& cfg, const std::string& full_key) {
    skip_ws(s, i);
    if (i >= s.size()) return;

    char c = s[i];

    if (c == '"') {
        std::string val = parse_string(s, i);
        // 根据 key 赋值
        if (full_key == "device_id") cfg.device_id = val;
        else if (full_key == "server.url") cfg.server.url = val;
        else if (full_key == "server.urls") cfg.server.urls = val;
        else if (full_key == "server.token") cfg.server.token = val;
        else if (full_key == "server.upload_path") cfg.server.upload_path = val;
        else if (full_key == "capture.backend") cfg.capture.backend = val;
        else if (full_key == "capture.format") cfg.capture.format = val;
        else if (full_key == "change_detection.algorithm") cfg.change_detection.algorithm = val;
        else if (full_key == "upload.queue_dir") cfg.upload.queue_dir = val;
        else if (full_key == "webrtc.signaling_url") cfg.webrtc.signaling_url = val;
        else if (full_key == "webrtc.turn.url") cfg.webrtc.turn.url = val;
        else if (full_key == "webrtc.turn.username") cfg.webrtc.turn.username = val;
        else if (full_key == "webrtc.turn.credential") cfg.webrtc.turn.credential = val;
        else if (full_key == "logging.level") cfg.logging.level = val;
        else if (full_key == "logging.path") cfg.logging.path = val;
    } else if (c == '{') {
        parse_object(s, i, cfg, full_key);
    } else if (c == 't' || c == 'f') {
        bool val = parse_bool(s, i);
        if (full_key == "change_detection.enabled") cfg.change_detection.enabled = val;
        else if (full_key == "upload.offline_queue") cfg.upload.offline_queue = val;
        else if (full_key == "upload.enabled") cfg.upload.enabled = val;
        else if (full_key == "upload.wifi_only") cfg.upload.wifi_only = val;
        else if (full_key == "capture.paused") cfg.capture.paused = val;
        else if (full_key == "webrtc.enabled") cfg.webrtc.enabled = val;
        else if (full_key == "capture.pause_on_screen_off") cfg.capture.pause_on_screen_off = val;
        else if (full_key == "capture.cellular_enabled") cfg.capture.cellular_enabled = val;
        else if (full_key == "capture.live_mode") cfg.capture.live_mode = val;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        double val = parse_number(s, i);
        // 判断是整数还是浮点
        if (full_key == "server.timeout_ms") cfg.server.timeout_ms = static_cast<int>(val);
        else if (full_key == "capture.interval_sec") cfg.capture.interval_sec = static_cast<int>(val);
        else if (full_key == "capture.quality") cfg.capture.quality = static_cast<int>(val);
        else if (full_key == "capture.jpeg_quality") cfg.capture.quality = static_cast<int>(val);
        else if (full_key == "capture.width") cfg.capture.width = static_cast<int>(val);
        else if (full_key == "capture.height") cfg.capture.height = static_cast<int>(val);
        else if (full_key == "capture.cellular_width") cfg.capture.cellular_width = static_cast<int>(val);
        else if (full_key == "capture.cellular_height") cfg.capture.cellular_height = static_cast<int>(val);
        else if (full_key == "capture.cellular_quality") cfg.capture.cellular_quality = static_cast<int>(val);
        else if (full_key == "change_detection.threshold") cfg.change_detection.threshold = val;
        else if (full_key == "upload.retry") cfg.upload.retry = static_cast<int>(val);
        else if (full_key == "upload.retry_delay_sec") cfg.upload.retry_delay_sec = static_cast<int>(val);
        else if (full_key == "upload.max_queue_mb") cfg.upload.max_queue_mb = static_cast<int>(val);
        else if (full_key == "upload.backlog_batch") cfg.upload.backlog_batch = static_cast<int>(val);
        else if (full_key == "server.config_poll_sec") cfg.server.config_poll_sec = static_cast<int>(val);
    } else if (c == 'n') {
        // null, skip
        i += 4;
    }
}

} // anonymous namespace

Config Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    Config cfg;
    size_t i = 0;
    skip_ws(content, i);
    parse_object(content, i, cfg, "");

    // 验证必填字段
    if (cfg.device_id.empty()) {
        throw std::runtime_error("device_id is required");
    }
    // url 与 urls 至少填一个；两者都填时优先用 urls（https）。
    if (cfg.server.url.empty() && cfg.server.urls.empty()) {
        throw std::runtime_error("server.url or server.urls is required");
    }
    if (cfg.server.token.empty()) {
        throw std::runtime_error("server.token is required");
    }

    clamp(cfg);
    return cfg;
}

bool Config::merge_json(Config& cfg, const std::string& json) {
    if (json.empty()) return false;

    Config parsed = cfg;   // 在副本上解析，失败时不影响原配置
    try {
        size_t i = 0;
        skip_ws(json, i);
        parse_object(json, i, parsed, "");
    } catch (const std::exception& e) {
        LOGW("Config: ignoring invalid server config (%s)", e.what());
        return false;
    }

    parsed.active_version = cfg.active_version;  // 版本号由调用方单独维护
    clamp(parsed);
    cfg = std::move(parsed);
    return true;
}

void Config::clamp(Config& cfg) {
    // 采集间隔：最小 1 秒（防止 0 导致忙循环），最大 1 小时
    if (cfg.capture.interval_sec < 1) cfg.capture.interval_sec = 1;
    if (cfg.capture.interval_sec > 3600) cfg.capture.interval_sec = 3600;

    // 心跳轮询：0 表示关闭，最大 1 小时
    if (cfg.server.config_poll_sec < 0) cfg.server.config_poll_sec = 0;
    if (cfg.server.config_poll_sec > 3600) cfg.server.config_poll_sec = 3600;

    // 画质与分辨率
    if (cfg.capture.quality < 1) cfg.capture.quality = 1;
    if (cfg.capture.quality > 100) cfg.capture.quality = 100;
    if (cfg.capture.width  < 64)  cfg.capture.width  = 64;
    if (cfg.capture.width  > 4096) cfg.capture.width  = 4096;
    if (cfg.capture.height < 64)  cfg.capture.height = 64;
    if (cfg.capture.height > 4096) cfg.capture.height = 4096;

    // 蜂窝档
    if (cfg.capture.cellular_quality < 1) cfg.capture.cellular_quality = 1;
    if (cfg.capture.cellular_quality > 100) cfg.capture.cellular_quality = 100;
    if (cfg.capture.cellular_width  < 64)  cfg.capture.cellular_width  = 64;
    if (cfg.capture.cellular_width  > 4096) cfg.capture.cellular_width  = 4096;
    if (cfg.capture.cellular_height < 64)  cfg.capture.cellular_height = 64;
    if (cfg.capture.cellular_height > 4096) cfg.capture.cellular_height = 4096;

    // 变化检测阈值：0 表示每帧都传，1 表示几乎不传
    if (cfg.change_detection.threshold < 0.0) cfg.change_detection.threshold = 0.0;
    if (cfg.change_detection.threshold > 1.0) cfg.change_detection.threshold = 1.0;

    // 上传重试与批量补传
    if (cfg.upload.retry < 0) cfg.upload.retry = 0;
    if (cfg.upload.retry > 5) cfg.upload.retry = 5;
    if (cfg.upload.retry_delay_sec < 0) cfg.upload.retry_delay_sec = 0;
    if (cfg.upload.retry_delay_sec > 30) cfg.upload.retry_delay_sec = 30;
    if (cfg.upload.backlog_batch < 1) cfg.upload.backlog_batch = 1;
    if (cfg.upload.backlog_batch > 50) cfg.upload.backlog_batch = 50;
    if (cfg.upload.max_queue_mb < 1) cfg.upload.max_queue_mb = 1;
    if (cfg.upload.max_queue_mb > 1024) cfg.upload.max_queue_mb = 1024;
    if (cfg.server.timeout_ms < 1000) cfg.server.timeout_ms = 1000;
    if (cfg.server.timeout_ms > 60000) cfg.server.timeout_ms = 60000;
}
