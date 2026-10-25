// ============================================================================
// test_blur.cpp - Phase 3 模糊算法测试
//
// 关键性质：
//   - Gaussian：冲量能量守恒 + 对称性（可分离卷积正确性）
//   - Box：均匀图像不变
//   - Motion：冲量沿方向扩散、垂直方向不扩散
//   - Radial：中心不动、均匀图像不变
// ============================================================================

#include "Tests.h"
#include "../../plugins/blur/src/BlurKernels.h"

namespace sai2test {

bool TestBlur() {
    using namespace sai2;
    using namespace sai2::blur;

    // ---- Gaussian：冲量响应能量守恒 + 对称 ----
    {
        ImageBuffer img(41, 41, PixelFormat::RGBA8);
        img.Clear(RGBA{});
        img.SetPixel(20, 20, RGBA{255, 255, 255, 255});

        GaussianBlur(img, 3.0f, nullptr);

        // 能量守恒：R 通道总和接近 255*41*41 的冲量总量（=255*1 个像素）
        // 用亮度总和检验
        double sum = 0;
        for (int y = 0; y < 41; ++y)
            for (int x = 0; x < 41; ++x) {
                RGBA c;
                img.GetPixel(x, y, c);
                sum += c.r;
            }
        CHECK(sum > 250.0 * 0.95 && sum < 255.0 * 1.05); // 255 ± 5%

        // 对称性：冲量在中心，模糊结果水平/垂直对称
        RGBA a, b;
        CHECK(img.GetPixel(18, 20, a) && img.GetPixel(22, 20, b) && a == b);
        CHECK(img.GetPixel(20, 18, a) && img.GetPixel(20, 22, b) && a == b);

        // 峰值仍在冲量位置：2D 高斯峰值理论值 255/(2πσ²) ≈ 19（σ=1.5）
        CHECK(img.GetPixel(20, 20, a) && a.r >= 15);
        CHECK(img.GetPixel(18, 20, a) && img.GetPixel(20, 20, b) &&
              a.r < b.r); // 峰值高于邻域
    }

    // ---- Gaussian：均匀图像不变 ----
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{80, 120, 160, 255});
        GaussianBlur(img, 4.0f, nullptr);
        RGBA c;
        CHECK(img.GetPixel(5, 5, c) && c.r == 80 && c.g == 120 && c.b == 160 &&
              c.a == 255);
    }

    // ---- Box：均匀图像不变（任意半径） ----
    {
        ImageBuffer img(20, 20, PixelFormat::RGBA16);
        img.Clear(RGBA{30000, 30000, 30000, 65535});
        BoxBlur(img, 5.0f, nullptr);
        RGBA c;
        CHECK(img.GetPixel(10, 10, c) && c.r == 30000 && c.a == 65535);
    }

    // ---- Motion：冲量沿方向扩散，垂直方向不扩散 ----
    {
        ImageBuffer img(31, 31, PixelFormat::RGBA8);
        img.Clear(RGBA{});
        img.SetPixel(15, 15, RGBA{255, 255, 255, 255});

        MotionBlur(img, 0.0f, 6.0f, 7, nullptr); // 水平向右

        // 水平方向：同一行相邻像素变亮
        RGBA c;
        CHECK(img.GetPixel(17, 15, c) && c.r > 20);
        // 垂直方向：邻行仍然接近 0（距离足够远）
        CHECK(img.GetPixel(15, 12, c) && c.r < 10);
        // 能量守恒
        double sum = 0;
        for (int y = 0; y < 31; ++y)
            for (int x = 0; x < 31; ++x) {
                RGBA p;
                img.GetPixel(x, y, p);
                sum += p.r;
            }
        CHECK(sum > 240 && sum < 270);
    }

    // ---- Radial：均匀图像不变；非中心像素被旋转涂抹 ----
    {
        ImageBuffer img(32, 32, PixelFormat::RGBA8);
        img.Clear(RGBA{100, 100, 100, 255});
        RadialBlur(img, 16, 16, 0.5f, 0, 12, nullptr);
        RGBA c;
        CHECK(img.GetPixel(8, 8, c) && c.r == 100);

        ImageBuffer img2(32, 32, PixelFormat::RGBA8);
        img2.Clear(RGBA{});
        img2.SetPixel(24, 16, RGBA{255, 255, 255, 255}); // 中心右侧 8px
        RadialBlur(img2, 16, 16, 2.0f, 0, 12, nullptr);     // 加大旋转涂抹角
        // 旋转涂抹后，原位置变暗，圆弧附近其他位置变亮
        CHECK(img2.GetPixel(24, 16, c) && c.r < 200);
        int bright = 0;
        for (int y = 10; y < 23; ++y)
            for (int x = 18; x < 31; ++x) {
                RGBA p;
                img2.GetPixel(x, y, p);
                if (p.r > 15) ++bright; // 单次采样贡献 ≈ 255/12 ≈ 21
            }
        CHECK(bright > 3); // 涂抹开了
    }

    // ---- Selection：只影响选区内 ----
    {
        ImageBuffer img(32, 32, PixelFormat::RGBA8);
        img.Clear(RGBA{0});
        img.SetPixel(16, 16, RGBA{255, 255, 255, 255});
        Selection sel(Rect{0, 0, 8, 8}); // 左上角，远离冲量
        GaussianBlur(img, 3.0f, &sel);
        RGBA c;
        CHECK(img.GetPixel(16, 16, c) && c.r == 255); // 选区外像素未动
    }

    // ---- 统一入口分发 ----
    {
        ImageBuffer img(8, 8, PixelFormat::RGBA8);
        img.Clear(RGBA{50, 50, 50, 255});
        BlurParams p;
        p.kind = BlurKind::Box;
        p.radius = 2.0f;
        ApplyBlur(img, p, nullptr);
        RGBA c;
        CHECK(img.GetPixel(4, 4, c) && c.r == 50); // 均匀不变
    }

    std::cout << "  blur: Gaussian/Box/Motion/Radial/Selection 全部通过\n";
    return true;
}

} // namespace sai2test
