#pragma once
// ============================================================================
// IFilter.h - 滤镜统一接口（Phase 5 Filter Gallery 使用，Phase 1/2 先定契约）
// ============================================================================

#include "sai2/PluginAPI.h"
#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

namespace sai2 {

struct FilterInfo {
    const char* id       = nullptr; // 如 "com.sai2.filter.oil-paint"
    const char* name     = nullptr; // 如 "Oil Paint"
    const char* category = nullptr; // Artistic / Sketch / Texture / Stylize
    const char* version  = nullptr;
};

class IFilter {
public:
    virtual ~IFilter() = default;

    virtual FilterInfo GetInfo() const = 0;

    // 对 buffer（或 buffer 内的 selection 区域）应用滤镜
    virtual bool Apply(ImageBuffer& buffer, const Selection* selection) = 0;

    // 可选参数面板
    virtual void* CreateParameterUI(void* parentWindow) {
        (void)parentWindow;
        return nullptr;
    }
};

} // namespace sai2
