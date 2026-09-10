#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include "config.h"

class ChangeDetector {
public:
    explicit ChangeDetector(const ChangeDetectionConfig& cfg);

    // 返回 true 表示"与上一帧差异足够大，需要上传"
    // precomputed_hash: 若提供（捕获阶段算好的 aHash），则直接复用，跳过解码
    bool should_upload(const std::vector<uint8_t>& jpeg,
                       std::optional<uint64_t> precomputed_hash = std::nullopt);

    // 热更新配置（服务端远程下发配置时调用）。
    // 阈值提高会立即减少上传量，无需重启。
    void apply_config(const ChangeDetectionConfig& cfg);

private:
    bool enabled_;
    std::string algorithm_;
    double threshold_;
    std::optional<uint64_t> last_hash_;

    // 与上一帧比较哈希，更新 last_hash_ 并返回是否应上传
    bool compare_and_decide(uint64_t h_cur);

    // 将 JPEG 解码为 8x8 灰度图
    std::vector<uint8_t> to_gray_8x8(const std::vector<uint8_t>& jpeg);

    // 计算 ahash
    uint64_t ahash(const std::vector<uint8_t>& gray8x8);

    // popcount
    int popcount(uint64_t x);
};
