// ============================================================================
// FilterPlugin.cpp - Filter Gallery 插件（Phase 5，规格书第九节）
//
// - FilterRegistry 注册 10 个滤镜（Artistic/Sketch/Texture/Stylize 四类）
// - Execute：对当前画布应用默认滤镜（完整 UndoGroup + Canvas Refresh）
// - 参数面板协议：ParamDefsJson() 供宿主动态生成 UI；预览走 BeginPreview
// ============================================================================

#include "FilterSet.h"
#include "GalleryFilters.h" // 规格表驱动的滤镜 + 已实现算法（Distort 批）
#include "GallerySpecs.h"  // 47 个 Photoshop 滤镜的规格表（自动生成）
#include "Filters.h"
#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"
#include "sai2/ParamHelpers.h"

#include <string>
#include <vector>

namespace {

using namespace sai2;
using namespace sai2::fg;

// ---------------------------------------------------------------------------
// FilterGalleryPlugin：注册全部滤镜并实现 IPlugin
// ---------------------------------------------------------------------------
class FilterGalleryPlugin final : public IPlugin {
public:
    FilterGalleryPlugin() {
        registry_.Add<OilPaintFilter>();
        registry_.Add<WatercolorFilter>();
        registry_.Add<RoughPastelFilter>();
        registry_.Add<PencilSketchFilter>();
        registry_.Add<CharcoalFilter>();
        registry_.Add<ChalkFilter>();
        registry_.Add<TexturizerFilter>();
        registry_.Add<GrainFilter>();
        registry_.Add<EmbossFilter>();
        registry_.Add<FindEdgesFilter>();

        // ---- 规格表驱动的新批量（移植自 photocraft 的 Photoshop 滤镜库）----
        // 每个滤镜的参数由参数记法自动生成，算法按 key 分派；未实现的 key
        // 由 GalleryApplyAll 返回 false（不会被当成"静默成功"）。
        {
            int n = 0;
            const GallerySpec* specs = GallerySpecs(n);
            // ★ 必须注册**规格表里的全部**滤镜：桥的界面是从同一张规格表生成的
            //   （47 项都在列表里），插件这边只注册一部分的话，界面上能选到、
            //   插件却不认识该 id → 回退成默认滤镜（find-edges）→ 用户看到
            //   "几乎全部都是黑色剪影"（实测踩坑，日志里就是
            //   "未知滤镜 id，回退默认: com.sai2.filter.gallery.xxx"）。
            for (int i = 0; i < n; ++i) {
                registry_.AddOwned(
                    std::make_unique<GalleryFilterBase>(specs[i], GalleryApplyAll));
            }
        }
    }

    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.filter-gallery";
        info.name = "Filter Gallery";
        info.version = "1.1.0";
        info.author = "SAI2 Plugin Team";
        info.type = "image-effect";
        info.description = "Photoshop-style filter gallery: gallery filters in "
                           "Artistic / Sketch / Texture / Stylize categories.";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        if (!host) return false;
        host_ = host;
        host_->Log(LogLevel::Info, ("FilterGalleryPlugin initialized: legacy 10 + gallery " + std::to_string(registry_.Count() - 10) + " filters").c_str());
        return true;
    }

    void Shutdown() override {
        if (host_) {
            host_->Log(LogLevel::Info, "FilterGalleryPlugin shutdown");
            host_ = nullptr;
        }
    }

    // 说明：参数化执行由宿主通过参数通道驱动（API 1.1）：
    //   filter.id            = 目标滤镜 id（缺省沿用默认滤镜）
    //   filter.param.<name>  = 该滤镜的参数值
    // 宿主未提供参数通道时（GetParam 返回 nullptr）行为与 API 1.0 完全一致。
    bool Execute() override {
        if (!host_) return false;
        ImageBuffer* canvas = host_->GetImageBuffer();
        if (!canvas || !canvas->IsValid()) {
            host_->Log(LogLevel::Error, "没有可用的画布");
            return false;
        }

        const std::string wanted =
            GetParamString(*host_, "filter.id", kDefaultFilterId);
        FilterBase* f = registry_.Find(wanted);
        if (!f) {
            host_->Log(LogLevel::Warning,
                       ("未知滤镜 id，回退默认: " + wanted).c_str());
            f = registry_.Find(kDefaultFilterId);
        }
        if (!f) {
            host_->Log(LogLevel::Error, "default filter missing");
            return false;
        }
        ApplyHostParams(*f);

        host_->BeginUndoGroup(("Filter Gallery: " +
                               std::string(f->GetInfo().name)).c_str());
        host_->SnapshotForUndo("Filter Gallery");

        const Selection* sel = host_->GetSelection();
        if (!f->Apply(*canvas, sel)) {
            // 未实现的滤镜（移植中）必须**明确失败**：否则界面显示"已应用"
            // 而画面毫无变化，用户会以为是 bug。写清 id 并退出撤销组。
            host_->Log(LogLevel::Warning,
                       ("filter not implemented yet: " +
                        std::string(f->GetInfo().id)).c_str());
            host_->EndUndoGroup();
            return false;
        }

        host_->Log(LogLevel::Info,
                   ("applied filter: " + std::string(f->GetInfo().id)).c_str());
        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

    // 参数面板：返回滤镜清单与参数描述的 JSON（自绘 UI 宿主据此生成）
    void* CreateUI(void* parentWindow) override {
        (void)parentWindow;
        uiJson_.clear();
        uiJson_ += "{\"categories\":[";
        bool firstCat = true;
        for (const auto& cat : registry_.Categories()) {
            if (!firstCat) uiJson_ += ",";
            firstCat = false;
            uiJson_ += "{\"name\":\"" + cat + "\",\"filters\":[";
            bool first = true;
            for (FilterBase* f : registry_.ByCategory(cat)) {
                if (!first) uiJson_ += ",";
                first = false;
                FilterInfo info = f->GetInfo();
                uiJson_ += "{\"id\":\"" + std::string(info.id) +
                           "\",\"name\":\"" + std::string(info.name) +
                           "\",\"params\":" + f->ParamDefsJson() + "}";
            }
            uiJson_ += "]}";
        }
        uiJson_ += "]}";
        return &uiJson_;
    }

private:
    static constexpr const char* kDefaultFilterId = "com.sai2.filter.find-edges";

    // 按宿主要求下发参数：只覆盖宿主明确给出的键，其余保留声明式默认值
    void ApplyHostParams(FilterBase& f) {
        for (const ParamDef& d : f.ParamDefs()) {
            const std::string key = "filter.param." + d.name;
            if (host_->GetParam(key.c_str()))
                f.SetParam(d.name, GetParamFloat(*host_, key.c_str(), d.def));
        }
    }

    IHostContext* host_ = nullptr;
    FilterRegistry registry_;
    std::string uiJson_;
};

} // namespace

SAI2_DEFINE_PLUGIN(FilterGalleryPlugin)
