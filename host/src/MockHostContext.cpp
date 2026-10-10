#include "sai2/host/MockHostContext.h"
#include "sai2/host/Logging.h"

#include <algorithm>

namespace sai2::host {

void MockHostContext::BeginUndoGroup(const char* name) {
    undo.BeginGroup(name ? name : "");
}

void MockHostContext::EndUndoGroup() { undo.EndGroup(); }

void MockHostContext::SnapshotForUndo(const char* label) {
    undo.PushSnapshot(label ? label : "", canvas_);
}

bool MockHostContext::Undo() {
    const bool ok = undo.Undo(canvas_);
    if (ok) ++refreshCount_;
    return ok;
}

ImageBuffer* MockHostContext::BeginPreview(int maxPreviewDim) {
    if (!canvas_.IsValid()) return nullptr;
    if (maxPreviewDim <= 0) return nullptr;

    const int w = canvas_.Width();
    const int h = canvas_.Height();
    const int longest = std::max(w, h);
    const double scale = longest <= maxPreviewDim
                             ? 1.0
                             : static_cast<double>(maxPreviewDim) / longest;
    const int pw = std::max(1, static_cast<int>(w * scale));
    const int ph = std::max(1, static_cast<int>(h * scale));

    preview_.Create(pw, ph, canvas_.Format());

    // 最近邻降采样（真实宿主可换 Box/Average 采样）
    RGBA c;
    for (int y = 0; y < ph; ++y) {
        const int sy = std::min(h - 1, static_cast<int>(y / scale));
        for (int x = 0; x < pw; ++x) {
            const int sx = std::min(w - 1, static_cast<int>(x / scale));
            canvas_.GetPixel(sx, sy, c);
            preview_.SetPixel(x, y, c);
        }
    }

    inPreview_ = true;
    ++previewSessions_;
    return &preview_;
}

void MockHostContext::EndPreview(bool commit) {
    if (!inPreview_) return;
    inPreview_ = false;
    if (commit && preview_.IsValid()) {
        // 最近邻放大回写主画布（真实宿主会让插件对全分辨率图重跑）
        const double scaleX =
            static_cast<double>(canvas_.Width()) / preview_.Width();
        const double scaleY =
            static_cast<double>(canvas_.Height()) / preview_.Height();
        RGBA c;
        for (int y = 0; y < canvas_.Height(); ++y) {
            const int py = std::min(preview_.Height() - 1,
                                    static_cast<int>(y / scaleY));
            for (int x = 0; x < canvas_.Width(); ++x) {
                const int px = std::min(preview_.Width() - 1,
                                        static_cast<int>(x / scaleX));
                preview_.GetPixel(px, py, c);
                canvas_.SetPixel(x, y, c);
            }
        }
        ++refreshCount_;
    }
}

void MockHostContext::Log(LogLevel level, const char* message) {
    std::string line = "[";
    line += LogLevelName(level);
    line += "] ";
    line += message ? message : "";
    logs_.push_back(std::move(line));
    if (Logger::Instance().IsEnabled())
        Logger::Instance().Write(level, line);
}

} // namespace sai2::host
