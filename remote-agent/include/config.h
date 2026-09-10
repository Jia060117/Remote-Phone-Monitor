#pragma once
#include <string>
#include <cstdint>

struct ServerConfig {
    // 服务端地址（http:// 或 https://），作为备选地址。
    std::string url;
    // 首选服务端地址（建议 https://），两者都填时优先使用 urls。
    // libcurl 内置 mbedtls TLS 后端，原生支持 https://，无需系统库。
    std::string urls;
    // 与服务端 tokens.json 中的 key 对应，不应提交真实值。
    std::string token;
    std::string upload_path = "/api/upload";
    int timeout_ms = 10000;
    // 配置心跳轮询间隔（秒）：无上传发生时按此间隔主动拉取服务端配置，
    // 0 表示关闭轮询（完全依赖上传响应捎带）。可由服务端远程修改。
    int config_poll_sec = 300;
};

struct CaptureConfig {
    std::string backend = "screencap";
    int interval_sec = 5;
    std::string format = "jpeg";
    int quality = 60;
    int width = 1080;
    int height = 2400;

    // 实时查看模式：服务端下发 true 时采集间隔强制为 1 秒，
    // 关闭后自动恢复原 interval_sec。用于浏览器端实时画面。
    bool live_mode = false;

    // 锁屏（息屏）自动暂停采集：屏幕熄灭时跳过采集/编码/上传以省电，
    // 亮屏后自动恢复。检测方式为 dumpsys power 的 mWakefulness。
    bool pause_on_screen_off = true;

    // 移动数据（蜂窝）低画质档：检测到默认路由不走 wlan/eth 时，
    // 自动使用更低分辨率/质量的一档，节省流量。
    bool cellular_enabled = true;
    int  cellular_width   = 480;
    int  cellular_height  = 1066;
    int  cellular_quality = 20;

    // 远程总开关：由服务端下发，true 时跳过整个采集链路（省电省流），
    // 但仍会补传离线队列。优先级高于定时策略。
    bool paused = false;
};

struct ChangeDetectionConfig {
    bool enabled = true;
    std::string algorithm = "ahash";
    double threshold = 0.92;
};

struct UploadConfig {
    int retry = 3;
    int retry_delay_sec = 5;
    bool offline_queue = true;
    std::string queue_dir;
    int max_queue_mb = 200;
    // 网络恢复后每个采集周期补传的队列张数（>1 可加速追平积压）
    int backlog_batch = 5;
    // 远程总开关：false 时不发起上传，帧仍入离线队列，等重新开启后补传（不丢数据）
    bool enabled = true;
    // 仅 Wi-Fi 上传：蜂窝网络下不上传，帧入离线队列，回 Wi-Fi 后按
    // backlog_batch 缓慢补传。与蜂窝低画质档同时开启时本项优先。
    bool wifi_only = false;
};

struct TurnConfig {
    std::string url;
    std::string username;
    std::string credential;
};

struct WebRTCConfig {
    bool enabled = false;
    std::string signaling_url;
    TurnConfig turn;
};

struct LoggingConfig {
    std::string level = "info";
    std::string path = "logs/agent.log";
};

struct Config {
    std::string device_id;
    ServerConfig server;
    CaptureConfig capture;
    ChangeDetectionConfig change_detection;
    UploadConfig upload;
    WebRTCConfig webrtc;
    LoggingConfig logging;

    // 当前生效的服务端配置版本号（运行时状态，不来自 JSON）。
    // 每次上传随 X-Config-Version 上报，服务端据此判断是否需要下发新配置。
    int64_t active_version = 0;

    static Config load(const std::string& path);

    // 把一段 JSON 合并进 cfg：只覆盖 JSON 中实际出现的字段，其余保持不变。
    // 用于服务端下发配置的热更新（复用 load() 同一套解析器）。
    // 解析失败返回 false 且 cfg 不被修改，调用方可安全忽略。
    static bool merge_json(Config& cfg, const std::string& json);

    // 把数值字段钳制到合法范围，防止服务端下发异常值（如 interval=0）导致死循环。
    // 服务端同样会做一次钳制，这里是 agent 侧的二次防御。
    static void clamp(Config& cfg);
};
