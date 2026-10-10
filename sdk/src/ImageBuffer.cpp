#include "sai2/ImageBuffer.h"

namespace sai2 {

ImageBuffer::ImageBuffer(int width, int height, PixelFormat format) {
    Create(width, height, format);
}

bool ImageBuffer::Create(int width, int height, PixelFormat format) {
    if (width <= 0 || height <= 0) {
        width_ = 0; height_ = 0; pixels_.clear();
        return false;
    }
    width_ = width;
    height_ = height;
    format_ = format;
    pixels_.assign(static_cast<size_t>(width_) * height_ * 4 *
                       BytesPerChannel(), 0);
    return true;
}

uint32_t ImageBuffer::BytesPerChannel() const {
    return format_ == PixelFormat::RGBA16 ? 2u : 1u;
}

uint32_t ImageBuffer::BitsPerPixel() const {
    return BytesPerChannel() * 4u * 8u;
}

size_t ImageBuffer::Stride() const {
    return static_cast<size_t>(width_) * 4u * BytesPerChannel();
}

size_t ImageBuffer::DataSize() const { return pixels_.size(); }

bool ImageBuffer::GetPixel(int x, int y, RGBA& out) const {
    if (!InBounds(x, y)) return false;
    const size_t ch = BytesPerChannel();
    const size_t idx = (static_cast<size_t>(y) * width_ + x) * 4;
    if (ch == 1) {
        out.r = pixels_[idx + 0];
        out.g = pixels_[idx + 1];
        out.b = pixels_[idx + 2];
        out.a = pixels_[idx + 3];
    } else {
        out.r = static_cast<uint16_t>(pixels_[idx * 2 + 0]) |
                static_cast<uint16_t>(pixels_[idx * 2 + 1] << 8);
        out.g = static_cast<uint16_t>(pixels_[idx * 2 + 2]) |
                static_cast<uint16_t>(pixels_[idx * 2 + 3] << 8);
        out.b = static_cast<uint16_t>(pixels_[idx * 2 + 4]) |
                static_cast<uint16_t>(pixels_[idx * 2 + 5] << 8);
        out.a = static_cast<uint16_t>(pixels_[idx * 2 + 6]) |
                static_cast<uint16_t>(pixels_[idx * 2 + 7] << 8);
    }
    return true;
}

bool ImageBuffer::SetPixel(int x, int y, const RGBA& c) {
    if (!InBounds(x, y)) return false;
    const size_t ch = BytesPerChannel();
    const size_t idx = (static_cast<size_t>(y) * width_ + x) * 4;
    if (ch == 1) {
        pixels_[idx + 0] = static_cast<uint8_t>(c.r & 0xFF);
        pixels_[idx + 1] = static_cast<uint8_t>(c.g & 0xFF);
        pixels_[idx + 2] = static_cast<uint8_t>(c.b & 0xFF);
        pixels_[idx + 3] = static_cast<uint8_t>(c.a & 0xFF);
    } else {
        pixels_[idx * 2 + 0] = static_cast<uint8_t>(c.r & 0xFF);
        pixels_[idx * 2 + 1] = static_cast<uint8_t>((c.r >> 8) & 0xFF);
        pixels_[idx * 2 + 2] = static_cast<uint8_t>(c.g & 0xFF);
        pixels_[idx * 2 + 3] = static_cast<uint8_t>((c.g >> 8) & 0xFF);
        pixels_[idx * 2 + 4] = static_cast<uint8_t>(c.b & 0xFF);
        pixels_[idx * 2 + 5] = static_cast<uint8_t>((c.b >> 8) & 0xFF);
        pixels_[idx * 2 + 6] = static_cast<uint8_t>(c.a & 0xFF);
        pixels_[idx * 2 + 7] = static_cast<uint8_t>((c.a >> 8) & 0xFF);
    }
    return true;
}

void ImageBuffer::Clear(const RGBA& c) {
    for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x)
            SetPixel(x, y, c);
}

void ImageBuffer::FillRect(int x, int y, int w, int h, const RGBA& c) {
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const int x1 = (x + w) > width_ ? width_ : (x + w);
    const int y1 = (y + h) > height_ ? height_ : (y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            SetPixel(xx, yy, c);
}

ImageBuffer ImageBuffer::Clone() const {
    ImageBuffer copy(*this);
    return copy;
}

ImageBuffer ImageBuffer::Convert(const ImageBuffer& src, PixelFormat target) {
    if (src.Format() == target) return src.Clone();
    ImageBuffer dst(src.Width(), src.Height(), target);
    if (!dst.IsValid()) return {};
    RGBA c;
    for (int y = 0; y < src.Height(); ++y) {
        for (int x = 0; x < src.Width(); ++x) {
            src.GetPixel(x, y, c);
            if (src.Format() == PixelFormat::RGBA8 &&
                target == PixelFormat::RGBA16) {
                c.r = Scale8To16(static_cast<uint8_t>(c.r));
                c.g = Scale8To16(static_cast<uint8_t>(c.g));
                c.b = Scale8To16(static_cast<uint8_t>(c.b));
                c.a = Scale8To16(static_cast<uint8_t>(c.a));
            } else if (src.Format() == PixelFormat::RGBA16 &&
                       target == PixelFormat::RGBA8) {
                c.r = Scale16To8(c.r);
                c.g = Scale16To8(c.g);
                c.b = Scale16To8(c.b);
                c.a = Scale16To8(c.a);
            }
            dst.SetPixel(x, y, c);
        }
    }
    return dst;
}

} // namespace sai2
