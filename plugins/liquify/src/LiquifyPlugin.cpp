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
#include <vector>

namespace {

using namespace sai2;

class LiquifyPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.liquify";
        info.name = "Liquify";
        info.version = "1.2.0";
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

        // 笔级撤销协议（宿主 Ctrl+Z / 撤销按钮）：
        //   liquify.undo = 1 → 置换场回退到上一笔开始前的快照并重渲染
        //   liquify.begin = 1 → 本笔应用前先快照当前置换场
        // 撤销时不产生新的形变，直接渲染回退后的场。
        if (GetParamBool(*host_, "liquify.undo", false)) {
            if (!undoStack_.empty()) {
                // swap 而非深拷贝：undo 恢复后栈顶快照即被丢弃，
                // 直接交换可省一次全画布置换场的内存分配
                std::swap(field_, undoStack_.back());
                undoStack_.pop_back();
            }
            liquify::RenderLiquify(original_, field_, *canvas, host_->GetSelection(),
                               shiftX_, shiftY_);
            host_->Log(LogLevel::Info,
                       ("Liquify stroke undo, stack=" +
                        std::to_string(undoStack_.size()))
                           .c_str());
            host_->RefreshCanvas();
            return true;
        }
        if (GetParamBool(*host_, "liquify.begin", false)) {
            // 撤销快照按内存预算限深：置换场是全画布分辨率、每像素 8 字节，
            // 4000x3000 画布一步就要 96MB——固定 30 步要 2.9GB，足以把机器
            // 推进内存交换、拖垮整个系统（用户反馈"用完液化一直卡，必须
            // 关插件重启 SAI2"）。按 384MB 预算折算最大步数，
            // 小画布（1920x1080 ≈ 16MB/步）仍保留 30 步的完整撤销体验。
            const size_t bytesPerStep =
                static_cast<size_t>(field_.Width()) *
                static_cast<size_t>(field_.Height()) * 8;
            const size_t kUndoBudget = 384ull << 20;
            const size_t maxSteps =
                std::max<size_t>(3, kUndoBudget / std::max<size_t>(bytesPerStep, 1));
            undoStack_.push_back(field_);
            while (undoStack_.size() > maxSteps)
                undoStack_.erase(undoStack_.begin());
        }

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

        // 宿主协议扩展（内部参数；不传就是原来的"应用 + 渲染"，向后兼容）：
        //   liquify.mode = "accumulate" → 只把这一笔累加进置换场，不做逆映射渲染。
        //       大图上实时涂抹时，鼠标每个采样点都会调到这里，而逆映射是全图
        //       O(N)（8MP 要几十上百毫秒）——逐点渲染会把宿主的输入队列堵死，
        //       画面根本不刷新。所以宿主用这个模式廉价地"记一笔"。
        //   liquify.mode = "render"     → 只渲染当前置换场，不再应用新的一笔。
        //       宿主按显示帧率单独调一次，拿到这一帧要显示的画面。
        const std::string mode = GetParamString(*host_, "liquify.mode", "apply");

        if (mode != "render") {
            ApplyTool(tool, cx, cy, radius, strength, dirX, dirY, angleRad);
        }
        if (mode == "accumulate") {
            // 画布保持原样：宿主只关心插件内部累积的置换场，不取这个结果
            host_->Log(LogLevel::Info, ("Liquify " + tool + " accumulate").c_str());
            host_->EndUndoGroup();
            return true;
        }

        // 一次性逆映射渲染：源恒为 original_，笔笔干净
        liquify::RenderLiquify(original_, field_, *canvas, host_->GetSelection(),
                               shiftX_, shiftY_);
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

    // 会话（置换场身份）与源图身份**分开** —— 这是"多图层一起液化"的关键：
    //
    //   liquify.session  = 置换场身份。变化才清空置换场（原语义："换了一张图"）。
    //   liquify.source   = 源图身份。变化**只换源图、保留置换场**，这样同一套
    //                      笔画可以依次施加到每一层。
    //   liquify.shiftX/Y = 本图局部坐标 → 置换场坐标 的偏移，等于
    //                      （本层画布偏移 − 基准层画布偏移）；基准层为 0。
    //
    // 兼容性：调用方不传 liquify.source（旧行为）时，尺寸变化仍整套重置，
    // 与改动前完全一致；只有显式用了新参数才启用"换源不换场"。
    void BeginSessionIfNeeded(const ImageBuffer& canvas) {
        const std::string session =
            GetParamString(*host_, "liquify.session", "default");
        const std::string source =
            GetParamString(*host_, "liquify.source", "");
        shiftX_ = GetParamInt(*host_, "liquify.shiftX", 0);
        shiftY_ = GetParamInt(*host_, "liquify.shiftY", 0);

        const bool srcMismatch = !original_.IsValid() ||
                                 original_.Width() != canvas.Width() ||
                                 original_.Height() != canvas.Height();
        const bool legacy = source.empty();
        const bool fieldReset =
            GetParamBool(*host_, "liquify.reset", false) ||
            session != session_ || !field_.IsValid() ||
            (legacy && srcMismatch);   // 旧行为：不带 source 时换尺寸即重置

        if (fieldReset) {
            original_ = canvas.Clone();
            field_ = liquify::DisplacementField(original_.Width(),
                                                original_.Height());
            undoStack_.clear();
            session_ = session;
            source_ = source;
            host_->Log(LogLevel::Info,
                       ("Liquify session started: " + session).c_str());
            return;
        }
        // 置换场保持不变，只把源图换成新的一层（多图层一起液化走这条）
        if (srcMismatch || source != source_) {
            original_ = canvas.Clone();
            source_ = source;
            host_->Log(LogLevel::Info,
                       ("Liquify source switched, shift=(" +
                        std::to_string(shiftX_) + "," +
                        std::to_string(shiftY_) + ") size=" +
                        std::to_string(canvas.Width()) + "x" +
                        std::to_string(canvas.Height()))
                           .c_str());
        }
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
    std::vector<liquify::DisplacementField> undoStack_; // 笔级撤销快照（按内存预算限深，≤384MB）
    std::string session_;
    std::string source_;      // 源图身份（多图层一起液化时逐层切换，场不变）
    int shiftX_ = 0;          // 本图局部坐标 → 置换场坐标 的偏移
    int shiftY_ = 0;
};

} // namespace

SAI2_DEFINE_PLUGIN(LiquifyPlugin)
