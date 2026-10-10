// ============================================================================
// BlurPlugin.cpp - Phase 3 模糊插件
//
// Execute 读取宿主参数通道（API 1.1）选择模糊种类与参数：
//   blur.kind     = gaussian | box | motion | radial
//   blur.radius   = 高斯/方框半径（像素）
//   blur.angle    = 动感模糊方向（度）
//   blur.distance = 动感模糊距离（像素）
//   blur.amount   = 径向模糊强度（0..1）
//   blur.samples  = 采样数
// 宿主未提供参数通道时（GetParam 返回 nullptr）沿用原有默认值
// （Gaussian radius=4），行为与 API 1.0 完全一致。
// ============================================================================

#include "BlurKernels.h"
#include "sai2/IComputeBackend.h"
#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"
#include "sai2/ParamHelpers.h"

#include <string>

namespace {

using namespace sai2;

class BlurPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.blur";
        info.name = "Blur";
        info.version = "1.1.0";
        info.author = "SAI2 Plugin Team";
        info.type = "image-effect";
        info.description =
            "Gaussian / Box / Motion / Radial blur with preview and undo.";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        if (!host) return false;
        host_ = host;
        host_->Log(LogLevel::Info, "BlurPlugin initialized");
        return true;
    }

    void Shutdown() override {
        if (host_) {
            host_->Log(LogLevel::Info, "BlurPlugin shutdown");
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

        const blur::BlurParams p = ReadParams(*host_);
        const char* kindName = KindName(p.kind);

        // 一次完整操作 = 一个 UndoGroup
        host_->BeginUndoGroup("Blur");
        host_->SnapshotForUndo("Blur");

        const Selection* sel = host_->GetSelection();
        // GPU 路径仅覆盖高斯；其余种类走 CPU 算法
        if (p.kind == blur::BlurKind::Gaussian && !sel &&
            canvas->Format() == PixelFormat::RGBA8 &&
            ExecuteGaussianGpu(*host_, *canvas, p.radius / 3.0f)) {
            // GPU 完成
        } else {
            blur::ApplyBlur(*canvas, p, sel);
        }

        host_->Log(LogLevel::Info,
                   (std::string("blur applied: ") + kindName + " radius=" +
                    std::to_string(p.radius) + " distance=" +
                    std::to_string(p.distance) + " amount=" +
                    std::to_string(p.amount))
                       .c_str());
        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

    // GPU 加速路径（规格书第十一节）：无选区且 RGBA8 时优先用宿主计算后端；
    // 后端不可用/不支持时自动回退 CPU 算法。
    static bool ExecuteGaussianGpu(IHostContext& host, ImageBuffer& img,
                                   float sigma) {
        if (sigma <= 0.0f) return false;
        IComputeBackend* backend = host.GetComputeBackend();
        if (!backend) return false;
        std::string err;
        if (!backend->GaussianBlur(img, sigma, err)) {
            host.Log(LogLevel::Info,
                     ("GPU backend unavailable, fallback CPU: " + err).c_str());
            return false;
        }
        host.Log(LogLevel::Info,
                 ("GPU blur via backend: " + backend->Info().name).c_str());
        return true;
    }

private:
    // 从宿主参数通道读取模糊参数（缺省即原有默认行为）
    static blur::BlurParams ReadParams(IHostContext& host) {
        blur::BlurParams p;
        const std::string kind = GetParamString(host, "blur.kind", "gaussian");
        if (kind == "box")         p.kind = blur::BlurKind::Box;
        else if (kind == "motion") p.kind = blur::BlurKind::Motion;
        else if (kind == "radial") p.kind = blur::BlurKind::Radial;
        else                       p.kind = blur::BlurKind::Gaussian;

        p.radius   = GetParamFloat(host, "blur.radius", p.radius);
        p.angleDeg = GetParamFloat(host, "blur.angle", p.angleDeg);
        p.distance = GetParamFloat(host, "blur.distance", p.distance);
        p.amount   = GetParamFloat(host, "blur.amount", p.amount);
        p.samples  = GetParamInt(host, "blur.samples", p.samples);

        // 防御非法值（面板范围之外的手工调用）
        if (p.radius < 0.0f) p.radius = 0.0f;
        if (p.distance < 0.0f) p.distance = 0.0f;
        if (p.amount < 0.0f) p.amount = 0.0f;
        if (p.amount > 1.0f) p.amount = 1.0f;
        if (p.samples < 2) p.samples = 2;
        if (p.samples > 64) p.samples = 64;
        return p;
    }

    static const char* KindName(blur::BlurKind k) {
        switch (k) {
            case blur::BlurKind::Gaussian: return "gaussian";
            case blur::BlurKind::Box:      return "box";
            case blur::BlurKind::Motion:   return "motion";
            case blur::BlurKind::Radial:   return "radial";
        }
        return "?";
    }

    IHostContext* host_ = nullptr;
};

} // namespace

SAI2_DEFINE_PLUGIN(BlurPlugin)
