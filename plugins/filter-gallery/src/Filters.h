#pragma once
// ============================================================================
// Filters.h - Filter Gallery 滤镜算法核心（Phase 5，纯算法，便于单元测试）
//
// 规格书第九节：分类 Artistic / Sketch / Texture / Stylize，
// 实现示例滤镜：油画、水彩、粗糙蜡笔、素描、炭笔、粉笔、纹理化、颗粒、
// 浮雕、查找边缘（共 10 个）。
// 所有算法：
//   - 支持 RGBA8 / RGBA16（通道值按格式原样处理）
//   - 尊重 Selection（仅处理选区 Bounds 内像素；蒙版内像素才修改）
//   - 使用确定性噪声（hash），测试可复现
// ============================================================================

#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sai2::fg {

// ---------------------------------------------------------------------------
// 通用辅助
// ---------------------------------------------------------------------------

// 选区 -> 处理 ROI（与 BlurKernels 相同的裁剪逻辑）
inline Rect FilterRoi(const ImageBuffer& img, const Selection* sel) {
    Rect roi = sel ? sel->Bounds() : Rect{0, 0, img.Width(), img.Height()};
    roi.x = std::max(0, roi.x);
    roi.y = std::max(0, roi.y);
    roi.w = std::min(img.Width() - roi.x, roi.w);
    roi.h = std::min(img.Height() - roi.y, roi.h);
    return roi;
}

// 蒙版选区：返回该像素是否允许修改（统一走 Contains：矩形/蒙版语义一致）
inline bool SelectionAllows(const Selection* sel, int x, int y) {
    return !sel || sel->Contains(x, y);
}

inline float Luma(const RGBA& c) {
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

// clamp 采样
inline RGBA SampleClamp(const ImageBuffer& img, int x, int y) {
    RGBA c;
    img.GetPixel(std::min(img.Width() - 1, std::max(0, x)),
                 std::min(img.Height() - 1, std::max(0, y)), c);
    return c;
}

inline uint16_t ClampCh(float v, uint32_t maxV) {
    v = std::min(static_cast<float>(maxV),
                 std::max(0.0f, v));
    return static_cast<uint16_t>(v + 0.5f);
}

// 确定性 hash 噪声：[0,1)
inline float HashNoise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

// 对单像素应用变换后写回（保留原 alpha）
template <typename Fn>
inline void ForEachPixel(ImageBuffer& img, const Selection* sel, Fn&& fn) {
    const Rect roi = FilterRoi(img, sel);
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA src;
            img.GetPixel(x, y, src);
            RGBA dst = fn(x, y, src, maxV);
            dst.a = src.a;
            img.SetPixel(x, y, dst);
        }
    }
}

// ---------------------------------------------------------------------------
// 1. Oil Paint 油画（Artistic）：邻域强度直方图投票 + 主桶平均色
// ---------------------------------------------------------------------------
inline void OilPaint(ImageBuffer& img, int radius, int levels,
                          const Selection* sel) {
    if (!img.IsValid()) return;
    radius = std::max(1, std::min(8, radius));
    levels = std::max(2, std::min(64, levels));
    const Rect roi = FilterRoi(img, sel);
    const ImageBuffer src = img.Clone();
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float lv = static_cast<float>(levels);

    std::vector<int> count(static_cast<size_t>(levels));
    std::vector<double> sr(static_cast<size_t>(levels)),
        sg(static_cast<size_t>(levels)), sb(static_cast<size_t>(levels));

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA center;
            src.GetPixel(x, y, center);
            std::fill(count.begin(), count.end(), 0);
            std::fill(sr.begin(), sr.end(), 0.0);
            std::fill(sg.begin(), sg.end(), 0.0);
            std::fill(sb.begin(), sb.end(), 0.0);
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    const RGBA c = SampleClamp(src, x + dx, y + dy);
                    const int bin = std::min(levels - 1,
                        static_cast<int>(Luma(c) / (maxV + 1.0f) * lv));
                    ++count[bin];
                    sr[bin] += c.r; sg[bin] += c.g; sb[bin] += c.b;
                }
            }
            int best = 0;
            for (int i = 1; i < levels; ++i)
                if (count[i] > count[best]) best = i;
            const double inv = 1.0 / std::max(1, count[best]);
            RGBA out;
            out.r = ClampCh(static_cast<float>(sr[best] * inv), maxV);
            out.g = ClampCh(static_cast<float>(sg[best] * inv), maxV);
            out.b = ClampCh(static_cast<float>(sb[best] * inv), maxV);
            out.a = center.a;
            img.SetPixel(x, y, out);
        }
    }
}

// ---------------------------------------------------------------------------
// 可分离盒式模糊（水彩/素描的内部步骤）
// ---------------------------------------------------------------------------
inline void FastBoxBlur(const ImageBuffer& src, ImageBuffer& dst,
                        int radius) {
    if (!src.IsValid() || radius < 1) { dst = src.Clone(); return; }
    const int w = src.Width(), h = src.Height();
    const uint32_t maxV = src.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    // 目标缓冲区按需创建：调用方（Watercolor / SketchDodge）传进来的
    // 通常是默认构造的空 buffer，不创建的话 SetPixel 会全部越界失败，
    // 模糊结果恒为空 —— 素描会因此完全失效、水彩会退化成单纯压暗。
    if (dst.Width() != w || dst.Height() != h || dst.Format() != src.Format())
        dst.Create(w, h, src.Format());
    ImageBuffer tmp;
    tmp.Create(w, h, src.Format());

    auto blurH = [&](const ImageBuffer& in, ImageBuffer& out) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; ++i) {
                    const RGBA c = SampleClamp(in, x + i, y);
                    acc[0] += c.r; acc[1] += c.g; acc[2] += c.b; acc[3] += c.a;
                }
                const float inv = 1.0f / (2 * radius + 1);
                RGBA o;
                o.r = ClampCh(acc[0] * inv, maxV);
                o.g = ClampCh(acc[1] * inv, maxV);
                o.b = ClampCh(acc[2] * inv, maxV);
                o.a = ClampCh(acc[3] * inv, maxV);
                out.SetPixel(x, y, o);
            }
    };
    auto blurV = [&](const ImageBuffer& in, ImageBuffer& out) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int i = -radius; i <= radius; ++i) {
                    const RGBA c = SampleClamp(in, x, y + i);
                    acc[0] += c.r; acc[1] += c.g; acc[2] += c.b; acc[3] += c.a;
                }
                const float inv = 1.0f / (2 * radius + 1);
                RGBA o;
                o.r = ClampCh(acc[0] * inv, maxV);
                o.g = ClampCh(acc[1] * inv, maxV);
                o.b = ClampCh(acc[2] * inv, maxV);
                o.a = ClampCh(acc[3] * inv, maxV);
                out.SetPixel(x, y, o);
            }
    };
    blurH(src, tmp);
    blurV(tmp, dst);
}

// ---------------------------------------------------------------------------
// 2. Watercolor 水彩（Artistic）：柔化 + 饱和度提升 + 边缘沉淀
// ---------------------------------------------------------------------------
inline void Watercolor(ImageBuffer& img, int radius, float saturation,
                       float edgeDarken, const Selection* sel) {
    if (!img.IsValid()) return;
    radius = std::max(1, std::min(10, radius));
    const Rect roi = FilterRoi(img, sel);
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;

    ImageBuffer blurred;
    FastBoxBlur(img, blurred, radius);

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA c, b;
            img.GetPixel(x, y, c);
            blurred.GetPixel(x, y, b);

            // 边缘强度：柔化图梯度
            const float lx = Luma(SampleClamp(blurred, x + 1, y)) -
                             Luma(SampleClamp(blurred, x - 1, y));
            const float ly = Luma(SampleClamp(blurred, x, y + 1)) -
                             Luma(SampleClamp(blurred, x, y - 1));
            const float edge = std::min(1.0f,
                std::sqrt(lx * lx + ly * ly) / (maxV * 0.15f));

            // 基色 = 原色与柔化色混合（保留一些笔触）
            float r = c.r * 0.4f + b.r * 0.6f;
            float g = c.g * 0.4f + b.g * 0.6f;
            float bl = c.b * 0.4f + b.b * 0.6f;

            // 饱和度提升（围绕亮度）
            const float l = 0.299f * r + 0.587f * g + 0.114f * bl;
            r = l + (r - l) * (1.0f + saturation);
            g = l + (g - l) * (1.0f + saturation);
            bl = l + (bl - l) * (1.0f + saturation);

            // 边缘沉淀变暗
            const float k = 1.0f - edge * edgeDarken;
            RGBA out;
            out.r = ClampCh(r * k, maxV);
            out.g = ClampCh(g * k, maxV);
            out.b = ClampCh(bl * k, maxV);
            out.a = c.a;
            img.SetPixel(x, y, out);
        }
    }
}

// ---------------------------------------------------------------------------
// 3. Rough Pastel 粗糙蜡笔（Artistic）：色阶量化 + 纸纹 + 分块明度扰动
// ---------------------------------------------------------------------------
inline void RoughPastel(ImageBuffer& img, int cellSize, int levels,
                        float textureAmount, const Selection* sel) {
    if (!img.IsValid()) return;
    cellSize = std::max(2, cellSize);
    levels = std::max(2, std::min(16, levels));
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float step = (maxV + 1.0f) / levels;

    ForEachPixel(img, sel, [&](int x, int y, RGBA c, uint32_t mv) {
        // 色阶量化
        float r = std::floor(c.r / step) * step + step * 0.5f;
        float g = std::floor(c.g / step) * step + step * 0.5f;
        float b = std::floor(c.b / step) * step + step * 0.5f;

        // 分块亮度扰动（蜡笔块面感）
        const float block = HashNoise(x / cellSize, y / cellSize, 71) - 0.5f;
        const float k = 1.0f + block * 0.12f;

        // 纸纹（高频噪声）
        const float grain = (HashNoise(x, y, 913) - 0.5f) * textureAmount * maxV;

        RGBA out;
        out.r = ClampCh(r * k + grain, mv);
        out.g = ClampCh(g * k + grain, mv);
        out.b = ClampCh(b * k + grain, mv);
        return out;
    });
}

// ---------------------------------------------------------------------------
// 铅笔素描核心：Color Dodge(gray, invertBlur(gray))
// ---------------------------------------------------------------------------
inline void SketchDodge(ImageBuffer& img, int blurRadius, float darken,
                        const Selection* sel) {
    if (!img.IsValid()) return;
    const Rect roi = FilterRoi(img, sel);
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float mx = static_cast<float>(maxV);

    // 灰度图
    ImageBuffer gray;
    gray.Create(img.Width(), img.Height(), img.Format());
    for (int y = 0; y < img.Height(); ++y)
        for (int x = 0; x < img.Width(); ++x) {
            RGBA c;
            img.GetPixel(x, y, c);
            const float l = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
            RGBA g;
            g.r = g.g = g.b = ClampCh(l, maxV);
            g.a = c.a;
            gray.SetPixel(x, y, g);
        }

    // 反色 + 模糊
    ImageBuffer inv;
    inv.Create(img.Width(), img.Height(), img.Format());
    for (int y = 0; y < img.Height(); ++y)
        for (int x = 0; x < img.Width(); ++x) {
            RGBA c;
            gray.GetPixel(x, y, c);
            RGBA t;
            t.r = t.g = t.b = ClampCh(mx - c.r, maxV);
            t.a = c.a;
            inv.SetPixel(x, y, t);
        }
    ImageBuffer invBlur;
    FastBoxBlur(inv, invBlur, blurRadius);

    // Color Dodge: out = gray / (1 - invBlur)，加 darken 整体压暗
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA c, b;
            img.GetPixel(x, y, c);
            invBlur.GetPixel(x, y, b);
            RGBA out;
            for (int ch = 0; ch < 3; ++ch) {
                const float gch = ch == 0 ? c.r : ch == 1 ? c.g : c.b;
                const float v = b.r; // 灰度图三通道一致
                float res = (v >= mx) ? mx
                    : std::min(mx, gch * mx / (mx - v));
                res *= darken;
                if (ch == 0) out.r = ClampCh(res, maxV);
                else if (ch == 1) out.g = ClampCh(res, maxV);
                else out.b = ClampCh(res, maxV);
            }
            out.a = c.a;
            img.SetPixel(x, y, out);
        }
    }
}

// ---------------------------------------------------------------------------
// 4. Pencil Sketch 素描（Sketch）
// ---------------------------------------------------------------------------
inline void PencilSketch(ImageBuffer& img, int blurRadius,
                         const Selection* sel) {
    SketchDodge(img, std::max(2, blurRadius), 1.0f, sel);
}

// ---------------------------------------------------------------------------
// 5. Charcoal 炭笔（Sketch）：素描基础上压暗并加炭粒噪声
// ---------------------------------------------------------------------------
inline void Charcoal(ImageBuffer& img, int blurRadius, float grainAmount,
                     const Selection* sel) {
    SketchDodge(img, std::max(2, blurRadius), 0.62f, sel);
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    ForEachPixel(img, sel, [&](int x, int y, RGBA c, uint32_t mv) {
        const float n = (HashNoise(x, y, 457) - 0.5f) * grainAmount * mv;
        RGBA out;
        out.r = ClampCh(c.r + n, mv);
        out.g = ClampCh(c.g + n, mv);
        out.b = ClampCh(c.b + n, mv);
        return out;
    });
}

// ---------------------------------------------------------------------------
// 6. Chalk 粉笔（Sketch）：灰度 + 噪声阈值保留颗粒笔触
// ---------------------------------------------------------------------------
inline void Chalk(ImageBuffer& img, float strength, const Selection* sel) {
    if (!img.IsValid()) return;
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float mx = static_cast<float>(maxV);
    ForEachPixel(img, sel, [&](int x, int y, RGBA c, uint32_t mv) {
        float l = Luma(c);
        // 粉笔纹理：噪声与灰度联合调制的颗粒感
        const float n = HashNoise(x, y, 2028);
        const float tex = 0.65f + 0.7f * n;
        l = std::min(mx, l * tex * strength);
        // 略微上提（粉笔偏白）
        l = l + (mx - l) * 0.18f;
        RGBA out;
        out.r = out.g = out.b = ClampCh(l, mv);
        return out;
    });
}

// ---------------------------------------------------------------------------
// 7. Texturizer 纹理化（Texture）：画布纹理调制
// ---------------------------------------------------------------------------
inline void Texturizer(ImageBuffer& img, int scale, float relief,
                       const Selection* sel) {
    if (!img.IsValid()) return;
    scale = std::max(2, scale);
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float mx = static_cast<float>(maxV);
    ForEachPixel(img, sel, [&](int x, int y, RGBA c, uint32_t mv) {
        // 编织纹理：经纬明暗 + 噪声
        const float wx = std::sin(x * 3.14159265f / scale) * 0.5f + 0.5f;
        const float wy = std::sin(y * 3.14159265f / scale) * 0.5f + 0.5f;
        float tex = 0.82f + 0.12f * wx + 0.12f * wy
                  + (HashNoise(x, y, 55) - 0.5f) * 0.08f;
        tex = 1.0f + (tex - 1.0f) * relief * 2.0f;
        RGBA out;
        out.r = ClampCh(c.r * tex, mv);
        out.g = ClampCh(c.g * tex, mv);
        out.b = ClampCh(c.b * tex, mv);
        return out;
    });
}

// ---------------------------------------------------------------------------
// 8. Grain 颗粒（Texture）
// ---------------------------------------------------------------------------
inline void Grain(ImageBuffer& img, float amount, int monochrome,
                  const Selection* sel) {
    if (!img.IsValid()) return;
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    ForEachPixel(img, sel, [&](int x, int y, RGBA c, uint32_t mv) {
        const float n1 = (HashNoise(x, y, 17) - 0.5f) * amount * maxV;
        float r, g, b;
        if (monochrome) {
            r = c.r + n1; g = c.g + n1; b = c.b + n1;
        } else {
            r = c.r + (HashNoise(x, y, 17) - 0.5f) * amount * maxV;
            g = c.g + (HashNoise(x, y, 53) - 0.5f) * amount * maxV;
            b = c.b + (HashNoise(x, y, 89) - 0.5f) * amount * maxV;
        }
        RGBA out;
        out.r = ClampCh(r, mv);
        out.g = ClampCh(g, mv);
        out.b = ClampCh(b, mv);
        return out;
    });
}

// ---------------------------------------------------------------------------
// 3x3 卷积（浮雕/查找边缘共用）
// ---------------------------------------------------------------------------
inline void Convolve3x3(ImageBuffer& img, const float k[3][3], float bias,
                        const Selection* sel) {
    if (!img.IsValid()) return;
    const Rect roi = FilterRoi(img, sel);
    const ImageBuffer src = img.Clone();
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA c;
            src.GetPixel(x, y, c);
            float acc[3] = {0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; dx += 1) {
                    const RGBA s = SampleClamp(src, x + dx, y + dy);
                    const float w = k[dy + 1][dx + 1];
                    acc[0] += s.r * w; acc[1] += s.g * w; acc[2] += s.b * w;
                }
            RGBA out;
            out.r = ClampCh(acc[0] + bias, maxV);
            out.g = ClampCh(acc[1] + bias, maxV);
            out.b = ClampCh(acc[2] + bias, maxV);
            out.a = c.a;
            img.SetPixel(x, y, out);
        }
    }
}

// ---------------------------------------------------------------------------
// 9. Emboss 浮雕（Texture）
// ---------------------------------------------------------------------------
inline void Emboss(ImageBuffer& img, float strength, const Selection* sel) {
    // 对角浮雕核（核总和为 0：平坦区域输出 = 偏置 = 中性灰）
    const float k[3][3] = {
        {-2 * strength, -strength, 0},
        {-strength,      0,        strength},
        {0,              strength, 2 * strength},
    };
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    Convolve3x3(img, k, static_cast<float>(maxV) * 0.5f, sel); // 平坦区 -> 中性灰
}

// ---------------------------------------------------------------------------
// 10. Find Edges 查找边缘（Stylize）：Sobel 梯度
// ---------------------------------------------------------------------------
inline void FindEdges(ImageBuffer& img, int invert, const Selection* sel) {
    if (!img.IsValid()) return;
    const Rect roi = FilterRoi(img, sel);
    const ImageBuffer src = img.Clone();
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float mx = static_cast<float>(maxV);

    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            RGBA c;
            src.GetPixel(x, y, c);

            const float tl = Luma(SampleClamp(src, x - 1, y - 1));
            const float t  = Luma(SampleClamp(src, x,     y - 1));
            const float tr = Luma(SampleClamp(src, x + 1, y - 1));
            const float l  = Luma(SampleClamp(src, x - 1, y));
            const float r  = Luma(SampleClamp(src, x + 1, y));
            const float bl = Luma(SampleClamp(src, x - 1, y + 1));
            const float b  = Luma(SampleClamp(src, x,     y + 1));
            const float br = Luma(SampleClamp(src, x + 1, y + 1));

            const float gx = (tr + 2 * r + br) - (tl + 2 * l + bl);
            const float gy = (bl + 2 * b + br) - (tl + 2 * t + tr);
            float mag = std::sqrt(gx * gx + gy * gy) * 0.25f;
            mag = std::min(mx, mag);
            if (invert) mag = mx - mag;

            RGBA out;
            out.r = out.g = out.b = ClampCh(mag, maxV);
            out.a = c.a;
            img.SetPixel(x, y, out);
        }
    }
}

} // namespace sai2::fg
