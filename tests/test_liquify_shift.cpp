// ============================================================================
// test_liquify_shift.cpp - 多图层"同一形变"的偏移采样正确性
//
// 需求：在 SAI2 里 Ctrl 多选图层后一起液化，要求**每一层被同一套形变处理**
//（几何完全一致），同时**分层结构不丢**。做法是把在某层局部坐标里建立的
// 置换场，按各层的画布偏移采样到别的图层上（shiftX/shiftY）。
//
// 这个测试要钉死的就是那条等式：
//     图层 i 局部 (u,v) ↔ 基准层局部 (u + shiftX, v + shiftY)   （同一画布点）
// 因此：把同一张画布切成两块当"两个图层"，用同一个场分别渲染，
//       在画布重叠区里，两层的输出必须逐像素一致。
//
// 关键回归价值：Dx/Dy 原本**没有边界检查**，带偏移后必然越界采样；
// 若有人把渲染改回 Dx/Dy，这里会立刻炸（或读到越界内存给出错值）。
// ============================================================================

#include "Tests.h"
#include "../../plugins/liquify/src/LiquifyCore.h"

namespace sai2test {

namespace {

// 确定性画布图案：渐变 + 几块色块，保证"同一点内容相同"是可判定的
sai2::ImageBuffer MakeCanvas(int w, int h) {
    sai2::ImageBuffer c(w, h, sai2::PixelFormat::RGBA8);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            sai2::RGBA p;
            p.r = static_cast<uint16_t>((x * 255) / (w - 1));
            p.g = static_cast<uint16_t>((y * 255) / (h - 1));
            p.b = static_cast<uint16_t>(((x ^ y) * 7) & 0xFF);
            p.a = 255;
            if ((x / 16 + y / 16) % 3 == 0) { p.r = 240; p.b = 16; }
            c.SetPixel(x, y, p);
        }
    }
    return c;
}

sai2::ImageBuffer SubRect(const sai2::ImageBuffer& src, int x0, int y0,
                          int w, int h) {
    sai2::ImageBuffer o(w, h, sai2::PixelFormat::RGBA8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            sai2::RGBA p;
            if (src.GetPixel(x0 + x, y0 + y, p)) o.SetPixel(x, y, p);
        }
    return o;
}

size_t CountDiff(const sai2::ImageBuffer& a, const sai2::ImageBuffer& b) {
    size_t n = 0;
    for (int y = 0; y < a.Height() && y < b.Height(); ++y)
        for (int x = 0; x < a.Width() && x < b.Width(); ++x) {
            sai2::RGBA pa, pb;
            a.GetPixel(x, y, pa);
            b.GetPixel(x, y, pb);
            if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b) ++n;
        }
    return n;
}

} // namespace

bool TestLiquifyShift() {
    using namespace sai2;
    using namespace sai2::liquify;

    const int CW = 160, CH = 160;
    const ImageBuffer canvas = MakeCanvas(CW, CH);

    // 两个"图层"= 同一画布的不同子矩形（内容天然一致，便于判定）
    const int ax = 10, ay = 10, aw = 100, ah = 100;   // 基准层
    const int bx = 40, by = 40, bw = 60, bh = 60;     // 第二个图层
    const ImageBuffer A = SubRect(canvas, ax, ay, aw, ah);
    const ImageBuffer B = SubRect(canvas, bx, by, bw, bh);

    // 在**基准层局部坐标**里建一个非均匀置换场（= 用户在图层1 上涂抹）
    DisplacementField field(aw, ah);
    field.Twirl(50.0f, 50.0f, 30.0f, 0.8f);
    field.ForwardWarp(20.0f, 70.0f, 25.0f, 1.0f, 0.0f, 6.0f);
    field.Smooth(1);
    CHECK(field.MaxMagnitude() > 0.5f);   // 场确实非零、非均匀

    ImageBuffer outA, outB;
    RenderLiquify(A, field, outA, nullptr, 0, 0);
    const int shiftX = bx - ax, shiftY = by - ay;   // 第二层相对基准层的偏移
    RenderLiquify(B, field, outB, nullptr, shiftX, shiftY);

    // ---- 核心断言：落在同一画布点上的像素，两层输出必须一致 ----
    //
    // 但只能比**两层共同的内部区域**：SampleBilinear 会把采样坐标夹到各自图像
    // 边界内，而每层只包含自己的像素 —— 靠近 A 的边缘时，A 夹到自己的边，
    // B 却还能采到"A 没有但那块画布上有"的内容，两边必然不同。
    // 这是分层表示的固有性质（层外无像素），不是偏移算错。
    // 余量取场的最大位移 + 2（覆盖双线性额外需要的 1 像素）。
    const int margin =
        static_cast<int>(std::ceil(field.MaxMagnitude())) + 2;
    size_t checked = 0, mismatch = 0, edgeSkipped = 0;
    for (int v = 0; v < bh; ++v) {
        for (int u = 0; u < bw; ++u) {
            const int au = u + shiftX, av = v + shiftY;
            if (au < 0 || av < 0 || au >= aw || av >= ah) continue;
            if (au < margin || av < margin || au >= aw - margin ||
                av >= ah - margin || u < margin || v < margin ||
                u >= bw - margin || v >= bh - margin) {
                ++edgeSkipped;   // 落在任一层的边界带内 → 不可比
                continue;
            }
            RGBA pa, pb;
            outA.GetPixel(au, av, pa);
            outB.GetPixel(u, v, pb);
            ++checked;
            if (pa.r != pb.r || pa.g != pb.g || pa.b != pb.b) ++mismatch;
        }
    }
    CHECK_MSG(checked > 1000,
              "内部可比区太小，测试没意义: checked=" + std::to_string(checked) +
                  " margin=" + std::to_string(margin));
    CHECK_MSG(mismatch == 0,
              "两层形变不一致（内部区域应在同一画布点上逐像素相同）: mismatch=" +
                  std::to_string(mismatch) + " / " + std::to_string(checked));

    // ---- 偏移必须真的起作用：用 shift=0 渲染同一层应当得到不同结果 ----
    ImageBuffer outB0;
    RenderLiquify(B, field, outB0, nullptr, 0, 0);
    CHECK_MSG(CountDiff(outB0, outB) > 100,
              "shift 未生效：带偏移与不带偏移结果几乎相同，diff=" +
                  std::to_string(CountDiff(outB0, outB)));

    // ---- 基准层行为不回归：shift=0 的渲染结果与原实现一致 ----
    // （原实现即 Dx/Dy 直接采样；在场范围内 Clamped 与它等价）
    size_t baseMismatch = 0;
    for (int y = 0; y < ah; ++y)
        for (int x = 0; x < aw; ++x) {
            RGBA c;
            SampleBilinear(A, x + field.Dx(x, y), y + field.Dy(x, y), c);
            RGBA got;
            outA.GetPixel(x, y, got);
            if (c.r != got.r || c.g != got.g || c.b != got.b) ++baseMismatch;
        }
    CHECK_MSG(baseMismatch == 0,
              "基准层渲染结果变了（不该动）: mismatch=" +
                  std::to_string(baseMismatch));

    // ---- 越界安全：偏移把场推到图外时不能崩、且该处应保持原像素 ----
    ImageBuffer outFar;
    RenderLiquify(B, field, outFar, nullptr, -100000, -100000);
    CHECK_MSG(CountDiff(outFar, B) == 0,
              "偏移完全出场外时应逐像素等于原图（无位移）");

    std::cout << "  双层同形变：内部 " << checked << " 像素全部一致（跳过边界带 " << edgeSkipped << "）；"
              << "shift 生效（diff=" << CountDiff(outB0, outB) << "）；"
              << "越界安全\n";
    return true;
}

} // namespace sai2test
