#include "logger.h"
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <filesystem>

std::ofstream Logger::file_;
std::string   Logger::path_;
std::string   Logger::level_str_;
int           Logger::min_priority_ = ANDROID_LOG_INFO;
size_t        Logger::bytes_written_ = 0;

#if !defined(__ANDROID__)
void __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    static const char kLevelChars[] = "VDIWE";
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    int idx = prio - ANDROID_LOG_VERBOSE;
    if (idx < 0 || idx > 4) idx = 2;
    fprintf(stderr, "%c/%s: %s\n", kLevelChars[idx], tag, buf);
}
#endif

void Logger::init(const std::string& path, const std::string& level) {
    level_str_ = level;
    min_priority_ = level_to_priority(level);
    path_ = path;
    bytes_written_ = 0;

    if (!path.empty()) {
        file_.open(path, std::ios::app);
        if (!file_.is_open()) {
            Logger::log(ANDROID_LOG_WARN, "Logger: cannot open log file %s", path.c_str());
        } else {
            // 获取当前文件大小
            file_.seekp(0, std::ios::end);
            bytes_written_ = static_cast<size_t>(file_.tellp());
        }
    }

    Logger::log(ANDROID_LOG_INFO, "Logger initialized: level=%s, file=%s",
                level.c_str(), path.c_str());
}

void Logger::shutdown() {
    if (file_.is_open()) {
        file_.close();
    }
}

void Logger::rotate_if_needed() {
    if (!file_.is_open() || bytes_written_ < kMaxBytes || path_.empty()) return;

    file_.close();

    // 删除最旧的: path_.3
    const std::string oldest = path_ + "." + std::to_string(kKeepFiles);
    std::error_code ec;
    std::filesystem::remove(oldest, ec);

    // 依次重命名: .2 <- .1, .1 <- .0, .0 <- current
    for (int i = kKeepFiles - 1; i >= 1; i--) {
        const std::string from = path_ + "." + std::to_string(i - 1);
        const std::string to   = path_ + "." + std::to_string(i);
        std::filesystem::rename(from, to, ec);
    }
    std::filesystem::rename(path_, path_ + ".0", ec);

    // 重新打开新文件，失败则打 logcat 告警（不静默）
    file_.open(path_, std::ios::app);
    bytes_written_ = 0;
    if (!file_.is_open()) {
        // logcat 不受影响，运维至少能从 adb logcat 看到这条
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Logger: log rotation open failed for %s, file logging suspended", path_.c_str());
    }
}

void Logger::log(int priority, const char* fmt, ...) {
    if (priority < min_priority_) return;

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // logcat（Android 上走系统日志，不走 stdout/stderr）
    __android_log_print(priority, LOG_TAG, "%s", buf);

    // 文件
    if (file_.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        struct tm tm_buf;
#if defined(_WIN32)
        localtime_s(&tm_buf, &now_t);
#else
        localtime_r(&now_t, &tm_buf);
#endif

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

        const char* level_str = "?";
        switch (priority) {
            case ANDROID_LOG_VERBOSE: level_str = "V"; break;
            case ANDROID_LOG_DEBUG:   level_str = "D"; break;
            case ANDROID_LOG_INFO:    level_str = "I"; break;
            case ANDROID_LOG_WARN:    level_str = "W"; break;
            case ANDROID_LOG_ERROR:   level_str = "E"; break;
        }

        file_ << time_str << "." << ms.count() << " "
              << level_str << " " << buf << std::endl;

        bytes_written_ += static_cast<size_t>(n) + 30;
        rotate_if_needed();
    } else if (!path_.empty()) {
        // 文件未打开（轮转失败），低频重试（每 100 条尝试重开一次）
        static int retry_counter = 0;
        if (++retry_counter >= 100) {
            retry_counter = 0;
            file_.open(path_, std::ios::app);
            if (file_.is_open()) {
                file_.seekp(0, std::ios::end);
                bytes_written_ = static_cast<size_t>(file_.tellp());
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                    "Logger: file logging recovered for %s", path_.c_str());
            }
        }
    }
}

int Logger::level_to_priority(const std::string& level) {
    if (level == "verbose") return ANDROID_LOG_VERBOSE;
    if (level == "debug")   return ANDROID_LOG_DEBUG;
    if (level == "info")    return ANDROID_LOG_INFO;
    if (level == "warn")    return ANDROID_LOG_WARN;
    if (level == "error")   return ANDROID_LOG_ERROR;
    return ANDROID_LOG_INFO;
}
