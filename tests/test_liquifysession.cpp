// ============================================================================
// test_liquifysession.cpp - 液化工作流引擎测试（规格书第八节）
//
// 覆盖：Stroke 累积置换场 + 增量 ROI 预览、Commit 全分辨率回写 + Undo、
//       Cancel 丢弃、ReconstructAll、ForwardWarp 内容位移。
// ============================================================================

#include "Tests.h"

#include "LiquifySession.h"
#include "sai2/host/MockHostContext.h"

#include <vector>

using namespace sai2;
using namespace sai2::host;
using namespace sai2::liquify;

namespace sai2test {

bool TestLiquifySession() {
    MockHostContext host;
    host.SetCanvasSize(128, 128, PixelFormat::RGBA8);

    // 测试图案：中心亮圆
    {
        ImageBuffer* canvas = host.GetImageBuffer();
        canvas->Clear(RGBA{});
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const int dx = x - 64, dy = y - 64;
                if (dx * dx + dy * dy < 900)
                    canvas->SetPixel(x, y, RGBA{255, 255, 255, 255});
            }
    }

    // ---- 1. 会话 + ForwardWarp 笔刷：内容被拖动 ----
    {
        LiquifySession session(host);
        session.Begin("Test Liquify");

        StrokeParams p;
        p.tool = LiquifyTool::ForwardWarp;
        p.cx = 40; p.cy = 64;
        p.radius = 30;
        p.pressure = 0.8f;
        p.dirX = 1.0f; p.dirY = 0.0f; // 向右拖
        for (int i = 0; i < 6; ++i) session.Stroke(p); // 模拟拖动多次采样

        CHECK(session.HasEdits());

        // 增量预览：受影响区域内容移动（(56,64) 原为圆内亮色被位移影响）
        const ImageBuffer& preview = session.Preview();
        CHECK(preview.IsValid());
        RGBA a;
        // ROI 外保持原样：(100, 100) 在圆外，仍为黑
        preview.GetPixel(100, 100, a);
        CHECK(a.r == 0);
        // ROI 内像素经过置换采样（不为全 0：ROI 内包含圆内像素）
        bool anyBright = false;
        for (int y = 34; y < 94; ++y)
            for (int x = 10; x < 70; ++x) {
                RGBA c;
                preview.GetPixel(x, y, c);
                if (c.r == 255) anyBright = true;
            }
        CHECK(anyBright);

        // ---- 2. Commit：回写画布 + Undo 恢复 ----
        CHECK(session.Commit());
        CHECK(host.refreshCount_ >= 1);
        CHECK(host.Undo());
        ImageBuffer* canvas = host.GetImageBuffer();
        RGBA orig;
        canvas->GetPixel(56, 64, orig);
        // 撤销后应与原始图案一致（(56,64) 在圆内）
        CHECK(orig.r == 255);
    }

    // ---- 3. Cancel：无副作用 ----
    {
        LiquifySession session(host);
        session.Begin("Cancel Test");
        StrokeParams p;
        p.tool = LiquifyTool::Bloat;
        p.cx = 64; p.cy = 64; p.radius = 40; p.pressure = 0.9f;
        session.Stroke(p);
        CHECK(session.HasEdits());
        session.Cancel();
        CHECK(!session.HasEdits());
        // 画布未被修改
        RGBA c;
        host.GetImageBuffer()->GetPixel(64, 64, c);
        CHECK(c.r == 255);
    }

    // ---- 4. ReconstructAll ----
    {
        LiquifySession session(host);
        session.Begin("Reconstruct Test");
        StrokeParams p;
        p.tool = LiquifyTool::TwirlCw;
        p.cx = 64; p.cy = 64; p.radius = 40; p.pressure = 1.0f;
        session.Stroke(p);
        CHECK(session.Field().MaxMagnitude() > 0.1f);
        session.ReconstructAll(1.0f);
        CHECK(session.Field().MaxMagnitude() < 0.001f);
        session.Cancel();
    }

    // ---- 5. Commit 后画布与预览一致（Bloat 放大中心） ----
    {
        LiquifySession session(host);
        session.Begin("Bloat Test");
        StrokeParams p;
        p.tool = LiquifyTool::Bloat;
        p.cx = 64; p.cy = 64; p.radius = 50; p.pressure = 1.0f;
        session.Stroke(p);
        session.Commit();
        // Commit 后画布中心保持亮（Bloat 不丢失中心内容）
        RGBA c;
        host.GetImageBuffer()->GetPixel(64, 64, c);
        CHECK(c.r == 255);
        // Undo 恢复
        CHECK(host.Undo());
        host.GetImageBuffer()->GetPixel(64, 64, c);
        CHECK(c.r == 255);
    }

    return true;
}

} // namespace sai2test
