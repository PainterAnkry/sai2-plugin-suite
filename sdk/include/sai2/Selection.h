#pragma once
// ============================================================================
// Selection.h - 选区（Phase 2）
//
// 支持矩形选区与蒙版选区（8bit 灰度：非 0 表示选中）。
// 蒙版尺寸可以与画布不同，按 (px, py) 直接索引，越界视为未选中。
// ============================================================================

#include "sai2/PluginAPI.h"

#include <cstdint>
#include <vector>

namespace sai2 {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    bool IsValid() const { return w > 0 && h > 0; }
    bool Contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

inline bool operator==(const Rect& l, const Rect& r) {
    return l.x == r.x && l.y == r.y && l.w == r.w && l.h == r.h;
}

class Selection {
public:
    Selection() = default;
    explicit Selection(Rect rect);

    // 矩形选区
    void SetRect(Rect rect);
    // 蒙版选区（bounds 由蒙版尺寸决定）
    void SetMask(int width, int height, std::vector<uint8_t> mask);
    // 矩形 + 蒙版
    void SetRectWithMask(Rect rect, std::vector<uint8_t> mask);

    void Clear();

    bool IsEmpty() const { return empty_; }
    bool HasMask() const { return hasMask_; }

    // 返回选区外接矩形；空选区返回 {0,0,0,0}
    Rect Bounds() const;

    // 点是否被选中
    bool Contains(int px, int py) const;

    // 蒙版访问（HasMask() 为 true 时有效）
    int MaskWidth() const { return maskW_; }
    int MaskHeight() const { return maskH_; }
    const std::vector<uint8_t>& Mask() const { return mask_; }

private:
    bool empty_ = true;
    Rect rect_{};
    bool hasMask_ = false;
    int maskW_ = 0, maskH_ = 0;
    std::vector<uint8_t> mask_;
};

} // namespace sai2
