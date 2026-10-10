#pragma once
// ============================================================================
// AdjustKernels.h - PS 风格调整算法（反相/去色/亮度对比度/色相饱和度/色阶）
//
// 所有通道统一用 uint16 承载（RGBA8: 0..255，RGBA16: 0..65535），
// 以 ChannelMax(format) 为满幅，逐像素处理；支持 Selection（矩形+蒙版）。
// ============================================================================

#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sai2::adjust {

using sai2::ImageBuffer;
using sai2::RGBA;
using sai2::Selection;

inline uint16_t ChannelMax(PixelFormat f) {
    return f == PixelFormat::RGBA8 ? 255 : 65535;
}

// ---------------------------------------------------------------- 反相
inline void ApplyInvert(ImageBuffer& img, const Selection* sel) {
    const uint16_t mx = ChannelMax(img.Format());
    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds()
                                         : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            c.r = static_cast<uint16_t>(mx - c.r);
            c.g = static_cast<uint16_t>(mx - c.g);
            c.b = static_cast<uint16_t>(mx - c.b);
            img.SetPixel(x, y, c);
        }
    }
}

// ---------------------------------------------------------------- 去色
inline void ApplyGrayscale(ImageBuffer& img, const Selection* sel) {
    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            // Rec.601 亮度（按满幅归一后加权，再回到本格式满幅）
            const double lum = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
            const uint16_t g = static_cast<uint16_t>(
                std::min<double>(ChannelMax(img.Format()),
                                 std::max(0.0, lum)));
            c.r = g;
            c.g = g;
            c.b = g;
            img.SetPixel(x, y, c);
        }
    }
}

// ------------------------------------------------------- 亮度 / 对比度
// 亮度 -100..100（满幅的百分比偏移）；对比度 -100..100（以中灰为轴的斜率）
inline void ApplyBrightnessContrast(ImageBuffer& img, const Selection* sel,
                                    float brightness, float contrast) {
    const double mx = ChannelMax(img.Format());
    const double half = mx / 2.0;
    // 斜率：contrast=100 → ×2，-100 → ×0（PS 近似 tan 映射更陡，这里取线性+平方混合）
    const double c = contrast / 100.0;
    const double slope = c >= 0.0 ? 1.0 + c * 1.0
                                  : 1.0 + c * 0.9;
    const double offset = brightness / 100.0 * mx;
    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    auto map = [&](double v) {
        v = (v - half) * slope + half + offset;
        return static_cast<uint16_t>(
            std::min(mx, std::max(0.0, v + 0.5)));
    };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            c.r = map(c.r);
            c.g = map(c.g);
            c.b = map(c.b);
            img.SetPixel(x, y, c);
        }
    }
}

// ------------------------------------------------------ 色相 / 饱和度
namespace hsl {

inline void RgbToHsl(double r, double g, double b, double& h, double& s,
                     double& l) {
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    if (mx - mn < 1e-12) { h = 0; s = 0; return; }
    const double d = mx - mn;
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    h /= 6.0;
}

inline double HueToRgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

inline void HslToRgb(double h, double s, double l, double& r, double& g,
                     double& b) {
    if (s <= 0) { r = g = b = l; return; }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    r = HueToRgb(p, q, h + 1.0 / 3);
    g = HueToRgb(p, q, h);
    b = HueToRgb(p, q, h - 1.0 / 3);
}

} // namespace hsl

// 色相 -180..180（度）、饱和度 -100..100、明度 -100..100
inline void ApplyHueSaturation(ImageBuffer& img, const Selection* sel,
                               float hueDeg, float saturation,
                               float lightness) {
    const double mx = ChannelMax(img.Format());
    const double dh = hueDeg / 360.0;
    const double ds = saturation / 100.0;  // 饱和度乘 (1+ds)
    const double dl = lightness / 100.0;
    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            double r = c.r / mx, g = c.g / mx, b = c.b / mx;
            double hu, sa, li;
            hsl::RgbToHsl(r, g, b, hu, sa, li);
            hu = std::fmod(hu + dh + 1.0, 1.0);
            sa = std::min(1.0, std::max(0.0, sa * (1.0 + ds)));
            li = std::min(1.0, std::max(0.0,
                    dl >= 0 ? li + (1 - li) * dl : li * (1 + dl)));
            hsl::HslToRgb(hu, sa, li, r, g, b);
            c.r = static_cast<uint16_t>(std::min(mx, r * mx + 0.5));
            c.g = static_cast<uint16_t>(std::min(mx, g * mx + 0.5));
            c.b = static_cast<uint16_t>(std::min(mx, b * mx + 0.5));
            img.SetPixel(x, y, c);
        }
    }
}

// ---------------------------------------------------------------- 色阶
// inBlack/inWhite/gamma：输入黑场/白场/中间调 gamma（PS 色阶基本形态）
inline void ApplyLevels(ImageBuffer& img, const Selection* sel,
                        float inBlack, float inWhite, float gamma) {
    const double mx = ChannelMax(img.Format());
    double ib = std::min(inBlack, inWhite - 1.0f) / 255.0 * mx;
    double iw = std::max(inWhite, inBlack + 1.0f) / 255.0 * mx;
    const double g = 1.0 / std::max(0.05, static_cast<double>(gamma));
    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    auto map = [&](double v) {
        double t = (v - ib) / std::max(1.0, iw - ib);
        t = std::min(1.0, std::max(0.0, t));
        t = std::pow(t, g);
        return static_cast<uint16_t>(std::min(mx, t * mx + 0.5));
    };
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            c.r = map(c.r);
            c.g = map(c.g);
            c.b = map(c.b);
            img.SetPixel(x, y, c);
        }
    }
}

} // namespace sai2::adjust
