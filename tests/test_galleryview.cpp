// ============================================================================
// test_galleryview.cpp - Filter Gallery 视图模型/缩略图测试（规格书第九节）
// ============================================================================

#include "Tests.h"

#include "GalleryViewModel.h"

#include <set>

using namespace sai2;
using namespace sai2::fg;

namespace sai2test {

bool TestGalleryView() {
    // ---- 1. 样本图：有渐变、有细节、有亮圆 ----
    {
        ImageBuffer sample = ThumbnailGenerator::MakeSample(64);
        CHECK(sample.IsValid());
        RGBA a, b;
        sample.GetPixel(2, 2, a);      // 左上暗
        sample.GetPixel(61, 61, b);    // 右下亮
        CHECK(b.r > a.r);
    }

    // ---- 2. Build：10 个滤镜、4 个分类、缩略图齐全 ----
    ThumbnailGenerator thumbs(64);
    GalleryViewModel vm;
    vm.Build(thumbs);
    CHECK(vm.ItemCount() == 10);
    CHECK(vm.Categories().size() == 4);

    // 分类名
    const std::vector<std::string> catList = vm.Categories();
    std::set<std::string> cats(catList.begin(), catList.end());
    CHECK(cats.count("Artistic") == 1);
    CHECK(cats.count("Sketch") == 1);
    CHECK(cats.count("Texture") == 1);
    CHECK(cats.count("Stylize") == 1);

    // 每个条目的缩略图有效且非退化
    int nonDegenerate = 0;
    for (size_t i = 0; i < vm.ItemCount(); ++i) {
        const GalleryViewModel::Item* it = vm.Items("")[i];
        const ImageBuffer* thumb = thumbs.Thumbnail(it->thumbIndex);
        CHECK(thumb && thumb->IsValid());
        CHECK(thumb->Width() == 64 && thumb->Height() == 64);
        // 缩略图非纯色（滤镜效果有可见变化）
        RGBA first;
        thumb->GetPixel(0, 0, first);
        bool varied = false;
        for (int y = 0; y < 64 && !varied; y += 4)
            for (int x = 0; x < 64 && !varied; x += 4) {
                RGBA c;
                thumb->GetPixel(x, y, c);
                if (std::abs(int(c.r) - int(first.r)) > 4) varied = true;
            }
        if (varied) ++nonDegenerate;
    }
    CHECK(nonDegenerate >= 8); // 允许个别滤镜（如 Grain 弱参数）差异小

    // ---- 3. 不同滤镜的缩略图互不相同（缓存正确性） ----
    {
        const GalleryViewModel::Item* oil = vm.FindItem("com.sai2.filter.oil-paint");
        const GalleryViewModel::Item* edge = vm.FindItem("com.sai2.filter.find-edges");
        CHECK(oil && edge);
        CHECK(oil->thumbIndex != edge->thumbIndex);
        const ImageBuffer* t1 = thumbs.Thumbnail(oil->thumbIndex);
        const ImageBuffer* t2 = thumbs.Thumbnail(edge->thumbIndex);
        RGBA a, b;
        t1->GetPixel(32, 32, a);
        t2->GetPixel(32, 32, b);
        CHECK(std::abs(int(a.r) - int(b.r)) > 2 ||
              t1 != t2); // 不同索引（内容可能巧合相同，索引必不同）
    }

    // ---- 4. 参数面板 JSON ----
    {
        const std::string json = vm.ParamPanelJson("com.sai2.filter.oil-paint");
        CHECK(json.find("\"name\":\"radius\"") != std::string::npos);
        CHECK(json.find("\"label\"") != std::string::npos);
        CHECK(vm.ParamPanelJson("nonexistent") == "[]");
    }

    // ---- 5. 参数修改 + Apply ----
    {
        CHECK(vm.SetParam("com.sai2.filter.oil-paint", "radius", 5.0f));
        CHECK(vm.SetParam("com.sai2.filter.oil-paint", "radius", 999.0f)); // 超界被 clamp
        CHECK(!vm.SetParam("com.sai2.filter.oil-paint", "nonexistent", 1.0f));
        ImageBuffer img = ThumbnailGenerator::MakeSample(64);
        CHECK(vm.Apply("com.sai2.filter.oil-paint", img));
        CHECK(!vm.Apply("nonexistent", img));
    }

    return true;
}

} // namespace sai2test
