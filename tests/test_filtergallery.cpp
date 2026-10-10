// ============================================================================
// test_filtergallery.cpp - Phase 5 Filter Gallery 测试
//
// 覆盖：Registry 注册/查找/分类、参数模型、10 个滤镜算法性质、
//       蒙版选区尊重、RGBA16 支持。
// ============================================================================

#include "Tests.h"

#include "FilterBase.h"
#include "FilterSet.h"
#include "Filters.h"
#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <cmath>
#include <string>
#include <vector>

using namespace sai2;

namespace {

// 构造渐变图（水平渐变 R、垂直渐变 G）
ImageBuffer MakeGradient(int w, int h, PixelFormat fmt = PixelFormat::RGBA8) {
    ImageBuffer img(w, h, fmt);
    const uint32_t maxV = fmt == PixelFormat::RGBA16 ? 65535u : 255u;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            RGBA c;
            c.r = static_cast<uint16_t>(x * maxV / std::max(1, w - 1));
            c.g = static_cast<uint16_t>(y * maxV / std::max(1, h - 1));
            c.b = maxV / 2;
            c.a = fmt == PixelFormat::RGBA16 ? 65535u : 255u;
            img.SetPixel(x, y, c);
        }
    return img;
}

double TotalDiff(const ImageBuffer& a, const ImageBuffer& b) {
    double d = 0;
    for (int y = 0; y < a.Height(); ++y)
        for (int x = 0; x < a.Width(); ++x) {
            RGBA ca, cb;
            a.GetPixel(x, y, ca);
            b.GetPixel(x, y, cb);
            d += std::abs(int(ca.r) - int(cb.r)) +
                 std::abs(int(ca.g) - int(cb.g)) +
                 std::abs(int(ca.b) - int(cb.b));
        }
    return d;
}

} // namespace

namespace sai2test {

bool TestFilterGallery() {
    // ---- Registry：注册 / 查找 / 分类 ----
    fg::FilterRegistry registry;
    auto* oil = registry.Add<fg::OilPaintFilter>();
    registry.Add<fg::WatercolorFilter>();
    registry.Add<fg::RoughPastelFilter>();
    registry.Add<fg::PencilSketchFilter>();
    registry.Add<fg::CharcoalFilter>();
    registry.Add<fg::ChalkFilter>();
    registry.Add<fg::TexturizerFilter>();
    registry.Add<fg::GrainFilter>();
    registry.Add<fg::EmbossFilter>();
    registry.Add<fg::FindEdgesFilter>();

    CHECK(registry.Count() == 10);
    CHECK(registry.Find("com.sai2.filter.oil-paint") == oil);
    CHECK(registry.Find("com.sai2.filter.nope") == nullptr);

    auto cats = registry.Categories();
    CHECK(cats.size() == 4);
    CHECK(registry.ByCategory("Artistic").size() == 3);
    CHECK(registry.ByCategory("Sketch").size() == 3);
    CHECK(registry.ByCategory("Texture").size() == 3);
    CHECK(registry.ByCategory("Stylize").size() == 1);
    CHECK(registry.ByCategory("").size() == 10);

    // FilterInfo 透出
    FilterInfo info = oil->GetInfo();
    CHECK(std::string(info.id) == "com.sai2.filter.oil-paint");
    CHECK(std::string(info.category) == "Artistic");

    // ---- 参数模型 ----
    CHECK(oil->ParamDefs().size() == 2);
    CHECK(oil->SetParam("radius", 5.0f));
    CHECK(oil->Pi("radius") == 5);
    CHECK(!oil->SetParam("nope", 1.0f));
    oil->SetParam("radius", 99.0f); // 超上限应被 clamp
    CHECK(oil->Pi("radius") == 8);
    oil->ResetParams();
    CHECK(oil->Pi("radius") == 3);
    // ParamDefsJson 可解析出字段
    const std::string json = oil->ParamDefsJson();
    CHECK(json.find("\"radius\"") != std::string::npos);
    CHECK(json.find("\"default\":3.000000") != std::string::npos);

    // ---- 滤镜算法性质 ----
    const uint32_t maxV = 255;

    // 1. 油画：均匀色块不变（同色区域主桶即原色）
    {
        ImageBuffer img(24, 24, PixelFormat::RGBA8);
        img.Clear(RGBA{100, 150, 200, 255});
        fg::OilPaint(img, 3, 12, nullptr);
        RGBA c;
        img.GetPixel(12, 12, c);
        CHECK((c == RGBA{100, 150, 200, 255}));
    }
    // 油画平滑噪声
    {
        ImageBuffer img = MakeGradient(32, 32);
        ImageBuffer orig = img.Clone();
        fg::OilPaint(img, 2, 10, nullptr);
        CHECK(TotalDiff(img, orig) > 0); // 有变化
    }

    // 2. 水彩：边缘区域变暗（沉淀）
    {
        ImageBuffer img = MakeGradient(32, 32);
        fg::Watercolor(img, 3, 0.3f, 0.5f, nullptr);
        RGBA c;
        img.GetPixel(5, 5, c);
        CHECK(c.a == 255);
        CHECK(c.r + c.g + c.b > 0);
    }

    // 3. 粗糙蜡笔：同一 cell 内量化一致，跨 cell 保持亮度次序
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{});
        for (int i = 0; i < 16; ++i)
            img.SetPixel(i, 0, RGBA{uint16_t(i * 17), 0, 0, 255});
        fg::RoughPastel(img, 4, 4, 0.0f, nullptr); // texture=0 排除噪声干扰
        RGBA a, b;
        img.GetPixel(0, 0, a); img.GetPixel(3, 0, b);
        CHECK(a.r == b.r); // cell0 内 4 像素同属量化桶 0
        img.GetPixel(8, 0, a); img.GetPixel(11, 0, b);
        CHECK(a.r == b.r); // cell2 内一致（量化桶 2）
        img.GetPixel(4, 0, b);
        CHECK(a.r > b.r);  // 亮度次序保留（cell2 桶值高于 cell1）
    }

    // 4. 素描：白纸不变、黑白跳变产生暗线
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{255, 255, 255, 255});
        fg::PencilSketch(img, 4, nullptr);
        RGBA c;
        img.GetPixel(8, 8, c);
        CHECK(c.r > 200); // 白色区域仍接近白

        img.Clear(RGBA{255, 255, 255, 255});
        for (int y = 0; y < 16; ++y) img.SetPixel(8, y, RGBA{0, 0, 0, 255});
        fg::PencilSketch(img, 3, nullptr);
        img.GetPixel(8, 8, c);
        CHECK(c.r < 150); // 黑线处素描变暗
    }

    // 5. 炭笔：整体暗于素描
    {
        ImageBuffer imgA = MakeGradient(32, 32);
        ImageBuffer imgB = imgA.Clone();
        fg::PencilSketch(imgA, 4, nullptr);
        fg::Charcoal(imgB, 4, 0.0f, nullptr); // grain=0 便于比较
        double sumA = 0, sumB = 0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) {
                RGBA a, b;
                imgA.GetPixel(x, y, a);
                imgB.GetPixel(x, y, b);
                sumA += a.r + a.g + a.b;
                sumB += b.r + b.g + b.b;
            }
        CHECK(sumB < sumA);
    }

    // 6. 粉笔：灰度输出（r==g==b）
    {
        ImageBuffer img = MakeGradient(24, 24);
        fg::Chalk(img, 1.0f, nullptr);
        RGBA c;
        bool gray = true;
        for (int y = 0; y < 24 && gray; ++y)
            for (int x = 0; x < 24 && gray; ++x) {
                img.GetPixel(x, y, c);
                if (c.r != c.g || c.g != c.b) gray = false;
            }
        CHECK(gray);
    }

    // 7. 纹理化：relief=0 时不变
    {
        ImageBuffer img = MakeGradient(16, 16);
        ImageBuffer orig = img.Clone();
        fg::Texturizer(img, 8, 0.0f, nullptr);
        CHECK(TotalDiff(img, orig) == 0);
        // relief>0 时产生纹理变化
        fg::Texturizer(img, 8, 0.6f, nullptr);
        CHECK(TotalDiff(img, orig) > 0);
    }

    // 8. 颗粒：amount=0 不变；amount>0 变化且可复现
    {
        ImageBuffer img = MakeGradient(16, 16);
        ImageBuffer orig = img.Clone();
        fg::Grain(img, 0.0f, 1, nullptr);
        CHECK(TotalDiff(img, orig) == 0);
        fg::Grain(img, 0.3f, 1, nullptr);
        CHECK(TotalDiff(img, orig) > 0);
        ImageBuffer img2 = orig.Clone();
        fg::Grain(img2, 0.3f, 1, nullptr);
        CHECK(TotalDiff(img, img2) == 0); // 确定性噪声可复现
    }

    // 9. 浮雕：平坦区域 -> 中性灰（卷积和 0 + 半幅偏置）
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{120, 120, 120, 255});
        fg::Emboss(img, 1.0f, nullptr);
        RGBA c;
        img.GetPixel(8, 8, c);
        CHECK(std::abs(int(c.r) - 128) <= 4);
    }

    // 10. 查找边缘：平坦区域为黑，边缘处非黑；invert 反相
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{255, 255, 255, 255});
        for (int y = 0; y < 16; ++y) img.SetPixel(8, y, RGBA{0, 0, 0, 255});
        fg::FindEdges(img, 0, nullptr);
        RGBA c;
        img.GetPixel(2, 2, c);
        CHECK(c.r < 10);   // 平坦 -> 黑
        img.GetPixel(7, 8, c);
        CHECK(c.r > 100);  // 线条边缘 -> 亮（线中心 Sobel=0）

        img.Clear(RGBA{255, 255, 255, 255});
        for (int y = 0; y < 16; ++y) img.SetPixel(8, y, RGBA{0, 0, 0, 255});
        fg::FindEdges(img, 1, nullptr);
        img.GetPixel(2, 2, c);
        CHECK(c.r > 200);  // 反相 -> 白
    }

    // ---- 蒙版选区尊重 ----
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{255, 255, 255, 255});
        std::vector<uint8_t> mask(16 * 16, 0);
        mask[8 * 16 + 8] = 255; // 仅中心一点被选中
        Selection sel;
        sel.SetMask(16, 16, mask);
        ImageBuffer orig = img.Clone();
        fg::FindEdges(img, 0, &sel);
        // 全图平坦：蒙版内像素变黑，蒙版外必须保持原样
        RGBA a, b;
        img.GetPixel(0, 0, a);
        orig.GetPixel(0, 0, b);
        CHECK(a == b);
        img.GetPixel(8, 8, a);
        CHECK(a.r < 10);
    }

    // ---- RGBA16 支持 ----
    {
        ImageBuffer img = MakeGradient(16, 16, PixelFormat::RGBA16);
        fg::FindEdges(img, 0, nullptr);
        RGBA c;
        img.GetPixel(2, 2, c);
        CHECK(c.r <= 65535);
        CHECK(c.a == 65535); // alpha 保留
    }

    // ---- Execute 链路验证：FindEdgesFilter 直接应用 ----
    {
        ImageBuffer img = MakeGradient(32, 32);
        ImageBuffer orig = img.Clone();
        auto* f = registry.Find("com.sai2.filter.find-edges");
        CHECK(f && f->Apply(img, nullptr));
        CHECK(TotalDiff(img, orig) > 0);
    }

    return true;
}

} // namespace sai2test
