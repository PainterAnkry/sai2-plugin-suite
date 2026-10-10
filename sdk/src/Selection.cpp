#include "sai2/Selection.h"

namespace sai2 {

Selection::Selection(Rect rect) : empty_(false), rect_(rect) {
    if (!rect.IsValid()) {
        empty_ = true;
        rect_ = Rect{};
    }
}

void Selection::SetRect(Rect rect) {
    hasMask_ = false;
    mask_.clear();
    maskW_ = maskH_ = 0;
    if (rect.IsValid()) {
        empty_ = false;
        rect_ = rect;
    } else {
        empty_ = true;
        rect_ = Rect{};
    }
}

void Selection::SetMask(int width, int height, std::vector<uint8_t> mask) {
    if (width <= 0 || height <= 0 ||
        mask.size() != static_cast<size_t>(width) * height) {
        Clear();
        return;
    }
    empty_ = false;
    hasMask_ = true;
    maskW_ = width;
    maskH_ = height;
    mask_ = std::move(mask);
    rect_ = Rect{0, 0, width, height};
}

void Selection::SetRectWithMask(Rect rect, std::vector<uint8_t> mask) {
    if (mask.size() != static_cast<size_t>(rect.w) * rect.h) {
        Clear();
        return;
    }
    empty_ = false;
    hasMask_ = true;
    maskW_ = rect.w;
    maskH_ = rect.h;
    mask_ = std::move(mask);
    rect_ = rect;
}

void Selection::Clear() {
    empty_ = true;
    hasMask_ = false;
    rect_ = Rect{};
    maskW_ = maskH_ = 0;
    mask_.clear();
}

Rect Selection::Bounds() const { return empty_ ? Rect{} : rect_; }

bool Selection::Contains(int px, int py) const {
    if (empty_) return false;
    if (!rect_.Contains(px, py)) return false;
    if (hasMask_) {
        const int mx = px - rect_.x;
        const int my = py - rect_.y;
        if (mx < 0 || my < 0 || mx >= maskW_ || my >= maskH_) return false;
        return mask_[static_cast<size_t>(my) * maskW_ + mx] != 0;
    }
    return true;
}

} // namespace sai2
