#pragma once
// ============================================================================
// BlurKernels.h - 模糊算法核心（Phase 3，纯算法，便于单元测试）
//
// 规格书第十节：
//   - Gaussian Blur 使用可分离卷积 Horizontal Blur -> Vertical Blur
//   - Motion Blur 方向采样；Radial Blur 围绕中心采样
// 所有算法支持 RGBA8 / RGBA16（通道值按格式原样处理），并尊重 Selection。
// ============================================================================

#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sai2::blur {

enum class BlurKind { Gaussian, Box, Motion, Radial, Surface, Smart, Lens, Average };

struct BlurParams {
    BlurKind kind = BlurKind::Gaussian;
    float radius = 4.0f;       // Gaussian / Box / Surface / Smart / Lens 半径（像素）
    float angleDeg = 0.0f;     // Motion 方向（度，0=水平向右）
    float distance = 8.0f;     // Motion 采样距离（像素）
    float amount = 0.5f;       // Radial 数量（0..1）
    float threshold = 15.0f;   // Surface / Smart 阈值（0..255 口径）
    float highlight = 200.0f;  // Lens 高光阈值（0..255）
    int method = 0;            // Radial：0 = 旋转，1 = 缩放
    int quality = 1;           // 0 草图 / 1 好 / 2 最好
    int mode = 0;              // Smart：0 正常 / 1 仅边缘 / 2 叠加边缘
    float centerX = 0.0f;      // Radial / Lens 中心（默认图像中心）
    float centerY = 0.0f;
    int samples = 12;          // Motion / Radial 采样数
};

// 品质 → 采样数（PS 的"草图/好/最好"三档）
inline int SamplesForQuality(int quality, int lo, int mid, int hi) {
    if (quality <= 0) return lo;
    if (quality >= 2) return hi;
    return mid;
}

// ---------------------------------------------------------------------------
// 通道级读写辅助（把 RGBA 当 4 个 float 通道处理）
// ---------------------------------------------------------------------------
inline float GetChannel(const ImageBuffer& img, int x, int y, int ch) {
    RGBA c;
    img.GetPixel(x, y, c);
    const uint16_t v[4] = {c.r, c.g, c.b, c.a};
    return static_cast<float>(v[ch]);
}

inline void SetChannel(ImageBuffer& img, int x, int y, int ch, float v) {
    RGBA c;
    img.GetPixel(x, y, c);
    uint16_t arr[4] = {c.r, c.g, c.b, c.a};
    v = std::min(65535.0f, std::max(0.0f, v));
    arr[ch] = static_cast<uint16_t>(v + 0.5f);
    c.r = arr[0]; c.g = arr[1]; c.b = arr[2]; c.a = arr[3];
    img.SetPixel(x, y, c);
}

// 高斯核（sigma 与 kernel 半径由 radius 推导）
inline std::vector<float> GaussianKernel(float radius) {
    const float sigma = std::max(0.4f, radius * 0.5f);
    const int k = std::max(1, static_cast<int>(std::ceil(radius)));
    std::vector<float> kernel(2 * k + 1);
    float sum = 0.0f;
    for (int i = -k; i <= k; ++i) {
        const float v = std::exp(-(i * i) / (2.0f * sigma * sigma));
        kernel[i + k] = v;
        sum += v;
    }
    for (float& v : kernel) v /= sum;
    return kernel;
}

namespace detail {

// 单通道可分离水平/垂直模糊（边界 clamp）
template <typename KernelFn>
void SeparablePass(ImageBuffer& img, const Rect& roi, int kHalf,
                   KernelFn kernelAt, std::vector<float>& temp) {
    const int w = img.Width(), h = img.Height();
    temp.assign(static_cast<size_t>(roi.w) * roi.h * 4, 0.0f);

    // Horizontal：src -> temp
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -kHalf; i <= kHalf; ++i) {
                const int sx = std::min(w - 1, std::max(0, x + i));
                const float kv = kernelAt(i + kHalf);
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += GetChannel(img, sx, y, ch) * kv;
            }
            const size_t idx =
                (static_cast<size_t>(y - roi.y) * roi.w + (x - roi.x)) * 4;
            for (int ch = 0; ch < 4; ++ch) temp[idx + ch] = acc[ch];
        }
    }
    // Vertical：temp -> img
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = -kHalf; i <= kHalf; ++i) {
                const int sy = std::min(h - 1, std::max(0, y + i));
                const size_t idx =
                    (static_cast<size_t>(sy - roi.y) * roi.w + (x - roi.x)) * 4;
                const float kv = kernelAt(i + kHalf);
                for (int ch = 0; ch < 4; ++ch) acc[ch] += temp[idx + ch] * kv;
            }
            for (int ch = 0; ch < 4; ++ch)
                SetChannel(img, x, y, ch, acc[ch]);
        }
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Gaussian Blur（可分离卷积）
// ---------------------------------------------------------------------------
inline void GaussianBlur(ImageBuffer& img, float radius,
                         const Selection* sel) {
    if (!img.IsValid() || radius < 0.5f) return;
    const std::vector<float> kernel = GaussianKernel(radius);
    const int kHalf = static_cast<int>(kernel.size()) / 2;

    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(img.Width() - roi.x, roi.w);
    roi.h = std::min(img.Height() - roi.y, roi.h);
    if (roi.w <= 0 || roi.h <= 0) return;

    std::vector<float> temp;
    detail::SeparablePass(
        img, roi, kHalf,
        [&](size_t i) { return kernel[i]; }, temp);
}

// ---------------------------------------------------------------------------
// Box Blur（可分离滑动平均）
// ---------------------------------------------------------------------------
inline void BoxBlur(ImageBuffer& img, float radius, const Selection* sel) {
    if (!img.IsValid() || radius < 0.5f) return;
    const int kHalf = std::max(1, static_cast<int>(radius));
    const float norm = 1.0f / (2 * kHalf + 1);

    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(img.Width() - roi.x, roi.w);
    roi.h = std::min(img.Height() - roi.y, roi.h);
    if (roi.w <= 0 || roi.h <= 0) return;

    std::vector<float> temp;
    detail::SeparablePass(
        img, roi, kHalf, [&](size_t) { return norm; }, temp);
}

// ---------------------------------------------------------------------------
// Motion Blur（方向采样）
// ---------------------------------------------------------------------------
inline void MotionBlur(ImageBuffer& img, float angleDeg, float distance,
                       int samples, const Selection* sel) {
    if (!img.IsValid() || distance < 1.0f || samples < 2) return;
    const int w = img.Width(), h = img.Height();
    const float rad = angleDeg * 3.14159265358979f / 180.0f;
    const float dx = std::cos(rad), dy = std::sin(rad);

    Rect roi = sel ? sel->Bounds() : Rect{0, 0, w, h};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(w - roi.x, roi.w);
    roi.h = std::min(h - roi.y, roi.h);
    if (roi.w <= 0 || roi.h <= 0) return;

    // 复制源，避免采样到已修改数据
    const ImageBuffer src = img.Clone();
    const float step = distance / (samples - 1);

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int s = 0; s < samples; ++s) {
                const float off = (s - (samples - 1) * 0.5f) * step;
                const int sx = std::min(w - 1, std::max(0,
                        static_cast<int>(x + dx * off + 0.5f)));
                const int sy = std::min(h - 1, std::max(0,
                        static_cast<int>(y + dy * off + 0.5f)));
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += GetChannel(src, sx, sy, ch);
            }
            for (int ch = 0; ch < 4; ++ch)
                SetChannel(img, x, y, ch, acc[ch] / samples);
        }
    }
}

// ---------------------------------------------------------------------------
// Radial Blur（围绕中心采样）
//   method = 0「旋转」：按角度旋转采样（自旋拖影，PS 默认）
//   method = 1「缩放」：沿中心 → 像素的射线做放射状拖影
// ---------------------------------------------------------------------------
inline void RadialBlur(ImageBuffer& img, float centerX, float centerY,
                       float amount, int method, int samples,
                       const Selection* sel) {
    if (!img.IsValid() || amount <= 0.0f || samples < 2) return;
    const int w = img.Width(), h = img.Height();
    if (centerX <= 0 && centerY <= 0) {
        centerX = w * 0.5f;
        centerY = h * 0.5f;
    }
    const float maxR =
        std::sqrt(static_cast<float>(w * w + h * h)) * 0.5f;

    Rect roi = sel ? sel->Bounds() : Rect{0, 0, w, h};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(w - roi.x, roi.w);
    roi.h = std::min(h - roi.y, roi.h);
    if (roi.w <= 0 || roi.h <= 0) return;

    const ImageBuffer src = img.Clone();
    const float totalAngle = amount * 0.5f; // 旋转：最大总角度（弧度）
    const float zoomSpan = amount * 0.45f;  // 缩放：最大额外倍率

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            const float rx = x - centerX, ry = y - centerY;
            const float r = std::sqrt(rx * rx + ry * ry);
            const float falloff = 1.0f - std::min(1.0f, r / maxR);

            float acc[4] = {0, 0, 0, 0};
            for (int s = 0; s < samples; ++s) {
                const float t = samples > 1
                    ? (s / (samples - 1.0f) - 0.5f) : 0.0f;
                float fx, fy;
                if (method == 1) {
                    // 缩放：沿射线前后取样（t>0 往外、t<0 往内）
                    const float scale = 1.0f + zoomSpan * falloff * t * 2.0f;
                    fx = centerX + rx * scale;
                    fy = centerY + ry * scale;
                } else {
                    const float a = totalAngle * falloff * t;
                    const float ca = std::cos(a), sa = std::sin(a);
                    fx = centerX + rx * ca - ry * sa;
                    fy = centerY + rx * sa + ry * ca;
                }
                const int sx = std::min(w - 1, std::max(0,
                        static_cast<int>(fx + 0.5f)));
                const int sy = std::min(h - 1, std::max(0,
                        static_cast<int>(fy + 0.5f)));
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += GetChannel(src, sx, sy, ch);
            }
            for (int ch = 0; ch < 4; ++ch)
                SetChannel(img, x, y, ch, acc[ch] / samples);
        }
    }
}

namespace detail {

inline float Luma16(const ImageBuffer& img, int x, int y) {
    RGBA c;
    img.GetPixel(x, y, c);
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

inline void ClampRoi(Rect& roi, const ImageBuffer& img) {
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(img.Width() - roi.x, roi.w);
    roi.h = std::min(img.Height() - roi.y, roi.h);
}

// 保边平滑（表面模糊 / 特殊模糊共用）：先横后竖两趟，只平均"亮度差在阈值内"
// 的邻居。PS 的这两个滤镜本身就是这种"按相似度加权"的思路；分离成两趟是
// 精度上的取舍，换来的是预览时能跑到实时（O(r) 而不是 O(r²)）。
inline void EdgeAwarePass(ImageBuffer& img, const Rect& roi, float radius,
                          float threshold) {
    const int w = img.Width(), h = img.Height();
    const int r = std::max(1, static_cast<int>(radius));
    const float thr = std::max(1.0f, threshold);

    std::vector<float> tmp(static_cast<size_t>(roi.w) * roi.h * 4, 0.0f);

    // 横向：src -> tmp（中心像素取原图，比较基准不随结果漂移）
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            const float l0 = Luma16(img, x, y);
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.0f;
            for (int i = -r; i <= r; ++i) {
                const int sx = std::min(w - 1, std::max(0, x + i));
                const float d = std::fabs(Luma16(img, sx, y) - l0);
                if (d > thr) continue;
                // 三角形权重：越近越重；相似度越高越重（PS 的平滑感来源）
                const float wgt = (1.0f - std::fabs(static_cast<float>(i)) /
                                               (r + 1.0f)) *
                                  (1.0f - d / thr * 0.5f);
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += GetChannel(img, sx, y, ch) * wgt;
                wsum += wgt;
            }
            const size_t idx =
                (static_cast<size_t>(y - roi.y) * roi.w + (x - roi.x)) * 4;
            for (int ch = 0; ch < 4; ++ch)
                tmp[idx + ch] = wsum > 0.0f ? acc[ch] / wsum
                                            : GetChannel(img, x, y, ch);
        }
    }
    // 纵向：tmp -> img
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            const float l0 = Luma16(img, x, y);
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.0f;
            for (int i = -r; i <= r; ++i) {
                const int sy = std::min(h - 1, std::max(0, y + i));
                const float d = std::fabs(Luma16(img, x, sy) - l0);
                if (d > thr) continue;
                const float wgt = (1.0f - std::fabs(static_cast<float>(i)) /
                                               (r + 1.0f)) *
                                  (1.0f - d / thr * 0.5f);
                const size_t idx =
                    (static_cast<size_t>(sy - roi.y) * roi.w + (x - roi.x)) * 4;
                for (int ch = 0; ch < 4; ++ch) acc[ch] += tmp[idx + ch] * wgt;
                wsum += wgt;
            }
            const size_t idx =
                (static_cast<size_t>(y - roi.y) * roi.w + (x - roi.x)) * 4;
            for (int ch = 0; ch < 4; ++ch)
                SetChannel(img, x, y, ch,
                           wsum > 0.0f ? acc[ch] / wsum : tmp[idx + ch]);
        }
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Surface Blur（表面模糊）：保边平滑，阈值 = 允许多大的亮度差被抹平
// ---------------------------------------------------------------------------
inline void SurfaceBlur(ImageBuffer& img, float radius, float threshold,
                        const Selection* sel) {
    if (!img.IsValid() || radius < 1.0f) return;
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    detail::ClampRoi(roi, img);
    if (roi.w <= 0 || roi.h <= 0) return;
    detail::EdgeAwarePass(img, roi, radius, threshold);
}

// ---------------------------------------------------------------------------
// Smart Blur（特殊模糊）：半径 / 阈值 / 品质 / 模式
//   正常   = 保边平滑
//   仅边缘 = 只保留被判定为"边缘"的像素（白底黑线）
//   叠加边缘 = 原图上叠加边缘线
// ---------------------------------------------------------------------------
inline void SmartBlur(ImageBuffer& img, float radius, float threshold,
                      int mode, const Selection* sel) {
    if (!img.IsValid() || radius < 1.0f) return;
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    detail::ClampRoi(roi, img);
    if (roi.w <= 0 || roi.h <= 0) return;

    const ImageBuffer orig = img.Clone();
    detail::EdgeAwarePass(img, roi, radius, threshold);
    if (mode <= 0) return;

    const int w = img.Width(), h = img.Height();
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            const float l0 = detail::Luma16(orig, x, y);
            float lb = 0.0f;
            {
                RGBA c;
                img.GetPixel(x, y, c);
                lb = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            }
            // 平滑前后的差 = 边缘强度；阈值越小越敏感
            const float edge =
                std::min(1.0f, std::fabs(l0 - lb) / std::max(1.0f, threshold));
            RGBA o;
            orig.GetPixel(x, y, o);
            RGBA out = o;
            const float k = 65535.0f;
            if (mode == 1) {
                out.r = out.g = out.b =
                    static_cast<uint16_t>(k * (1.0f - edge) + 0.5f);
            } else {
                // 叠加：原图 + 边缘（提亮暗侧，压暗暗侧以外保持原样）
                const float add = edge * 0.9f * k;
                out.r = static_cast<uint16_t>(std::min(k, o.r + add));
                out.g = static_cast<uint16_t>(std::min(k, o.g + add));
                out.b = static_cast<uint16_t>(std::min(k, o.b + add));
            }
            img.SetPixel(x, y, out);
        }
    }
}

// ---------------------------------------------------------------------------
// Lens Blur（镜头模糊）：圆盘采样 + 高光溢出（"光圈"感）
//   highlight = 高光阈值（0..255 口径）：比它亮的像素在合成里权重更高，
//   于是亮部会晕成光斑 —— 这就是 PS 镜头模糊里"镜面高光"的观感来源。
// ---------------------------------------------------------------------------
inline void LensBlur(ImageBuffer& img, float radius, float highlight,
                     const Selection* sel) {
    if (!img.IsValid() || radius < 1.0f) return;
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    detail::ClampRoi(roi, img);
    if (roi.w <= 0 || roi.h <= 0) return;

    const int w = img.Width(), h = img.Height();
    const ImageBuffer src = img.Clone();
    const int samples = std::max(8, std::min(48, static_cast<int>(radius * 3)));
    const float hi = std::max(0.0f, highlight) * 257.0f; // 8bit → 16bit

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.0f;
            for (int s = 0; s < samples; ++s) {
                // 黄金角螺旋：均匀铺满圆盘，避免规则采样产生的环纹
                const float t = (s + 0.5f) / samples;
                const float ang = s * 2.39996323f;
                const float rr = radius * std::sqrt(t);
                const int sx = std::min(w - 1, std::max(0,
                        static_cast<int>(x + std::cos(ang) * rr + 0.5f)));
                const int sy = std::min(h - 1, std::max(0,
                        static_cast<int>(y + std::sin(ang) * rr + 0.5f)));
                const float l = detail::Luma16(src, sx, sy);
                const float wgt = (l >= hi && hi > 0.0f) ? 3.0f : 1.0f;
                for (int ch = 0; ch < 4; ++ch)
                    acc[ch] += GetChannel(src, sx, sy, ch) * wgt;
                wsum += wgt;
            }
            for (int ch = 0; ch < 4; ++ch)
                SetChannel(img, x, y, ch, wsum > 0.0f ? acc[ch] / wsum : 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Average（平均）：整块填成平均色
// ---------------------------------------------------------------------------
inline void AverageBlur(ImageBuffer& img, const Selection* sel) {
    if (!img.IsValid()) return;
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    detail::ClampRoi(roi, img);
    if (roi.w <= 0 || roi.h <= 0) return;
    double sum[4] = {0, 0, 0, 0};
    for (int y = roi.y; y < roi.y + roi.h; ++y)
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            RGBA c;
            img.GetPixel(x, y, c);
            sum[0] += c.r; sum[1] += c.g; sum[2] += c.b; sum[3] += c.a;
        }
    const double n = static_cast<double>(roi.w) * roi.h;
    RGBA fill;
    fill.r = static_cast<uint16_t>(sum[0] / n + 0.5);
    fill.g = static_cast<uint16_t>(sum[1] / n + 0.5);
    fill.b = static_cast<uint16_t>(sum[2] / n + 0.5);
    fill.a = static_cast<uint16_t>(sum[3] / n + 0.5);
    for (int y = roi.y; y < roi.y + roi.h; ++y)
        for (int x = roi.x; x < roi.x + roi.w; ++x) img.SetPixel(x, y, fill);
}

// 统一入口
inline void ApplyBlur(ImageBuffer& img, const BlurParams& p,
                      const Selection* sel) {
    switch (p.kind) {
        case BlurKind::Gaussian:
            GaussianBlur(img, p.radius, sel);
            break;
        case BlurKind::Box:
            BoxBlur(img, p.radius, sel);
            break;
        case BlurKind::Motion:
            MotionBlur(img, p.angleDeg, p.distance, p.samples, sel);
            break;
        case BlurKind::Radial:
            RadialBlur(img, p.centerX, p.centerY, p.amount, p.method,
                       p.samples, sel);
            break;
        case BlurKind::Surface:
            SurfaceBlur(img, p.radius, p.threshold, sel);
            break;
        case BlurKind::Smart:
            SmartBlur(img, p.radius, p.threshold, p.mode, sel);
            break;
        case BlurKind::Lens:
            LensBlur(img, p.radius, p.highlight, sel);
            break;
        case BlurKind::Average:
            AverageBlur(img, sel);
            break;
    }
}

} // namespace sai2::blur
