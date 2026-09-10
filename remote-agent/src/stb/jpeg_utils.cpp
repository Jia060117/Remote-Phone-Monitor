#include "jpeg_utils.h"

// 在 include stb 头文件前关闭它们会触发的常见告警，保持构建输出干净。
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cstring>

namespace {

// stb_image_write 的回调：把编码出来的字节累积进 vector
void write_callback(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + static_cast<size_t>(size));
}

// 把 w×h×3 的 RGB 用 box 平均下采样到 tw×th（保持内容，仅缩小）。
// 每个目标像素取源块内像素平均值。
std::vector<uint8_t> box_downscale(const unsigned char* src, int w, int h, int tw, int th) {
    std::vector<uint8_t> dst(static_cast<size_t>(tw) * th * 3);
    const double sx = static_cast<double>(w) / tw;
    const double sy = static_cast<double>(h) / th;
    for (int y = 0; y < th; y++) {
        int sy0 = static_cast<int>(y * sy);
        int sy1 = static_cast<int>((y + 1) * sy);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;  // 防止越界
        for (int x = 0; x < tw; x++) {
            int sx0 = static_cast<int>(x * sx);
            int sx1 = static_cast<int>((x + 1) * sx);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;  // 防止越界
            long long r = 0, g = 0, b = 0;
            long long cnt = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char* p = src + (static_cast<size_t>(yy) * w + xx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; cnt++;
                }
            }
            unsigned char* o = dst.data() + (static_cast<size_t>(y) * tw + x) * 3;
            o[0] = static_cast<unsigned char>(r / cnt);
            o[1] = static_cast<unsigned char>(g / cnt);
            o[2] = static_cast<unsigned char>(b / cnt);
        }
    }
    return dst;
}

} // namespace

uint64_t ahash_from_pixels(const unsigned char* pix, int w, int h, int ch) {
    long long cell_sum[64] = {0};
    int      cell_cnt[64] = {0};
    for (int y = 0; y < h; y++) {
        int cy = y * 8 / h;
        for (int x = 0; x < w; x++) {
            int cx = x * 8 / w;
            int idx = (y * w + x) * ch;
            unsigned char r, g, b;
            if (ch >= 3) { r = pix[idx]; g = pix[idx + 1]; b = pix[idx + 2]; }
            else          { r = g = b = pix[idx]; }  // 灰度
            unsigned char gray = static_cast<unsigned char>(
                (int(r) * 299 + int(g) * 587 + int(b) * 114) / 1000);
            int c = cy * 8 + cx;
            cell_sum[c] += gray;
            cell_cnt[c] += 1;
        }
    }
    unsigned char cell_avg[64];
    long long total = 0;
    for (int i = 0; i < 64; i++) {
        cell_avg[i] = cell_cnt[i] ? static_cast<unsigned char>(cell_sum[i] / cell_cnt[i]) : 0;
        total += cell_avg[i];
    }
    int mean = static_cast<int>(total / 64);
    uint64_t hash = 0;
    for (int i = 0; i < 64; i++) {
        if (cell_avg[i] >= mean) hash |= (1ULL << i);
    }
    return hash;
}

bool png_to_jpeg(const uint8_t* data, size_t len, int quality,
                 std::vector<uint8_t>& out,
                 int target_w, int target_h,
                 uint64_t* out_hash, int* out_w, int* out_h) {
    if (!data || len == 0) return false;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    int w = 0, h = 0, channels = 0;
    // 强制解码为 RGB（3 通道），JPEG 不需要 alpha
    stbi_uc* pixels = stbi_load_from_memory(data, static_cast<int>(len), &w, &h, &channels, 3);
    if (!pixels) {
        return false;
    }

    // 感知哈希：基于原始分辨率像素计算，最稳定，供变化检测复用（免二次解码）。
    if (out_hash) *out_hash = ahash_from_pixels(pixels, w, h, 3);

    // 下采样（仅当目标小于原始尺寸）
    const unsigned char* enc_pixels = pixels;
    int enc_w = w, enc_h = h;
    std::vector<uint8_t> scaled;
    if ((target_w > 0 && target_w < w) || (target_h > 0 && target_h < h)) {
        double scale = 1.0;
        if (target_w > 0) scale = std::min(scale, static_cast<double>(target_w) / w);
        if (target_h > 0) scale = std::min(scale, static_cast<double>(target_h) / h);
        int tw = std::max(1, static_cast<int>(w * scale));
        int th = std::max(1, static_cast<int>(h * scale));
        scaled = box_downscale(pixels, w, h, tw, th);
        enc_pixels = scaled.data();
        enc_w = tw; enc_h = th;
    }

    if (out_w) *out_w = enc_w;
    if (out_h) *out_h = enc_h;

    out.clear();
    int ok = stbi_write_jpg_to_func(write_callback, &out, enc_w, enc_h, 3, enc_pixels, quality);
    stbi_image_free(pixels);

    return ok != 0 && !out.empty();
}
