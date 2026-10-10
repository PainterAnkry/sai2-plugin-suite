// ============================================================================
// ComputeBench.cpp - 4K 预览性能基准（规格书第十一节：目标 30~60 FPS）
//
// 测量：4K 图像降采样预览（最长边 1024）上 Gaussian Blur 单帧耗时，
//       报告各后端 FPS。用法：ComputeBench.exe [backend: dx11|cpu]
// ============================================================================

#include "sai2/host/ComputeRegistry.h"
#include "LiquifySession.h"
#include "sai2/host/MockHostContext.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace sai2;
using namespace sai2::host;
using namespace sai2::liquify;

namespace {

// 4K 降采样到预览 buffer（最近邻，模拟 BeginPreview）
ImageBuffer MakePreview(int fullW, int fullH, int maxDim) {
    ImageBuffer full(fullW, fullH, PixelFormat::RGBA8);
    // 填充确定性的测试图案（渐变 + 圆形）
    for (int y = 0; y < fullH; y += 1)
        for (int x = 0; x < fullW; x += 1) {
            const int dx = x - fullW / 2, dy = y - fullH / 2;
            const bool disc = dx * dx + dy * dy <
                              (fullH / 3) * (fullH / 3);
            RGBA c{static_cast<uint16_t>(x * 255 / fullW),
                   static_cast<uint16_t>(y * 255 / fullH),
                   static_cast<uint16_t>(disc ? 255 : 40), 255};
            full.SetPixel(x, y, c);
        }
    const double scale =
        std::min(1.0, static_cast<double>(maxDim) /
                          std::max(fullW, fullH));
    const int pw = std::max(1, static_cast<int>(fullW * scale));
    const int ph = std::max(1, static_cast<int>(fullH * scale));
    ImageBuffer preview(pw, ph, PixelFormat::RGBA8);
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            RGBA c;
            full.GetPixel(std::min(fullW - 1,
                                   static_cast<int>(x / scale)),
                          std::min(fullH - 1,
                                   static_cast<int>(y / scale)),
                          c);
            preview.SetPixel(x, y, c);
        }
    return preview;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string which = argc > 1 ? argv[1] : "all";

    printf("生成 4K 预览 buffer (maxDim=1024)...\n");
    ImageBuffer preview = MakePreview(3840, 2160, 1024);
    printf("预览尺寸: %dx%d, sigma=4, 31 tap 可分离高斯\n\n", preview.Width(),
           preview.Height());

    ComputeRegistry& reg = ComputeRegistry::Instance();
    struct Item {
        const char* label;
        IComputeBackend* backend;
    };
    std::vector<Item> items;
    if (which == "all" || which == "dx11") {
        IComputeBackend* dx = reg.Preferred(ComputeBackendType::DirectX11);
        if (dx && dx->Available())
            items.push_back({"DirectX11", dx});
        else
            printf("[SKIP] DirectX11 不可用\n");
    }
    if (which == "all" || which == "cpu") {
        IComputeBackend* cpu = reg.Preferred(ComputeBackendType::CpuSimd);
        if (cpu) items.push_back({"CPU", cpu});
    }

    const int frames = 20;
    for (const Item& it : items) {
        // 预热 2 帧
        for (int i = 0; i < 2; ++i) {
            ImageBuffer warm = preview.Clone();
            std::string err;
            it.backend->GaussianBlur(warm, 4.0f, err);
        }
        const auto t0 = std::chrono::steady_clock::now();
        int ok = 0;
        for (int i = 0; i < frames; ++i) {
            ImageBuffer work = preview.Clone();
            std::string err;
            if (it.backend->GaussianBlur(work, 4.0f, err)) ++ok;
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count() / frames;
        printf("[%s] 平均 %.2f ms/帧 -> %.1f FPS（%d/%d 成功）%s\n",
               it.label, ms, ok ? 1000.0 / ms : 0.0, ok, frames,
               ms <= 33.3 ? "  ✓ 达到 30 FPS 目标" : "  ✗ 未达标");
    }

    // ---- 液化笔刷预览基准（4K 画布，局部 Region 渲染） ----
    printf("\n液化笔刷预览（4K 画布，笔刷 r=80 局部 Region 渲染）...\n");
    {
        MockHostContext host;
        host.SetCanvasSize(3840, 2160, PixelFormat::RGBA8);
        {
            ImageBuffer* canvas = host.GetImageBuffer();
            for (int y = 0; y < 2160; ++y)
                for (int x = 0; x < 3840; ++x)
                    canvas->SetPixel(x, y,
                                     RGBA{static_cast<uint16_t>(x * 255 / 3840),
                                          static_cast<uint16_t>(y * 255 / 2160),
                                          128, 255});
        }
        LiquifySession session(host);
        session.Begin("Bench");
        // 模拟一次 60 帧的笔刷拖动（每帧一次 Stroke，ROI 增量预览）
        StrokeParams p;
        p.tool = LiquifyTool::ForwardWarp;
        p.radius = 80;
        p.pressure = 0.7f;
        const auto t0 = std::chrono::steady_clock::now();
        int frames2 = 60;
        for (int i = 0; i < frames2; ++i) {
            p.cx = 1920.0f + 200.0f * std::sin(i * 0.1f);
            p.cy = 1080.0f + 100.0f * std::cos(i * 0.1f);
            p.dirX = std::cos(i * 0.1f);
            p.dirY = std::sin(i * 0.1f);
            session.Stroke(p); // 含增量 ROI 预览
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count() / frames2;
        (void)session.Preview(); // 确保预览缓冲有效
        printf("液化笔刷预览: 平均 %.2f ms/帧 -> %.1f FPS %s\n", ms,
               1000.0 / ms, ms <= 33.3 ? " ✓ 达到 30 FPS 目标"
                                       : " ✗ 未达标");
        session.Cancel();
    }
    return 0;
}
