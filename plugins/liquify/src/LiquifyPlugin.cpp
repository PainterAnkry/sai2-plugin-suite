// ============================================================================
// LiquifyPlugin.cpp - Phase 4 液化插件
//
// 与 MVP 版本的差别：这里实现的是"可被宿主逐笔驱动"的液化会话，而不是
// 一段写死的演示脚本。规格书第八节的画质要求（Displacement Field +
// Inverse Mapping，避免反复直接修改原始像素）通过两条规则落地：
//
//   1. 会话内始终保留原始图像 original_，每一笔都从 original_ 重新逆映射
//      渲染 → 无论画多少笔都不累积重采样损失（LiquifySession 同策略）；
//   2. 置换场 field_ 跨笔累积，因此笔刷可以连续涂抹。
//
// 宿主参数通道（API 1.1）：
//   liquify.session  = 会话标识，变化即"换了一张图"，重置原图缓存与置换场
//   liquify.tool     = forward-warp | bloat | pinch | twirl-cw | twirl-ccw
//                      | smooth | reconstruct
//   liquify.cx/cy    = 笔刷中心（0..1 归一化，尺度无关）
//   liquify.radius   = 笔刷半径（相对 min(w,h) 的比例，尺度无关）
//   liquify.strength = 强度
//   liquify.dirX/dirY= 向前变形的拖动方向
//   liquify.angle    = 旋转角度（度）
// 无参数通道时退化为单笔向前变形（API 1.0 行为）。
// ============================================================================

#include "LiquifyCore.h"
#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"
#include "sai2/ParamHelpers.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

using namespace sai2;

class LiquifyPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.liquify";
        info.name = "Liquify";
        info.version = "1.1.0";
        info.author = "SAI2 Plugin Team";
        info.type = "image-effect";
        info.description =
            "Forward Warp / Bloat / Pinch / Twirl / Smooth / Reconstruct "
            "via displacement field + inverse mapping.";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        if (!host) return false;
        host_ = host;
        host_->Log(LogLevel::Info, "LiquifyPlugin initialized");
        return true;
    }

    void Shutdown() override {
        if (host_) {
            host_->Log(LogLevel::Info, "LiquifyPlugin shutdown");
            host_ = nullptr;
        }
        original_ = ImageBuffer();
        session_.clear();
    }

    bool Execute() override {
        if (!host_) return false;
        ImageBuffer* canvas = host_->GetImageBuffer();
        if (!canvas || !canvas->IsValid()) {
            host_->Log(LogLevel::Error, "没有可用的画布");
            return false;
        }

        BeginSessionIfNeeded(*canvas);

        host_->BeginUndoGroup("Liquify");
        host_->SnapshotForUndo("Liquify");

        // 向后兼容（规格书第十五节"API 必须考虑向后兼容"）：
        // 宿主没有参数通道（如测试用 MockHostContext / 旧版宿主）时，
        // 沿用 1.0 的内置演示笔画，保证 Execute 依旧"看得见效果"。
        if (!host_->GetParam("liquify.tool")) {
            LegacyDemoStroke(*canvas);
            host_->Log(LogLevel::Info, "Liquify legacy demo stroke applied");
            host_->RefreshCanvas();
            host_->EndUndoGroup();
            return true;
        }

        const int w = original_.Width();
        const int h = original_.Height();
        const std::string tool =
            GetParamString(*host_, "liquify.tool", "forward-warp");

        // 笔刷几何：归一化输入 × 图像尺寸（尺度无关，预览/全分辨率一致）
        const float cx = GetParamFloat(*host_, "liquify.cx", 0.5f) * w;
        const float cy = GetParamFloat(*host_, "liquify.cy", 0.5f) * h;
        float radius =
            GetParamFloat(*host_, "liquify.radius", 0.15f) *
            static_cast<float>(std::min(w, h));
        radius = std::max(2.0f, radius);
        const float strength = GetParamFloat(*host_, "liquify.strength", 1.0f);
        const float dirX = GetParamFloat(*host_, "liquify.dirX", 0.0f);
        const float dirY = GetParamFloat(*host_, "liquify.dirY", 0.0f);
        const float angleDeg = GetParamFloat(*host_, "liquify.angle", 45.0f);
        const float angleRad = angleDeg * 3.14159265358979f / 180.0f;

        ApplyTool(tool, cx, cy, radius, strength, dirX, dirY, angleRad);

        // 一次性逆映射渲染：源恒为 original_，笔笔干净
        liquify::RenderLiquify(original_, field_, *canvas,
                               host_->GetSelection());

        // 注意：这里不能为了日志去调 field_.MaxMagnitude() —— 它是全图 O(N)
        // 求模，每笔鼠标移动都执行一次，大图上就是几十毫秒的纯浪费。
        host_->Log(LogLevel::Info,
                   ("Liquify " + tool + " at (" + std::to_string(cx) + "," +
                    std::to_string(cy) + ")")
                       .c_str());
        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

private:
    // API 1.0 行为：没有宿主参数时的一段确定性演示笔画
    // （向前变形多戳模拟拖动 + 右上角扭转 + 置换场平滑）
    static void LegacyDemoStroke(ImageBuffer& canvas) {
        const int w = canvas.Width(), h = canvas.Height();
        if (w <= 0 || h <= 0) return;
        liquify::DisplacementField field(w, h);
        const float cy = h * 0.5f;
        for (int i = 0; i < 8; ++i) {
            const float cx = w * 0.2f + i * (w * 0.05f);
            field.ForwardWarp(cx, cy, w * 0.12f, 1.0f, 0.0f, 6.0f);
        }
        field.Twirl(w * 0.75f, h * 0.25f, w * 0.15f, 0.6f);
        field.Smooth(2);
        liquify::RenderLiquify(canvas, field, canvas, nullptr);
    }

    // 新会话（换图 / 尺寸变化 / 显式 reset）时缓存原图并清空置换场
    void BeginSessionIfNeeded(const ImageBuffer& canvas) {
        const std::string session =
            GetParamString(*host_, "liquify.session", "default");
        const bool sizeMismatch = !original_.IsValid() ||
                                  original_.Width() != canvas.Width() ||
                                  original_.Height() != canvas.Height();
        const bool reset =
            GetParamBool(*host_, "liquify.reset", false) ||
            session != session_ || sizeMismatch;
        if (!reset) return;

        original_ = canvas.Clone();
        field_ = liquify::DisplacementField(original_.Width(),
                                            original_.Height());
        session_ = session;
        host_->Log(LogLevel::Info,
                   ("Liquify session started: " + session).c_str());
    }

    void ApplyTool(const std::string& tool, float cx, float cy, float radius,
                   float strength, float dirX, float dirY, float angleRad) {
        if (tool == "bloat") {
            field_.Bloat(cx, cy, radius, strength * 0.25f);
        } else if (tool == "pinch") {
            field_.Pinch(cx, cy, radius, strength * 0.25f);
        } else if (tool == "twirl-cw") {
            field_.Twirl(cx, cy, radius, angleRad * strength);
        } else if (tool == "twirl-ccw") {
            field_.Twirl(cx, cy, radius, -angleRad * strength);
        } else if (tool == "smooth") {
            field_.Smooth(2);
        } else if (tool == "reconstruct") {
            field_.Reconstruct(cx, cy, radius,
                               std::min(1.0f, std::max(0.0f, strength)));
        } else { // forward-warp（默认）
            const float len = std::sqrt(dirX * dirX + dirY * dirY);
            if (len > 1e-4f) {
                field_.ForwardWarp(cx, cy, radius, dirX / len, dirY / len,
                                   strength * 6.0f);
            }
        }
    }

    IHostContext* host_ = nullptr;
    ImageBuffer original_;            // 会话原始图像（画质无损的基准）
    liquify::DisplacementField field_; // 跨笔累积的置换场
    std::string session_;
};

} // namespace

SAI2_DEFINE_PLUGIN(LiquifyPlugin)
