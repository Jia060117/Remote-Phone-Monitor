#include "capture.h"
#include "logger.h"
#include "netutil.h"
#include "jpeg_utils.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#endif

// ============================================================================
// ScreencapCapture（root 静默截屏）
// ============================================================================
// 通过系统自带的 /system/bin/screencap 截屏。
// 支持 PNG（无损）和 JPEG（有损，通过 quality 控制压缩率）两种输出格式。
// 优势：无需 MediaProjection 授权弹窗，状态栏无投屏图标，完全静默。
// 要求：以 root 运行（KernelSU/Magisk service.sh 启动即满足）。
// 配置: "capture": { "backend": "screencap", "format": "jpeg", "quality": 30 }
//   format  : "png"（默认，无损）或 "jpeg"（有损，文件更小）
//   quality : JPEG 质量 0-100，越低文件越小（仅 format=jpeg 时生效）
// ============================================================================

class ScreencapCapture : public CaptureBackend {
public:
    explicit ScreencapCapture(const Config& cfg)
        : quality_(cfg.capture.quality)
        , use_jpeg_(cfg.capture.format == "jpeg" || cfg.capture.format == "jpg")
        , target_w_(cfg.capture.width)
        , target_h_(cfg.capture.height)
        , cellular_enabled_(cfg.capture.cellular_enabled)
        , cellular_w_(cfg.capture.cellular_width)
        , cellular_h_(cfg.capture.cellular_height)
        , cellular_q_(cfg.capture.cellular_quality)
    {
        // 临时文件放在队列目录（已确认可写），否则退回 /data/local/tmp
        if (!cfg.upload.queue_dir.empty()) {
            tmp_path_ = cfg.upload.queue_dir + "/.shot.tmp";
        } else {
            tmp_path_ = "/data/local/tmp/.rpm_shot";
        }
    }

    bool start() override {
        // 探测 screencap 是否存在
        if (std::FILE* fp = std::fopen("/system/bin/screencap", "rb")) {
            std::fclose(fp);
            LOGI("ScreencapCapture: start() (silent root capture via screencap, format=%s, quality=%d)",
                 use_jpeg_ ? "jpeg" : "png", quality_);
            if (cellular_enabled_) {
                LOGI("ScreencapCapture: cellular low-quality tier enabled (%dx%d q=%d)",
                     cellular_w_, cellular_h_, cellular_q_);
            }
            return true;
        }
        LOGE("ScreencapCapture: /system/bin/screencap not found on this device");
        return false;
    }

    std::optional<Frame> capture() override {
#if defined(_WIN32)
        LOGE("ScreencapCapture is only available on Android/POSIX targets");
        return std::nullopt;
#else
        // 根据网络类型选择有效画质档：
        // 蜂窝（默认路由不走 wlan/eth）时切换到 cellular_* 低画质档，
        // 回到 Wi-Fi 恢复常规档。只在档位切换时打一条日志，避免刷屏。
        int w = target_w_, h = target_h_, q = quality_;
        if (cellular_enabled_ && net_is_cellular()) {
            w = cellular_w_; h = cellular_h_; q = cellular_q_;
            if (!cellular_active_) {
                cellular_active_ = true;
                LOGI("ScreencapCapture: cellular network detected, "
                     "switching to low-quality tier %dx%d q=%d", w, h, q);
            }
        } else if (cellular_active_) {
            cellular_active_ = false;
            LOGI("ScreencapCapture: back on Wi-Fi/ethernet, restoring tier %dx%d q=%d",
                 w, h, q);
        }

        // 全尺寸截图（国产 ROM 的 screencap 普遍不支持 -w/-H），
        // 由 stb 在 PNG->JPEG 转码时同步完成下采样，CPU/内存开销可控。
        // 直接 fork+execv，不经过 shell，避免命令注入和额外进程开销。
        {
            const pid_t pid = fork();
            if (pid < 0) {
                LOGW("ScreencapCapture: fork failed");
                return std::nullopt;
            }
            if (pid == 0) {
                char* const argv[] = {
                    const_cast<char*>("screencap"),
                    const_cast<char*>("-p"),
                    const_cast<char*>(tmp_path_.c_str()),
                    nullptr
                };
                execv("/system/bin/screencap", argv);
                _exit(127);
            }
            // waitpid 带超时（5秒），防止 screencap 挂起导致 agent 永久假死
            int status = 0;
            bool done = false;
            for (int i = 0; i < 100; i++) {  // 100 × 50ms = 5秒
                pid_t ret = waitpid(pid, &status, WNOHANG);
                if (ret == pid) { done = true; break; }
                if (ret < 0 && errno != EINTR) break;  // EINTR 继续等，其他错误退出
                usleep(50000);
            }
            if (!done) {
                kill(pid, SIGKILL);
                // 收尸也用 WNOHANG + 重试，防止 D 状态子进程永久阻塞
                for (int i = 0; i < 30; i++) {  // 30 × 100ms = 3秒
                    if (waitpid(pid, &status, WNOHANG) == pid) break;
                    usleep(100000);
                }
                LOGW("ScreencapCapture: screencap timeout (5s), killed");
                return std::nullopt;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                LOGW("ScreencapCapture: screencap failed (status=%d)", status);
                return std::nullopt;
            }
        }

        std::FILE* fp = std::fopen(tmp_path_.c_str(), "rb");
        if (!fp) {
            LOGW("ScreencapCapture: cannot open screenshot file");
            return std::nullopt;
        }

        Frame f;
        // 一次性预分配，避免逐块 insert 的多次 realloc
        std::fseek(fp, 0, SEEK_END);
        const long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz <= 0) {
            std::fclose(fp);
            std::remove(tmp_path_.c_str());
            LOGW("ScreencapCapture: empty screenshot file");
            return std::nullopt;
        }
        f.encoded.resize(static_cast<size_t>(sz));
        std::fread(f.encoded.data(), 1, f.encoded.size(), fp);
        std::fclose(fp);
        std::remove(tmp_path_.c_str());

        if (f.encoded.empty()) {
            LOGW("ScreencapCapture: empty screenshot output");
            return std::nullopt;
        }

        // 按魔数自动识别实际输出格式（不管命令参数怎么写）
        // PNG: 89 50 4E 47 0D 0A 1A 0A
        // JPEG: FF D8 FF
        static const uint8_t png_magic[8]  = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        static const uint8_t jpeg_magic[3] = {0xFF, 0xD8, 0xFF};

        if (f.encoded.size() >= 8 &&
            std::memcmp(f.encoded.data(), png_magic, 8) == 0) {
            // PNG：从 IHDR 解析宽高（大端，偏移 16/20）
            f.is_png = true;
            if (f.encoded.size() >= 24) {
                f.width  = (f.encoded[16] << 24) | (f.encoded[17] << 16)
                         | (f.encoded[18] << 8)  |  f.encoded[19];
                f.height = (f.encoded[20] << 24) | (f.encoded[21] << 16)
                         | (f.encoded[22] << 8)  |  f.encoded[23];
            }
            LOGD("ScreencapCapture: PNG %dx%d, %zu bytes", f.width, f.height, f.encoded.size());
        } else if (f.encoded.size() >= 3 &&
                   std::memcmp(f.encoded.data(), jpeg_magic, 3) == 0) {
            // JPEG：宽高解析较复杂，服务端用 PIL 自行读取，此处不解析
            f.is_png = false;
            LOGD("ScreencapCapture: JPEG quality=%d, %zu bytes", q, f.encoded.size());
        } else {
            LOGW("ScreencapCapture: unknown image format (%zu bytes, first byte 0x%02X)",
                 f.encoded.size(), f.encoded[0]);
            return std::nullopt;
        }

        // 需要 JPEG 且当前是 PNG：用 stb 转码为 JPEG（质量由当前有效画质档控制）。
        // 转码时顺便算好 aHash（存进 f.hash）并按当前有效目标尺寸下采样，
        // 这样变化检测可以直接复用哈希、无需二次解码图像。
        if (use_jpeg_ && f.is_png) {
            uint64_t h2 = 0;
            int out_w = 0, out_h = 0;
            std::vector<uint8_t> jpeg;
            if (png_to_jpeg(f.encoded.data(), f.encoded.size(), q,
                            jpeg, w, h, &h2, &out_w, &out_h)) {
                f.encoded = std::move(jpeg);
                f.is_png = false;
                f.hash = h2;
                if (out_w > 0) f.width = out_w;
                if (out_h > 0) f.height = out_h;
                LOGD("ScreencapCapture: converted to JPEG quality=%d, %zu bytes, %dx%d",
                     q, f.encoded.size(), f.width, f.height);
            } else {
                LOGW("ScreencapCapture: PNG->JPEG conversion failed, keeping PNG");
            }
        }

        return f;
#endif
    }

    void stop() override {
        LOGI("ScreencapCapture: stop()");
        std::remove(tmp_path_.c_str());
    }

    void apply_config(const CaptureConfig& cfg) override {
        quality_          = cfg.quality;
        use_jpeg_         = (cfg.format == "jpeg" || cfg.format == "jpg");
        target_w_         = cfg.width;
        target_h_         = cfg.height;
        cellular_enabled_ = cfg.cellular_enabled;
        cellular_w_       = cfg.cellular_width;
        cellular_h_       = cfg.cellular_height;
        cellular_q_       = cfg.cellular_quality;
        // 重置档位标记：下一帧重新判定网络类型，必要时补一条切换日志
        cellular_active_  = false;
        LOGI("ScreencapCapture: config applied (%dx%d q=%d, cellular tier %dx%d q=%d)",
             target_w_, target_h_, quality_, cellular_w_, cellular_h_, cellular_q_);
    }

private:
    std::string tmp_path_;
    int  quality_;
    bool use_jpeg_;
    int  target_w_;
    int  target_h_;
    // 蜂窝低画质档
    bool cellular_enabled_;
    int  cellular_w_;
    int  cellular_h_;
    int  cellular_q_;
    bool cellular_active_ = false;  // 当前是否处于蜂窝低画质档（用于切换日志）
};

// ============================================================================
// MediaProjectionCapture（合规实现骨架）
// ============================================================================
// 真正的 MediaProjection 采集需要 companion 前台服务配合：
// 1. Android App/Service 持有 MediaProjection 权限
// 2. 通过 ImageReader / HardwareBuffer 获取帧
// 3. 经共享内存（Ashmem）/ Binder 传递给 native agent
//
// 这里提供接口桩，便于后续对接。
// ============================================================================

class MediaProjectionCapture : public CaptureBackend {
public:
    explicit MediaProjectionCapture(const Config& cfg)
        : width_(cfg.capture.width)
        , height_(cfg.capture.height) {
    }

    bool start() override {
        LOGI("MediaProjectionCapture: start() called");
        // TODO: 连接 companion 服务，建立采集会话
        // 1. 绑定到 companion 服务的 Binder
        // 2. 请求 MediaProjection 授权
        // 3. 创建 ImageReader / HardwareBuffer
        // 4. 注册回调，持续接收帧
        LOGW("MediaProjectionCapture: not yet implemented, returning placeholder frames");
        return true;
    }

    std::optional<Frame> capture() override {
        // TODO: 从 companion 服务获取最新帧
        // 返回占位帧用于测试
        Frame f;
        f.width = width_;
        f.height = height_;
        f.rgba.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4, 0);
        return f;
    }

    void stop() override {
        LOGI("MediaProjectionCapture: stop() called");
        // TODO: 断开 companion 服务，释放资源
    }

private:
    int width_;
    int height_;
};

// ============================================================================
// TestPatternCapture（测试用）
// ============================================================================
// 生成随时间变化的渐变 + 移动标记画面，用于在 PC 上端到端验证
// 编码 / 变化检测 / 上传 / 离线队列整条链路，无需真实设备。
// 配置: "capture": { "backend": "testpattern", "width": 320, "height": 640 }
// ============================================================================

class TestPatternCapture : public CaptureBackend {
public:
    explicit TestPatternCapture(const Config& cfg)
        : width_(cfg.capture.width > 0 ? cfg.capture.width : 320)
        , height_(cfg.capture.height > 0 ? cfg.capture.height : 640) {
    }

    bool start() override {
        LOGI("TestPatternCapture: start() (%dx%d)", width_, height_);
        return true;
    }

    std::optional<Frame> capture() override {
        auto now = std::chrono::steady_clock::now();
        double t = std::chrono::duration<double>(now.time_since_epoch()).count();

        Frame f;
        f.width = width_;
        f.height = height_;
        f.rgba.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4);

        const double two_pi = 6.28318530718;
        for (int y = 0; y < height_; y++) {
            for (int x = 0; x < width_; x++) {
                size_t idx = (static_cast<size_t>(y) * width_ + x) * 4;
                double u = static_cast<double>(x) / width_;
                double v = static_cast<double>(y) / height_;

                uint8_t r = static_cast<uint8_t>(128 + 127 * std::sin(two_pi * (u + t * 0.05)));
                uint8_t g = static_cast<uint8_t>(128 + 127 * std::sin(two_pi * (v + t * 0.03)));
                uint8_t b = static_cast<uint8_t>(128 + 127 * std::sin(two_pi * ((u + v) / 2 + t * 0.07)));

                // 移动的白色竖条，保证画面随时间明显变化
                double bar_x = std::fmod(t * 0.15, 1.0);
                if (std::fabs(u - bar_x) < 0.02) {
                    r = g = b = 255;
                }

                f.rgba[idx]     = r;
                f.rgba[idx + 1] = g;
                f.rgba[idx + 2] = b;
                f.rgba[idx + 3] = 255;
            }
        }
        return f;
    }

    void stop() override {
        LOGI("TestPatternCapture: stop()");
    }

private:
    int width_;
    int height_;
};

// ============================================================================
// 工厂函数
// ============================================================================

CaptureBackend* create_capture_backend(const Config& cfg) {
    if (cfg.capture.backend == "screencap") {
        LOGI("Creating screencap capture backend (root, silent)");
        return new ScreencapCapture(cfg);
    }

    if (cfg.capture.backend == "mediaprojection") {
        LOGI("Creating MediaProjection capture backend");
        return new MediaProjectionCapture(cfg);
    }

    if (cfg.capture.backend == "testpattern") {
        LOGI("Creating test-pattern capture backend");
        return new TestPatternCapture(cfg);
    }

    // 未来可扩展其他后端
    // if (cfg.capture.backend == "surfaceflinger") {
    //     return new SurfaceFlingerCapture(cfg);
    // }

    LOGE("Unknown capture backend: %s", cfg.capture.backend.c_str());
    return nullptr;
}
