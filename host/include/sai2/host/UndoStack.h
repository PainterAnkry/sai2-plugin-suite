#pragma once
// ============================================================================
// UndoStack.h - 快照式撤销栈（Phase 2，规格书第七节）
//
// 一次完整插件操作 = 一个 UndoGroup。
// 组内可压入多张快照（如笔刷分步操作），撤销时整组恢复（恢复组内最早的快照）。
// ============================================================================

#include "sai2/ImageBuffer.h"

#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace sai2::host {

struct UndoEntry {
    std::string name;      // 操作名，如 "Liquify"
    ImageBuffer snapshot;  // 该组最早快照
};

class UndoStack {
public:
    explicit UndoStack(size_t maxGroups = 50);

    void BeginGroup(const std::string& name);
    void EndGroup();
    bool InGroup() const { return inGroup_; }

    // 把当前图像压入当前组（不在组内时自动创建匿名单步组）
    void PushSnapshot(const std::string& label, const ImageBuffer& image);

    // 撤销：把最近一个组的快照写回 active；返回是否成功
    bool Undo(ImageBuffer& active);

    bool CanUndo() const { return !groups_.empty(); }
    size_t Size() const { return groups_.size(); }
    void Clear();
    const std::string& LastGroupName() const;

private:
    struct PendingGroup {
        std::string name;
        ImageBuffer first;
        bool hasFirst = false;
    };

    size_t maxGroups_;
    bool inGroup_ = false;
    PendingGroup pending_;
    std::deque<UndoEntry> groups_;
};

} // namespace sai2::host
