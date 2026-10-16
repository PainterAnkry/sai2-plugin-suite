// ============================================================================
// test_gallery_quality.cpp - 47 个滤镜的"质量"度量（不只是"有没有变像素"）
//
// 背景：原来只断言"改过像素 + 可复现"，于是"整体变成黑 / 几乎没变化"这
// 类实现缺陷照样通过（用户实测反馈："很多滤镜都没效果，颜色还都变成黑了"）。
// 这里对**彩色实心图形**统计两个量：
//   平均色差 mean   —— 太小说明几乎没效果
//   变黑比例 blkOn  —— 太大说明把画面糊成黑的了
// 先只打印可疑清单（便于逐个修），并把"整体黑"定为硬失败（这是明显缺陷）。
// ============================================================================

#include "Tests.h"

#include "GalleryFilters.h"
#include "GallerySpecs.h"

#include <cmath>
#include <string>
#include <vector>

namespace sai2test {

bool TestGalleryQuality() {
    using namespace sai2;
    using namespace sai2::fg;

    int count = 0;
    const GallerySpec* specs = GallerySpecs(count);
    CHECK(count == 47);

    // 彩色实心椭圆（内部有色相/明暗变化），其余透明 —— 接近真实图层内容
    ImageBuffer base(96, 64, PixelFormat::RGBA8);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 96; ++x) {
            const float dx = static_cast<float>(x) - 48.0f;
            const float dy = static_cast<float>(y) - 32.0f;
            RGBA c{0, 0, 0, 0};
            if (dx * dx / 900.0f + dy * dy / 400.0f < 1.0f) {
                c.r = static_cast<uint16_t>(170 + x / 2);
                c.g = static_cast<uint16_t>(50 + y);
                c.b = static_cast<uint16_t>(70 + (x + y) / 3);
                c.a = 255;
            }
            base.SetPixel(x, y, c);
        }
    }

    struct Row { std::string key; double mean; double blk; };
    std::vector<Row> rows;
    int hardBlack = 0;
    for (int i = 0; i < count; ++i) {
        GalleryFilterBase filt(specs[i], GalleryApplyAll);
        ImageBuffer a = base.Clone();
        if (!filt.Apply(a, nullptr)) {
            rows.push_back({specs[i].key, -1.0, -1.0});
            continue;
        }
        double sum = 0;
        int opaque = 0, black = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 96; ++x) {
                RGBA p, q;
                base.GetPixel(x, y, p);
                a.GetPixel(x, y, q);
                if (p.a == 0) continue;
                ++opaque;
                sum += (std::abs(int(p.r) - int(q.r)) + std::abs(int(p.g) - int(q.g)) +
                        std::abs(int(p.b) - int(q.b))) / 3.0;
                if (q.r < 24 && q.g < 24 && q.b < 24) ++black;
            }
        const double mean = opaque ? sum / opaque : 0.0;
        const double blk = opaque ? double(black) / opaque : 0.0;
        rows.push_back({specs[i].key, mean, blk});
        if (blk > 0.95) ++hardBlack; // 整个实心区全黑：明显缺陷
    }

    // 打印全部（按可疑程度排序：先几乎无变化，再大部分变黑）
    std::cout << "  滤镜质量表（平均色差 / 变黑比例）\n";
    int noEffect = 0, mostlyBlack = 0;
    for (const auto& r : rows) {
        const bool noEff = (r.mean >= 0.0 && r.mean < 1.5);
        const bool blkOK = (r.blk > 0.75);
        if (noEff) ++noEffect;
        if (blkOK) ++mostlyBlack;
        std::cout << (noEff || blkOK ? "    [!] " : "        ") << r.key
                  << "  mean=" << r.mean << "  black=" << (r.blk * 100) << "%\n";
    }
    std::cout << "  几乎无变化: " << noEffect << " 个；大部分变黑: " << mostlyBlack
              << " 个；全黑: " << hardBlack << " 个\n";

    // 常驻回归：整块变黑与"几乎无效果"都必须为 0。
    // （修完阈值族偏黑 + posterEdges/watercolor 的整数除法 + sponge/craquelure
    //   幅度之后，47 个全部通过。）
    CHECK(hardBlack == 0);
    CHECK(noEffect == 0);
    return true;
}

} // namespace sai2test
