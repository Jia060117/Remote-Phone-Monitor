#include "queue.h"
#include "logger.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>

namespace fs = std::filesystem;

OfflineQueue::OfflineQueue(const std::string& dir, int max_mb)
    : dir_(dir)
    , max_mb_(max_mb) {
    if (!dir_.empty()) {
        std::error_code ec;
        fs::create_directories(dir_, ec);
        if (ec) {
            LOGW("OfflineQueue: cannot create queue dir %s: %s",
                 dir_.c_str(), ec.message().c_str());
        }
        recover_sending();
    }
}

void OfflineQueue::recover_sending() {
    // 启动时把上次崩溃遗留的 .sending 文件恢复为正常队列项
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() > 8 && name.substr(name.size() - 8) == ".sending") {
            std::string restored = entry.path().parent_path().string() + "/"
                                 + name.substr(0, name.size() - 8);
            fs::rename(entry.path(), restored, ec);
            if (!ec) {
                LOGI("OfflineQueue: recovered %s", name.c_str());
            }
        }
    }
}

void OfflineQueue::save(const std::vector<uint8_t>& data, int64_t ts) {
    if (dir_.empty()) return;

    if (ts == 0) {
        ts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    std::string filename = std::to_string(ts) + ".jpg";
    std::string path = dir_ + "/" + filename;
    std::string tmp = path + ".tmp";

    // 先写临时文件
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.is_open()) {
            LOGE("OfflineQueue: cannot write to %s", tmp.c_str());
            return;
        }
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        f.flush();
        if (!f.good()) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            LOGE("OfflineQueue: write failed, discarded (%zu bytes)", data.size());
            return;
        }
    }

    // 原子替换
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        LOGE("OfflineQueue: rename failed: %s", ec.message().c_str());
        fs::remove(tmp, ec);
        return;
    }

    LOGD("OfflineQueue: saved %s (%zu bytes)", filename.c_str(), data.size());
    cleanup();
}

std::optional<QueuedItem> OfflineQueue::pop() {
    if (dir_.empty()) return std::nullopt;

    // 找到最旧的正常文件（排除 .sending 和 .tmp）
    std::string oldest_path;
    int64_t oldest_ts = INT64_MAX;

    try {
        for (const auto& entry : fs::directory_iterator(dir_)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();

            // 跳过临时文件和发送中文件
            if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") continue;
            if (name.size() > 8 && name.substr(name.size() - 8) == ".sending") continue;

            size_t dot = name.find('.');
            if (dot == std::string::npos) continue;

            try {
                int64_t ts = std::stoll(name.substr(0, dot));
                if (ts < oldest_ts) {
                    oldest_ts = ts;
                    oldest_path = entry.path().string();
                }
            } catch (...) {
                continue;
            }
        }
    } catch (const fs::filesystem_error& e) {
        LOGE("OfflineQueue: directory iteration error: %s", e.what());
        return std::nullopt;
    }

    if (oldest_path.empty()) return std::nullopt;

    // 重命名为 .sending（原子操作，崩溃后可恢复）
    std::string sending = oldest_path + ".sending";
    std::error_code ec;
    fs::rename(oldest_path, sending, ec);
    if (ec) {
        LOGW("OfflineQueue: rename to .sending failed: %s", ec.message().c_str());
        return std::nullopt;
    }

    // 读取内容
    std::ifstream f(sending, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        fs::rename(sending, oldest_path, ec);
        return std::nullopt;
    }

    std::streampos end_pos = f.tellg();
    size_t sz = (end_pos > 0) ? static_cast<size_t>(end_pos) : 0;
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    if (sz > 0) {
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    f.close();

    if (data.empty()) {
        fs::remove(sending, ec);
        return std::nullopt;
    }

    QueuedItem item;
    item.data = std::move(data);
    item.ts = oldest_ts;
    item.path = sending;
    return item;
}

void OfflineQueue::confirm(const QueuedItem& item) {
    if (item.path.empty()) return;
    std::error_code ec;
    fs::remove(item.path, ec);
}

size_t OfflineQueue::size() const {
    if (dir_.empty()) return 0;

    size_t count = 0;
    try {
        for (const auto& entry : fs::directory_iterator(dir_)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            // 只计正常队列文件，不计 .sending / .tmp
            if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") continue;
            if (name.size() > 8 && name.substr(name.size() - 8) == ".sending") continue;
            size_t dot = name.find('.');
            if (dot == std::string::npos) continue;
            try { std::stoll(name.substr(0, dot)); count++; } catch (...) {}
        }
    } catch (...) {}
    return count;
}

void OfflineQueue::cleanup() {
    if (dir_.empty()) return;

    size_t total_bytes = 0;
    std::vector<std::pair<int64_t, std::string>> files;

    try {
        for (const auto& entry : fs::directory_iterator(dir_)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp") continue;
            if (name.size() > 8 && name.substr(name.size() - 8) == ".sending") continue;

            size_t dot = name.find('.');
            if (dot == std::string::npos) continue;

            try {
                int64_t ts = std::stoll(name.substr(0, dot));
                files.emplace_back(ts, entry.path().string());
                total_bytes += static_cast<size_t>(entry.file_size());
            } catch (...) {
                continue;
            }
        }
    } catch (...) {
        return;
    }

    size_t max_bytes = static_cast<size_t>(max_mb_) * 1024 * 1024;
    if (total_bytes <= max_bytes) return;

    std::sort(files.begin(), files.end());

    for (const auto& [ts, path] : files) {
        if (total_bytes <= max_bytes) break;
        try {
            size_t fsize = static_cast<size_t>(fs::file_size(path));
            std::error_code ec;
            fs::remove(path, ec);
            total_bytes -= fsize;
            LOGW("OfflineQueue: evicted old frame %s", path.c_str());
        } catch (...) {}
    }
}
