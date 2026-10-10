#pragma once
// ============================================================================
// MockHostContext.h - 测试/演示宿主（Phase 2 验证 Host API 契约）
//
// 实现 IHostContext 全部接口：
//   - 持有画布 ImageBuffer 与 Selection
//   - UndoStack + UndoGroup
//   - 预览会话（BeginPreview 返回最近邻降采样缓冲；EndPreview(true) 回写主图）
//   - RefreshCanvas / ShowMessage / Log 计数，便于测试断言
// ============================================================================

#include "sai2/host/ComputeRegistry.h"
#include "sai2/host/UndoStack.h"
#include "sai2/IHostContext.h"

#include <string>
#include <vector>

namespace sai2::host {

class MockHostContext final : public IHostContext {
public:
    // ---- IHostContext ----------------------------------------------------
    ImageBuffer* GetImageBuffer() override { return &canvas_; }
    const Selection* GetSelection() const override {
        return selection_.IsEmpty() ? nullptr : &selection_;
    }

    void BeginUndoGroup(const char* name) override;
    void EndUndoGroup() override;
    void SnapshotForUndo(const char* label) override;
    bool Undo() override;

    ImageBuffer* BeginPreview(int maxPreviewDim) override;
    void EndPreview(bool commit) override;

    void RefreshCanvas() override { ++refreshCount_; }
    void ShowMessage(const char* message) override {
        messages_.emplace_back(message ? message : "");
    }
    void Log(LogLevel level, const char* message) override;

    // GPU 加速：返回注册表最优后端（DX11 优先，CPU 兜底）
    IComputeBackend* GetComputeBackend() override {
        return ComputeRegistry::Instance().Best();
    }

    // ---- 测试辅助 ----------------------------------------------------------
    void SetCanvasSize(int w, int h, PixelFormat fmt) { canvas_.Create(w, h, fmt); }
    void SetSelection(const Selection& sel) { selection_ = sel; }
    void ClearSelection() { selection_.Clear(); }

    int refreshCount_ = 0;
    int previewSessions_ = 0;
    std::vector<std::string> messages_;
    std::vector<std::string> logs_;
    UndoStack undo;

private:
    ImageBuffer canvas_;
    Selection selection_;
    ImageBuffer preview_;
    bool inPreview_ = false;
};

} // namespace sai2::host
