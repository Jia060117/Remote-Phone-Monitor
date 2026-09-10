#include "encoder.h"
#include "logger.h"
#include <cstring>

#if RPM_HAVE_JPEG
#include <jpeglib.h>
#include <csetjmp>

// ----------------------------------------------------------------------------
// libjpeg 错误处理：
// 默认 error handler 在出错时会直接 exit() 杀死整个进程，
// 这里改为 longjmp 回到 setjmp 点，由我们决定返回 nullopt。
// ----------------------------------------------------------------------------
namespace {

struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

void jpeg_error_exit(j_common_ptr cinfo) {
    JpegErrorMgr* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    char msg[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, msg);
    LOGE("Encoder: libjpeg error: %s", msg);
    longjmp(err->setjmp_buffer, 1);
}

} // namespace
#endif // RPM_HAVE_JPEG

Encoder::Encoder(const CaptureConfig& cfg)
    : format_(cfg.format)
    , quality_(cfg.quality) {
}

std::optional<std::vector<uint8_t>> Encoder::encode(const Frame& frame) {
    if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0) {
        LOGE("Encoder: invalid frame");
        return std::nullopt;
    }

#if !RPM_HAVE_JPEG
    (void)format_; (void)quality_;
    LOGE("Encoder: built without libjpeg, JPEG encoding unavailable "
         "(see BUILD.md to enable)");
    return std::nullopt;
#else
    if (format_ != "jpeg") {
        // WebP 支持需要 libwebp（尚未实现）
        LOGE("Encoder: unsupported format '%s'", format_.c_str());
        return std::nullopt;
    }

    if (frame.rgba.size() < static_cast<size_t>(frame.width) *
                             static_cast<size_t>(frame.height) * 4) {
        LOGE("Encoder: frame buffer smaller than width*height*4");
        return std::nullopt;
    }

    // RGBA -> RGB
    size_t pixel_count = static_cast<size_t>(frame.width) *
                         static_cast<size_t>(frame.height);
    std::vector<uint8_t> rgb(pixel_count * 3);
    for (size_t i = 0, j = 0; i < pixel_count; i++, j += 3) {
        const uint8_t* px = &frame.rgba[i * 4];
        rgb[j]     = px[0];
        rgb[j + 1] = px[1];
        rgb[j + 2] = px[2];
    }

    jpeg_compress_struct cinfo{};
    JpegErrorMgr jerr{};
    unsigned char* out_buffer = nullptr;   // 由 libjpeg 分配（malloc）
    unsigned long out_size = 0;
    bool compress_ok = false;

    cinfo.err = jpeg_std_error(reinterpret_cast<struct jpeg_error_mgr*>(&jerr));
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        // libjpeg 出错，从 longjmp 跳到这里：清理后返回失败
        jpeg_destroy_compress(&cinfo);
        if (out_buffer) free(out_buffer);
        return std::nullopt;
    }

    jpeg_create_compress(&cinfo);

    // 内存目标
    jpeg_mem_dest(&cinfo, &out_buffer, &out_size);

    cinfo.image_width      = static_cast<JDIMENSION>(frame.width);
    cinfo.image_height     = static_cast<JDIMENSION>(frame.height);
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality_, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    JSAMPROW row = rgb.data();
    while (cinfo.next_scanline < cinfo.image_height) {
        jpeg_write_scanlines(&cinfo, &row, 1);
        row += static_cast<size_t>(frame.width) * 3;
    }

    jpeg_finish_compress(&cinfo);
    compress_ok = true;

    jpeg_destroy_compress(&cinfo);

    if (!compress_ok || !out_buffer || out_size == 0) {
        if (out_buffer) free(out_buffer);
        LOGE("Encoder: JPEG compression produced empty output");
        return std::nullopt;
    }

    std::vector<uint8_t> out(out_buffer, out_buffer + out_size);
    free(out_buffer);
    return out;
#endif // RPM_HAVE_JPEG
}
