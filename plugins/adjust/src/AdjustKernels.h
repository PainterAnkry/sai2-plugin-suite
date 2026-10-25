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
#include <cstdlib>
#include <string>
#include <vector>

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

// float 版（8bit 快路径用；和 double 版逐项一致，只是少一半精度开销）
inline float HueToRgbF(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6.0f;
    return p;
}

} // namespace hsl

// 色相 -180..180（度）、饱和度 -100..100、明度 -100..100
//
// 性能：这是**拖动滑杆时要实时重算**的一类调整（预览图 900px 级）。
// 第一版逐像素走 SDK 的 GetPixel/SetPixel —— 跨 TU 调用 + 边界检查，
// 实测一帧要 30~50ms，拖动明显跟不上。8bit 图像改走行指针直写
// （透明像素直接跳过），其余格式保留原来的 16bit 路径。
inline void ApplyHueSaturation(ImageBuffer& img, const Selection* sel,
                               float hueDeg, float saturation,
                               float lightness) {
    if (!img.IsValid()) return;
    // 三个参数都没动：什么都不用做（拖动开始时常见）
    if (hueDeg == 0.0f && saturation == 0.0f && lightness == 0.0f) return;

    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);

    const float dh = hueDeg / 360.0f;
    const float ds = saturation / 100.0f; // 饱和度乘 (1+ds)
    const float dl = lightness / 100.0f;

    if (img.Format() == PixelFormat::RGBA8) {
        const size_t stride = img.Stride();
        uint8_t* base = img.Data();
        auto to8 = [](float v) {
            const float q = std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f;
            return static_cast<uint8_t>(q < 0.0f ? 0.0f
                                                 : (q > 255.0f ? 255.0f : q));
        };
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = base + stride * static_cast<size_t>(y);
            for (int x = x0; x < x1; ++x) {
                if (sel && !sel->Contains(x, y)) continue;
                uint8_t* p = row + static_cast<size_t>(x) * 4;
                if (p[3] == 0) continue; // 全透明：改了也看不见
                const float r = p[0] * (1.0f / 255.0f);
                const float g = p[1] * (1.0f / 255.0f);
                const float b = p[2] * (1.0f / 255.0f);

                const float mxc = std::max(r, std::max(g, b));
                const float mnc = std::min(r, std::min(g, b));
                float li = (mxc + mnc) * 0.5f;
                float hu = 0.0f, sa = 0.0f;
                const float d = mxc - mnc;
                if (d > 1e-6f) {
                    sa = li > 0.5f ? d / (2.0f - mxc - mnc) : d / (mxc + mnc);
                    if (mxc == r)
                        hu = (g - b) / d + (g < b ? 6.0f : 0.0f);
                    else if (mxc == g)
                        hu = (b - r) / d + 2.0f;
                    else
                        hu = (r - g) / d + 4.0f;
                    hu *= (1.0f / 6.0f);
                }
                hu += dh;
                if (hu < 0.0f) hu += 1.0f;
                else if (hu >= 1.0f) hu -= 1.0f;
                if (ds != 0.0f) {
                    sa *= (1.0f + ds);
                    sa = sa < 0.0f ? 0.0f : (sa > 1.0f ? 1.0f : sa);
                }
                if (dl > 0.0f) li += (1.0f - li) * dl;
                else if (dl < 0.0f) li *= (1.0f + dl);
                li = li < 0.0f ? 0.0f : (li > 1.0f ? 1.0f : li);

                float rr, gg, bb;
                if (sa <= 0.0f) {
                    rr = gg = bb = li;
                } else {
                    const float q = li < 0.5f ? li * (1.0f + sa) : li + sa - li * sa;
                    const float pp = 2.0f * li - q;
                    rr = hsl::HueToRgbF(pp, q, hu + 1.0f / 3);
                    gg = hsl::HueToRgbF(pp, q, hu);
                    bb = hsl::HueToRgbF(pp, q, hu - 1.0f / 3);
                }
                p[0] = to8(rr);
                p[1] = to8(gg);
                p[2] = to8(bb);
            }
        }
        return;
    }

    // ---- RGBA16（保留原实现） ----
    const double mx = ChannelMax(img.Format());
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


// ---------------------------------------------------------------- 曲线（Curves）
//
// 参数从宿主以字符串下发（每个通道一条）：
//   adjust.curve.rgb = "0:0,64:50,128:128,192:205,255:255"   （输入:输出，0..255）
//   adjust.curve.r / .g / .b 同上，缺省 = 恒等
// 映射顺序与 Photoshop 一致：先各通道曲线，再叠加 RGB 合成曲线。
struct CurvePoint {
    float x = 0.0f; // 输入 0..255
    float y = 0.0f; // 输出 0..255
};

// 解析 "x:y,x:y,…"；点数 < 2 或格式错误时返回空（调用方按恒等处理）
inline std::vector<CurvePoint> ParseCurve(const std::string& s) {
    std::vector<CurvePoint> pts;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || s[i] == ' ')) ++i;
        if (i >= s.size()) break;
        const size_t c = s.find(':', i);
        if (c == std::string::npos) break;
        size_t e = s.find(',', c);
        if (e == std::string::npos) e = s.size();
        const float x = std::strtof(s.substr(i, c - i).c_str(), nullptr);
        const float y = std::strtof(s.substr(c + 1, e - c - 1).c_str(), nullptr);
        pts.push_back({std::max(0.0f, std::min(255.0f, x)),
                       std::max(0.0f, std::min(255.0f, y))});
        i = e;
    }
    std::sort(pts.begin(), pts.end(),
              [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    if (pts.size() < 2) pts.clear();
    return pts;
}

// 单调三次插值（PCHIP）→ 256 项 LUT。
// 为什么不是普通 Catmull-Rom：用户拖出"过冲"的控制点时会甩出 S 形波纹
// （曲线的 y 超出相邻点范围），而 Photoshop 的曲线永远是单调的。
inline void BuildCurveLut(const std::vector<CurvePoint>& pts, float lut[256]) {
    for (int i = 0; i < 256; ++i) lut[i] = static_cast<float>(i);
    const int n = static_cast<int>(pts.size());
    if (n < 2) return;

    // 区间斜率
    std::vector<float> h(n - 1), d(n - 1);
    for (int k = 0; k < n - 1; ++k) {
        h[k] = std::max(0.001f, pts[k + 1].x - pts[k].x);
        d[k] = (pts[k + 1].y - pts[k].y) / h[k];
    }
    std::vector<float> m(n, 0.0f);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int k = 1; k < n - 1; ++k)
        m[k] = (d[k - 1] * d[k] <= 0.0f) ? 0.0f : (d[k - 1] + d[k]) * 0.5f;

    auto eval = [&](float x) {
        if (x <= pts[0].x) return pts[0].y;
        if (x >= pts[n - 1].x) return pts[n - 1].y;
        int k = 0;
        while (k < n - 2 && x > pts[k + 1].x) ++k;
        const float t = (x - pts[k].x) / h[k];
        const float t2 = t * t, t3 = t2 * t;
        const float h00 = 2 * t3 - 3 * t2 + 1;
        const float h10 = t3 - 2 * t2 + t;
        const float h01 = -2 * t3 + 3 * t2;
        const float h11 = t3 - t2;
        return h00 * pts[k].y + h10 * h[k] * m[k] + h01 * pts[k + 1].y +
               h11 * h[k] * m[k + 1];
    };
    for (int i = 0; i < 256; ++i) {
        const float v = eval(static_cast<float>(i));
        lut[i] = std::max(0.0f, std::min(255.0f, v));
    }
}

// 曲线调整：channelLut 三张（r/g/b）+ 合成 LUT（rgb）
inline void ApplyCurves(ImageBuffer& img, const Selection* sel,
                        const float lutR[256], const float lutG[256],
                        const float lutB[256], const float lutRgb[256],
                        float inBlack = 0.0f, float inWhite = 255.0f,
                        float gamma = 1.0f) {
    const double mx = ChannelMax(img.Format());
    const double scale = mx / 255.0;
    // 先把 4 张 8bit LUT 串成"通道 → 合成"的等效表，逐像素只查两次
    float chLut[3][256];
    const float* src3[3] = {lutR, lutG, lutB};
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256; ++i) {
            const int mid = static_cast<int>(std::lround(src3[ch][i]));
            chLut[ch][i] = lutRgb[std::max(0, std::min(255, mid))];
        }

    // 输入黑场/白场/中间调：PS 的曲线面板下方就是这三块三角块，语义等同
    // 色阶 —— 串在曲线之后，所以"曲线 + 色阶"能一次调完
    {
        const double ib = std::min<double>(inBlack, inWhite - 1.0);
        const double iw = std::max<double>(inWhite, inBlack + 1.0);
        const double g = 1.0 / std::max(0.05, static_cast<double>(gamma));
        auto levels = [&](float v) {
            double t = (v - ib) / std::max(1.0, iw - ib);
            t = std::min(1.0, std::max(0.0, t));
            return static_cast<float>(std::pow(t, g) * 255.0);
        };
        for (int ch = 0; ch < 3; ++ch)
            for (int i = 0; i < 256; ++i) chLut[ch][i] = levels(chLut[ch][i]);
    }

    const int w = img.Width(), h = img.Height();
    Rect bounds = sel && !sel->IsEmpty() ? sel->Bounds() : Rect{0, 0, w, h};
    const int x0 = std::max(0, bounds.x), y0 = std::max(0, bounds.y);
    const int x1 = std::min(w, bounds.x + bounds.w);
    const int y1 = std::min(h, bounds.y + bounds.h);
    int lutIdx[3][256]; // 各格式的 256 档 → 输出值（避免逐像素乘法）
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 256; ++i)
            lutIdx[ch][i] = static_cast<int>(chLut[ch][i] * scale + 0.5);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (sel && !sel->Contains(x, y)) continue;
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            const int ir = c.r * 255 / static_cast<int>(mx);
            const int ig = c.g * 255 / static_cast<int>(mx);
            const int ib = c.b * 255 / static_cast<int>(mx);
            c.r = static_cast<uint16_t>(lutIdx[0][ir]);
            c.g = static_cast<uint16_t>(lutIdx[1][ig]);
            c.b = static_cast<uint16_t>(lutIdx[2][ib]);
            img.SetPixel(x, y, c);
        }
    }
}

} // namespace sai2::adjust

