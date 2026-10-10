// ============================================================================
// BridgeHostContext.cpp
// ============================================================================

#include "BridgeHostContext.h"

#include <cstdio>
#include <utility>

namespace sai2::bridge {

BridgeHostContext::BridgeHostContext(LogSink sink) : logSink_(std::move(sink)) {}

void BridgeHostContext::SetCanvas(ImageBuffer img, bool clearUndo) {
    canvas_ = std::move(img);
    if (clearUndo) undo_.Clear();
    ++refreshCount_;
}

void BridgeHostContext::SetParam(const std::string& key,
                                 const std::string& value) {
    params_[key] = value;
}

void BridgeHostContext::SetParam(const std::string& key, float value) {
    // 保留 4 位小数，避免 "3.0000000001" 之类的噪声
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(value));
    params_[key] = buf;
}

void BridgeHostContext::SetParam(const std::string& key, int value) {
    params_[key] = std::to_string(value);
}

void BridgeHostContext::ClearParams() { params_.clear(); }

const char* BridgeHostContext::GetParam(const char* key) {
    if (!key) return nullptr;
    auto it = params_.find(key);
    return it == params_.end() ? nullptr : it->second.c_str();
}

void BridgeHostContext::BeginUndoGroup(const char* name) {
    if (!recordUndo_) return;
    undo_.BeginGroup(name ? name : "operation");
}

void BridgeHostContext::EndUndoGroup() {
    if (!recordUndo_) return;
    undo_.EndGroup();
}

void BridgeHostContext::SnapshotForUndo(const char* label) {
    if (!recordUndo_) return;
    undo_.PushSnapshot(label ? label : "snapshot", canvas_);
}

bool BridgeHostContext::Undo() { return undo_.Undo(canvas_); }

ImageBuffer* BridgeHostContext::BeginPreview(int) {
    inPreview_ = true;
    return nullptr; // 预览由桥自己降采样，走的仍是 GetImageBuffer 路径
}

void BridgeHostContext::EndPreview(bool) { inPreview_ = false; }

} // namespace sai2::bridge
