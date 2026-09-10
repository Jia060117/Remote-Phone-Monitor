#include "uploader.h"
#include "logger.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>
#include <sys/stat.h>

#if defined(_WIN32) && !defined(S_ISDIR)
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif

#include <curl/curl.h>

// 按魔数识别图片类型
static void image_mime(const std::vector<uint8_t>& img,
                       const char** mime, const char** filename) {
    static const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (img.size() >= 8 && std::memcmp(img.data(), png_magic, 8) == 0) {
        *mime = "image/png";
        *filename = "shot.png";
    } else {
        *mime = "image/jpeg";
        *filename = "shot.jpg";
    }
}

static size_t collect_response(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    std::string* out = static_cast<std::string*>(userdata);
    if (out && out->size() < 256 * 1024) out->append(static_cast<char*>(ptr), n);
    return n;
}

Uploader::Uploader(const std::string& device_id, const ServerConfig& server_cfg, const UploadConfig& upload_cfg)
    : device_id_(device_id)
    , url_(server_cfg.url)
    , urls_(server_cfg.urls)
    , token_(server_cfg.token)
    , upload_path_(server_cfg.upload_path)
    , timeout_ms_(server_cfg.timeout_ms)
    , retry_(upload_cfg.retry)
    , retry_delay_sec_(upload_cfg.retry_delay_sec) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    LOGI("Uploader: libcurl %s (mbedtls TLS) ready", curl_version());
}

Uploader::~Uploader() {
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
    curl_global_cleanup();
}

CURL* Uploader::get_curl() {
    if (!curl_) {
        curl_ = curl_easy_init();
        if (!curl_) return nullptr;

        // 一次性配置：TCP 保活、低速保护、信号屏蔽
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 60L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 30L);
        curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    }
    return curl_;
}

void Uploader::setup_ssl(CURL* c) {
    if (ssl_configured_) return;

    static const char* const kCACertDir  = "/system/etc/security/cacerts";
    static const char* const kCACertFile = "/system/etc/security/cacerts/ca-certificates.crt";

    struct stat st;
    if (stat(kCACertDir, &st) == 0 && S_ISDIR(st.st_mode)) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_CAPATH, kCACertDir);
        ssl_configured_ = true;  // 仅探测成功时置位
        return;
    }
    if (stat(kCACertFile, &st) == 0 && st.st_size > 0) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_CAINFO, kCACertFile);
        ssl_configured_ = true;
        return;
    }
    // 证书目录暂不可用时保持 libcurl 的严格默认值，绝不降级为不校验 TLS。
    // 本轮不置位，下个周期重试（可能只是开机早期 SELinux 尚未就绪）。
    LOGW("Uploader: CA certs not found — HTTPS will fail closed; retrying later");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
}

std::string Uploader::pick_base_url() const {
    if (!urls_.empty()) return urls_;
    return url_;
}

std::string Uploader::effective_url() const {
    return pick_base_url();
}

bool Uploader::upload(const std::vector<uint8_t>& image, int64_t capture_ts,
                      int64_t config_version, std::string* response_body) {
    if (std::chrono::steady_clock::now() < cooldown_until_) {
        return false;
    }

    const int attempts = retry_ > 0 ? retry_ : 1;

    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (perform_upload(image, capture_ts, config_version, response_body)) {
            consec_fail_ = 0;
            return true;
        }

        if (attempt < attempts) {
            const int delay = retry_delay_sec_ > 0 ? retry_delay_sec_ : 5;
            LOGW("Uploader: attempt %d/%d failed, retrying in %d sec...",
                 attempt, attempts, delay);
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        }
    }

    consec_fail_++;
    const int backoff = (std::min)(30 << (std::min)(consec_fail_ - 1, 4), 600);
    cooldown_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(backoff);
    LOGW("Uploader: %d consecutive failure(s), cooling down %d sec", consec_fail_, backoff);
    return false;
}

bool Uploader::perform_upload(const std::vector<uint8_t>& image, int64_t capture_ts,
                              int64_t config_version, std::string* response_body) {
    CURL* curl = get_curl();
    if (!curl) {
        LOGE("Uploader: curl_easy_init failed");
        return false;
    }

    const std::string full_url = pick_base_url() + upload_path_;

    // 请求头（每次创建，用完释放）
    struct curl_slist* headers = nullptr;
    const std::string id_header = "X-Device-ID: " + device_id_;
    headers = curl_slist_append(headers, id_header.c_str());
    const std::string token_header = "X-Device-Token: " + token_;
    headers = curl_slist_append(headers, token_header.c_str());
    if (capture_ts > 0) {
        const std::string ts_header = "X-Capture-Ts: " + std::to_string(capture_ts);
        headers = curl_slist_append(headers, ts_header.c_str());
    }
    if (config_version > 0) {
        const std::string ver_header = "X-Config-Version: " + std::to_string(config_version);
        headers = curl_slist_append(headers, ver_header.c_str());
    }

    // multipart form（每次创建）
    const char* mime = "image/jpeg";
    const char* mime_filename = "shot.jpg";
    image_mime(image, &mime, &mime_filename);
    curl_mime* form = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(form);
    curl_mime_name(part, "image");
    curl_mime_data(part, reinterpret_cast<const char*>(image.data()), image.size());
    curl_mime_filename(part, mime_filename);
    curl_mime_type(part, mime);

    // 设置本次请求参数
    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));

    std::string body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    setup_ssl(curl);

    const CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // 释放本次请求的资源
    curl_mime_free(form);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOGW("Uploader: curl error: %s", curl_easy_strerror(res));
        online_ = false;
        return false;
    }

    if (http_code != 200) {
        LOGW("Uploader: HTTP %ld", http_code);
        online_ = (http_code > 0);
        return false;
    }

    if (response_body) *response_body = std::move(body);
    online_ = true;
    return true;
}

int Uploader::http_get(const std::string& path, int64_t config_version, std::string& out_body) {
    CURL* curl = get_curl();
    if (!curl) {
        LOGE("Uploader: curl_easy_init failed");
        return 0;
    }

    struct curl_slist* headers = nullptr;
    const std::string id_header = "X-Device-ID: " + device_id_;
    headers = curl_slist_append(headers, id_header.c_str());
    const std::string token_header = "X-Device-Token: " + token_;
    headers = curl_slist_append(headers, token_header.c_str());
    if (config_version > 0) {
        const std::string ver_header =
            "If-None-Match: \"" + std::to_string(config_version) + "\"";
        headers = curl_slist_append(headers, ver_header.c_str());
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, (pick_base_url() + path).c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms_));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    setup_ssl(curl);

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        LOGW("Uploader: config fetch curl error: %s", curl_easy_strerror(res));
        online_ = false;
        return 0;
    }

    out_body = std::move(body);
    online_ = true;
    return static_cast<int>(http_code);
}

bool Uploader::fetch_config(int64_t current_version, std::string& out_json) {
    out_json.clear();
    const int code = http_get("/api/config", current_version, out_json);
    if (code == 0) return false;
    if (code == 304) { out_json.clear(); return true; }
    if (code != 200) {
        LOGW("Uploader: config fetch HTTP %d", code);
        return false;
    }
    return true;
}

bool Uploader::online() const {
    return online_;
}
