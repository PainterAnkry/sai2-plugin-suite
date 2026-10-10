#pragma once
// ============================================================================
// BridgeHostContext.h - 伴随宿主（sai2_bridge）的 IHostContext 实现
//
// 与 MockHostContext（测试用）的区别：这是"真宿主"。
//   - 画布 = 从 SAI2 剪贴板抓来 / 正在处理中的图像
//   - Undo  = 真实快照栈，面板上的"撤销"按钮即 IHostContext::Undo
//   - 计算后端 = ComputeRegistry（DX11 优先，CPU 兜底）
//   - 参数通道 = GetParam，把面板上的滑块值下发给插件（API 1.1）
//
// 插件完全不知道自己跑在伴随宿主里还是 SAI2 里 —— 这正是规格书第四节
// "插件不得直接修改 SAI2 内部对象，应通过 Host API 操作图像和状态"的落地。
// ============================================================================

#include "sai2/host/ComputeRegistry.h"
#include "sai2/host/UndoStack.h"
#include "sai2/IHostContext.h"

#include <functional>
#include <map>
#include <string>

namespace sai2::bridge {

class BridgeHostContext final : public IHostContext {
public:
    using LogSink = std::function<void(LogLevel, const std::string&)>;

    explicit BridgeHostContext(LogSink sink = nullptr);

    // ---- 画布管理（桥内部使用） -----------------------------------------
    void SetCanvas(ImageBuffer img, bool clearUndo);
    ImageBuffer& Canvas() { return canvas_; }
    const ImageBuffer& Canvas() const { return canvas_; }

    // ---- 参数通道（桥内部使用） -----------------------------------------
    void SetParam(const std::string& key, const std::string& value);
    void SetParam(const std::string& key, float value);
    void SetParam(const std::string& key, int value);
    void ClearParams();

    // ---- 状态查询（桥 UI 使用） -----------------------------------------
    int RefreshCount() const { return refreshCount_; }
    std::string LastMessage() const { return message_; }
    const host::UndoStack& Undo() const { return undo_; }
    size_t UndoDepth() const { return undo_.Size(); }

    // 预览阶段不应污染撤销栈（拖动滑块会产生大量中间状态），
    // 由桥在跑预览时关闭记录、真正应用时打开。
    void SetRecordingUndo(bool on) { recordUndo_ = on; }
    bool RecordingUndo() const { return recordUndo_; }

    // ---- IHostContext ---------------------------------------------------
    ImageBuffer* GetImageBuffer() override { return &canvas_; }
    const Selection* GetSelection() const override { return nullptr; }

    void BeginUndoGroup(const char* name) override;
    void EndUndoGroup() override;
    void SnapshotForUndo(const char* label) override;
    bool Undo() override;

    // 预览由桥统一负责（降采样后复用同一条插件执行路径），
    // 因此这里不额外提供预览缓冲，返回 nullptr。
    ImageBuffer* BeginPreview(int maxPreviewDim) override;
    void EndPreview(bool commit) override;

    void RefreshCanvas() override { ++refreshCount_; }
    void ShowMessage(const char* message) override {
        message_ = message ? message : "";
    }
    void Log(LogLevel level, const char* message) override {
        if (logSink_ && message) logSink_(level, message);
    }

    IComputeBackend* GetComputeBackend() override {
        IComputeBackend* b = host::ComputeRegistry::Instance().Best();
        return (b && b->Available()) ? b : nullptr;
    }

    const char* GetParam(const char* key) override;

private:
    ImageBuffer canvas_;
    Selection selection_;
    host::UndoStack undo_{50};
    std::map<std::string, std::string> params_;
    LogSink logSink_;
    std::string message_;
    int refreshCount_ = 0;
    bool inPreview_ = false;
    bool recordUndo_ = true;
};

} // namespace sai2::bridge
