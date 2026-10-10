// ============================================================================
// test_imagebuffer.cpp - ImageBuffer / PixelFormat / Selection
// ============================================================================

#include "Tests.h"
#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

namespace sai2test {

bool TestImageBuffer() {
    using namespace sai2;

    // ---- RGBA8 布局 ----
    ImageBuffer buf(4, 3, PixelFormat::RGBA8);
    CHECK(buf.IsValid());
    CHECK(buf.DataSize() == 4u * 3u * 4u);
    CHECK(buf.Stride() == 16u);
    CHECK(buf.BitsPerPixel() == 32u);

    RGBA c{0x11, 0x22, 0x33, 0xFF};
    CHECK(buf.SetPixel(1, 2, c));
    RGBA got;
    CHECK(buf.GetPixel(1, 2, got));
    CHECK(got == c);
    CHECK(!buf.GetPixel(4, 0, got));
    CHECK(!buf.SetPixel(-1, 0, c));

    // ---- RGBA16 布局 ----
    ImageBuffer buf16(2, 2, PixelFormat::RGBA16);
    CHECK(buf16.Stride() == 16u); // 2px * 4ch * 2B
    CHECK(buf16.DataSize() == 2u * 2u * 4u * 2u);
    CHECK(buf16.BitsPerPixel() == 64u);
    RGBA c16{0xFFFF, 0x8000, 0x1234, 0xFFFF};
    CHECK(buf16.SetPixel(0, 1, c16));
    RGBA got16;
    CHECK(buf16.GetPixel(0, 1, got16));
    CHECK(got16 == c16);

    // ---- Clear / FillRect（含裁剪） ----
    buf.Clear(RGBA{});
    CHECK(buf.GetPixel(1, 2, got) && got.r == 0);
    buf.FillRect(-2, -2, 4, 4, c); // 裁剪到 (0,0)-(1,1)
    CHECK(buf.GetPixel(0, 0, got) && got == c);
    CHECK(buf.GetPixel(2, 0, got) && got.r == 0);

    // ---- Clone 独立性 ----
    ImageBuffer clone = buf.Clone();
    clone.SetPixel(0, 0, RGBA{});
    CHECK(buf.GetPixel(0, 0, got) && got == c);

    // ---- 格式转换 255 <-> 65535 ----
    ImageBuffer small(1, 1, PixelFormat::RGBA8);
    small.SetPixel(0, 0, RGBA{0xFF, 0x80, 0x40, 0xFF});
    ImageBuffer wide = ImageBuffer::Convert(small, PixelFormat::RGBA16);
    CHECK(wide.Format() == PixelFormat::RGBA16);
    CHECK(wide.GetPixel(0, 0, got16));
    CHECK(got16.r == 0xFFFF && got16.g == 0x8080 && got16.b == 0x4040);
    ImageBuffer back = ImageBuffer::Convert(wide, PixelFormat::RGBA8);
    CHECK(back.GetPixel(0, 0, got) && got.r == 0xFF && got.g == 0x80 &&
          got.b == 0x40);

    // ---- Selection ----
    Selection empty;
    CHECK(empty.IsEmpty() && !empty.Contains(0, 0));
    CHECK(empty.Bounds().w == 0);

    Selection rect(Rect{2, 3, 4, 5});
    CHECK(!rect.IsEmpty());
    CHECK(rect.Contains(2, 3));
    CHECK(rect.Contains(5, 7));
    CHECK(!rect.Contains(6, 7)); // x+w = 6 不含
    CHECK(!rect.Contains(1, 3));
    CHECK(rect.Bounds().w == 4 && rect.Bounds().h == 5);

    // 蒙版选区
    std::vector<uint8_t> mask(4, 0);
    mask[1] = 255;
    Selection masked;
    masked.SetMask(2, 2, mask);
    CHECK(masked.HasMask() && !masked.IsEmpty());
    CHECK(!masked.Contains(0, 0));
    CHECK(masked.Contains(1, 0));
    CHECK(masked.Bounds().w == 2 && masked.Bounds().h == 2);

    std::cout << "  imagebuffer: RGBA8/RGBA16/Selection 全部通过\n";
    return true;
}

} // namespace sai2test
