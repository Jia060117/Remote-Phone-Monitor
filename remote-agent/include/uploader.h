#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include "config.h"

// 前置声明，避免在头文件 include curl.h
typedef void CURL;

class Uploader {
public:
    Uploader(const std::string& device_id, const ServerConfig& server_cfg, const UploadConfig& upload_cfg);
    ~Uploader();

    bool upload(const std::vector<uint8_t>& image,
                int64_t capture_ts = 0,
                int64_t config_version = 0,
                std::string* response_body = nullptr);

    bool fetch_config(int64_t current_version, std::string& out_json);

    bool online() const;
    std::string effective_url() const;

private:
    bool perform_upload(const std::vector<uint8_t>& image,
                        int64_t capture_ts,
                        int64_t config_version,
                        std::string* response_body);

    int http_get(const std::string& path, int64_t config_version, std::string& out_body);

    std::string pick_base_url() const;
    CURL* get_curl();         // 懒初始化，复用 handle
    void setup_ssl(CURL* c);  // SSL 配置（只做一次）

    std::string device_id_;
    std::string url_;
    std::string urls_;
    std::string token_;
    std::string upload_path_;
    int timeout_ms_;
    int retry_;
    int retry_delay_sec_;
    bool online_ = true;

    // CURL 复用
    CURL* curl_ = nullptr;
    bool ssl_configured_ = false;

    // 熔断器
    int consec_fail_ = 0;
    std::chrono::steady_clock::time_point cooldown_until_{};
};
