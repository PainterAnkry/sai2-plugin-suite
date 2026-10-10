#include "sai2/host/UndoStack.h"
#include "sai2/host/Logging.h"

namespace sai2::host {

UndoStack::UndoStack(size_t maxGroups) : maxGroups_(maxGroups) {}

void UndoStack::BeginGroup(const std::string& name) {
    pending_ = PendingGroup{};
    pending_.name = name;
    inGroup_ = true;
}

void UndoStack::EndGroup() {
    if (!inGroup_) return;
    if (pending_.hasFirst) {
        groups_.push_back(
            UndoEntry{pending_.name, pending_.first});
        while (groups_.size() > maxGroups_) groups_.pop_front();
    }
    pending_ = PendingGroup{};
    inGroup_ = false;
}

void UndoStack::PushSnapshot(const std::string& label,
                             const ImageBuffer& image) {
    if (!inGroup_) {
        // 自动匿名单步组
        BeginGroup(label);
        if (!pending_.hasFirst) {
            pending_.first = image.Clone();
            pending_.hasFirst = true;
        }
        EndGroup();
        return;
    }
    // 组内第一张快照作为该组撤销目标
    if (!pending_.hasFirst) {
        pending_.first = image.Clone();
        pending_.hasFirst = true;
    }
    (void)label;
}

bool UndoStack::Undo(ImageBuffer& active) {
    if (groups_.empty()) return false;
    UndoEntry& entry = groups_.back();
    if (!entry.snapshot.IsValid()) return false;
    active = entry.snapshot.Clone();
    groups_.pop_back();
    return true;
}

void UndoStack::Clear() {
    groups_.clear();
    pending_ = PendingGroup{};
    inGroup_ = false;
}

const std::string& UndoStack::LastGroupName() const {
    static const std::string kEmpty;
    return groups_.empty() ? kEmpty : groups_.back().name;
}

} // namespace sai2::host
