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

enum class BlurKind { Gaussian, Box, Motion, Radial };

struct BlurParams {
    BlurKind kind = BlurKind::Gaussian;
    float radius = 4.0f;       // Gaussian / Box 半径（像素）
    float angleDeg = 0.0f;     // Motion 方向（度，0=水平向右）
    float distance = 8.0f;     // Motion 采样距离（像素）
    float amount = 0.5f;       // Radial 强度（0..1）
    float centerX = 0.0f;      // Radial 中心（默认图像中心）
    float centerY = 0.0f;
    int samples = 12;          // Motion / Radial 采样数
};

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
// Radial Blur（围绕中心旋转采样，自旋式）
// ---------------------------------------------------------------------------
inline void RadialBlur(ImageBuffer& img, float centerX, float centerY,
                       float amount, int samples, const Selection* sel) {
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
    const float totalAngle = amount * 0.5f; // 最大总角度（弧度）

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            const float rx = x - centerX, ry = y - centerY;
            const float r = std::sqrt(rx * rx + ry * ry);
            const float falloff = 1.0f - std::min(1.0f, r / maxR);
            const float angle = totalAngle * falloff;

            float acc[4] = {0, 0, 0, 0};
            for (int s = 0; s < samples; ++s) {
                const float t = samples > 1
                    ? (s / (samples - 1.0f) - 0.5f) : 0.0f;
                const float a = angle * t;
                const float ca = std::cos(a), sa = std::sin(a);
                const float fx = centerX + rx * ca - ry * sa;
                const float fy = centerY + rx * sa + ry * ca;
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
            RadialBlur(img, p.centerX, p.centerY, p.amount, p.samples, sel);
            break;
    }
}

} // namespace sai2::blur
