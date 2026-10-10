// ============================================================================
// CpuSimdBackend.cpp - CPU fallback 后端（可分离 Gaussian Blur）
// 编译器 -O2 下对行内循环自动向量化（SSE/AVX）；后续可显式 intrinsics。
// ============================================================================

#include "sai2/host/ComputeRegistry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sai2::host {

namespace {

class CpuSimdBackend final : public IComputeBackend {
public:
    ComputeBackendInfo Info() const override {
        ComputeBackendInfo info;
        info.type = ComputeBackendType::CpuSimd;
        info.name = "CPU (SIMD fallback)";
        info.device = "host CPU";
        return info;
    }

    bool Available() const override { return true; } // CPU 永远可用

    bool GaussianBlur(ImageBuffer& img, float sigma,
                      std::string& error) override {
        if (sigma <= 0.0f) { error = "sigma 必须为正"; return false; }
        if (!img.IsValid()) { error = "图像无效"; return false; }
        if (img.Format() != PixelFormat::RGBA8 &&
            img.Format() != PixelFormat::RGBA16) {
            error = "CPU 后端仅支持 RGBA8/RGBA16";
            return false;
        }

        const int w = img.Width(), h = img.Height();
        if (w <= 0 || h <= 0) { error = "图像尺寸为 0"; return false; }
        if (sigma > 64.0f) { error = "sigma 过大（>64）"; return false; }

        // 半径 = 3σ，权重归一化
        const int radius = std::max(1, std::min(64,
            static_cast<int>(std::ceil(sigma * 3.0f))));
        std::vector<float> kernel(static_cast<size_t>(radius * 2 + 1));
        const double denom = 2.0 * sigma * sigma;
        double sum = kernel[radius] = 1.0;
        for (int i = 1; i <= radius; ++i) {
            const double v = std::exp(-(i * i) / denom);
            kernel[radius - i] = static_cast<float>(v);
            kernel[radius + i] = static_cast<float>(v);
            sum += 2.0 * v;
        }
        for (float& v : kernel) v = static_cast<float>(v / sum);

        const bool is16 = img.Format() == PixelFormat::RGBA16;
        const uint32_t maxV = is16 ? 65535u : 255u;

        // 中间缓冲（行/列分离各一次；浮点避免中间取整误差累积）
        std::vector<float> tmp(static_cast<size_t>(w) * h * 4, 0.0f);
        auto readPixel = [&](int x, int y, float out[4]) {
            RGBA c;
            img.GetPixel(x, y, c);
            out[0] = static_cast<float>(c.r);
            out[1] = static_cast<float>(c.g);
            out[2] = static_cast<float>(c.b);
            out[3] = static_cast<float>(c.a);
        };

        // Horizontal pass
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int k = -radius; k <= radius; ++k) {
                    const int sx = std::clamp(x + k, 0, w - 1);
                    float px[4];
                    readPixel(sx, y, px);
                    const float wv = kernel[static_cast<size_t>(k + radius)];
                    for (int c = 0; c < 3; ++c) acc[c] += px[c] * wv;
                }
                const size_t dst = (static_cast<size_t>(y) * w + x) * 4;
                float a;
                { RGBA c; img.GetPixel(x, y, c); a = static_cast<float>(c.a); }
                for (int c = 0; c < 3; ++c) tmp[dst + c] = acc[c];
                tmp[dst + 3] = a;
            }
        }

        // Vertical pass（从 tmp 读）
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int k = -radius; k <= radius; ++k) {
                    const int sy = std::clamp(y + k, 0, h - 1);
                    const float* px =
                        &tmp[(static_cast<size_t>(sy) * w + x) * 4];
                    const float wv = kernel[static_cast<size_t>(k + radius)];
                    for (int c = 0; c < 3; ++c) acc[c] += px[c] * wv;
                }
                const size_t dst = (static_cast<size_t>(y) * w + x) * 4;
                RGBA out;
                auto conv = [&](float v) -> uint16_t {
                    return static_cast<uint16_t>(std::clamp(
                        static_cast<uint64_t>(v + 0.5f),
                        0ull, static_cast<uint64_t>(maxV)));
                };
                out.r = conv(acc[0]);
                out.g = conv(acc[1]);
                out.b = conv(acc[2]);
                out.a = static_cast<uint16_t>(tmp[dst + 3]);
                img.SetPixel(x, y, out);
            }
        }
        return true;
    }
};

} // namespace

IComputeBackend* CreateCpuSimdBackend() { return new CpuSimdBackend(); }

} // namespace sai2::host
