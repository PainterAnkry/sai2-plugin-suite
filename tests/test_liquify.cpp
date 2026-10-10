// ============================================================================
// test_liquify.cpp - Phase 4 液化算法测试
//
// 关键性质：
//   - 零场渲染 = 原图（逆映射正确性）
//   - Bilinear：整数点精确、半像素插值正确
//   - Forward Warp：内容沿拖动方向移动
//   - Bloat：D 指向中心；Pinch：D 指向外侧
//   - Twirl：中心不动；Smooth：场更平滑；Reconstruct：可完全重建为零
// ============================================================================

#include "Tests.h"
#include "../../plugins/liquify/src/LiquifyCore.h"

namespace sai2test {

bool TestLiquify() {
    using namespace sai2;
    using namespace sai2::liquify;

    // ---- Bilinear：整数点精确 + 半像素插值 ----
    {
        ImageBuffer img(4, 4, PixelFormat::RGBA8);
        for (int x = 0; x < 4; ++x)
            for (int y = 0; y < 4; ++y)
                img.SetPixel(x, y, RGBA{uint16_t(x * 80),
                                        uint16_t(y * 80), 0, 255});
        RGBA c;
        SampleBilinear(img, 1.0f, 1.0f, c);
        CHECK(c.r == 80 && c.g == 80 && c.a == 255);
        SampleBilinear(img, 1.5f, 1.0f, c);
        CHECK(c.r == 120 && c.g == 80); // x 方向插值
        SampleBilinear(img, 1.0f, 1.5f, c);
        CHECK(c.r == 80 && c.g == 120); // y 方向插值
        // 越界 clamp
        SampleBilinear(img, -5.0f, 99.0f, c);
        CHECK(c.r == 0 && c.g == 240);
    }

    // ---- 零场渲染 = 原图 ----
    {
        ImageBuffer src(16, 16, PixelFormat::RGBA8);
        for (int i = 0; i < 16; ++i)
            src.SetPixel(i, i, RGBA{255, 0, 0, 255});
        DisplacementField field(16, 16);
        ImageBuffer dst;
        RenderLiquify(src, field, dst, nullptr);
        CHECK(dst.IsValid());
        bool same = true;
        for (int y = 0; y < 16 && same; ++y)
            for (int x = 0; x < 16 && same; ++x) {
                RGBA a, b;
                src.GetPixel(x, y, a);
                dst.GetPixel(x, y, b);
                if (a != b) same = false;
            }
        CHECK(same);
    }

    // ---- Forward Warp：内容沿拖动方向移动 ----
    {
        ImageBuffer src(64, 64, PixelFormat::RGBA8);
        src.Clear(RGBA{});
        src.SetPixel(20, 32, RGBA{255, 255, 255, 255});

        DisplacementField field(64, 64);
        field.ForwardWarp(32, 32, 20.0f, 1.0f, 0.0f, 8.0f); // 向右推
        ImageBuffer dst;
        RenderLiquify(src, field, dst, nullptr);

        // 原位置变暗，右侧新位置变亮
        RGBA c;
        CHECK(dst.GetPixel(20, 32, c) && c.r < 128);
        // 在 (20+位移) 附近寻找亮点
        int brightX = -1;
        for (int x = 22; x < 40; ++x) {
            RGBA p;
            dst.GetPixel(x, 32, p);
            if (p.r > 128) { brightX = x; break; }
        }
        CHECK(brightX > 22);
    }

    // ---- Bloat：D 指向中心（采样点向中心收 => 内容放大） ----
    {
        DisplacementField field(64, 64);
        field.Bloat(32, 32, 20.0f, 0.5f);
        // 中心右侧 10px 处：D.x 应为负（采样点向中心收）
        CHECK(field.Dx(42, 32) < -1.0f);
        // 中心上方 10px 处：D.y 应为正（+y 即朝向中心）
        CHECK(field.Dy(32, 22) > 1.0f);
        // 中心处位移为 0
        CHECK(field.Dx(32, 32) == 0.0f && field.Dy(32, 32) == 0.0f);
    }

    // ---- Pinch：D 指向外侧（与 Bloat 相反） ----
    {
        DisplacementField field(64, 64);
        field.Pinch(32, 32, 20.0f, 0.3f);
        CHECK(field.Dx(42, 32) > 1.0f);
        CHECK(field.Dy(32, 22) < -1.0f);
    }

    // ---- Twirl：中心不动，周围旋转 ----
    {
        DisplacementField field(64, 64);
        field.Twirl(32, 32, 20.0f, 1.5f);
        // 中心位移小
        const float cmag = std::sqrt(field.Dx(32, 32) * field.Dx(32, 32) +
                                     field.Dy(32, 32) * field.Dy(32, 32));
        CHECK(cmag < 0.5f);
        // 右侧点 (48,32)：逆时针旋转 -> 采样点移向下方 -> Dy > 0
        CHECK(field.Dy(48, 32) > 0.5f);
        // 场非零
        CHECK(field.MaxMagnitude() > 1.0f);
    }

    // ---- Smooth：场更平滑（最大值不增、能量减少） ----
    {
        DisplacementField field(32, 32);
        field.ForwardWarp(8, 8, 8.0f, 1.0f, 0.0f, 10.0f);
        field.ForwardWarp(24, 24, 8.0f, -1.0f, 0.0f, 10.0f);
        const float before = field.MaxMagnitude();
        field.Smooth(2);
        CHECK(field.MaxMagnitude() <= before + 0.001f);
    }

    // ---- Reconstruct：笔刷衰减部分恢复 + 全域重建完全归零 ----
    {
        DisplacementField field(64, 64);
        field.Twirl(32, 32, 24.0f, 1.0f);
        field.ForwardWarp(16, 16, 12.0f, 0.0f, 1.0f, 5.0f);
        const float before = field.MaxMagnitude();
        CHECK(before > 1.0f);
        field.Reconstruct(32, 32, 24.0f, 1.0f); // 笔刷重建：中心区域恢复
        CHECK(field.MaxMagnitude() < before);
        field.ReconstructAll(1.0f);             // 整体重建：完全归零
        CHECK(field.MaxMagnitude() < 0.001f);
    }

    // ---- 置换是连续的：平滑渐变经逆映射后仍平滑（bilinear 无锯齿跳变） ----
    {
        ImageBuffer src(32, 32, PixelFormat::RGBA8);
        src.Clear(RGBA{});
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                src.SetPixel(x, y, RGBA{uint16_t(x * 8), 0, 0, 255}); // 渐变
        DisplacementField field(32, 32);
        field.ForwardWarp(16, 16, 10.0f, 1.0f, 0.0f, 3.0f);
        ImageBuffer dst;
        RenderLiquify(src, field, dst, nullptr);
        // 中间行的水平剖面：相邻像素差应平滑（< 40）
        int row = 16, prev = -1;
        bool smooth = true;
        for (int x = 0; x < 32; ++x) {
            RGBA c;
            dst.GetPixel(x, row, c);
            if (prev >= 0 && std::abs(static_cast<int>(c.r) - prev) > 40)
                smooth = false;
            prev = c.r;
        }
        CHECK(smooth);
    }

    std::cout << "  liquify: Warp/Bloat/Pinch/Twirl/Smooth/Reconstruct 全部通过\n";
    return true;
}

} // namespace sai2test
