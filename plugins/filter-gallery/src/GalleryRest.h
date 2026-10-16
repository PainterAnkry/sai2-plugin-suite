#pragma once
// ============================================================================
// GalleryRest.h - 剩余两批算法：Artistic 后 8 个 + Brush Strokes 全部 8 个
//
// Artistic：彩色铅笔 / 剪贴画 / 霓虹灯光 / 绘画涂抹 / 调色刀 / 涂抹棒 /
//           底纹效果 / 水彩
// Brush Strokes：强化的边缘 / 成角的线条 / 交叉线 / 深色线条 / 油墨概况 /
//               喷溅 / 喷色描边 / 烟灰墨
// 积木：盒式模糊、双线性回采、确定性噪声、Sobel、色调分离、方向性偏移。
// （单独文件的原因见 GalleryArtistic.h 顶部注释。）
// ============================================================================

#include "Filters.h"
#include "GalleryFilters.h"

namespace sai2::fg {

inline bool GalleryApplyRest(const char* key, ImageBuffer& img,
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
    auto post = [](float v, int levels) {
        const float l = static_cast<float>(std::max(2, levels));
        return std::floor(std::min(1.0f, std::max(0.0f, v)) * l) / (l - 1.0f);
    };
    auto addRGB = [&](int x, int y, float d, RGBA c) {
        RGBA o;
        o.r = ClampCh(c.r + d, maxV);
        o.g = ClampCh(c.g + d, maxV);
        o.b = ClampCh(c.b + d, maxV);
        o.a = c.a;
        img.SetPixel(x, y, o);
    };
    auto gray = [&](int x, int y, float v, uint16_t a) {
        RGBA o;
        const float c = std::min(static_cast<float>(maxV), std::max(0.0f, v * maxV));
        o.r = o.g = o.b = static_cast<uint16_t>(c);
        o.a = a;
        img.SetPixel(x, y, o);
    };
    auto alphaAt = [&](int x, int y) { RGBA c; img.GetPixel(x, y, c); return c.a; };
    // 方向性回采（沿 (dx,dy) 平均 len 个样本）
    auto smear = [&](const ImageBuffer& src, int x, int y, float dx, float dy,
                     int len) {
        float r = 0, g = 0, b = 0, a = 0;
        const int n = std::max(1, len);
        for (int i = 0; i < n; ++i) {
            RGBA c = Bilinear(src, static_cast<float>(x) + dx * i,
                              static_cast<float>(y) + dy * i);
            r += c.r; g += c.g; b += c.b; a += c.a;
        }
        RGBA o;
        o.r = static_cast<uint16_t>(r / n);
        o.g = static_cast<uint16_t>(g / n);
        o.b = static_cast<uint16_t>(b / n);
        o.a = static_cast<uint16_t>(a / n);
        return o;
    };
    // Sobel 幅度（用亮度）
    auto sobel = [&](const ImageBuffer& src, int x, int y, int d) {
        auto at = [&](int xx, int yy) { return luma01(src, xx, yy); };
        const float gx = -at(x - d, y - d) + at(x + d, y - d) -
                         2.0f * at(x - d, y) + 2.0f * at(x + d, y) -
                         at(x - d, y + d) + at(x + d, y + d);
        const float gy = -at(x - d, y - d) - 2.0f * at(x, y - d) -
                         at(x + d, y - d) + at(x - d, y + d) +
                         2.0f * at(x, y + d) + at(x + d, y + d);
        return std::min(1.0f, std::sqrt(gx * gx + gy * gy));
    };

    // ================= Artistic 后 8 个 =================

    // 彩色铅笔：保留原色 + 纸纹方向的细铅笔线，背景按纸张亮度抬亮
    if (k == "coloredPencil") {
        const int pw = std::max(1, f.Pi("pencilWidth"));
        const float press = static_cast<float>(f.Pi("strokePressure")) / 15.0f;
        const float paper = f.P("paperBrightness") / 50.0f;
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                const float hatch = Hash01((x + y) / pw, (x - y) / pw, 101u);
                const float w = 0.55f + 0.45f * hatch * (0.5f + press);
                const float lift = paper * 40.0f * sc;
                RGBA o;
                o.r = ClampCh(c.r * w + lift, maxV);
                o.g = ClampCh(c.g * w + lift, maxV);
                o.b = ClampCh(c.b * w + lift, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // 剪贴画：模糊（边缘简化）+ 色调分离（色阶数）
    if (k == "cutout") {
        const int levels = std::max(2, f.Pi("numberOfLevels"));
        const int simp = std::max(0, f.Pi("edgeSimplicity"));
        const int fid = std::max(1, f.Pi("edgeFidelity"));
        ImageBuffer src = img.Clone();
        if (simp > 0) BoxBlur(src, std::max(1, simp / 2), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c, s2;
                img.GetPixel(x, y, c);
                src.GetPixel(x, y, s2);
                // edgeFidelity 越大越贴近原图（保留细节）
                const float t = static_cast<float>(fid) / 3.0f;
                RGBA o;
                o.r = ClampCh(post((s2.r * t + c.r * (1 - t)) / maxV, levels) * maxV, maxV);
                o.g = ClampCh(post((s2.g * t + c.g * (1 - t)) / maxV, levels) * maxV, maxV);
                o.b = ClampCh(post((s2.b * t + c.b * (1 - t)) / maxV, levels) * maxV, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // 霓虹灯光：背景压暗 + 发光色（源里是 json 取色，这里用青/品红两色交替）
    if (k == "neonGlow") {
        const int size = std::abs(f.Pi("glowSize"));
        const float bright = static_cast<float>(f.Pi("glowBrightness")) / 50.0f;
        ImageBuffer bl = img.Clone();
        BoxBlur(bl, std::max(1, size / 3 + 1), 2);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA b;
                bl.GetPixel(x, y, b);
                const float lum = Luma(b) / (255.0f * sc);
                // 发光颜色：亮部偏青、暗部偏品红（固定两色，避免取色控件）
                const float gr = 0.2f + 0.8f * (1.0f - lum);
                const float gg = 0.9f * lum + 0.2f;
                const float gb = 0.9f;
                const float w = bright * (0.35f + lum);
                RGBA c;
                img.GetPixel(x, y, c);
                RGBA o;
                o.r = ClampCh(c.r * 0.35f * (1.0f - w) + gr * maxV * w, maxV);
                o.g = ClampCh(c.g * 0.35f * (1.0f - w) + gg * maxV * w, maxV);
                o.b = ClampCh(c.b * 0.35f * (1.0f - w) + gb * maxV * w, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // 绘画涂抹 / 调色刀：同族——按笔触尺度做随机方向的块状回采 + 细节保留
    if (k == "paintDaubs" || k == "paletteKnife") {
        const bool knife = (k == "paletteKnife");
        const int size = knife ? std::max(1, f.Pi("strokeSize"))
                               : std::max(1, f.Pi("brushSize"));
        // 注意：÷3 时 strokeDetail 默认值 3 会让系数变成 1 → 公式退化成
        // 恒等（什么都不改）。测试里"必须真的改像素"正是抓到这个。改用 ÷4。
        const float det = knife ? f.P("strokeDetail") / 4.0f
                                : 1.0f - f.P("sharpness") / 40.0f;
        const float soft = knife ? f.P("softness") / 10.0f : 0.0f;
        const int cell = std::max(2, size / 2 + 1);
        ImageBuffer src = img.Clone();
        if (soft > 0.0f) BoxBlur(src, std::max(1, static_cast<int>(soft * 3)), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                // 每个格子一个随机方向 + 随机长度 → 像一笔一笔涂上去
                const int cx = x / cell, cy = y / cell;
                const float ang = Hash01(cx, cy, 103u) * 6.2831853f;
                const int len = 1 + static_cast<int>(Hash01(cx, cy, 107u) * cell);
                RGBA s2 = smear(src, x, y, std::cos(ang), std::sin(ang), len);
                RGBA c;
                img.GetPixel(x, y, c);
                RGBA o;
                o.r = ClampCh(s2.r * (1.0f - det) * 0.6f + c.r * det, maxV);
                o.g = ClampCh(s2.g * (1.0f - det) * 0.6f + c.g * det, maxV);
                o.b = ClampCh(s2.b * (1.0f - det) * 0.6f + c.b * det, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // 涂抹棒：斜向拖尾 + 高光区域提亮
    if (k == "smudgeStick") {
        const int len = std::max(1, f.Pi("strokeLength") * 2 + 1);
        const float hiArea = static_cast<float>(f.Pi("highlightArea")) / 20.0f;
        const float inten = static_cast<float>(f.Pi("intensity")) / 10.0f;
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA s2 = smear(src, x, y, 0.7071f, -0.7071f, len);
                const float lum = Luma(s2) / (255.0f * sc);
                const float add = std::max(0.0f, lum - (1.0f - hiArea - 0.2f)) *
                                  inten * 120.0f * sc;
                addRGB(x, y, add, s2);
            }
        return true;
    }

    // 底纹效果：纹理覆盖 + 浮雕（把纹理高度场压到画面里）
    if (k == "underpainting") {
        const int brush = std::max(0, f.Pi("brushSize"));
        const float cover = static_cast<float>(f.Pi("textureCoverage")) / 40.0f;
        const int tsel = f.Pi("texture");
        const float scaling = std::max(0.1f, f.P("scaling") / 100.0f);
        const float relief = static_cast<float>(f.Pi("relief")) / 50.0f;
        const bool invert = f.Pi("invert") != 0;
        const float cell = std::max(2.0f, (tsel == 1 ? 10.0f : tsel == 2 ? 3.0f : 5.0f) * scaling);
        ImageBuffer src = img.Clone();
        if (brush > 0) BoxBlur(src, std::max(1, brush / 4), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                float h = Hash01(static_cast<int>(x / cell), static_cast<int>(y / cell), 109u);
                if (invert) h = 1.0f - h;
                RGBA c, s2;
                img.GetPixel(x, y, c);
                src.GetPixel(x, y, s2);
                RGBA m;
                m.r = static_cast<uint16_t>(s2.r * (1 - cover) + c.r * cover);
                m.g = static_cast<uint16_t>(s2.g * (1 - cover) + c.g * cover);
                m.b = static_cast<uint16_t>(s2.b * (1 - cover) + c.b * cover);
                const float d = (h - 0.5f) * relief * 80.0f * sc;
                m.r = ClampCh(m.r + d, maxV);
                m.g = ClampCh(m.g + d, maxV);
                m.b = ClampCh(m.b + d, maxV);
                m.a = c.a;
                img.SetPixel(x, y, m);
            }
        return true;
    }

    // 水彩：模糊 + 色调分离 + 暗部沉淀（边缘变暗）
    if (k == "watercolor") {
        const int detail = std::max(1, f.Pi("brushDetail"));
        const float shadow = static_cast<float>(f.Pi("shadowIntensity")) / 10.0f;
        ImageBuffer src = img.Clone();
        BoxBlur(src, std::max(1, detail / 4 + 1), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA s2;
                src.GetPixel(x, y, s2);
                const float q = 3.0f + detail / 2.0f;
                RGBA o;
                o.r = ClampCh(post(s2.r / maxV, static_cast<int>(q)) * maxV, maxV);
                o.g = ClampCh(post(s2.g / maxV, static_cast<int>(q)) * maxV, maxV);
                o.b = ClampCh(post(s2.b / maxV, static_cast<int>(q)) * maxV, maxV);
                // 暗部沉淀：亮度低的区域再压暗
                const float lum = Luma(o) / (255.0f * sc);
                const float dark = std::max(0.0f, 0.55f - lum) * shadow * 160.0f * sc;
                o.r = ClampCh(o.r - dark, maxV);
                o.g = ClampCh(o.g - dark, maxV);
                o.b = ClampCh(o.b - dark, maxV);
                o.a = alphaAt(x, y);
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // ================= Brush Strokes 全部 8 个 =================

    // 强化的边缘：Sobel 边缘提亮（白线）
    if (k == "accentedEdges") {
        const int wdt = std::max(1, f.Pi("edgeWidth"));
        const float bright = static_cast<float>(f.Pi("edgeBrightness")) / 50.0f;
        const int smooth = std::max(1, f.Pi("smoothness"));
        ImageBuffer src = img.Clone();
        if (smooth > 1) BoxBlur(src, std::max(1, smooth / 4), 1);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                const float m = sobel(src, x, y, wdt);
                addRGB(x, y, m * (0.3f + bright) * 200.0f * sc, c);
            }
        return true;
    }

    // 成角的线条：45° 方向拖尾 + 锐化
    if (k == "angledStrokes") {
        const float bal = f.P("directionBalance") / 100.0f;
        const int len = std::max(3, f.Pi("strokeLength"));
        const float sharp = static_cast<float>(f.Pi("sharpness")) / 10.0f;
        const float dx = (bal < 0.5f) ? 0.7071f : -0.7071f;
        const float dy = 0.7071f;
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA s2 = smear(src, x, y, dx, dy, len);
                RGBA c;
                img.GetPixel(x, y, c);
                // 锐化：在原图与拖尾之间偏向原图
                RGBA o;
                o.r = ClampCh(s2.r * (1.0f - sharp) + c.r * sharp, maxV);
                o.g = ClampCh(s2.g * (1.0f - sharp) + c.g * sharp, maxV);
                o.b = ClampCh(s2.b * (1.0f - sharp) + c.b * sharp, maxV);
                o.a = c.a;
                img.SetPixel(x, y, o);
            }
        return true;
    }

    // 交叉线：按亮度叠加多角度排线（暗→密）
    if (k == "crosshatch") {
        const int len = std::max(3, f.Pi("strokeLength"));
        const float sharp = static_cast<float>(f.Pi("sharpness")) / 20.0f;
        const int strength = std::max(1, f.Pi("strength"));
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                const float lum = Luma(c) / (255.0f * sc);
                const int need = static_cast<int>((1.0f - lum) * (strength + 1));
                int hits = 0;
                for (int a = 0; a < need; ++a) {
                    const float ang = 3.14159265f * static_cast<float>(a) / 3.0f;
                    const float u = std::cos(ang) * static_cast<float>(x) +
                                    std::sin(ang) * static_cast<float>(y);
                    const float ph = u / std::max(3, len / 2);
                    if ((ph - std::floor(ph)) < 0.35f * (1.0f - sharp)) ++hits;
                }
                gray(x, y, hits ? 0.15f : 0.95f, c.a);
            }
        return true;
    }

    // 深色线条：暗部更黑、亮部更白（斜向拖尾后按平衡分离）
    if (k == "darkStrokes") {
        const float bal = f.P("balance") / 10.0f;
        const float bk = static_cast<float>(f.Pi("blackIntensity")) / 10.0f;
        const float wt = static_cast<float>(f.Pi("whiteIntensity")) / 10.0f;
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA s2 = smear(src, x, y, 0.7071f, 0.7071f, 5);
                const float lum = Luma(s2) / (255.0f * sc);
                const float v = (lum < bal) ? lum * (1.0f - bk)
                                            : lum + (1.0f - lum) * wt;
                gray(x, y, std::min(1.0f, std::max(0.0f, v)), alphaAt(x, y));
            }
        return true;
    }

    // 油墨概况：边缘描黑线（暗），其余保持亮
    if (k == "inkOutlines") {
        const int len = std::max(1, f.Pi("strokeLength"));
        const float dark = static_cast<float>(f.Pi("darkIntensity")) / 50.0f;
        const float lightI = static_cast<float>(f.Pi("lightIntensity")) / 50.0f;
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float m = sobel(src, x, y, std::max(1, len / 8 + 1));
                const float lum = luma01(src, x, y);
                float v = lum + lightI * 0.3f - m * (0.6f + dark);
                gray(x, y, std::min(1.0f, std::max(0.0f, v)), alphaAt(x, y));
            }
        return true;
    }

    // 喷溅：随机颗粒点（喷枪）
    if (k == "spatter") {
        const int rad = std::max(1, f.Pi("sprayRadius"));
        const int smooth = std::max(1, f.Pi("smoothness"));
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                // 在半径内撒点：越靠近中心概率越高（smoothness 控制衰减）
                const float n = Hash01(x / std::max(1, rad / 2 + 1),
                                       y / std::max(1, rad / 2 + 1), 113u);
                const float thr = 0.5f + 0.4f * (1.0f - 1.0f / smooth);
                if (n > thr) continue;   // 大部分像素保持原样，只有散点被改
                const float v = (n < thr * 0.5f) ? 0.0f : 1.0f;
                gray(x, y, v, c.a);
            }
        return true;
    }

    // 喷色描边：方向性描边 + 散点
    if (k == "sprayedStrokes") {
        const int len = std::max(1, f.Pi("strokeLength"));
        const int rad = std::max(1, f.Pi("sprayRadius"));
        const int dir = f.Pi("strokeDirection");
        float dx = 1.0f, dy = 0.0f;
        if (dir == 0) { dx = 0.7071f; dy = -0.7071f; }
        else if (dir == 2) { dx = 0.7071f; dy = 0.7071f; }
        else if (dir == 3) { dx = 0.0f; dy = 1.0f; }
        ImageBuffer src = img.Clone();
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA s2 = smear(src, x, y, dx, dy, len);
                const float n = Hash01(x / std::max(1, rad / 2 + 1),
                                       y / std::max(1, rad / 2 + 1), 127u);
                const float lum = Luma(s2) / (255.0f * sc);
                const float v = (n < 0.35f) ? 0.1f : (0.35f + 0.65f * lum);
                gray(x, y, std::min(1.0f, std::max(0.0f, v)), alphaAt(x, y));
            }
        return true;
    }

    // 烟灰墨：强对比的水墨（模糊 + 高压阈值 + 对比拉伸）
    if (k == "sumiE") {
        const int wdt = std::max(3, f.Pi("strokeWidth"));
        const float press = static_cast<float>(f.Pi("strokePressure")) / 15.0f;
        const float contr = static_cast<float>(f.Pi("contrast")) / 40.0f;
        ImageBuffer src = img.Clone();
        BoxBlur(src, std::max(1, wdt / 4), 1);
        const float thr = 0.45f + press * 0.25f;
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                float lum = luma01(src, x, y);
                // 对比拉伸 + 阈值 → 水墨块面
                lum = std::min(1.0f, std::max(0.0f, (lum - 0.5f) * (1.0f + contr * 3.0f) + 0.5f));
                const float v = (lum < thr) ? 0.05f : 0.95f;
                gray(x, y, v, alphaAt(x, y));
            }
        return true;
    }

    return false;
}

} // namespace sai2::fg
