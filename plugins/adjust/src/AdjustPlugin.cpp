// ============================================================================
// AdjustPlugin.cpp - PS 风格调整插件
//
// Execute 读取宿主参数通道选择调整种类与参数：
//   adjust.kind       = invert | grayscale | brightness-contrast |
//                       hue-saturation | levels
//   adjust.brightness = -100..100（亮度，满幅百分比）
//   adjust.contrast   = -100..100（对比度，中灰为轴）
//   adjust.hue        = -180..180（色相，度）
//   adjust.saturation = -100..100（饱和度）
//   adjust.lightness  = -100..100（明度）
//   adjust.inBlack    = 0..254（色阶输入黑场，8bit 口径）
//   adjust.inWhite    = 1..255（色阶输入白场，8bit 口径）
//   adjust.gamma      = 0.1..3（色阶中间调 gamma）
// ============================================================================

#include "AdjustKernels.h"
#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"
#include "sai2/ParamHelpers.h"

#include <string>

namespace {

using namespace sai2;

class AdjustPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.adjust";
        info.name = "Adjust";
        info.version = "1.0.0";
        info.author = "SAI2 Plugin Team";
        info.type = "image-effect";
        info.description =
            "PS-style adjustments: Invert / Grayscale / Brightness-Contrast / "
            "Hue-Saturation / Levels.";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        if (!host) return false;
        host_ = host;
        host_->Log(LogLevel::Info, "AdjustPlugin initialized");
        return true;
    }

    void Shutdown() override {
        if (host_) {
            host_->Log(LogLevel::Info, "AdjustPlugin shutdown");
            host_ = nullptr;
        }
    }

    bool Execute() override {
        if (!host_) return false;
        ImageBuffer* canvas = host_->GetImageBuffer();
        if (!canvas || !canvas->IsValid()) {
            host_->Log(LogLevel::Error, "没有可用的画布");
            return false;
        }

        const std::string kind = GetParamString(*host_, "adjust.kind", "invert");
        const Selection* sel = host_->GetSelection();

        host_->BeginUndoGroup("Adjust");
        host_->SnapshotForUndo("Adjust");

        if (kind == "invert") {
            adjust::ApplyInvert(*canvas, sel);
        } else if (kind == "grayscale") {
            adjust::ApplyGrayscale(*canvas, sel);
        } else if (kind == "brightness-contrast") {
            const float b = GetParamFloat(*host_, "adjust.brightness", 0.0f);
            const float c = GetParamFloat(*host_, "adjust.contrast", 0.0f);
            adjust::ApplyBrightnessContrast(*canvas, sel, b, c);
            host_->Log(LogLevel::Info,
                       ("brightness=" + std::to_string(b) +
                        " contrast=" + std::to_string(c))
                           .c_str());
        } else if (kind == "hue-saturation") {
            const float h = GetParamFloat(*host_, "adjust.hue", 0.0f);
            const float s = GetParamFloat(*host_, "adjust.saturation", 0.0f);
            const float l = GetParamFloat(*host_, "adjust.lightness", 0.0f);
            adjust::ApplyHueSaturation(*canvas, sel, h, s, l);
            host_->Log(LogLevel::Info,
                       ("hue=" + std::to_string(h) + " sat=" +
                        std::to_string(s) + " light=" + std::to_string(l))
                           .c_str());
        } else if (kind == "levels") {
            const float ib = GetParamFloat(*host_, "adjust.inBlack", 0.0f);
            const float iw = GetParamFloat(*host_, "adjust.inWhite", 255.0f);
            const float g = GetParamFloat(*host_, "adjust.gamma", 1.0f);
            adjust::ApplyLevels(*canvas, sel, ib, iw, g);
            host_->Log(LogLevel::Info,
                       ("levels in=[" + std::to_string(ib) + "," +
                        std::to_string(iw) + "] gamma=" + std::to_string(g))
                           .c_str());
        } else {
            host_->Log(LogLevel::Error,
                       ("未知 adjust.kind: " + kind).c_str());
            host_->EndUndoGroup();
            return false;
        }

        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

private:
    IHostContext* host_ = nullptr;
};

} // namespace

SAI2_DEFINE_PLUGIN(AdjustPlugin)
