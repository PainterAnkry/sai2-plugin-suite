#pragma once
// ============================================================================
// LiquifyCore.h - 液化算法核心（Phase 4，纯算法，便于单元测试）
//
// 规格书第八节：
//   - Displacement Field + Inverse Mapping + Bilinear Sampling
//   - 避免反复直接修改原始像素造成画质损失：所有笔刷只修改置换场，
//     最后一次性由置换场渲染结果
//   - 工具：Forward Warp / Bloat / Pinch / Twirl / Smooth / Reconstruct
// ============================================================================

#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace sai2::liquify {

// ---------------------------------------------------------------------------
// Bilinear Sampling（边界 clamp）
// ---------------------------------------------------------------------------
inline void SampleBilinear(const ImageBuffer& src, float fx, float fy,
                           RGBA& out) {
    const int w = src.Width(), h = src.Height();
    fx = std::max(0.0f, std::min(static_cast<float>(w - 1), fx));
    fy = std::max(0.0f, std::min(static_cast<float>(h - 1), fy));

    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(w - 1, x0 + 1);
    const int y1 = std::min(h - 1, y0 + 1);
    const float tx = fx - x0, ty = fy - y0;

    RGBA c00, c10, c01, c11;
    src.GetPixel(x0, y0, c00);
    src.GetPixel(x1, y0, c10);
    src.GetPixel(x0, y1, c01);
    src.GetPixel(x1, y1, c11);

    const uint16_t* p00 = &c00.r;
    const uint16_t* p10 = &c10.r;
    const uint16_t* p01 = &c01.r;
    const uint16_t* p11 = &c11.r;
    uint16_t* po = &out.r;
    for (int ch = 0; ch < 4; ++ch) {
        const float top = p00[ch] + (p10[ch] - p00[ch]) * tx;
        const float bot = p01[ch] + (p11[ch] - p01[ch]) * tx;
        po[ch] = static_cast<uint16_t>(top + (bot - top) * ty + 0.5f);
    }
}

// ---------------------------------------------------------------------------
// Displacement Field
// 语义：dest(p) = src(p + D(p))（逆映射采样偏移）
//   - ForwardWarp（把内容沿 dir 推动）：D -= dir * falloff
//   - Bloat（内容放大）：D 指向中心
//   - Pinch（内容收缩）：D 指向外侧
// ---------------------------------------------------------------------------
class DisplacementField {
public:
    DisplacementField() = default;
    DisplacementField(int w, int h) { Reset(w, h); }

    void Reset(int w, int h) {
        w_ = w; h_ = h;
        dx_.assign(static_cast<size_t>(w) * h, 0.0f);
        dy_.assign(static_cast<size_t>(w) * h, 0.0f);
    }

    int Width() const { return w_; }
    int Height() const { return h_; }
    bool IsValid() const {
        return w_ > 0 && h_ > 0 &&
               dx_.size() == static_cast<size_t>(w_) * h_ &&
               dy_.size() == dx_.size();
    }

    float Dx(int x, int y) const {
        return dx_[static_cast<size_t>(y) * w_ + x];
    }
    float Dy(int x, int y) const {
        return dy_[static_cast<size_t>(y) * w_ + x];
    }
    void Set(int x, int y, float dx, float dy) {
        const size_t i = static_cast<size_t>(y) * w_ + x;
        dx_[i] = dx;
        dy_[i] = dy;
    }
    void Add(int x, int y, float ddx, float ddy) {
        const size_t i = static_cast<size_t>(y) * w_ + x;
        dx_[i] += ddx;
        dy_[i] += ddy;
    }

    float MaxMagnitude() const {
        float m = 0.0f;
        for (size_t i = 0; i < dx_.size(); ++i)
            m = std::max(m, std::sqrt(dx_[i] * dx_[i] + dy_[i] * dy_[i]));
        return m;
    }

    // 笔刷衰减函数（Photoshop 风格：(1 - (r/R)^2)^2）
    static float Falloff(float dist, float radius) {
        if (radius <= 0 || dist >= radius) return 0.0f;
        const float t = dist / radius;
        const float q = 1.0f - t * t;
        return q * q;
    }

    // ---- 工具 -------------------------------------------------------------
    void ForwardWarp(float cx, float cy, float radius, float dirX,
                     float dirY, float strength) {
        Stamp(cx, cy, radius, [&](int x, int y, float f) {
            Add(x, y, -dirX * strength * f, -dirY * strength * f);
        });
    }

    void Bloat(float cx, float cy, float radius, float strength) {
        // 内容放大：dest(p) 取 src 于 r/(1+a) 处 -> D = -(a/(1+a)) * (p-c)
        const float k = strength / (1.0f + strength);
        Stamp(cx, cy, radius, [&](int x, int y, float f) {
            Add(x, y, -(x - cx) * k * f, -(y - cy) * k * f);
        });
    }

    void Pinch(float cx, float cy, float radius, float strength) {
        // 内容收缩：dest(p) 取更远处 -> D = +k * (p-c)
        Stamp(cx, cy, radius, [&](int x, int y, float f) {
            Add(x, y, (x - cx) * strength * f, (y - cy) * strength * f);
        });
    }

    void Twirl(float cx, float cy, float radius, float angleRad) {
        const float ca0 = std::cos(angleRad), sa0 = std::sin(angleRad);
        Stamp(cx, cy, radius, [&](int x, int y, float f) {
            // 旋转角 = angle * falloff（分步旋转，f 当角度因子）
            const float a = angleRad * f;
            const float ca = std::cos(a), sa = std::sin(a);
            const float rx = x - cx, ry = y - cy;
            // dest(p) 取旋转后的源位置：D = R(a)*(p-c) - (p-c)
            Add(x, y, rx * ca - ry * sa - rx, rx * sa + ry * ca - ry);
            (void)ca0; (void)sa0;
        });
    }

    // Smooth：对置换场本身做一次小半径盒式模糊（抑制噪点）
    void Smooth(int kHalf = 2) {
        if (w_ <= 0 || h_ <= 0 || kHalf < 1) return;
        const float norm = 1.0f / (2 * kHalf + 1);
        SmoothPass(dx_, kHalf, norm);
        SmoothPass(dy_, kHalf, norm);
    }

    // Reconstruct：把场向零恢复（strength=1 完全重建）
    void Reconstruct(float cx, float cy, float radius, float strength) {
        Stamp(cx, cy, radius, [&](int x, int y, float f) {
            const size_t i = static_cast<size_t>(y) * w_ + x;
            const float k = 1.0f - strength * f;
            dx_[i] *= k;
            dy_[i] *= k;
        });
    }

    // 全域重建（Photoshop"整体重建"）：不经过笔刷衰减，直接全图向零恢复
    void ReconstructAll(float strength) {
        const float k = 1.0f - std::min(1.0f, std::max(0.0f, strength));
        for (size_t i = 0; i < dx_.size(); ++i) {
            dx_[i] *= k;
            dy_[i] *= k;
        }
    }

private:
    template <typename Fn>
    void Stamp(float cx, float cy, float radius, Fn&& fn) {
        const int x0 = std::max(0, static_cast<int>(cx - radius));
        const int x1 = std::min(w_ - 1, static_cast<int>(cx + radius));
        const int y0 = std::max(0, static_cast<int>(cy - radius));
        const int y1 = std::min(h_ - 1, static_cast<int>(cy + radius));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const float dist = std::sqrt((x - cx) * (x - cx) +
                                             (y - cy) * (y - cy));
                const float f = Falloff(dist, radius);
                if (f > 0.0f) fn(x, y, f);
            }
    }

    void SmoothPass(std::vector<float>& field, int kHalf, float norm) {
        std::vector<float> tmp(field.size());
        // 水平
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                float acc = 0;
                for (int i = -kHalf; i <= kHalf; ++i) {
                    const int sx = std::min(w_ - 1, std::max(0, x + i));
                    acc += field[static_cast<size_t>(y) * w_ + sx];
                }
                tmp[static_cast<size_t>(y) * w_ + x] = acc * norm;
            }
        // 垂直
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                float acc = 0;
                for (int i = -kHalf; i <= kHalf; ++i) {
                    const int sy = std::min(h_ - 1, std::max(0, y + i));
                    acc += tmp[static_cast<size_t>(sy) * w_ + x];
                }
                field[static_cast<size_t>(y) * w_ + x] = acc * norm;
            }
    }

    int w_ = 0, h_ = 0;
    std::vector<float> dx_, dy_;
};

// 单波段渲染（多线程时每个线程负责一段行区间，互不重叠）
inline void RenderLiquifyRows(const ImageBuffer& src,
                              const DisplacementField& field,
                              ImageBuffer& dst, const Rect& roi, int y0,
                              int y1) {
    const int w = src.Width();
    for (int y = y0; y < y1; ++y) {
        const bool rowInRoi = y >= roi.y && y < roi.y + roi.h;
        for (int x = 0; x < w; ++x) {
            if (!rowInRoi || x < roi.x || x >= roi.x + roi.w) {
                RGBA keep;
                src.GetPixel(x, y, keep);
                dst.SetPixel(x, y, keep);
                continue;
            }
            RGBA c;
            SampleBilinear(src, x + field.Dx(x, y), y + field.Dy(x, y), c);
            dst.SetPixel(x, y, c);
        }
    }
}

// 渲染：按置换场逆映射整幅图像（一次性采样，画质无损策略）。
// 液化笔画要求实时（鼠标移动逐帧调用），整幅 4~6MP 的双线性逆映射在
// 单线程下要 100ms+，是"笔刷卡顿"的主因 —— 按行分波段多线程并行。
inline void RenderLiquify(const ImageBuffer& src,
                          const DisplacementField& field,
                          ImageBuffer& dst, const Selection* sel) {
    if (!src.IsValid() || !field.IsValid()) return;
    if (dst.Width() != src.Width() || dst.Height() != src.Height() ||
        dst.Format() != src.Format())
        dst.Create(src.Width(), src.Height(), src.Format());

    const int w = src.Width(), h = src.Height();
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, w, h};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(w - roi.x, roi.w);
    roi.h = std::min(h - roi.y, roi.h);

    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 1;
    const int bands = static_cast<int>(std::min(hc, 16u));
    if (bands < 2 || h < 128) {
        RenderLiquifyRows(src, field, dst, roi, 0, h);
        return;
    }
    const int band = (h + bands - 1) / bands;
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(bands));
    for (int i = 0; i < bands; ++i) {
        const int y0 = i * band;
        if (y0 >= h) break;
        pool.emplace_back([&src, &field, &dst, roi, y0,
                           y1 = std::min(h, y0 + band)]() mutable {
            RenderLiquifyRows(src, field, dst, roi, y0, y1);
        });
    }
    for (auto& t : pool) t.join();
}

} // namespace sai2::liquify
