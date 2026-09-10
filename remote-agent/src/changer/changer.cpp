#include "changer.h"
#include "logger.h"
#include "jpeg_utils.h"
#include <chrono>
#include <cmath>

#if RPM_HAVE_JPEG
#include <jpeglib.h>
#include <csetjmp>

namespace {

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    JpegErrorMgr* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->setjmp_buffer, 1);
}

} // namespace
#endif // RPM_HAVE_JPEG

ChangeDetector::ChangeDetector(const ChangeDetectionConfig& cfg)
    : enabled_(cfg.enabled)
    , algorithm_(cfg.algorithm)
    , threshold_(cfg.threshold) {
}

void ChangeDetector::apply_config(const ChangeDetectionConfig& cfg) {
    enabled_   = cfg.enabled;
    algorithm_ = cfg.algorithm;
    threshold_ = cfg.threshold;
    LOGI("ChangeDetector: config applied (enabled=%s, threshold=%.2f)",
         enabled_ ? "true" : "false", threshold_);
}

#if !RPM_HAVE_JPEG
// 无 libjpeg：用 stb_image 解码（自动识别 PNG/JPEG）做感知哈希（aHash）。
// aHash 对像素而非字节做比较，可忽略状态栏时钟/电量等微变化，避免重复画面重复上传。
#include "stb_image.h"
#endif // !RPM_HAVE_JPEG

bool ChangeDetector::compare_and_decide(uint64_t h_cur) {
    if (last_hash_.has_value()) {
        int diff_bits = popcount(h_cur ^ *last_hash_);
        double sim = 1.0 - diff_bits / 64.0;
        last_hash_ = h_cur;
        LOGD("ChangeDetector: similarity=%.2f (threshold=%.2f), %s",
             sim, threshold_, sim >= threshold_ ? "skipping upload" : "uploading");
        return sim < threshold_; // 相似(sim>=阈值)跳过，不同才上传
    }
    last_hash_ = h_cur;
    return true; // 首帧总是上传
}

bool ChangeDetector::should_upload(const std::vector<uint8_t>& jpeg,
                                   std::optional<uint64_t> precomputed_hash) {
#if !RPM_HAVE_JPEG
    if (!enabled_) return true;

    uint64_t h_cur;
    if (precomputed_hash.has_value()) {
        // 捕获阶段已算好哈希，直接复用，省去一次完整图像解码
        h_cur = *precomputed_hash;
    } else {
        int w = 0, h = 0, ch = 0;
        stbi_uc* pix = stbi_load_from_memory(jpeg.data(),
                                             static_cast<int>(jpeg.size()),
                                             &w, &h, &ch, 0);
        if (!pix) {
            LOGW("ChangeDetector: cannot decode frame for comparison, uploading");
            return true; // 解码失败保守上传
        }
        h_cur = ahash_from_pixels(pix, w, h, ch);
        stbi_image_free(pix);
    }
    return compare_and_decide(h_cur);
#else
    if (!enabled_) return true;

    uint64_t h_cur;
    if (precomputed_hash.has_value()) {
        h_cur = *precomputed_hash;
    } else {
        auto gray = to_gray_8x8(jpeg);
        if (gray.empty()) {
            LOGW("ChangeDetector: failed to decode for comparison");
            return true; // 解码失败时保守上传
        }
        h_cur = ahash(gray);
    }
    return compare_and_decide(h_cur);
#endif // RPM_HAVE_JPEG
}

#if RPM_HAVE_JPEG
std::vector<uint8_t> ChangeDetector::to_gray_8x8(const std::vector<uint8_t>& jpeg) {
    // 解码 JPEG 到灰度（1/8 缩放）
    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jerr{};

    cinfo.err = jpeg_std_error(reinterpret_cast<struct jpeg_error_mgr*>(&jerr));
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return {};
    }

    jpeg_create_decompress(&cinfo);

    jpeg_mem_src(&cinfo, jpeg.data(), jpeg.size());

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return {};
    }

    cinfo.out_color_space = JCS_GRAYSCALE;
    cinfo.scale_num = 1;
    cinfo.scale_denom = 8; // 缩放到 1/8

    jpeg_start_decompress(&cinfo);

    int w = static_cast<int>(cinfo.output_width);
    int h = static_cast<int>(cinfo.output_height);
    if (w <= 0 || h <= 0) {
        jpeg_destroy_decompress(&cinfo);
        return {};
    }

    std::vector<uint8_t> gray(static_cast<size_t>(w) * static_cast<size_t>(h));

    JSAMPROW row = gray.data();
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, &row, 1);
        row += w;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    // 如果不是 8x8，缩放（最近邻采样）
    if (w != 8 || h != 8) {
        std::vector<uint8_t> result(64);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int src_x = x * w / 8;
                int src_y = y * h / 8;
                result[y * 8 + x] = gray[static_cast<size_t>(src_y) * w + src_x];
            }
        }
        return result;
    }

    return gray;
}
#else
std::vector<uint8_t> ChangeDetector::to_gray_8x8(const std::vector<uint8_t>&) {
    return {};
}
#endif // RPM_HAVE_JPEG

uint64_t ChangeDetector::ahash(const std::vector<uint8_t>& gray8x8) {
    uint64_t hash = 0;
    int sum = 0;
    for (int i = 0; i < 64; i++) sum += gray8x8[i];
    int avg = sum / 64;
    for (int i = 0; i < 64; i++) {
        if (gray8x8[i] >= avg) hash |= (1ULL << i);
    }
    return hash;
}

int ChangeDetector::popcount(uint64_t x) {
    int count = 0;
    while (x) {
        count += static_cast<int>(x & 1);
        x >>= 1;
    }
    return count;
}
