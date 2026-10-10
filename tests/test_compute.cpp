// ============================================================================
// test_compute.cpp - 计算后端测试（规格书第十一节）
//
// 覆盖：CPU 后端正确性（能量守恒/均匀不变）、注册表优先级、
//       DX11（含 WARP）可用性探测与 CPU/DX11 交叉一致性（可用时）。
// ============================================================================

#include "Tests.h"

#include "sai2/host/ComputeRegistry.h"
#include "sai2/host/MockHostContext.h"

#include <algorithm>
#include <cmath>

using namespace sai2;
using namespace sai2::host;

namespace {

double TotalR(const ImageBuffer& img) {
    double sum = 0;
    for (int y = 0; y < img.Height(); ++y)
        for (int x = 0; x < img.Width(); ++x) {
            RGBA c;
            img.GetPixel(x, y, c);
            sum += c.r;
        }
    return sum;
}

} // namespace

namespace sai2test {

bool TestCompute() {
    // ---- 1. 注册表：CPU 后端永远可用 ----
    ComputeRegistry& reg = ComputeRegistry::Instance();
    IComputeBackend* cpu = reg.Preferred(ComputeBackendType::CpuSimd);
    CHECK(cpu && cpu->Available());

    // ---- 2. CPU 后端：冲量 -> 能量守恒 + 峰值位置 ----
    {
        ImageBuffer img(33, 33, PixelFormat::RGBA8);
        img.Clear(RGBA{});
        img.SetPixel(16, 16, RGBA{255, 255, 255, 255});
        const double before = TotalR(img);
        std::string err;
        CHECK_MSG(cpu->GaussianBlur(img, 2.0f, err), err.c_str());
        const double after = TotalR(img);
        // 整数 RGBA8 输出：暗像素取整必然损失能量（约 8%），允许范围
        CHECK(after >= before * 0.85 && after <= before + 1.0);

        RGBA center, side;
        CHECK(img.GetPixel(16, 16, center));
        CHECK(img.GetPixel(10, 16, side));
        CHECK(center.r >= side.r); // 峰值仍在冲量位置
        CHECK(center.r >= 8);      // 2D 峰值理论值 255/(2πσ²) ≈ 10（σ=2）
    }

    // ---- 3. CPU 后端：均匀图像不变 ----
    {
        ImageBuffer img(16, 16, PixelFormat::RGBA8);
        img.Clear(RGBA{77, 120, 200, 255});
        std::string err;
        CHECK_MSG(cpu->GaussianBlur(img, 3.0f, err), err.c_str());
        RGBA c;
        img.GetPixel(8, 8, c);
        CHECK(c.r == 77 && c.g == 120 && c.b == 200 && c.a == 255);
    }

    // ---- 4. 参数校验 ----
    {
        ImageBuffer img(8, 8, PixelFormat::RGBA8);
        img.Clear(RGBA{});
        std::string err;
        CHECK(!cpu->GaussianBlur(img, -1.0f, err));
        CHECK(!cpu->GaussianBlur(img, 0.0f, err));
    }

    // ---- 5. DX11 后端（WARP 兜底，Win8+ 环境应可用） ----
    IComputeBackend* dx = reg.Preferred(ComputeBackendType::DirectX11);
    if (dx && dx->Available()) {
        // 交叉一致性：同一冲量图分别用 CPU/DX11 模糊，结果应在容差内
        ImageBuffer ref(64, 64, PixelFormat::RGBA8);
        ref.Clear(RGBA{});
        ref.SetPixel(32, 32, RGBA{255, 255, 255, 255});
        ImageBuffer gpuImg = ref.Clone();
        std::string err;
        CHECK_MSG(cpu->GaussianBlur(ref, 2.0f, err), err.c_str());
        CHECK_MSG(dx->GaussianBlur(gpuImg, 2.0f, err), err.c_str());

        double maxDiff = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                RGBA a, b;
                ref.GetPixel(x, y, a);
                gpuImg.GetPixel(x, y, b);
                maxDiff = std::max(
                    maxDiff, static_cast<double>(std::abs(
                                 static_cast<int>(a.r) - static_cast<int>(b.r))));
            }
        // UNORM8 + 权重舍入，容差 3
        CHECK_MSG(maxDiff <= 3, "CPU/DX11 结果偏差过大");
    } else {
        std::cout << "  [SKIP] DirectX 11 后端不可用（无 d3d11/WARP 环境）\n";
    }

    // ---- 6. Best() 非空 ----
    CHECK(reg.Best() != nullptr);
    return true;
}

} // namespace sai2test
