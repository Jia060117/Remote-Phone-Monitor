#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include "capture.h"
#include "config.h"

class Encoder {
public:
    explicit Encoder(const CaptureConfig& cfg);

    // 将 Frame 编码为 JPEG/WebP 字节流
    std::optional<std::vector<uint8_t>> encode(const Frame& frame);

private:
    std::string format_;
    int quality_;
};
