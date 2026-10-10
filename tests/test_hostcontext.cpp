// ============================================================================
// test_hostcontext.cpp - MockHostContext 契约：UndoGroup / Preview / RefreshCanvas
// ============================================================================

#include "Tests.h"
#include "sai2/host/MockHostContext.h"

namespace sai2test {

bool TestHostContext() {
    using namespace sai2;
    using namespace sai2::host;

    MockHostContext host;
    host.SetCanvasSize(64, 64, PixelFormat::RGBA8);

    // ---- 选区 ----
    host.SetSelection(Selection(Rect{0, 0, 16, 16}));
    CHECK(host.GetSelection() != nullptr);
    CHECK(host.GetSelection()->Contains(0, 0));
    CHECK(!host.GetSelection()->Contains(16, 16));
    host.ClearSelection();
    CHECK(host.GetSelection() == nullptr);

    // ---- UndoGroup + 快照 + 撤销 ----
    ImageBuffer* canvas = host.GetImageBuffer();
    CHECK(canvas != nullptr && canvas->IsValid());

    host.BeginUndoGroup("Test Op");
    host.SnapshotForUndo("Test Op");
    canvas->FillRect(0, 0, 8, 8, RGBA{255, 0, 0, 255});
    host.RefreshCanvas();
    host.EndUndoGroup();
    CHECK(host.refreshCount_ == 1);

    RGBA got;
    CHECK(canvas->GetPixel(0, 0, got) && got.r == 255);

    CHECK(host.Undo());
    CHECK(canvas->GetPixel(0, 0, got) && got.r == 0); // 恢复
    CHECK(host.refreshCount_ == 2);                   // Undo 触发刷新
    CHECK(!host.Undo());

    // ---- 预览会话：降采样 -> 提交回写 ----
    host.BeginUndoGroup("Preview Op");
    host.SnapshotForUndo("Preview Op");
    canvas->Clear(RGBA{0, 0, 255, 255}); // 整图变蓝

    ImageBuffer* preview = host.BeginPreview(32); // 64 -> 32
    CHECK(preview != nullptr);
    CHECK(preview->Width() == 32 && preview->Height() == 32);
    CHECK(host.previewSessions_ == 1);
    CHECK(preview->GetPixel(0, 0, got) && got.b == 255);

    // 在预览上"调整"：右下角改红
    preview->FillRect(16, 16, 16, 16, RGBA{255, 0, 0, 255});
    host.EndPreview(true); // 提交 -> 放大回写主画布

    CHECK(canvas->GetPixel(63, 63, got) && got.r == 255 && got.b == 0);
    CHECK(canvas->GetPixel(0, 0, got) && got.b == 255); // 左上仍是蓝
    CHECK(host.refreshCount_ == 3);
    host.EndUndoGroup(); // 关闭 "Preview Op" 组

    // 撤销整个预览操作
    CHECK(host.Undo());
    CHECK(canvas->GetPixel(63, 63, got) && got.r == 0 && got.b == 0);

    // ---- 放弃预览（commit=false 不改主图） ----
    host.ClearSelection();
    ImageBuffer* p2 = host.BeginPreview(16);
    CHECK(p2 != nullptr && p2->Width() == 16);
    p2->Clear(RGBA{9, 9, 9, 255});
    host.EndPreview(false);
    CHECK(canvas->GetPixel(0, 0, got) && got.r != 9);
    CHECK(host.previewSessions_ == 2);

    // ---- 日志 / 消息 ----
    host.Log(LogLevel::Warning, "warn test");
    host.ShowMessage("hello");
    CHECK(host.logs_.size() == 1 && host.logs_[0].find("warn test") != std::string::npos);
    CHECK(host.messages_.size() == 1 && host.messages_[0] == "hello");

    std::cout << "  hostcontext: UndoGroup/Preview/Refresh 契约全部通过\n";
    return true;
}

} // namespace sai2test
