#pragma once
// ============================================================================
// ImageBuffer.h - 统一图像缓冲区（Phase 2）
//
// 第一版支持 RGBA8 与 RGBA16 两种像素格式，后续可扩展 RGBA32F。
// 所有通道统一用 uint16_t 承载：
//   - RGBA8 : 每通道取值 0..255（低 8 位有效）
//   - RGBA16: 每通道取值 0..65535
// 内存布局：紧凑（无行填充），行 stride = width * 4 * BytesPerChannel()
// ============================================================================

#include "sai2/PluginAPI.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sai2 {

enum class PixelFormat : uint32_t {
    RGBA8  = 0, // 4 通道 x  8 bit
    RGBA16 = 1, // 4 通道 x 16 bit
    // RGBA32F = 2,  // 预留
};

// 与像素格式无关的颜色值；写入时会按格式截断
struct RGBA {
    uint16_t r = 0, g = 0, b = 0, a = 0;
};

inline bool operator==(const RGBA& l, const RGBA& r) {
    return l.r == r.r && l.g == r.g && l.b == r.b && l.a == r.a;
}
inline bool operator!=(const RGBA& l, const RGBA& r) { return !(l == r); }

class ImageBuffer {
public:
    ImageBuffer() = default;
    ImageBuffer(int width, int height, PixelFormat format);

    bool Create(int width, int height, PixelFormat format);

    int         Width()  const { return width_; }
    int         Height() const { return height_; }
    PixelFormat Format() const { return format_; }
    bool        IsValid() const { return width_ > 0 && height_ > 0 && !pixels_.empty(); }

    uint32_t BytesPerChannel() const; // 1 或 2
    uint32_t BitsPerPixel() const;    // 32 或 64
    size_t   Stride() const;          // 每行字节数 = width * 4 * BytesPerChannel()
    size_t   DataSize() const;

    uint8_t*       Data()       { return pixels_.data(); }
    const uint8_t* Data() const { return pixels_.data(); }

    bool InBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    // 读写像素（含边界检查，越界返回 false）
    bool GetPixel(int x, int y, RGBA& out) const;
    bool SetPixel(int x, int y, const RGBA& c);

    void Clear(const RGBA& c);
    void FillRect(int x, int y, int w, int h, const RGBA& c);

    // 深拷贝
    ImageBuffer Clone() const;

    // 格式转换（通道值按线性比例缩放 0..255 <-> 0..65535）
    static ImageBuffer Convert(const ImageBuffer& src, PixelFormat target);

private:
    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::RGBA8;
    std::vector<uint8_t> pixels_; // 原始字节
};

// 8bit <-> 16bit 通道值换算
inline uint16_t Scale8To16(uint8_t v)  { return static_cast<uint16_t>((v << 8) | v); }
inline uint8_t  Scale16To8(uint16_t v) { return static_cast<uint8_t>(v >> 8); }

} // namespace sai2
