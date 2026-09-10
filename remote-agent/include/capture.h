#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include "config.h"

struct Frame {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // RGBA8888，行优先

    // 预编码图像（如 screencap 直接输出的 PNG）。
    // 非空时主循环跳过 encoder，直接进入上传链路。
    std::vector<uint8_t> encoded;
    bool is_png = false;

    // 捕获阶段顺带算好的 8x8 感知哈希（aHash），供变化检测直接复用，
    // 避免对编码后的图二次解码。空表示未计算。
    std::optional<uint64_t> hash;
};

class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;

    // 建立长期采集会话（只在启动时调用一次）
    virtual bool start() = 0;

    // 在已建立的会话上取一帧（主循环每次调用）
    virtual std::optional<Frame> capture() = 0;

    // 释放会话
    virtual void stop() = 0;

    // 热更新配置（服务端远程下发配置时调用）。
    // 只更新可在运行中安全切换的字段（目标尺寸、JPEG 质量、蜂窝档参数）。
    // 结构性字段（如 backend）变更后需重启进程，默认实现为空操作。
    virtual void apply_config(const CaptureConfig& cfg) { (void)cfg; }
};

// 工厂：根据配置选择实现
CaptureBackend* create_capture_backend(const Config& cfg);
