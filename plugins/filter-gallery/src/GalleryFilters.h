#pragma once
// ============================================================================
// GalleryFilters.h - 规格表驱动的通用滤镜 + 已实现的算法
//
// 设计：47 个 Photoshop 滤镜共用**一个**通用滤镜类 GalleryFilterBase ——
//   id       = com.sai2.filter.gallery.<key>
//   name/cat = 规格表里的显示名/分类
//   参数     = 由 GalleryParams 从参数记法自动生成（Range/Choice/Bool）
// 算法侧只写一个按 key 分派的函数 GalleryApply(key, img, sel, f)，这样新增
// 一个滤镜 = 规格表里已有 + 在分派里加一个 case，不需要再手写包装类。
//
// 与老的 10 个滤镜（FilterSet.h 里手写的类）共存：它们 id 是
// com.sai2.filter.<name>，互不冲突；等 47 个实现齐了可以再合并风格。
// ============================================================================

#include "FilterBase.h"
#include "Filters.h"
#include "GalleryImplemented.h"
#include "GalleryParams.h"
#include "GallerySpecs.h"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace sai2::fg {

// 算法分派函数：按 key 执行；未实现时返回 false（宿主/UI 可据此标灰）
using GalleryApplyFn = bool (*)(const char* key, ImageBuffer& img,
                                const Selection* sel,
                                const class GalleryFilterBase& f);

// ---------------------------------------------------------------------------
// 通用滤镜类
// ---------------------------------------------------------------------------
class GalleryFilterBase final : public FilterBase {
public:
    GalleryFilterBase(const GallerySpec& spec, GalleryApplyFn fn)
        : spec_(spec), fn_(fn) {
        info_.id = std::string("com.sai2.filter.gallery.") + spec.key;
        info_.name = spec.name;
        info_.category = spec.category;
        info_.version = "1.2.0";
        const std::vector<GalleryParamSpec> ps = ParseGalleryParams(spec.params);
        // 标签用 deque 存：DefParam 只保存 const char*，deque 的地址稳定
        for (const auto& p : ps) {
            if (p.kind == GalleryParamSpec::Range) {
                DefParam(p.key.c_str(), p.key.c_str(), p.def, p.min, p.max,
                         p.isInt);
            } else if (p.kind == GalleryParamSpec::Choice) {
                // 参数模型是纯 float：枚举用 0..n-1 的整数表达，标签里写明
                // 取值含义（`纹理(0=canvas 1=brick …)`），避免用户不知道数字含义
                std::string lab = p.key + "(";
                for (size_t k = 0; k < p.choices.size(); ++k) {
                    if (k) lab += " ";
                    lab += std::to_string(k) + "=" + p.choices[k];
                }
                lab += ")";
                labels_.push_back(lab);
                DefParam(p.key.c_str(), labels_.back().c_str(), 0.0f, 0.0f,
                         static_cast<float>(p.choices.size() - 1), true);
            } else {
                DefParam(p.key.c_str(), p.key.c_str(), 0.0f, 0.0f, 1.0f, true);
            }
        }
    }

    bool Apply(ImageBuffer& b, const Selection* s) override {
        return fn_(spec_.key, b, s, *this);
    }

    const GallerySpec& Spec() const { return spec_; }
    int ChoiceCount(const char* key) const {
        const std::vector<GalleryParamSpec> ps = ParseGalleryParams(spec_.params);
        for (const auto& p : ps)
            if (p.key == key && p.kind == GalleryParamSpec::Choice)
                return static_cast<int>(p.choices.size());
        return 0;
    }

private:
    GallerySpec spec_;
    GalleryApplyFn fn_;
    std::deque<std::string> labels_; // DefParam 引用的标签存储（地址稳定）
};

// ---------------------------------------------------------------------------
// 算法积木（双线性采样 + 可分离盒式模糊）
// ---------------------------------------------------------------------------
namespace galleryfx {

inline RGBA Bilinear(const ImageBuffer& img, float x, float y) {
    const int W = img.Width(), H = img.Height();
    x = std::min(static_cast<float>(W - 1), std::max(0.0f, x));
    y = std::min(static_cast<float>(H - 1), std::max(0.0f, y));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(W - 1, x0 + 1), y1 = std::min(H - 1, y0 + 1);
    const float fx = x - x0, fy = y - y0;
    RGBA c00, c10, c01, c11;
    img.GetPixel(x0, y0, c00);
    img.GetPixel(x1, y0, c10);
    img.GetPixel(x0, y1, c01);
    img.GetPixel(x1, y1, c11);
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    RGBA o;
    o.r = static_cast<uint16_t>(mix(mix(c00.r, c10.r, fx), mix(c01.r, c11.r, fx), fy));
    o.g = static_cast<uint16_t>(mix(mix(c00.g, c10.g, fx), mix(c01.g, c11.g, fx), fy));
    o.b = static_cast<uint16_t>(mix(mix(c00.b, c10.b, fx), mix(c01.b, c11.b, fx), fy));
    o.a = static_cast<uint16_t>(mix(mix(c00.a, c10.a, fx), mix(c01.a, c11.a, fx), fy));
    return o;
}

// 确定性 hash 噪声（可复现，测试友好）
inline float Hash01(int x, int y, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777215.0f;
}

// 可分离盒式模糊（半径 r，就地到副本）
inline void BoxBlur(ImageBuffer& img, int r, int passes = 2) {
    if (r < 1) return;
    const int W = img.Width(), H = img.Height();
    ImageBuffer tmp = img.Clone();
    for (int p = 0; p < passes; ++p) {
        // 横向
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float sr = 0, sg = 0, sb = 0, sa = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sx = std::min(W - 1, std::max(0, x + k));
                    RGBA c;
                    tmp.GetPixel(sx, y, c);
                    sr += c.r; sg += c.g; sb += c.b; sa += c.a; ++n;
                }
                RGBA o;
                o.r = static_cast<uint16_t>(sr / n);
                o.g = static_cast<uint16_t>(sg / n);
                o.b = static_cast<uint16_t>(sb / n);
                o.a = static_cast<uint16_t>(sa / n);
                img.SetPixel(x, y, o);
            }
        }
        // 纵向
        for (int x = 0; x < W; ++x) {
            for (int y = 0; y < H; ++y) {
                float sr = 0, sg = 0, sb = 0, sa = 0;
                int n = 0;
                for (int k = -r; k <= r; ++k) {
                    const int sy = std::min(H - 1, std::max(0, y + k));
                    RGBA c;
                    img.GetPixel(x, sy, c);
                    sr += c.r; sg += c.g; sb += c.b; sa += c.a; ++n;
                }
                RGBA o;
                o.r = static_cast<uint16_t>(sr / n);
                o.g = static_cast<uint16_t>(sg / n);
                o.b = static_cast<uint16_t>(sb / n);
                o.a = static_cast<uint16_t>(sa / n);
                tmp.SetPixel(x, y, o);
            }
        }
        img = tmp.Clone();
    }
}

// 位移渲染：对 ROI 内像素按 (du,dv) 双线性回采
template <typename Fn>
inline void Displace(ImageBuffer& img, const Selection* sel, Fn field) {
    const Rect roi = FilterRoi(img, sel);
    ImageBuffer src = img.Clone();
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            float du = 0.0f, dv = 0.0f;
            field(x, y, du, dv);
            if (du == 0.0f && dv == 0.0f) continue;
            img.SetPixel(x, y, Bilinear(src, x + du, y + dv));
        }
    }
}

} // namespace galleryfx

// ---------------------------------------------------------------------------
// 已实现算法：Distort（Glass / Ocean Ripple / Diffuse Glow）
// ---------------------------------------------------------------------------
inline bool GalleryApplyDistort(const char* key, ImageBuffer& img,
                                const Selection* sel,
                                const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    const int W = img.Width(), H = img.Height();
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float scaleToMax = static_cast<float>(maxV) / 255.0f;

    if (k == "oceanRipple") {
        // 涟漪尺寸 1..15=9 → 波长；幅度 0..20=9 → 位移像素
        const int size = std::max(1, f.Pi("rippleSize"));
        const float mag = static_cast<float>(f.Pi("rippleMagnitude")) * 0.6f;
        const float lambda = std::max(2.0f, size * 4.0f);
        if (mag <= 0.0f) return true;
        Displace(img, sel, [&](int x, int y, float& du, float& dv) {
            const float t = 6.2831853f;
            du = mag * std::sin(t * static_cast<float>(y) / lambda);
            dv = mag * std::cos(t * static_cast<float>(x) / lambda);
        });
        return true;
    }

    if (k == "glass") {
        // 扭曲量 0..20=5、平滑度 1..15=3、纹理选择、缩放 50..200=100
        const float dist = static_cast<float>(f.Pi("distortion"));
        if (dist <= 0.0f) return true;
        const int smooth = std::max(1, f.Pi("smoothness"));
        const int tsel = f.Pi("texture");
        const float scaling = std::max(0.1f, f.P("scaling") / 100.0f);
        const bool invert = f.Pi("invert") != 0;
        // 纹理 → 位移场：0=frosted(细噪) 1=blocks(方块) 2=canvas 3=tinyLens
        const float cell = (tsel == 1) ? 12.0f : (tsel == 2 ? 4.0f : 2.0f);
        const float amp = dist * 0.8f * (invert ? -1.0f : 1.0f);
        const float inv = 1.0f / (cell * scaling);
        Displace(img, sel, [&](int x, int y, float& du, float& dv) {
            const float u = static_cast<float>(x) * inv;
            const float v = static_cast<float>(y) * inv;
            float nx, ny;
            if (tsel == 1) { // blocks：整块常量
                nx = Hash01(static_cast<int>(u), static_cast<int>(v), 7u) * 2 - 1;
                ny = Hash01(static_cast<int>(u), static_cast<int>(v), 11u) * 2 - 1;
            } else {
                nx = Hash01(static_cast<int>(u), static_cast<int>(v), 7u) * 2 - 1;
                ny = Hash01(static_cast<int>(u), static_cast<int>(v + 37), 11u) * 2 - 1;
            }
            du = nx * amp;
            dv = ny * amp;
        });
        if (smooth > 1) BoxBlur(img, std::min(3, smooth / 4));
        return true;
    }

    if (k == "diffuseGlow") {
        // 颗粒 0..10=6、发光量 0..20=10、清除量 0..20=15
        const float grain = static_cast<float>(f.Pi("graininess")) / 10.0f;
        const float glow = static_cast<float>(f.Pi("glowAmount")) / 20.0f;
        const float clearAmt = static_cast<float>(f.Pi("clearAmount")) / 20.0f;
        const Rect roi = FilterRoi(img, sel);

        // 发光：亮部模糊后叠加（暗部被 clearAmount 压回）
        if (glow > 0.0f) {
            ImageBuffer bl = img.Clone();
            BoxBlur(bl, 3, 2);
            for (int y = roi.y; y < roi.y + roi.h; ++y) {
                for (int x = roi.x; x < roi.x + roi.w; ++x) {
                    if (!SelectionAllows(sel, x, y)) continue;
                    RGBA c, b;
                    img.GetPixel(x, y, c);
                    bl.GetPixel(x, y, b);
                    const float lum = Luma(c) / (255.0f * scaleToMax);
                    // 亮部发光更强；clearAmount 把"清除"的那部分拉回原图
                    const float w = glow * lum * (1.0f - 0.5f * clearAmt);
                    RGBA o;
                    o.r = ClampCh((c.r + (b.r + maxV * 0.25f) * w) / (1.0f + w * 0.5f), maxV);
                    o.g = ClampCh((c.g + (b.g + maxV * 0.25f) * w) / (1.0f + w * 0.5f), maxV);
                    o.b = ClampCh((c.b + (b.b + maxV * 0.25f) * w) / (1.0f + w * 0.5f), maxV);
                    o.a = c.a;
                    img.SetPixel(x, y, o);
                }
            }
        }
        // 颗粒：确定性噪声，按亮度调制（亮部颗粒弱，符合"漫射"观感）
        if (grain > 0.0f) {
            for (int y = roi.y; y < roi.y + roi.h; ++y) {
                for (int x = roi.x; x < roi.x + roi.w; ++x) {
                    if (!SelectionAllows(sel, x, y)) continue;
                    RGBA c;
                    img.GetPixel(x, y, c);
                    const float lum = Luma(c) / (255.0f * scaleToMax);
                    const float n = (Hash01(x, y, 3u) * 2.0f - 1.0f) * grain * 40.0f *
                                    scaleToMax * (1.0f - 0.6f * lum);
                    RGBA o;
                    o.r = ClampCh(c.r + n, maxV);
                    o.g = ClampCh(c.g + n, maxV);
                    o.b = ClampCh(c.b + n, maxV);
                    o.a = c.a;
                    img.SetPixel(x, y, o);
                }
            }
        }
        return true;
    }

    return false; // 未实现（UI 应标灰）
}

// ---------------------------------------------------------------------------
// 已实现算法：Texture（龟裂/颗粒/马赛克拼贴/拼缀图/染色玻璃/纹理化）
//               Stylize（照亮边缘）
// ---------------------------------------------------------------------------
inline bool GalleryApplyTexture(const char* key, ImageBuffer& img,
                                const Selection* sel,
                                const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    const int W = img.Width(), H = img.Height();
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float sc = static_cast<float>(maxV) / 255.0f;
    const Rect roi = FilterRoi(img, sel);
    if (roi.w <= 0 || roi.h <= 0) return true;

    // 细胞化（马赛克/拼缀图/染色玻璃共用）：按 cell 取整块均值
    auto cellAverage = [&](ImageBuffer& src, ImageBuffer& dst, int cell) {
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const int cx = (x / cell) * cell, cy = (y / cell) * cell;
                float sr = 0, sg = 0, sb = 0, sa = 0;
                int n = 0;
                for (int yy = cy; yy < std::min(cy + cell, H); ++yy) {
                    for (int xx = cx; xx < std::min(cx + cell, W); ++xx) {
                        RGBA c;
                        src.GetPixel(xx, yy, c);
                        sr += c.r; sg += c.g; sb += c.b; sa += c.a; ++n;
                    }
                }
                if (!n) continue;
                RGBA o;
                o.r = static_cast<uint16_t>(sr / n);
                o.g = static_cast<uint16_t>(sg / n);
                o.b = static_cast<uint16_t>(sb / n);
                o.a = static_cast<uint16_t>(sa / n);
                dst.SetPixel(x, y, o);
            }
        }
    };

    if (k == "mosaicTiles") {
        const int tile = std::max(2, f.Pi("tileSize"));
        const int grout = std::max(0, f.Pi("groutWidth"));
        const float lighten = static_cast<float>(f.Pi("lightenGrout")) / 10.0f;
        ImageBuffer src = img.Clone();
        ImageBuffer dst = img.Clone();
        cellAverage(src, dst, tile);
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                dst.GetPixel(x, y, c);
                const bool isGrout = (x % tile) < grout || (y % tile) < grout;
                if (isGrout && lighten > 0.0f) {
                    const float add = lighten * 60.0f * sc;
                    c.r = ClampCh(c.r + add, maxV);
                    c.g = ClampCh(c.g + add, maxV);
                    c.b = ClampCh(c.b + add, maxV);
                } else if (isGrout) {
                    c.r = static_cast<uint16_t>(c.r * 0.75f);
                    c.g = static_cast<uint16_t>(c.g * 0.75f);
                    c.b = static_cast<uint16_t>(c.b * 0.75f);
                }
                img.SetPixel(x, y, c);
            }
        }
        return true;
    }

    if (k == "patchwork" || k == "stainedGlass") {
        const bool stained = (k == "stainedGlass");
        const int cell = stained ? std::max(2, f.Pi("cellSize"))
                                 : std::max(2, f.Pi("squareSize") * 3 + 4);
        const int border = stained ? std::max(1, f.Pi("borderThickness")) : 0;
        const float relief = stained ? 0.0f
                                     : static_cast<float>(f.Pi("relief")) / 25.0f;
        const float lightI = stained ? static_cast<float>(f.Pi("lightIntensity")) / 10.0f
                                     : 0.0f;
        ImageBuffer src = img.Clone();
        ImageBuffer dst = img.Clone();
        cellAverage(src, dst, cell);
        // 拼缀图的"浮雕"：用相邻细胞均值的差当作高度差做定向打光
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                dst.GetPixel(x, y, c);
                if (!stained) {
                    const int cx = (x / cell) * cell, cy = (y / cell) * cell;
                    const int rx = std::min(W - 1, cx + cell), ry = std::min(H - 1, cy + cell);
                    RGBA r0, r1;
                    src.GetPixel(std::min(W - 1, rx), std::min(H - 1, ry), r0);
                    src.GetPixel(std::min(W - 1, cx), std::min(H - 1, cy), r1);
                    const float d = (Luma(r0) - Luma(r1)) / (255.0f * sc);
                    const float add = d * relief * 90.0f * sc;
                    c.r = ClampCh(c.r + add, maxV);
                    c.g = ClampCh(c.g + add, maxV);
                    c.b = ClampCh(c.b + add, maxV);
                } else {
                    const bool isBorder = (x % cell) < border || (y % cell) < border;
                    if (isBorder) {
                        c.r = static_cast<uint16_t>(c.r * 0.35f);
                        c.g = static_cast<uint16_t>(c.g * 0.35f);
                        c.b = static_cast<uint16_t>(c.b * 0.35f);
                    } else if (lightI > 0.0f) {
                        const float add = lightI * 40.0f * sc *
                                          (0.5f + 0.5f * Hash01(x / cell, y / cell, 5u));
                        c.r = ClampCh(c.r + add, maxV);
                        c.g = ClampCh(c.g + add, maxV);
                        c.b = ClampCh(c.b + add, maxV);
                    }
                }
                img.SetPixel(x, y, c);
            }
        }
        return true;
    }

    if (k == "craquelure") {
        const int spacing = std::max(2, f.Pi("crackSpacing"));
        const float depth = static_cast<float>(f.Pi("crackDepth")) / 10.0f;
        const float bright = static_cast<float>(f.Pi("crackBrightness")) / 10.0f;
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                // 细胞边界 = 裂缝；用抖动让边界不规则（避免规则网格的假感）
                const int jx = static_cast<int>(Hash01(x / spacing, y / spacing, 13u) * spacing * 0.4f);
                const int jy = static_cast<int>(Hash01(x / spacing, y / spacing, 17u) * spacing * 0.4f);
                const bool onCrack = ((x + jx) % spacing) == 0 || ((y + jy) % spacing) == 0;
                if (!onCrack) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                // 原来 depth/bright 的默认值(0.6/0.9)几乎抵消 → 幅度≈1，看不见；
                // 改为只让"裂缝亮度"起正向作用、深度走负向，并放大系数
                const float d = (bright * 90.0f - depth * 40.0f + 20.0f) * sc;
                c.r = ClampCh(c.r - d, maxV);
                c.g = ClampCh(c.g - d, maxV);
                c.b = ClampCh(c.b - d, maxV);
                img.SetPixel(x, y, c);
            }
        }
        return true;
    }

    if (k == "grain") {
        const float intensity = static_cast<float>(f.Pi("intensity")) / 100.0f;
        const float contrast = static_cast<float>(f.Pi("contrast")) / 100.0f;
        const int type = f.Pi("grainType"); // 0..9 十种颗粒
        // 颗粒类型 → 噪声的空间尺度（regular 细、enlarged/horizontal/vertical 粗）
        const int cell = (type == 5) ? 4 : (type == 7 || type == 8) ? 3 : 1;
        const float amp = intensity * 110.0f * sc;
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                RGBA c;
                img.GetPixel(x, y, c);
                float n;
                const int gx = x / cell, gy = y / cell;
                if (type == 7) {              // horizontal
                    n = Hash01(0, gy, 23u) * 2.0f - 1.0f;
                } else if (type == 8) {       // vertical
                    n = Hash01(gx, 0, 29u) * 2.0f - 1.0f;
                } else if (type == 3) {       // clumped：块状
                    n = Hash01(gx, gy, 31u) * 2.0f - 1.0f;
                } else {
                    n = Hash01(x, y, 37u) * 2.0f - 1.0f;
                }
                const float v = n * amp * (1.0f + contrast);
                c.r = ClampCh(c.r + v, maxV);
                c.g = ClampCh(c.g + v, maxV);
                c.b = ClampCh(c.b + v, maxV);
                img.SetPixel(x, y, c);
            }
        }
        return true;
    }

    if (k == "texturizer") {
        // 纹理（canvas/brick/burlap/sandstone）+ 缩放 + 浮雕 + 光照方向 + 反相
        const int tsel = f.Pi("texture");
        const float scaling = std::max(0.1f, f.P("scaling") / 100.0f);
        const float relief = static_cast<float>(f.Pi("relief")) / 50.0f;
        const int light = f.Pi("light");           // 0..7 八个方向
        const bool invert = f.Pi("invert") != 0;
        const float baseCell = (tsel == 1) ? 10.0f : (tsel == 2 ? 3.0f : 5.0f);
        const float cell = std::max(2.0f, baseCell * scaling);
        // 光照方向 → (dx,dy) 卷积核
        const float ang = 6.2831853f * static_cast<float>(light) / 8.0f;
        const float lx = std::cos(ang), ly = std::sin(ang);
        for (int y = roi.y; y < roi.y + roi.h; ++y) {
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                auto height = [&](float px, float py) {
                    const float u = px / cell, v = py / cell;
                    float h;
                    if (tsel == 1) {   // brick：横缝 + 错缝
                        const int row = static_cast<int>(v);
                        const int off = (row % 2) ? static_cast<int>(cell * 0.5f) : 0;
                        h = ((static_cast<int>(u * 2.0f + off / cell) % 2) == 0) ? 0.8f : 0.2f;
                        if ((static_cast<int>(v) % 2) == 0) h += 0.1f;
                    } else if (tsel == 2) { // burlap：交错的粗织纹
                        h = (static_cast<int>(u) % 2 == 0 ? 0.7f : 0.3f) *
                            (static_cast<int>(v) % 2 == 0 ? 0.8f : 0.4f);
                    } else if (tsel == 3) { // sandstone：细噪
                        h = Hash01(static_cast<int>(u), static_cast<int>(v), 41u);
                    } else {                // canvas
                        h = 0.5f + 0.35f * std::sin(u * 3.14159265f) *
                                       std::sin(v * 3.14159265f);
                    }
                    return invert ? 1.0f - h : h;
                };
                const float h0 = height(static_cast<float>(x), static_cast<float>(y));
                const float h1 = height(static_cast<float>(x) + lx * cell,
                                        static_cast<float>(y) + ly * cell);
                const float d = (h1 - h0) * relief * 120.0f * sc;
                RGBA c;
                img.GetPixel(x, y, c);
                c.r = ClampCh(c.r + d, maxV);
                c.g = ClampCh(c.g + d, maxV);
                c.b = ClampCh(c.b + d, maxV);
                img.SetPixel(x, y, c);
            }
        }
        return true;
    }

    return false;
}

inline bool GalleryApplyStylize(const char* key, ImageBuffer& img,
                                const Selection* sel,
                                const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    if (k != "glowingEdges") return false;
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float sc = static_cast<float>(maxV) / 255.0f;
    const int width = std::max(1, f.Pi("edgeWidth"));
    const float bright = static_cast<float>(f.Pi("edgeBrightness")) / 20.0f;
    const int smooth = std::max(1, f.Pi("smoothness"));
    const Rect roi = FilterRoi(img, sel);

    ImageBuffer src = img.Clone();
    if (smooth > 1) BoxBlur(src, std::min(4, smooth / 3));
    for (int y = roi.y; y < roi.y + roi.h; ++y) {
        for (int x = roi.x; x < roi.x + roi.w; ++x) {
            if (!SelectionAllows(sel, x, y)) continue;
            // Sobel 幅度（用亮度）
            auto at = [&](int xx, int yy) {
                return Luma(SampleClamp(src, xx, yy)) / (255.0f * sc);
            };
            const float gx = -at(x - width, y - width) + at(x + width, y - width) +
                             -2.0f * at(x - width, y) + 2.0f * at(x + width, y) +
                             -at(x - width, y + width) + at(x + width, y + width);
            const float gy = -at(x - width, y - width) - 2.0f * at(x, y - width) -
                             at(x + width, y - width) + at(x - width, y + width) +
                             2.0f * at(x, y + width) + at(x + width, y + width);
            const float mag = std::min(1.0f, std::sqrt(gx * gx + gy * gy));
            // 照亮边缘：边缘亮、其余暗（PS 的观感）
            const float v = mag * (0.4f + bright) * 255.0f * sc;
            RGBA o;
            o.r = ClampCh(v * 1.0f, maxV);
            o.g = ClampCh(v * 1.0f, maxV);
            o.b = ClampCh(v * 1.0f, maxV);
            RGBA c;
            img.GetPixel(x, y, c);
            o.a = c.a;
            img.SetPixel(x, y, o);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 已实现算法：Sketch 批（基底凸现 / 绘图笔 / 半调图案 / 影印 / 图章）
// 共同骨架：亮度 → （可选模糊）→ 色阶/阈值/纹理 → 前景·背景两色映射。
// 注：记法里 foreground/background 是 json（颜色），本插件暂无颜色控件 →
//     用默认黑/白前景背景色（PS 默认即黑前景、白背景）。
// ---------------------------------------------------------------------------
inline bool GalleryApplySketch(const char* key, ImageBuffer& img,
                               const Selection* sel,
                               const GalleryFilterBase& f) {
    using namespace galleryfx;
    const std::string k = key ? key : "";
    const uint32_t maxV = img.Format() == PixelFormat::RGBA16 ? 65535u : 255u;
    const float sc = static_cast<float>(maxV) / 255.0f;
    const Rect roi = FilterRoi(img, sel);
    if (roi.w <= 0 || roi.h <= 0) return true;
    const int W = img.Width(), H = img.Height();

    auto luma01 = [&](const ImageBuffer& src, int x, int y) {
        return Luma(SampleClamp(src, x, y)) / (255.0f * sc);
    };
    auto setGray = [&](int x, int y, float v, uint16_t a) {
        RGBA o;
        const float c = std::min(static_cast<float>(maxV),
                                 std::max(0.0f, v * maxV));
        o.r = o.g = o.b = static_cast<uint16_t>(c);
        o.a = a;
        img.SetPixel(x, y, o);
    };

    if (k == "basRelief") {
        // 基底凸现：detail(1..15=13) 控制细节、smoothness(1..15=3) 模糊、
        // light(8 向) 决定打光方向；结果映射到灰阶（PS 的基底凸现观感）
        const int detail = std::max(1, f.Pi("detail"));
        const int smooth = std::max(1, f.Pi("smoothness"));
        const int light = f.Pi("light");
        const float a = 6.2831853f * static_cast<float>(light) / 8.0f;
        const float lx = std::cos(a), ly = std::sin(a);
        ImageBuffer src = img.Clone();
        if (smooth > 1) BoxBlur(src, std::min(4, smooth / 3));
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float d = luma01(src, x + static_cast<int>(lx * detail),
                                       y + static_cast<int>(ly * detail)) -
                                luma01(src, x, y);
                RGBA c;
                img.GetPixel(x, y, c);
                setGray(x, y, 0.5f + d * 2.2f, c.a);
            }
        return true;
    }

    if (k == "graphicPen") {
        // 绘图笔：按 strokeDirection(4 向) 画等间距墨线，线密度由亮度决定
        const int len = std::max(1, f.Pi("strokeLength"));
        const float balance = f.P("lightDarkBalance") / 100.0f;
        const int dir = f.Pi("strokeDirection"); // 0=rightDiag 1=horizontal 2=leftDiag 3=vertical
        const float period = std::max(2.0f, static_cast<float>(len) * 0.55f);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                float coord;
                if (dir == 1) coord = static_cast<float>(y);
                else if (dir == 3) coord = static_cast<float>(x);
                else if (dir == 2) coord = static_cast<float>(x) - static_cast<float>(y);
                else coord = static_cast<float>(x) + static_cast<float>(y);
                const float phase = (coord + period * 0.5f) / period;
                const float frac = phase - std::floor(phase);
                const float lum = luma01(img, x, y);
                // 暗 → 线更粗；亮 → 线更细甚至断
                // 阈值族普遍偏黑：线宽系数从 1.4 降到 0.55（只有明确暗部才落墨）
                const float width = std::min(0.95f,
                    (1.0f - lum) * (0.35f + balance) * 0.55f);
                RGBA c;
                img.GetPixel(x, y, c);
                setGray(x, y, frac < width ? 0.0f : 1.0f, c.a);
            }
        return true;
    }

    if (k == "halftonePattern") {
        // 半调图案：size(1..12=1) 网点大小、contrast(0..50=5) 对比、
        // patternType(0=dot 1=circle 2=line)
        const int size = std::max(1, f.Pi("size"));
        const float contrast = f.P("contrast") / 50.0f;
        const int type = f.Pi("patternType");
        const int cell = std::max(2, size * 3);
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float lum = luma01(img, x, y);
                float ink;
                if (type == 2) {                    // line：线宽随亮度
                    const float period = static_cast<float>(cell);
                    const float fpos = static_cast<float>(x % cell) / period;
                    ink = (fpos < (1.0f - lum)) ? 0.0f : 1.0f;
                } else {
                    const int cx = (x / cell) * cell + cell / 2;
                    const int cy = (y / cell) * cell + cell / 2;
                    const float dx = static_cast<float>(x - cx);
                    const float dy = static_cast<float>(y - cy);
                    const float r = (type == 1) ? std::sqrt(dx * dx + dy * dy)
                                                : std::max(std::fabs(dx), std::fabs(dy));
                    const float rad = (1.0f - lum) * (cell * 0.5f) *
                                      (0.25f + contrast * 0.5f);
                    ink = (r < rad) ? 0.0f : 1.0f;
                }
                RGBA c;
                img.GetPixel(x, y, c);
                setGray(x, y, ink, c.a);
            }
        return true;
    }

    if (k == "photocopy" || k == "stamp" || k == "tornEdges" || k == "reticulation") {
        // 影印 / 图章 / 撕边 / 网状：都是"亮度 → 阈值（带噪声/模糊）"家族
        const bool isCopy = (k == "photocopy");
        const bool isStamp = (k == "stamp");
        const bool isTorn = (k == "tornEdges");
        const float detail = isCopy ? static_cast<float>(std::max(1, f.Pi("detail"))) : 1.0f;
        const float darkness = isCopy ? static_cast<float>(std::max(1, f.Pi("darkness"))) / 50.0f : 0.5f;
        const float balance = isStamp ? f.P("lightDarkBalance") / 50.0f
                                      : (isTorn ? f.P("imageBalance") / 50.0f : 1.0f);
        const int smooth = isStamp ? std::max(1, f.Pi("smoothness"))
                                   : (isTorn ? std::max(1, f.Pi("smoothness")) : 1);
        ImageBuffer src = img.Clone();
        if (smooth > 1) BoxBlur(src, std::min(4, smooth / 5));
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                float lum = luma01(src, x, y);
                if (isCopy) {
                    // 细节越大越"锐"（高频保留），暗度抬升阈值
                    const float hi = luma01(src, x + static_cast<int>(detail),
                                            y + static_cast<int>(detail));
                    lum = std::min(1.0f, std::max(0.0f, lum + (lum - hi) * 1.5f));
                }
                float thr = 0.5f;
                // 阈值族：把阈值整体压低（原来 ~0.45-0.85，中暗图会大片落墨）
                if (isCopy) thr = 0.12f + (1.0f - darkness) * 0.18f;
                else if (isStamp) thr = 0.10f + balance * 0.22f;
                else if (isTorn) thr = 0.08f + balance * 0.26f;
                else thr = 0.18f; // reticulation 走密度
                float v = (lum < thr) ? 0.0f : 1.0f;
                if (isTorn) {
                    // 撕边：阈值附近用噪声抖动出粗糙边缘
                    const float n = Hash01(x, y, 53u) * 0.12f - 0.06f;
                    v = ((lum + n) < thr) ? 0.0f : 1.0f;
                }
                setGray(x, y, v, [&] { RGBA c; img.GetPixel(x, y, c); return c.a; }());
            }
        return true;
    }

    if (k == "waterPaper") {
        // 水彩纸：按 fiberLength 画纤维条纹，brightness/contrast 调整观感
        const int fiber = std::max(3, f.Pi("fiberLength"));
        const float bright = f.P("brightness") / 100.0f;
        const float contrast = f.P("contrast") / 100.0f;
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (!SelectionAllows(sel, x, y)) continue;
                const float along = Hash01(x / fiber, y, 61u);
                const float across = Hash01(x, y / std::max(2, fiber / 2), 67u);
                const float f01 = 0.5f + (along - 0.5f) * 0.5f + (across - 0.5f) * 0.35f;
                RGBA c;
                img.GetPixel(x, y, c);
                const float lum = luma01(img, x, y);
                const float v = std::min(1.0f, std::max(0.0f,
                    (lum * (0.6f + bright * 0.8f) + (f01 - 0.5f)) * (0.7f + contrast)));
                setGray(x, y, v, c.a);
            }
        return true;
    }

    return false;
}

// 分类算法的前置声明（定义在各自文件，本文件末尾 include 它们；
// 这样"新增一个分类"= 新建一个文件 + 在末尾加一行 include，不必改动
// 中间的任何代码，也就不会出现"锚点失效导致插入失败"的问题）。
inline bool GalleryApplyRest(const char* key, ImageBuffer& img,
                             const Selection* sel,
                             const GalleryFilterBase& f);
inline bool GalleryApplySketchExtra(const char* key, ImageBuffer& img,
                                   const Selection* sel,
                                   const GalleryFilterBase& f);
inline bool GalleryApplyArtistic(const char* key, ImageBuffer& img,
                                 const Selection* sel,
                                 const GalleryFilterBase& f);
// 总分派：Distort + Texture + Stylize + Sketch + Artistic
inline bool GalleryApplyAll(const char* key, ImageBuffer& img,
                            const Selection* sel, const GalleryFilterBase& f) {
    if (GalleryApplyDistort(key, img, sel, f)) return true;
    if (GalleryApplyTexture(key, img, sel, f)) return true;
    if (GalleryApplyStylize(key, img, sel, f)) return true;
    if (GalleryApplySketch(key, img, sel, f)) return true;
    if (GalleryApplyArtistic(key, img, sel, f)) return true;
    if (GalleryApplySketchExtra(key, img, sel, f)) return true;
    if (GalleryApplyRest(key, img, sel, f)) return true;
    return false; // 未实现（UI 应标灰 / Execute 会记日志）
}

} // namespace sai2::fg

// 各分类算法（必须在 GalleryFilterBase / galleryfx 之后）
#include "GalleryArtistic.h"

#include "GallerySketch.h"

#include "GalleryRest.h"
