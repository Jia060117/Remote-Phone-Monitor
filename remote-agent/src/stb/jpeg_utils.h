#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// 将内存中的 PNG（或任意 stb 支持的格式）转换为 JPEG。
// quality: 1-100，越低文件越小（有损）。
// target_w/target_h: 目标输出尺寸（保持宽高比，仅下采样）。传 0 表示不缩放。
// out_hash: 若非空，写入基于原始分辨率像素计算的 8x8 感知哈希（aHash），
//           供变化检测复用，避免二次解码。
// out_w/out_h: 若非空，写入最终 JPEG 的实际尺寸。
// 返回 true 表示转换成功，out 包含 JPEG 字节（FF D8 FF 开头）。
bool png_to_jpeg(const uint8_t* data, size_t len, int quality,
                 std::vector<uint8_t>& out,
                 int target_w = 0, int target_h = 0,
                 uint64_t* out_hash = nullptr,
                 int* out_w = nullptr, int* out_h = nullptr);

// 把 RGB(A) 像素下采样到 8x8 灰度并算平均哈希（与 changer 共用，避免重复实现）。
uint64_t ahash_from_pixels(const unsigned char* pix, int w, int h, int ch);
