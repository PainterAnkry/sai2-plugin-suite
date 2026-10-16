#pragma once
// ============================================================================
// GallerySketch.h - Sketch（素描）批的第二组算法
//
// 本批：粉笔和炭笔 / 炭笔 / 铬黄 / 炭精笔 / 便条纸 / 塑料效果
// 骨架与 GalleryFilters.h 里的 Sketch 批一致：
//   亮度 → （可选模糊）→ 色阶/阈值/纹理浮雕 → 前景·背景两色映射
// 记法里的 foreground/background 是 json（颜色），本插件暂无取色控件 →
// 用默认黑前景 / 白背景（与 Photoshop 默认一致）。
//
// 单独成文件的原因见 GalleryArtistic.h 顶部注释（避免用注释当锚点插入代码）。
// ============================================================================

#include "Filters.h"
#include "GalleryFilters.h"

namespace sai2::fg {

inline bool GalleryApplySketchExtra(const char* key, ImageBuffer& img,
                                    const Selection* sel,
                                    const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float sc = static_cast<float>(maxV) / 255.0f;
    const Rect roi = FilterRoi(img, sel);
    if (roi.w <= 0 || roi.h <= 0) return true;

    auto luma01 = [&](const ImageBuffer& s2, int x, int y) {
        return Luma(SampleClamp(s2, x, y)) / (255.0f * sc);
    };
    auto setV = [&](int x, int y, float v, uint16_t a) {
        RGBA o;
        const float c = std::min(static_cast<float>(maxV), std::max(0.0f, v * maxV));
        o.r = o.g = o.b = static_cast<uint16_t>(c);
        o.a = a;
        img.SetPixel(x, y, o);
    };
    auto alphaAt = [&](int x, int y) {
        RGBA c;
        img.GetPixel(x, y, c);
        return c.a;
    };
    auto lightDir = [&](int light, float& lx, float& ly) {
        const float a = 6.2831853f * static_cast<float>(light) / 8.0f;
        lx = std::cos(a);
        ly = std::sin(a);
    };

    // ---- 粉笔和炭笔：暗部走炭笔（深+噪），亮部走粉笔（浅+噪）----
    if (k == "chalkCharcoal") {
        const float chArea = static_cast<float>(f.Pi("charcoalArea")) / 20.0f;
        const float ckArea = static_cast<float>(f.Pi("chalkArea")) / 20.0f;
        const float press = static_cast<float>(f.Pi("strokePressure")) / 5.0f;
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float lum = luma01(img, x, y);
                const float n = Hash01(x, y, 83u) * 2.0f - 1.0f;
                float v;
                if (lum < 0.5f) {                 // 炭笔区
                    const float d = (0.5f - lum) * 2.0f;
                    v = 0.15f * (1.0f - chArea) + d * 0.25f +
                        n * 0.18f * (1.0f - press * 0.4f);
                } else {                          // 粉笔区
                    const float d = (lum - 0.5f) * 2.0f;
                    v = 0.75f + d * 0.2f * ckArea + n * 0.12f;
                }
                setV(x, y, v, alphaAt(x, y));
            }
        return true;
    }

    // ---- 炭笔：模糊后的暗部阈值化成粗线条 ----
    if (k == "charcoal") {
        const int thick = std::max(1, f.Pi("charcoalThickness"));
        const float detail = static_cast<float>(f.Pi("detail")) / 5.0f;
        const float bal = f.P("lightDarkBalance") / 100.0f;
        ImageBuffer src = img.Clone();
        if (thick > 1) BoxBlur(src, thick, 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float lum = luma01(src, x, y);
                const float hi = luma01(src, x + static_cast<int>(detail * 3.0f) + 1,
                                        y + static_cast<int>(detail * 3.0f) + 1);
                const float edge = std::min(1.0f, std::fabs(lum - hi) * 3.0f);
                const float thr = 0.35f + bal * 0.3f;
                const float v = (lum < thr) ? (0.05f + 0.15f * (1.0f - edge))
                                            : (0.85f - 0.2f * edge);
                setV(x, y, v, alphaAt(x, y));
            }
        return true;
    }

    // ---- 铬黄：金属带纹（亮度做正弦带状 + 平滑）----
    if (k == "chrome") {
        const int detail = std::max(0, f.Pi("detail"));
        const int smooth = std::max(0, f.Pi("smoothness"));
        const float freq = 2.0f + static_cast<float>(detail);
        ImageBuffer src = img.Clone();
        if (smooth > 1) BoxBlur(src, std::max(1, smooth / 3), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                // 沿亮度做带状：亮→暗→亮，产生铬的金属条纹
                const float lum = luma01(src, x, y);
                const float band = 0.5f + 0.5f * std::cos(lum * freq * 6.2831853f);
                const float v = 0.15f + 0.8f * band;
                setV(x, y, v, alphaAt(x, y));
            }
        return true;
    }

    // ---- 炭精笔：纹理高度场 + 双色阶（前景/背景两色）----
    if (k == "conteCrayon") {
        const int fgL = std::max(1, f.Pi("foregroundLevel"));
        const int bgL = std::max(1, f.Pi("backgroundLevel"));
        const int tsel = f.Pi("texture");
        const float scaling = std::max(0.1f, f.P("scaling") / 100.0f);
        const float relief = static_cast<float>(f.Pi("relief")) / 50.0f;
        const int light = f.Pi("light");
        const bool invert = f.Pi("invert") != 0;
        float lx = 0.7f, ly = 0.7f;
        lightDir(light, lx, ly);
        const float cell = std::max(2.0f, (tsel == 1 ? 10.0f : tsel == 2 ? 3.0f : 5.0f) * scaling);
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float lum = luma01(src, x, y);
                // 双色阶：暗→炭精（黑），亮→纸（白），级数由 fg/bgLevel 控制
                const float step = std::floor(lum * std::max(2, fgL + bgL)) /
                                   std::max(1.0f, static_cast<float>(fgL + bgL - 1));
                float h = Hash01(static_cast<int>(x / cell), static_cast<int>(y / cell), 89u);
                if (invert) h = 1.0f - h;
                const float d = (h - 0.5f) * relief * 60.0f * sc / maxV;
                setV(x, y, std::min(1.0f, std::max(0.0f, step + d)), alphaAt(x, y));
            }
        return true;
    }

    // ---- 便条纸 / 塑料效果：亮度浮雕 + 颗粒（塑料效果更平滑、浮雕更强）----
    if (k == "notePaper" || k == "plaster") {
        const bool plaster = (k == "plaster");
        const float balance = plaster ? f.P("imageBalance") / 50.0f
                                      : f.P("imageBalance") / 50.0f;
        const float grain = plaster ? 0.0f
                                    : static_cast<float>(f.Pi("graininess")) / 20.0f;
        const float relief = plaster ? static_cast<float>(f.Pi("relief")) / 25.0f
                                     : static_cast<float>(f.Pi("relief")) / 25.0f;
        const int smooth = std::max(1, f.Pi("smoothness"));
        const int light = f.Pi("light");
        float lx = 0.7f, ly = -0.7f;
        lightDir(light, lx, ly);
        ImageBuffer src = img.Clone();
        if (smooth > 1) BoxBlur(src, std::max(1, smooth / 2), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const int d = 2;
                const float dh = luma01(src, x + static_cast<int>(lx * d),
                                        y + static_cast<int>(ly * d)) -
                                 luma01(src, x, y);
                float v = 0.5f + balance * 0.2f + dh * (2.0f + relief * 4.0f);
                if (grain > 0.0f)
                    v += (Hash01(x, y, 97u) - 0.5f) * 0.5f * grain;
                setV(x, y, std::min(1.0f, std::max(0.0f, v)), alphaAt(x, y));
            }
        return true;
    }

    return false;
}

} // namespace sai2::fg
