#pragma once
// ============================================================================
// LiquifySession.h - 液化工作流引擎（规格书第八节，独立窗口的引擎层）
//
// 对应 Photoshop 液化工作流（UI 无关，供 Qt 6 / Win32 自绘窗口驱动）：
//   - 会话开始：以画布尺寸建立置换场与持久预览缓冲，BeginUndoGroup；
//   - 笔刷交互：Stroke() 把笔刷采样（大小/压力/速度）累积进置换场，
//     并增量渲染受影响 ROI（dest(p) 只依赖 D(p)，ROI = 笔刷范围精确），
//     每帧成本 = 笔刷区域像素数，目标 30~60 FPS；
//   - 确定：Commit() 全分辨率渲染回写 + SnapshotForUndo + RefreshCanvas；
//   - 取消：Cancel() 丢弃置换场与预览缓冲；
//   - ReconstructAll 整体重建 / SmoothAll 全场平滑（触发全预览刷新）。
// ============================================================================

#include "LiquifyCore.h"
#include "sai2/IHostContext.h"

#include <algorithm>
#include <cmath>

namespace sai2::liquify {

enum class LiquifyTool {
    ForwardWarp,
    Bloat,
    Pinch,
    TwirlCw,        // 顺时针旋转
    TwirlCcw,       // 逆时针旋转
    Smooth,
    Reconstruct,
};

struct StrokeParams {
    LiquifyTool tool = LiquifyTool::ForwardWarp;
    float cx = 0.0f, cy = 0.0f; // 笔刷中心（画布坐标）
    float radius = 50.0f;       // 笔刷大小
    float pressure = 0.5f;      // 压力/强度 0..1
    float speed = 1.0f;         // 拖动速度（笔刷采样间距因子）
    float dirX = 0.0f, dirY = 0.0f; // ForwardWarp 的拖动方向
};

class LiquifySession {
public:
    explicit LiquifySession(IHostContext& host)
        : host_(host) {
        const ImageBuffer* canvas = host_.GetImageBuffer();
        if (canvas && canvas->IsValid()) {
            preview_ = canvas->Clone(); // 持久预览缓冲（一次性成本）
            field_.Reset(preview_.Width(), preview_.Height());
        }
    }

    // ---- 会话生命周期 ------------------------------------------------------
    // 进入液化窗口时调用：打开一个 UndoGroup（一次完整操作 = 一次撤销）
    void Begin(const char* sessionName = "Liquify") {
        host_.BeginUndoGroup(sessionName);
        groupOpen_ = true;
    }

    // ---- 笔刷交互 + 增量预览 -----------------------------------------------
    // 一次笔刷采样（UI 拖动时按速度采样多次调用）。
    // 只修改置换场，并增量渲染受影响 ROI（每帧成本 = 笔刷区域）。
    void Stroke(const StrokeParams& p) {
        const float s = std::min(1.0f, std::max(0.0f, p.pressure)) * 4.0f;
        switch (p.tool) {
        case LiquifyTool::ForwardWarp: {
            const float len = std::sqrt(p.dirX * p.dirX + p.dirY * p.dirY);
            if (len > 1e-4f)
                field_.ForwardWarp(p.cx, p.cy, p.radius,
                                   p.dirX / len, p.dirY / len,
                                   s * p.speed);
            break;
        }
        case LiquifyTool::Bloat:
            field_.Bloat(p.cx, p.cy, p.radius, s * 0.25f);
            break;
        case LiquifyTool::Pinch:
            field_.Pinch(p.cx, p.cy, p.radius, s * 0.25f);
            break;
        case LiquifyTool::TwirlCw:
            field_.Twirl(p.cx, p.cy, p.radius, 0.4f * s * p.speed);
            break;
        case LiquifyTool::TwirlCcw:
            field_.Twirl(p.cx, p.cy, p.radius, -0.4f * s * p.speed);
            break;
        case LiquifyTool::Smooth:
            field_.Smooth(2);
            RefreshAllPreview();
            return;
        case LiquifyTool::Reconstruct:
            field_.Reconstruct(p.cx, p.cy, p.radius,
                               std::min(1.0f, p.pressure));
            break;
        }
        // 增量渲染：dest(p) 只依赖 D(p)，受影响像素 = 笔刷范围
        const int x0 = std::max(0, static_cast<int>(p.cx - p.radius));
        const int y0 = std::max(0, static_cast<int>(p.cy - p.radius));
        const int x1 = std::min(preview_.Width() - 1,
                                static_cast<int>(p.cx + p.radius) + 1);
        const int y1 = std::min(preview_.Height() - 1,
                                static_cast<int>(p.cy + p.radius) + 1);
        if (x1 >= x0 && y1 >= y0)
            RenderRegion(*host_.GetImageBuffer(), field_, preview_,
                         Rect{x0, y0, x1 - x0 + 1, y1 - y0 + 1});
    }

    // ---- 实时预览缓冲 ------------------------------------------------------
    // UI 直接把该缓冲贴到窗口（无拷贝）；ROI 增量更新在 Stroke 内完成
    const ImageBuffer& Preview() const { return preview_; }

    // ---- 确定 / 取消 -------------------------------------------------------
    // 用户点击"确定"：全分辨率渲染回写画布（作为当前 UndoGroup 的一部分）
    bool Commit() {
        ImageBuffer* canvas = host_.GetImageBuffer();
        if (!canvas || !canvas->IsValid()) return false;
        host_.SnapshotForUndo("Liquify"); // 画布尚未被修改，快照即原始状态
        ImageBuffer result;
        RenderLiquify(*canvas, field_, result, host_.GetSelection());
        *canvas = result;
        host_.RefreshCanvas();
        if (groupOpen_) {
            host_.EndUndoGroup();
            groupOpen_ = false;
        }
        return true;
    }

    // 用户点击"取消"：丢弃置换场与预览缓冲（画布从未被修改）
    void Cancel() {
        field_.Reset(0, 0);
        preview_ = ImageBuffer();
        if (groupOpen_) {
            host_.EndUndoGroup();
            groupOpen_ = false;
        }
    }

    // ---- 全场操作 ----------------------------------------------------------
    void ReconstructAll(float strength) {
        field_.ReconstructAll(strength);
        RefreshAllPreview();
    }

    void SmoothAll() {
        field_.Smooth(2);
        RefreshAllPreview();
    }

    const DisplacementField& Field() const { return field_; }
    bool HasEdits() const { return field_.MaxMagnitude() > 1e-4f; }

private:
    // 全预览刷新（Smooth / ReconstructAll 后调用，低频操作）
    void RefreshAllPreview() {
        const ImageBuffer* canvas = host_.GetImageBuffer();
        if (!canvas || !canvas->IsValid()) return;
        RenderLiquify(*canvas, field_, preview_, nullptr);
    }

    // ROI 局部渲染
    static void RenderRegion(const ImageBuffer& src,
                             const DisplacementField& field,
                             ImageBuffer& dst, const Rect& roi) {
        for (int y = roi.y; y < roi.y + roi.h; ++y)
            for (int x = roi.x; x < roi.x + roi.w; ++x) {
                if (x < 0 || y < 0 || x >= src.Width() ||
                    y >= src.Height()) continue;
                RGBA c;
                SampleBilinear(src, x + field.Dx(x, y), y + field.Dy(x, y), c);
                dst.SetPixel(x, y, c);
            }
    }

    IHostContext& host_;
    DisplacementField field_;
    ImageBuffer preview_; // 持久预览缓冲（增量 ROI 更新）
    bool groupOpen_ = false;
};

} // namespace sai2::liquify
