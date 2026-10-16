#pragma once
// ============================================================================
// GalleryArtistic.h - Artistic（艺术效果）批的算法
//
// 本批：干画笔 / 壁画 / 胶片颗粒 / 海绵 / 塑料包装 / 海报边缘 / 粗糙蜡笔
// 共同积木：盒式模糊、双线性回采、确定性噪声、Sobel、色彩量化（色调分离）、
//           纹理高度场 + 定向打光（与 Texture 批同源）。
//
// 单独一个文件而不是塞进 GalleryFilters.h：那边已经很长，而且用"注释当锚点"
// 插入代码在重构后极易失效（实际踩过：插入失败但 dispatch 已经调用 → 编译报
// undeclared identifier）。新分类一律新建文件 + 在 GalleryFilters.h 顶部 include。
// ============================================================================

#include "Filters.h"
// 注意：不在这里 include GalleryFilters.h —— 那个文件在**末尾**include 本文件，

namespace sai2::fg {

inline bool GalleryApplyArtistic(const char* key, ImageBuffer& img,
                                 const Selection* sel,
                                 const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float sc = static_cast<float>(maxV) / 255.0f;
    const Rect roi = FilterRoi(img, sel);
    if (roi.w <= 0 || roi.h <= 0) return true;
    const int W = img.Width(), H = img.Height();

    auto luma01 = [&](const ImageBuffer& s2, int x, int y) {
        return Luma(SampleClamp(s2, x, y)) / (255.0f * sc);
    };
    // 色彩量化（色调分离）
    auto post = [](float v, int levels) {
        const float l = static_cast<float>(std::max(2, levels));
        return std::floor(std::min(1.0f, std::max(0.0f, v)) * l) / (l - 1.0f);
    };
    // 沿光照方向的高度场（纹理浮雕用；与 Texture 批同一套）
    auto heightAt = [&](int tsel, float scaling, bool invert, float px,
                        float py) {
        const float cell =
            std::max(2.0f, (tsel == 1 ? 10.0f : tsel == 2 ? 3.0f : 5.0f) * scaling);
        const float u = px / cell, v = py / cell;
        float h;
        if (tsel == 1)
            h = (static_cast<int>(v) % 2) ? 0.8f : 0.2f;
        else if (tsel == 2)
            h = (static_cast<int>(u) % 2 ? 0.7f : 0.3f) *
                (static_cast<int>(v) % 2 ? 0.8f : 0.4f);
        else if (tsel == 3)
            h = Hash01(static_cast<int>(u), static_cast<int>(v), 41u);
        else
            h = 0.5f + 0.35f * std::sin(u * 3.14159265f) *
                           std::sin(v * 3.14159265f);
        return invert ? 1.0f - h : h;
    };

    // ---- 干画笔 / 壁画：模糊+细节混合 → 色调分离 → 干笔触噪声 ----
    if (k == "dryBrush" || k == "fresco") {
        const bool fresco = (k == "fresco");
        const int brush = std::max(0, f.Pi("brushSize"));
        const float detail = static_cast<float>(f.Pi("brushDetail")) / 10.0f;
        const int texture = std::max(1, f.Pi("texture"));
        ImageBuffer src = img.Clone();
        if (brush > 0) BoxBlur(src, std::max(1, brush / 2), 1);
        const int levels = 2 + texture + (fresco ? 2 : 0);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c, s2;
                img.GetPixel(x, y, c);
                src.GetPixel(x, y, s2);
                const float n = (Hash01(x / std::max(1, brush + 1), y, 71u) - 0.5f) *
                                (fresco ? 46.0f : 30.0f) * sc;
                RGBA o;
                const float mr = c.r * detail + s2.r * (1.0f - detail);
                const float mg = c.g * detail + s2.g * (1.0f - detail);
                const float mb = c.b * detail + s2.b * (1.0f - detail);
                o.r = ClampCh(post(mr / maxV, levels) * maxV + n, maxV);
                o.g = ClampCh(post(mg / maxV, levels) * maxV + n, maxV);
                o.b = ClampCh(post(mb / maxV, levels) * maxV + n, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ---- 胶片颗粒：中间调最强的颗粒 + 可选高光提亮 ----
    if (k == "filmGrain") {
        const float grain = static_cast<float>(f.Pi("grain")) / 20.0f;
        const float hiArea = static_cast<float>(f.Pi("highlightArea")) / 20.0f;
        const float inten = static_cast<float>(f.Pi("intensity")) / 10.0f;
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                const float lum = Luma(c) / (255.0f * sc);
                const float shape = 1.0f - std::fabs(lum * 2.0f - 1.0f);
                const float n = (Hash01(x, y, 73u) - 0.5f) * 120.0f * sc * grain *
                                (0.4f + inten) * (0.5f + 0.5f * shape);
                float add = n;
                if (hiArea > 0.0f)
                    add += std::max(0.0f, lum - (1.0f - hiArea)) * 90.0f * sc;
                RGBA o;
                o.r = ClampCh(c.r + add, maxV);
                o.g = ClampCh(c.g + add, maxV);
                o.b = ClampCh(c.b + add, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ---- 海绵：模糊图与原图按低频噪声"开孔"混合 ----
    if (k == "sponge") {
        const int brush = std::max(0, f.Pi("brushSize"));
        const float def = static_cast<float>(f.Pi("definition")) / 25.0f;
        const int smooth = std::max(1, f.Pi("smoothness"));
        ImageBuffer src = img.Clone();
        BoxBlur(src, std::max(1, (smooth + brush) / 3), 1);
        const int cell = std::max(1, brush + 2);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c, s2;
                img.GetPixel(x, y, c);
                src.GetPixel(x, y, s2);
                const float hole = Hash01(x / cell, y / cell, 79u);
                // 拉开对比：原来孔洞与模糊图差异太小 → 几乎看不出效果（实测 mean≈0.94）
                const float w = (hole < (0.30f + def * 0.7f)) ? 1.0f : (0.15f - def * 0.1f);
                RGBA o;
                o.r = ClampCh(c.r * w + s2.r * (1 - w), maxV);
                o.g = ClampCh(c.g * w + s2.g * (1 - w), maxV);
                o.b = ClampCh(c.b * w + s2.b * (1 - w), maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ---- 塑料包装：模糊图的"脊线"当高光叠加 ----
    if (k == "plasticWrap") {
        const float hi = static_cast<float>(f.Pi("highlightStrength")) / 20.0f;
        const int detail = std::max(1, f.Pi("detail"));
        const int smooth = std::max(1, f.Pi("smoothness"));
        ImageBuffer src = img.Clone();
        BoxBlur(src, std::max(1, smooth / 3), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float ridge =
                    std::max(0.0f, luma01(src, x + detail, y + detail) -
                                       luma01(src, x, y)) * 2.0f;
                RGBA c;
                img.GetPixel(x, y, c);
                const float add = ridge * hi * 220.0f * sc;
                RGBA o;
                o.r = ClampCh(c.r + add, maxV);
                o.g = ClampCh(c.g + add, maxV);
                o.b = ClampCh(c.b + add, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ---- 海报边缘：色调分离 + Sobel 边缘压暗 ----
    if (k == "posterEdges") {
        const int thick = std::max(0, f.Pi("edgeThickness"));
        const float eInten = static_cast<float>(f.Pi("edgeIntensity")) / 10.0f;
        const int levels = std::max(2, f.Pi("posterization") + 2);
        const int d = 1 + thick / 3;
        ImageBuffer src = img.Clone();
        if (thick > 0) BoxBlur(src, std::max(1, thick / 2), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                auto at = [&](int xx, int yy) { return luma01(src, xx, yy); };
                const float gx = -at(x - d, y - d) + at(x + d, y - d) -
                                 2.0f * at(x - d, y) + 2.0f * at(x + d, y) -
                                 at(x - d, y + d) + at(x + d, y + d);
                const float gy = -at(x - d, y - d) - 2.0f * at(x, y - d) -
                                 at(x + d, y - d) + at(x - d, y + d) +
                                 2.0f * at(x, y + d) + at(x + d, y + d);
                const float dark =
                    std::min(1.0f, std::sqrt(gx * gx + gy * gy)) *
                    (0.5f + eInten) * 255.0f * sc;
                RGBA o;
                // ★ 必须浮点除：c.r 是 uint16_t、maxV 是 uint32_t，写成 c.r/maxV 是
                //   整数除法 → 恒为 0 → 整幅变黑（实测 posterEdges/watercolor 100% 黑）
                o.r = ClampCh(post(static_cast<float>(c.r) / maxV, levels) * maxV - dark, maxV);
                o.g = ClampCh(post(static_cast<float>(c.g) / maxV, levels) * maxV - dark, maxV);
                o.b = ClampCh(post(static_cast<float>(c.b) / maxV, levels) * maxV - dark, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ---- 粗糙蜡笔：沿光照方向短笔触回采 + 纹理浮雕 ----
    if (k == "roughPastels") {
        const int len = std::max(1, f.Pi("strokeLength"));
        const int det = std::max(1, f.Pi("strokeDetail"));
        const int tsel = f.Pi("texture");
        const float scaling = std::max(0.1f, f.P("scaling") / 100.0f);
        const float relief = static_cast<float>(f.Pi("relief")) / 50.0f;
        const int light = f.Pi("light");
        const bool invert = f.Pi("invert") != 0;
        const float ang = 6.2831853f * static_cast<float>(light) / 8.0f;
        const float lx = std::cos(ang), ly = std::sin(ang);
        const float cell = std::max(2.0f, (tsel == 1 ? 10.0f : tsel == 2 ? 3.0f : 5.0f) * scaling);
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA a = Bilinear(src, static_cast<float>(x) + lx * len,
                                  static_cast<float>(y) + ly * len);
                RGBA c;
                img.GetPixel(x, y, c);
                RGBA m;
                m.r = static_cast<uint16_t>((a.r * det + c.r) / (det + 1));
                m.g = static_cast<uint16_t>((a.g * det + c.g) / (det + 1));
                m.b = static_cast<uint16_t>((a.b * det + c.b) / (det + 1));
                const float dh =
                    (heightAt(tsel, scaling, invert, static_cast<float>(x) + lx * cell,
                              static_cast<float>(y) + ly * cell) -
                     heightAt(tsel, scaling, invert, static_cast<float>(x),
                              static_cast<float>(y))) * relief * 110.0f * sc;
                m.r = ClampCh(m.r + dh, maxV);
                m.g = ClampCh(m.g + dh, maxV);
                m.b = ClampCh(m.b + dh, maxV);
                m.a = c.a;
                img.SetPixel(x, y, m);
            }
        return true;
    }

    return false;
}

} // namespace sai2::fg
