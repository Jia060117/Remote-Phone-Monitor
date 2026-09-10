#pragma once
#include <string>
#include <fstream>

#if defined(__ANDROID__)
#include <android/log.h>
#else
enum {
    ANDROID_LOG_VERBOSE = 2,
    ANDROID_LOG_DEBUG   = 3,
    ANDROID_LOG_INFO    = 4,
    ANDROID_LOG_WARN    = 5,
    ANDROID_LOG_ERROR   = 6
};
void __android_log_print(int prio, const char* tag, const char* fmt, ...);
#endif

#define LOG_TAG "remote-agent"

class Logger {
public:
    static void init(const std::string& path, const std::string& level = "info");
    static void shutdown();

    static void log(int priority, const char* fmt, ...);

private:
    static int level_to_priority(const std::string& level);
    static void rotate_if_needed();

    static std::ofstream file_;
    static std::string   path_;
    static std::string   level_str_;
    static int           min_priority_;
    static size_t        bytes_written_;

    static constexpr size_t kMaxBytes  = 8 * 1024 * 1024;  // 单文件 8MB
    static constexpr int    kKeepFiles = 3;                 // 保留 3 个旧文件
};

#define LOGV(...) Logger::log(ANDROID_LOG_VERBOSE, __VA_ARGS__)
#define LOGD(...) Logger::log(ANDROID_LOG_DEBUG,   __VA_ARGS__)
#define LOGI(...) Logger::log(ANDROID_LOG_INFO,    __VA_ARGS__)
#define LOGW(...) Logger::log(ANDROID_LOG_WARN,    __VA_ARGS__)
#define LOGE(...) Logger::log(ANDROID_LOG_ERROR,   __VA_ARGS__)
