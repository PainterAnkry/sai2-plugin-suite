// ============================================================================
// test_gallery_specs.cpp - 滤镜库移植（1/6）的规格表 + 参数记法解析器测试
//
// 移植自 photocraft crates/algo/src/artistic.rs 的 47 个 Photoshop 滤镜。
// 这里只验证"规格数据与解析"这一层（算法逐个实现时再各自加测试）：
//   - 47 条规格、6 个分类的数量与源一致
//   - 每条规格的参数记法都能解析，且**没有**结构性错误
//     （只允许 skip-json：颜色类参数本插件暂无控件）
//   - 若干条目的具体数值（含负数范围、布尔、枚举默认项）
// 生成器：plugins/filter-gallery/tools/gen_gallery_specs.py
// ============================================================================

#include "Tests.h"

#include "GalleryParams.h"
#include "GalleryFilters.h"
#include "GallerySpecs.h"

#include <map>
#include <string>

namespace sai2test {

bool TestGallerySpecs() {
    using namespace sai2;      // ImageBuffer / RGBA / PixelFormat
    using namespace sai2::fg;  // 滤镜与规格表

    int count = 0;
    const GallerySpec* specs = GallerySpecs(count);
    CHECK(count == kGallerySpecCount);
    CHECK(count == 47); // Photoshop 滤镜库总数（源文件一致）

    // ---- 分类数量与源一致 ----
    std::map<std::string, int> cats;
    for (int i = 0; i < count; ++i) cats[specs[i].category] += 1;
    CHECK(cats.size() == 6);
    CHECK(cats["Artistic"] == 15);
    CHECK(cats["Brush Strokes"] == 8);
    CHECK(cats["Distort"] == 3);
    CHECK(cats["Sketch"] == 14);
    CHECK(cats["Stylize"] == 1);
    CHECK(cats["Texture"] == 6);

    // ---- 每条规格的 key/显示名非空且不重复 ----
    std::map<std::string, int> keys;
    for (int i = 0; i < count; ++i) {
        CHECK(specs[i].key && specs[i].key[0]);
        CHECK(specs[i].name && specs[i].name[0]);
        keys[specs[i].key] += 1;
    }
    CHECK(static_cast<int>(keys.size()) == count); // key 唯一

    // ---- 全部参数记法可解析，且无结构性错误 ----
    int totalParams = 0;
    int totalSkippedJson = 0;
    int rangeCount = 0, choiceCount = 0, boolCount = 0;
    for (int i = 0; i < count; ++i) {
        std::string diag;
        const std::vector<GalleryParamSpec> ps =
            ParseGalleryParamsDiag(specs[i].params, &diag);
        // 只允许 skip-json:xxx；出现 bad-/missing-/unterminated 就是解析器或
        // 记法有问题，必须修（不能悄悄少一个参数）
        CHECK(diag.find("bad-") == std::string::npos);
        CHECK(diag.find("missing-") == std::string::npos);
        CHECK(diag.find("unterminated") == std::string::npos);
        CHECK(!ps.empty());
        totalParams += static_cast<int>(ps.size());
        size_t pos = 0;
        while ((pos = diag.find("skip-json:", pos)) != std::string::npos) {
            ++totalSkippedJson;
            pos += 10;
        }
        for (const auto& p : ps) {
            CHECK(!p.key.empty());
            CHECK(p.label == p.key); // 暂未本地化：显示名 = key
            if (p.kind == GalleryParamSpec::Range) {
                ++rangeCount;
                CHECK(p.min <= p.max);
                CHECK(p.def >= p.min && p.def <= p.max);
            } else if (p.kind == GalleryParamSpec::Choice) {
                ++choiceCount;
                CHECK(!p.choices.empty());
                CHECK(p.choiceDefault == 0); // 源注释：第一个选项为默认
            } else {
                ++boolCount;
            }
        }
    }
    CHECK(totalParams + totalSkippedJson > 60); // 参数总量量级
    CHECK(rangeCount > 40);
    CHECK(choiceCount > 5);
    CHECK(boolCount >= 1);

    // ---- 抽查具体条目的数值 ----
    const GallerySpec* cp = nullptr;
    const GallerySpec* tx = nullptr;
    const GallerySpec* ng = nullptr;
    for (int i = 0; i < count; ++i) {
        const std::string k = specs[i].key;
        if (k == "coloredPencil") cp = &specs[i];
        if (k == "texturizer") tx = &specs[i];
        if (k == "neonGlow") ng = &specs[i];
    }
    CHECK(cp && tx && ng);

    // coloredPencil: pencilWidth 1..24=4（整数）
    {
        const auto ps = ParseGalleryParams(cp->params);
        CHECK(ps.size() == 3);
        CHECK(ps[0].key == "pencilWidth");
        CHECK(ps[0].kind == GalleryParamSpec::Range);
        CHECK(ps[0].min == 1.0f && ps[0].max == 24.0f && ps[0].def == 4.0f);
        CHECK(ps[0].isInt);
    }
    // texturizer: 枚举 texture(4 项) + scaling 50..200=100 + relief + light(8 项) + bool
    {
        const auto ps = ParseGalleryParams(tx->params);
        CHECK(ps.size() == 5);
        CHECK(ps[0].key == "texture");
        CHECK(ps[0].kind == GalleryParamSpec::Choice);
        CHECK(ps[0].choices.size() == 4);
        CHECK(ps[0].choices[0] == "canvas"); // 第一个是默认
        CHECK(ps[1].key == "scaling");
        CHECK(ps[1].min == 50.0f && ps[1].max == 200.0f && ps[1].def == 100.0f);
        CHECK(ps[3].key == "light");
        CHECK(ps[3].kind == GalleryParamSpec::Choice);
        CHECK(ps[3].choices.size() == 8);
        CHECK(ps[4].key == "invert");
        CHECK(ps[4].kind == GalleryParamSpec::Bool);
    }
    // neonGlow: 负下界 -24..24=5；glowColor 是 json → 跳过
    {
        std::string diag;
        const auto ps = ParseGalleryParamsDiag(ng->params, &diag);
        CHECK(ps.size() == 2);
        CHECK(ps[0].key == "glowSize");
        CHECK(ps[0].min == -24.0f && ps[0].max == 24.0f && ps[0].def == 5.0f);
        CHECK(diag.find("skip-json:glowColor") != std::string::npos);
    }

    // ---- 已实现算法（Distort 批）：确实改像素 + 确定性 + 未实现必须失败 ----
    {
        ImageBuffer base(64, 40, PixelFormat::RGBA8);
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < 64; ++x) {
                RGBA c;
                c.r = static_cast<uint16_t>(x * 4);
                c.g = static_cast<uint16_t>(y * 6);
                c.b = static_cast<uint16_t>((x + y) * 3);
                c.a = 255;
                base.SetPixel(x, y, c);
            }
        }
        auto changed = [](const ImageBuffer& a, const ImageBuffer& b) {
            int n = 0;
            for (int y = 0; y < a.Height(); ++y)
                for (int x = 0; x < a.Width(); ++x) {
                    RGBA p, q;
                    a.GetPixel(x, y, p);
                    b.GetPixel(x, y, q);
                    if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a) ++n;
                }
            return n;
        };
        const char* impl[] = {"oceanRipple", "glass", "diffuseGlow",
                              "mosaicTiles", "patchwork", "stainedGlass",
                              "craquelure", "grain", "texturizer",
                              "glowingEdges",
                              // Sketch 批
                              "basRelief", "graphicPen", "halftonePattern",
                              "photocopy", "stamp", "tornEdges",
                              "reticulation", "waterPaper"};
        for (const char* want : impl) {
            const GallerySpec* sp = nullptr;
            for (int i = 0; i < count; ++i)
                if (std::string(specs[i].key) == want) sp = &specs[i];
            CHECK(sp != nullptr);
            GalleryFilterBase filt(*sp, GalleryApplyAll);
            // 参数默认值来自记法；glass 的 distortion 默认 5、oceanRipple 幅度 9
            ImageBuffer a = base.Clone();
            CHECK(filt.Apply(a, nullptr));
            CHECK(changed(base, a) > 0); // 必须真的改了像素
            // 确定性：同样输入两次结果逐字节一致
            ImageBuffer b = base.Clone();
            CHECK(filt.Apply(b, nullptr));
            CHECK(changed(a, b) == 0);
        }
        // 未实现的 key：必须返回 false（不能静默"成功但什么也没做"）
        const GallerySpec* cut = nullptr;
        for (int i = 0; i < count; ++i)
            if (std::string(specs[i].key) == "cutout") cut = &specs[i];
        CHECK(cut != nullptr);
        GalleryFilterBase notImpl(*cut, GalleryApplyAll);
        ImageBuffer c2 = base.Clone();
        CHECK(!notImpl.Apply(c2, nullptr));

        // 规格表驱动的滤镜：id/显示名/分类/参数都来自规格表
        const GallerySpec* gl = nullptr;
        for (int i = 0; i < count; ++i)
            if (std::string(specs[i].key) == "glass") gl = &specs[i];
        GalleryFilterBase gf(*gl, GalleryApplyAll);
        CHECK(std::string(gf.GetInfo().id) == "com.sai2.filter.gallery.glass");
        CHECK(std::string(gf.GetInfo().category) == "Distort");
        CHECK(gf.ParamDefs().size() == ParseGalleryParams(gl->params).size());
    }

    std::cout << "  47 条规格 / 6 分类  OK；参数 " << totalParams << " 个"
              << "（range " << rangeCount << ", choice " << choiceCount
              << ", bool " << boolCount << "），跳过 json " << totalSkippedJson
              << " 个；已实现算法 18 个（Distort 3 + Texture 6 + Stylize 1 + Sketch 8）\n";
    return true;
}

} // namespace sai2test
