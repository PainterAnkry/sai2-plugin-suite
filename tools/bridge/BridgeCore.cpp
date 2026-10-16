// ============================================================================
// BridgeCore.cpp - 伴随宿主核心流程实现（多窗口会话版）
//
// 约定：
//   * 预览 = 按窗口显示倍率跑一遍（非液化走降采样图，速度快）
//   * 提交 = 一律按全分辨率跑一遍，再写剪贴板
//   * 液化 = 置换场按全分辨率坐标累积（与显示倍率无关），因此"预览看到的
//     形变"与"提交得到的形变"一致；液化永远不重放笔画，只重复一次渲染。
// ============================================================================

#include "BridgeCore.h"

#include "BridgeHostContext.h"
#include "ClipboardImage.h"
#include "sai2/host/PluginManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sai2 {
namespace bridge {

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 按最长边缩放
int FitLongest(int w, int h, int longest) {
    const int m = std::max(w, h);
    if (m <= longest || m <= 0) return w;
    return std::max(1, static_cast<int>(static_cast<long long>(w) * longest / m));
}

} // namespace

// ---------------------------------------------------------------------------
// 单个窗口的预览会话
// ---------------------------------------------------------------------------
struct BridgeCore::UiPreview {
    bool open = false;
    int effectIndex = -1;
    ImageBuffer base;    // 该窗口的全分辨率基准（提交用）
    ImageBuffer src;     // 预览用（可能降采样；液化 = 预算内全图）
    ImageBuffer display; // 当前预览结果
    float scale = 1.0f;  // 预览倍率（显示像素 / 图像像素）
    float commitScale = 1.0f; // 当前 display 对应的倍率（1.0 = 已是提交结果）
    bool showApplied = true;
    double ms = 0.0;
};

BridgeCore::BridgeCore(std::string pluginsDir, std::string logDir)
    : pluginsDir_(std::move(pluginsDir)), logDir_(std::move(logDir)) {
    wins_.resize(kWinCount);
    sessionIds_.assign(kWinCount, std::string());
}

BridgeCore::~BridgeCore() { Shutdown(); }

BridgeCore::UiPreview* BridgeCore::Win(int winIdx) {
    return &wins_[static_cast<size_t>(Norm(winIdx))];
}

const BridgeCore::UiPreview* BridgeCore::Win(int winIdx) const {
    return &wins_[static_cast<size_t>(Norm(winIdx))];
}

bool BridgeCore::Startup(std::string& error) {
    host_ = std::make_unique<BridgeHostContext>(
        [this](LogLevel level, const std::string& msg) {
            Log(std::string("[") + LogLevelName(level) + "] " + msg);
        });

    manager_ = std::make_unique<sai2::host::PluginManager>(
        pluginsDir_, logDir_, kPluginApiVersion, host_.get());
    if (!manager_->Startup(error)) return false;

    effects_ = BuildEffects();

    // 插件没装/没启用时，对应效果标记为不可用（界面上灰掉，而不是让用户点了报错）
    available_.assign(effects_.size(), false);
    for (size_t i = 0; i < effects_.size(); ++i) {
        for (const auto& item : manager_->ListPlugins()) {
            if (item.id == effects_[i].pluginId && item.loaded && item.enabled) {
                available_[i] = true;
                break;
            }
        }
    }

    thumbs_.assign(effects_.size(), ImageBuffer());
    thumbDone_.assign(effects_.size(), false);

    started_ = true;
    for (int w = 0; w < kWinCount; ++w)
        sessionIds_[static_cast<size_t>(w)] =
            "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);

    // 主面板默认选中第一个可用效果
    int first = -1;
    for (size_t i = 0; i < effects_.size(); ++i)
        if (available_[i]) { first = static_cast<int>(i); break; }
    if (first < 0) first = 0;
    SetCurrentEffect(first, kWinMain);

    Log("bridge started, plugins: " + PluginSummary());
    return true;
}

void BridgeCore::Shutdown() {
    if (manager_) {
        manager_->Shutdown();
        manager_.reset();
    }
    host_.reset();
    started_ = false;
}

// ---------------------------------------------------------------------------
// 效果查询
// ---------------------------------------------------------------------------

bool BridgeCore::EffectAvailable(int index) const {
    return index >= 0 && index < static_cast<int>(available_.size()) &&
           available_[static_cast<size_t>(index)];
}

std::vector<int> BridgeCore::EffectsInGroup(const char* group) const {
    std::vector<int> out;
    for (size_t i = 0; i < effects_.size(); ++i)
        if (effects_[i].group == group) out.push_back(static_cast<int>(i));
    return out;
}

int BridgeCore::FindEffectById(const std::string& id) const {
    for (size_t i = 0; i < effects_.size(); ++i)
        if (effects_[i].id == id) return static_cast<int>(i);
    return -1;
}

void BridgeCore::LoadParams(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= static_cast<int>(effects_.size())) return;
    current_ = effectIndex;
    const std::string& id = effects_[static_cast<size_t>(effectIndex)].id;
    const auto it = effectParams_.find(id);
    if (it != effectParams_.end()) {
        paramValues_ = it->second;
        return;
    }
    paramValues_.clear();
    for (const UiParam& p : effects_[static_cast<size_t>(effectIndex)].params)
        paramValues_.push_back(p.def);
    effectParams_[id] = paramValues_;
}

void BridgeCore::SetCurrentEffect(int index, int winIdx) {
    if (index < 0 || index >= static_cast<int>(effects_.size())) return;
    const int w = Norm(winIdx);
    LoadParams(index);
    if (w == kWinMain) mainEffect_ = index;
    UiPreview* wp = Win(w);
    if (w != kWinMain && wp->open) wp->effectIndex = index;
    UpdatePreview(w);
}

std::vector<float> BridgeCore::ParamValues(int winIdx) const {
    EnsureParamsForWindow(winIdx);
    return paramValues_;
}

void BridgeCore::SetParamValue(size_t index, float value, int winIdx) {
    EnsureParamsForWindow(winIdx);
    if (index >= paramValues_.size() ||
        current_ >= static_cast<int>(effects_.size()))
        return;
    const UiParam& p = effects_[static_cast<size_t>(current_)].params[index];
    float v = std::min(p.max, std::max(p.min, value));
    if (p.isInt) v = std::round(v);
    paramValues_[index] = v;
    effectParams_[effects_[static_cast<size_t>(current_)].id] = paramValues_;
}

// 让 paramValues_ 对齐"该窗口选中的效果"的参数
void BridgeCore::EnsureParamsForWindow(int winIdx) const {
    const int w = Norm(winIdx);
    const UiPreview* wp = Win(w);
    const int eidx = (w != kWinMain && wp->effectIndex >= 0) ? wp->effectIndex
                                                             : current_;
    if (eidx < 0 || eidx >= static_cast<int>(effects_.size())) return;
    if (eidx == current_) return;
    const std::string& id = effects_[static_cast<size_t>(eidx)].id;
    const auto it = effectParams_.find(id);
    if (it != effectParams_.end()) {
        paramValues_ = it->second;
    } else {
        paramValues_.clear();
        for (const UiParam& p : effects_[static_cast<size_t>(eidx)].params)
            paramValues_.push_back(p.def);
        effectParams_[id] = paramValues_;
    }
    current_ = eidx;
}

void BridgeCore::ParamChanged(int winIdx) {
    const int w = Norm(winIdx);
    UiPreview* wp = Win(w);
    if (w != kWinMain && !wp->open) return;
    // 滑块类参数不改变液化置换场，因此这里重跑一次渲染即可（不重放笔画）
    wp->display = RenderEffect(w, wp->scale, false);
    wp->commitScale = wp->scale;
    wp->showApplied = false;
    lastPreviewMs_ = wp->ms;
}

// ---------------------------------------------------------------------------
// 图像接收
// ---------------------------------------------------------------------------

bool BridgeCore::HasWindow(int winIdx) const { return Win(winIdx)->open; }

const ImageBuffer& BridgeCore::DisplayImage(int winIdx) const {
    const UiPreview* w = Win(winIdx);
    if (w->showApplied && applied_.IsValid()) return applied_;
    if (w->display.IsValid()) return w->display;
    return original_;
}

float BridgeCore::RenderScale(int winIdx) const { return Win(winIdx)->scale; }

bool BridgeCore::DisplayIsFullRes(int winIdx) const {
    const UiPreview* w = Win(winIdx);
    return w->display.IsValid() && w->commitScale >= 0.999f;
}

bool BridgeCore::WindowSnapshotsEngineState(int winIdx) const {
    const UiPreview* w = Win(winIdx);
    if (w->effectIndex >= 0 &&
        w->effectIndex < static_cast<int>(effects_.size()) &&
        effects_[static_cast<size_t>(w->effectIndex)].fullResPreview)
        return true;
    if (current_ >= 0 && current_ < static_cast<int>(effects_.size()) &&
        effects_[static_cast<size_t>(current_)].fullResPreview)
        return true;
    return false;
}

ImageBuffer BridgeCore::WindowBase(int winIdx) const {
    return Win(winIdx)->base.Clone();
}

void BridgeCore::SetImage(ImageBuffer img, const std::string& sourceLabel) {
    original_ = std::move(img);
    applied_ = original_.Clone();
    history_.clear();

    // 换图 = 全部会话作废（插件据此重置缓存的原图与置换场）
    for (int w = 0; w < kWinCount; ++w) {
        sessionIds_[static_cast<size_t>(w)] =
            "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);
        Win(w)->showApplied = true;
    }
    thumbs_.assign(effects_.size(), ImageBuffer());
    thumbDone_.assign(effects_.size(), false);
    thumbSource_ = ImageBuffer();

    // ★ 保持"图 = 画布坐标"：已知画布上下文时立即补边成整幅画布。
    // 必须在下面重建预览之前调用，否则窗口会以包围盒尺寸建会话。
    PadBaseToCanvas();

    // 打开中的功能窗口全部重建会话（旧基准已过期）
    for (int w = 1; w < kWinCount; ++w)
        if (wins_[static_cast<size_t>(w)].open) OpenWindow(w);
    RebuildPreviewSource(kWinMain);

    status_ = sourceLabel + "  " + std::to_string(original_.Width()) + "x" +
              std::to_string(original_.Height());
    Log("image acquired (" + std::to_string(original_.Width()) + "x" +
        std::to_string(original_.Height()) + ") from " + sourceLabel);
    UpdatePreview(kWinMain);
}

bool BridgeCore::GrabFromClipboard(std::string& error) {
    ImageBuffer img;
    std::string format;
    if (!ReadClipboardImage(img, error, &format)) return false;
    SetImage(std::move(img), "剪贴板 " + format);
    return true;
}

// ---------------------------------------------------------------------------
// 窗口会话
// ---------------------------------------------------------------------------

void BridgeCore::OpenWindow(int winIdx) {
    const int w = Norm(winIdx);
    if (w == kWinMain) return;
    UiPreview* wp = Win(w);
    wp->open = true;
    if (wp->effectIndex < 0 || !EffectAvailable(wp->effectIndex)) {
        // 默认取该分组的第一个效果
        const std::vector<int> g = EffectsInGroup(kGroupNames[w - 1]);
        wp->effectIndex = g.empty() ? current_ : g.front();
        // 首次打开窗口：把引擎当前效果切到该窗口的默认效果，参数随之载入
        current_ = wp->effectIndex;
        paramValues_.clear();
        for (const UiParam& p : effects_[static_cast<size_t>(current_)].params)
            paramValues_.push_back(p.def);
    } else {
        // 重新打开窗口：沿用上次的效果，参数按该效果的默认值重置
        // （与 PS 一致：重开对话框不记忆上一次的拖动结果）
        current_ = wp->effectIndex;
        paramValues_.clear();
        for (const UiParam& p : effects_[static_cast<size_t>(current_)].params)
            paramValues_.push_back(p.def);
    }
    // 该窗口自己的液化会话（每次打开都是干净的置换场）
    sessionIds_[static_cast<size_t>(w)] =
        "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);
    RebuildPreviewSource(w);
    wp->showApplied = false;
    UpdatePreview(w);
    Log(std::string("window opened: ") + kGroupNames[w - 1] + " (" +
        std::to_string(static_cast<int>(wp->scale * 100)) + "%)");
}

void BridgeCore::CloseWindow(int winIdx) {
    const int w = Norm(winIdx);
    if (w == kWinMain) return;
    UiPreview* wp = Win(w);
    wp->open = false;
    wp->display = ImageBuffer();
    wp->src = ImageBuffer();
    wp->base = ImageBuffer();
    // 关窗即作废该窗口的液化会话（下次打开从干净原图开始）
    sessionIds_[static_cast<size_t>(w)] =
        "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);
}

void BridgeCore::CloseAllToolWindows() {
    for (int w = 1; w < kWinCount; ++w) CloseWindow(w);
}

void BridgeCore::RebuildPreviewSource(int winIdx) {
    const int w = Norm(winIdx);
    UiPreview* wp = Win(w);
    if (!original_.IsValid()) return;

    // 叠加模式下的基准 = 当前已应用结果（与主面板语义一致）
    const bool useApplied = chain_ && applied_.IsValid();
    const ImageBuffer base = useApplied ? applied_ : original_;
    wp->base = base.Clone();

    int limit = kMainPreviewMaxDim;
    if (w == kWinLiquify) {
        // 液化显示用图：按像素预算降采样（4MP ≈ 2000x2000，笔刷延迟可控），
        // 置换场仍在全分辨率坐标下求解，所以预览不会"变软就变味"
        const double px = static_cast<double>(base.Width()) * base.Height();
        if (px > kLiquifyPreviewPixels) {
            const double f = std::sqrt(kLiquifyPreviewPixels / px);
            limit = std::max(1, static_cast<int>(base.Width() * f));
        } else {
            limit = std::max(base.Width(), base.Height());
        }
    }
    wp->src = (w == kWinLiquify)
                  ? Downscale(base, limit)
                  : Downscale(base, kMainPreviewMaxDim);
    wp->scale = static_cast<float>(wp->src.Width()) /
                static_cast<float>(std::max(1, base.Width()));
    wp->commitScale = wp->scale;
}

// 最近邻 + 邻域平均的简单降采样（预览足够，且不引入额外依赖）
ImageBuffer BridgeCore::Downscale(const ImageBuffer& src, int maxDim) {
    if (!src.IsValid()) return ImageBuffer();
    const int w = src.Width(), h = src.Height();
    const int longest = std::max(w, h);
    if (longest <= maxDim || maxDim <= 0 || w <= 0 || h <= 0) return src.Clone();

    const double scale = static_cast<double>(maxDim) / longest;
    const int nw = std::max(1, static_cast<int>(w * scale));
    const int nh = std::max(1, static_cast<int>(h * scale));

    ImageBuffer out;
    out.Create(nw, nh, src.Format());
    const int bx = std::max(1, w / nw);
    const int by = std::max(1, h / nh);
    const uint32_t bpc = src.BytesPerChannel();

    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int dy = 0; dy < by; ++dy) {
                const int sy = std::min(h - 1, y * by + dy);
                for (int dx = 0; dx < bx; ++dx) {
                    const int sx = std::min(w - 1, x * bx + dx);
                    const uint8_t* p = src.Data() +
                                       static_cast<size_t>(sy) * src.Stride() +
                                       static_cast<size_t>(sx) * 4 * bpc;
                    for (int c = 0; c < 4; ++c) {
                        if (bpc == 1) {
                            acc[c] += p[c];
                        } else {
                            uint16_t v;
                            std::memcpy(&v, p + c * 2, 2);
                            acc[c] += v;
                        }
                    }
                    ++n;
                }
            }
            if (!n) continue;
            RGBA c;
            c.r = static_cast<uint16_t>(acc[0] / n);
            c.g = static_cast<uint16_t>(acc[1] / n);
            c.b = static_cast<uint16_t>(acc[2] / n);
            c.a = static_cast<uint16_t>(acc[3] / n);
            out.SetPixel(x, y, c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 执行链路
// ---------------------------------------------------------------------------

ImageBuffer BridgeCore::PreviewBase(int winIdx) {
    UiPreview* w = Win(winIdx);
    if (w->base.IsValid()) return w->base.Clone();
    return original_.Clone();
}

ImageBuffer BridgeCore::RenderEffect(int winIdx, float scale, bool recordUndo,
                                     const ImageBuffer* explicitBase,
                                     const char* liqMode) {
    const int wi = Norm(winIdx);
    UiPreview* w = Win(wi);
    const bool isTool = (wi != kWinMain);
    const int eidx = (isTool && w->effectIndex >= 0) ? w->effectIndex : current_;
    if (!manager_ || !host_ || eidx < 0 ||
        eidx >= static_cast<int>(effects_.size()))
        return ImageBuffer();

    // 参数与目标效果必须配对：以窗口选中的效果为准（不是"引擎里碰巧那个"）
    if (eidx != current_) EnsureParamsForWindow(wi);
    w->effectIndex = eidx;

    const EffectDef& e = effects_[static_cast<size_t>(eidx)];

    // 基准与倍率必须"成对"来自同一张图，否则预览与提交会不一致：
    //   * 顶部入口（explicitBase）与提交（scale >= 1）= 全分辨率
    //   * 普通预览 = 窗口预览图（降采样），倍率按它相对全分辨率基准的比例算
    //   * 液化永远用全分辨率基准：置换场与像素坐标绑定
    const bool fullRes = (explicitBase != nullptr) || scale >= 0.999f ||
                         e.fullResPreview;
    ImageBuffer base;
    float paramScale = 1.0f;
    if (explicitBase) {
        base = explicitBase->Clone();
    } else if (fullRes) {
        base = PreviewBase(wi);
    } else if (w->src.IsValid()) {
        base = w->src.Clone();
    } else {
        base = Downscale(PreviewBase(wi), kMainPreviewMaxDim);
    }
    if (!fullRes) {
        // 像素半径类参数按"预览图 / 全分辨率基准"的比例缩放
        const float baseW =
            static_cast<float>(std::max(1, w->base.IsValid() ? w->base.Width()
                                                            : base.Width()));
        paramScale = static_cast<float>(base.Width()) / baseW;
    }

    host_->ClearParams();
    for (const auto& kv : e.fixed) host_->SetParam(kv.first, kv.second);
    for (size_t i = 0; i < e.params.size() && i < paramValues_.size(); ++i) {
        const UiParam& p = e.params[i];
        float v = paramValues_[i];
        if (paramScale < 0.999f && p.pixelScale) v *= paramScale;
        if (p.isInt)
            host_->SetParam(p.key, static_cast<int>(std::lround(v)));
        else
            host_->SetParam(p.key, v);
    }

    if (e.group == kGroupLiquify) {
        host_->SetParam("liquify.session", sessionIds_[static_cast<size_t>(wi)]);
        host_->SetParam("liquify.cx", strokeX_);
        host_->SetParam("liquify.cy", strokeY_);
        host_->SetParam("liquify.dirX", pendingZeroDir_ ? 0.0f : pendingDirX_);
        host_->SetParam("liquify.dirY", pendingZeroDir_ ? 0.0f : pendingDirY_);
        // 涂抹过程中的"显示帧"：只重渲染置换场，不再重复应用最后一笔
        if (liqMode) host_->SetParam("liquify.mode", std::string(liqMode));
        // 笔级撤销协议：本笔开始前先让插件快照置换场；撤销时回退一笔
        if (strokeBeginPending_) {
            host_->SetParam("liquify.begin", 1);
            strokeBeginPending_ = false;
            const std::string& sid = sessionIds_[static_cast<size_t>(wi)];
            if (liquifyStrokeSession_ != sid) {
                liquifyStrokeSession_ = sid;
                liquifyStrokeCount_ = 0;
            }
            ++liquifyStrokeCount_;
        }
        if (liquifyUndoPending_) {
            host_->SetParam("liquify.undo", 1);
            liquifyUndoPending_ = false;
        }
    }

    host_->SetRecordingUndo(recordUndo);
    host_->SetCanvas(std::move(base), false);
    std::string err;
    if (!manager_->ExecutePlugin(e.pluginId, err)) {
        status_ = "执行失败: " + err;
        Log("execute failed: " + err);
    }
    host_->SetRecordingUndo(true);
    return std::move(host_->Canvas());
}

void BridgeCore::UpdatePreview(int winIdx, const char* liqMode) {
    const int wi = Norm(winIdx);
    if (!started_ || !HasImage() || effects_.empty()) return;
    UiPreview* w = Win(wi);
    if (wi != kWinMain && !w->open) return;

    const int eidx = (wi != kWinMain && w->effectIndex >= 0)
                         ? w->effectIndex
                         : (wi == kWinMain ? mainEffect_ : current_);
    if (!EffectAvailable(eidx)) {
        w->showApplied = true;
        w->display = applied_.Clone();
        return;
    }
    // 基准/预览图缺失（例如无界面的自检模式）时补建一次
    if (!w->base.IsValid() || !w->src.IsValid()) RebuildPreviewSource(wi);

    const auto t0 = Clock::now();
    // 预览不记录 Undo（拖动滑块会产生大量中间状态）
    w->display = RenderEffect(wi, w->scale, false, nullptr, liqMode);
    w->ms = MsSince(t0);
    w->commitScale = w->scale;
    w->showApplied = false;
    lastPreviewMs_ = w->ms;
}

void BridgeCore::PushAppliedHistory() {
    history_.push_back(applied_.IsValid() ? applied_.Clone() : ImageBuffer());
    if (history_.size() > 20) history_.erase(history_.begin());
}

void BridgeCore::SetCanvasContext(bool has, int canvasW, int canvasH,
                                  int offX, int offY) {
    hasCanvasCtx_ = has && canvasW > 0 && canvasH > 0;
    canvasW_ = canvasW;
    canvasH_ = canvasH;
    canvasX0_ = offX;
    canvasY0_ = offY;
    if (hasCanvasCtx_) {
        Log("canvas ctx: " + std::to_string(canvasW_) + "x" +
            std::to_string(canvasH_) + " offset(" +
            std::to_string(canvasX0_) + "," + std::to_string(canvasY0_) + ")");
    } else {
        Log("canvas ctx: none");
    }
    // 抓到图层后紧接着就会走到这里（先 SetImage 再 SetCanvasContext），
    // 此时把基准补边成整幅画布并重建预览，窗口立刻以画布取景。
    PadBaseToCanvas();
    if (hasCanvasCtx_ && original_.IsValid() &&
        original_.Width() == canvasW_ && original_.Height() == canvasH_) {
        for (int w = 0; w < kWinCount; ++w)
            if (w == kWinMain || wins_[static_cast<size_t>(w)].open)
                RebuildPreviewSource(w);
        UpdatePreview(kWinMain);
    }
}

// 把 img 摆到画布 (x0,y0) 并补边成 cw x ch（越界裁掉）。
// 抽成带显式参数的函数：多图层一起液化时，每一层的画布偏移都不同，
// 不能再用"当前会话记录的单个 ctx"。
static ImageBuffer PadImageAt(const ImageBuffer& img, int cw, int ch, int x0,
                              int y0) {
    if (!img.IsValid()) return ImageBuffer();
    if (img.Width() == cw && img.Height() == ch && x0 == 0 && y0 == 0)
        return img.Clone();
    ImageBuffer out(cw, ch, PixelFormat::RGBA8);
    out.Clear(RGBA{0, 0, 0, 0});
    const int px = x0, py = y0;
    const int iw = img.Width(), ih = img.Height();
    for (int y = 0; y < ih; ++y) {
        const int dy = py + y;
        if (dy < 0 || dy >= ch) continue;
        int sx = 0;                       // 源列起点（偏移为负时裁掉左侧）
        if (px < 0) {
            sx = -px;
            if (sx >= iw) continue;
        }
        const int dx = std::max(px, 0);   // 目标列起点
        const int copy = std::min(iw - sx, cw - dx);
        if (copy <= 0) continue;
        std::memcpy(out.Data() + out.Stride() * static_cast<size_t>(dy) +
                        static_cast<size_t>(dx) * 4,
                    img.Data() + img.Stride() * static_cast<size_t>(y) +
                        static_cast<size_t>(sx) * 4,
                    static_cast<size_t>(copy) * 4);
    }
    return out;
}

void BridgeCore::PadBaseToCanvas() {
    if (!hasCanvasCtx_ || !original_.IsValid()) return;
    if (original_.Width() == canvasW_ && original_.Height() == canvasH_) {
        canvasX0_ = 0;   // 已经是画布坐标：偏移恒为零
        canvasY0_ = 0;
        return;
    }
    const int ox = canvasX0_, oy = canvasY0_;
    original_ = PadImageAt(original_, canvasW_, canvasH_, ox, oy);
    if (applied_.IsValid())
        applied_ = PadImageAt(applied_, canvasW_, canvasH_, ox, oy);
    canvasX0_ = 0;
    canvasY0_ = 0;
    Log("base padded to canvas " + std::to_string(canvasW_) + "x" +
        std::to_string(canvasH_) + " (was at " + std::to_string(ox) + "," +
        std::to_string(oy) + ")");
}

ImageBuffer BridgeCore::PadToCanvas(const ImageBuffer& img) const {
    return PadImageAt(img, canvasW_, canvasH_, canvasX0_, canvasY0_);
}

bool BridgeCore::WriteImageToClipboard(const ImageBuffer& img,
                                       std::string& error) {
    if (!img.IsValid()) {
        error = "没有可写入的图像";
        return false;
    }
    return WriteClipboardImage(img, error);
}

// 多图层一起液化（方案 B：逐层施加同一形变、保留分层）的核心一步。
//
// 关键点：**同一个 liquify.session**（= 置换场身份，不许重置）、
// **新的 liquify.source**（= 换源图，插件只重缓存源图、保留置换场）、
// **liquify.shiftX/Y**（= 本层偏移 − 基准层偏移，置换场采样偏移）、
// **liquify.mode = "render"**（只渲染现有场，不要再叠一笔）。
//
// 还要显式带 liquify.tool：插件里"没有 liquify.tool 参数"会走 1.0 的
// LegacyDemoStroke（内置演示笔画），那样会凭空多出一笔形变。
ImageBuffer BridgeCore::RenderLiquifyLayerToCanvas(
    const ImageBuffer& layerSrc, const std::string& sourceId, int cw, int ch,
    int offX, int offY, int shiftX, int shiftY) {
    if (!started_ || !manager_ || !host_ || !layerSrc.IsValid() || cw <= 0 ||
        ch <= 0)
        return ImageBuffer();

    // 用液化窗口正在用的那个效果；当前效果不是液化组时退回第一个液化效果
    int idx = current_;
    if (idx < 0 || idx >= static_cast<int>(effects_.size()) ||
        effects_[static_cast<size_t>(idx)].group != kGroupLiquify) {
        idx = -1;
        for (size_t i = 0; i < effects_.size(); ++i)
            if (effects_[i].group == kGroupLiquify) {
                idx = static_cast<int>(i);
                break;
            }
        if (idx < 0) {
            Log("multilayer: no liquify effect available");
            return ImageBuffer();
        }
    }
    const EffectDef& e = effects_[static_cast<size_t>(idx)];

    host_->ClearParams();
    for (const auto& kv : e.fixed) host_->SetParam(kv.first, kv.second);
    host_->SetParam("liquify.session", sessionIds_[kWinLiquify]);
    host_->SetParam("liquify.source", sourceId);
    host_->SetParam("liquify.shiftX", shiftX);
    host_->SetParam("liquify.shiftY", shiftY);
    host_->SetParam("liquify.mode", std::string("render"));
    host_->SetRecordingUndo(false);
    host_->SetCanvas(layerSrc.Clone(), false);
    std::string err;
    if (!manager_->ExecutePlugin(e.pluginId, err)) {
        host_->SetRecordingUndo(true);
        Log("multilayer: execute failed: " + err);
        return ImageBuffer();
    }
    ImageBuffer out = std::move(host_->Canvas());
    host_->SetRecordingUndo(true);
    if (!out.IsValid()) {
        Log("multilayer: plugin produced no canvas");
        return ImageBuffer();
    }
    Log("multilayer: layer rendered src=" + sourceId + " size=" +
        std::to_string(out.Width()) + "x" + std::to_string(out.Height()) +
        " shift=(" + std::to_string(shiftX) + "," + std::to_string(shiftY) +
        ") place=(" + std::to_string(offX) + "," + std::to_string(offY) + ")");
    return PadImageAt(out, cw, ch, offX, offY);
}

bool BridgeCore::WriteAppliedToClipboard(std::string& error) {
    if (!applied_.IsValid()) {
        error = "没有可写入的图像";
        return false;
    }
    if (!hasCanvasCtx_) return WriteClipboardImage(applied_, error);
    const ImageBuffer padded = PadToCanvas(applied_);
    return WriteClipboardImage(padded, error);
}

BridgeCore::ApplyResult BridgeCore::ApplyToClipboard() {
    ApplyResult r;
    // 这些失败路径以前只写 status_/UI flash，**不进日志** ——
    // 界面上闪一下就没了，排查时完全看不到（踩过：--hotkey 触发应用后
    // 日志里连一句 applied 都没有，无法判断是没执行还是提前返回）。
    if (!started_ || !HasImage()) {
        r.error = "还没有图像：请先在 SAI2 中复制选区";
        status_ = r.error;
        Log("apply FAILED: " + r.error);
        return r;
    }
    if (!EffectAvailable(current_)) {
        r.error = "当前效果对应的插件未加载";
        status_ = r.error;
        Log("apply FAILED: " + r.error + " (effect#" +
            std::to_string(current_) + ")");
        return r;
    }

    const EffectDef& e = effects_[static_cast<size_t>(current_)];
    const int winIdx = (e.group == kGroupLiquify)  ? kWinLiquify
                       : (e.group == kGroupFilter) ? kWinFilter
                       : (e.group == kGroupAdjust) ? kWinAdjust
                       : (e.group == kGroupBlur)   ? kWinBlur
                                                   : kWinMain;
    UiPreview* w = Win(winIdx);

    // 提交基准：叠加模式下以"当前已应用结果"为底，否则用抓到的原图。
    // 注意这里不能沿用工具窗口的预览基准 —— 窗口可能没打开/会话与当前效果
    // 不一致，那样提交出来的就不是用户看到的那张。
    const bool useChain = chain_ && e.group != kGroupLiquify;
    const ImageBuffer commitBase =
        (useChain && applied_.IsValid()) ? applied_.Clone() : original_.Clone();

    const auto t0 = Clock::now();
    ImageBuffer result;
    // 窗口里的显示图已经是"全分辨率 + 同一效果 + 同一会话"时直接复用
    // （液化尤其重要：用同一个会话再渲染一次得到的是同一张图，但没必要多花时间）
    if (winIdx != kWinMain && w->open && w->effectIndex == current_ &&
        DisplayIsFullRes(winIdx)) {
        result = w->display.Clone();
        lastApplyMs_ = MsSince(t0);
    } else {
        result = RenderEffect(winIdx, 1.0f, true, &commitBase);
        lastApplyMs_ = MsSince(t0);
    }

    if (!result.IsValid()) {
        r.error = "插件未产出有效图像";
        status_ = r.error;
        Log("apply FAILED: " + r.error + " (effect=" + e.id + ")");
        return r;
    }

    PushAppliedHistory();
    applied_ = std::move(result);
    for (int i = 0; i < kWinCount; ++i) {
        UiPreview* wi = Win(i);
        wi->showApplied = true;
        // 应用后基准变化（叠加模式下尤其明显）：重建窗口会话
        if (i != kWinMain && wi->open) RebuildPreviewSource(i);
    }
    RebuildPreviewSource(kWinMain);

    std::string err;
    const bool wrote = WriteAppliedToClipboard(err);
    r.ok = true;
    r.clipboardWritten = wrote;
    r.error = wrote ? "" : err;
    r.clipboardFormat = wrote ? "CF_DIBV5 + CF_DIB" : "";
    status_ = std::string("已应用 ") + e.group + " / " + e.name;
    if (wrote)
        status_ += " → 已写入剪贴板（切到 SAI2 按 Ctrl+V 粘贴）";
    else
        status_ += " → 写剪贴板失败: " + err;
    Log("applied: " + e.id + " (" + std::to_string(lastApplyMs_) + " ms)" +
        (wrote ? " clipboard written" : " clipboard FAILED"));
    return r;
}

bool BridgeCore::WriteClipboard(std::string& error) {
    return WriteAppliedToClipboard(error);
}

bool BridgeCore::Undo(std::string& error) {
    if (history_.empty()) {
        error = "没有可撤销的操作";
        return false;
    }
    applied_ = std::move(history_.back());
    history_.pop_back();
    if (!applied_.IsValid()) applied_ = original_.Clone();

    for (int w = 0; w < kWinCount; ++w) {
        // 换基准 = 换会话（液化置换场作废）
        sessionIds_[static_cast<size_t>(w)] =
            "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);
        Win(w)->showApplied = true;
        if (w == kWinMain || Win(w)->open) RebuildPreviewSource(w);
    }
    if (!WriteClipboard(error)) {
        status_ = "已撤销（写剪贴板失败: " + error + "）";
        return true;
    }
    status_ = "已撤销，剪贴板已同步";
    Log("undo, history depth=" + std::to_string(history_.size()));
    return true;
}

bool BridgeCore::ResetToOriginal(std::string& error) {
    if (!HasImage()) {
        error = "还没有图像";
        return false;
    }
    PushAppliedHistory();
    applied_ = original_.Clone();
    for (int w = 0; w < kWinCount; ++w) {
        sessionIds_[static_cast<size_t>(w)] =
            "w" + std::to_string(w) + "-s" + std::to_string(++sessionCounter_);
        Win(w)->showApplied = true;
        if (w == kWinMain || Win(w)->open) RebuildPreviewSource(w);
    }
    if (!WriteClipboard(error)) {
        status_ = "已复位（写剪贴板失败: " + error + "）";
        return true;
    }
    status_ = "已复位到抓取原图，剪贴板已同步";
    Log("reset to original");
    return true;
}

void BridgeCore::SetChain(bool on) {
    if (chain_ == on) return;
    chain_ = on;
    for (int w = 0; w < kWinCount; ++w)
        if (w == kWinMain || Win(w)->open) RebuildPreviewSource(w);
    UpdatePreview(kWinMain);
}

// ---------------------------------------------------------------------------
// 液化笔画
//
// 一次 StrokeMove 可能携带多个采样点（鼠标高频抖动），每次调用按
// "上一采样点 → 本次点"提交一笔；膨胀/收缩/旋转类工具只用笔刷中心，
// 方向置零。步进过小（< 笔刷半径的 2%）时跳过，避免无意义的置换。
// ---------------------------------------------------------------------------

bool BridgeCore::CurrentEffectStrokes() const {
    return current_ >= 0 && current_ < static_cast<int>(effects_.size()) &&
           effects_[static_cast<size_t>(current_)].stroke;
}

void BridgeCore::StrokeBegin(float nx, float ny, int winIdx) {
    if (!HasImage()) return;
    const int w = Norm(winIdx);
    strokeActive_ = true;
    strokeWin_ = w;
    strokeX_ = std::min(1.0f, std::max(0.0f, nx));
    strokeY_ = std::min(1.0f, std::max(0.0f, ny));
    strokeDX_ = 0.0f;
    strokeDY_ = 0.0f;
    pendingDirX_ = 0.0f;
    pendingDirY_ = 0.0f;
    pendingZeroDir_ = true;
    // 通知插件在应用本笔前快照置换场（供 Ctrl+Z 撤销上一笔）
    strokeBeginPending_ = true;
    // 单击也算一笔（膨胀/收缩/旋转不需要拖动方向）：渲染首帧
    UpdatePreview(w);
}

void BridgeCore::StrokeMove(float nx, float ny, int winIdx) {
    if (!strokeActive_ || !HasImage()) return;
    const int w = Norm(winIdx);
    if (w != strokeWin_) return;

    const float px = strokeX_, py = strokeY_;
    strokeX_ = std::min(1.0f, std::max(0.0f, nx));
    strokeY_ = std::min(1.0f, std::max(0.0f, ny));

    strokeDX_ = (strokeX_ - px) * original_.Width();
    strokeDY_ = (strokeY_ - py) * original_.Height();

    const float len = std::sqrt(strokeDX_ * strokeDX_ + strokeDY_ * strokeDY_);
    const int eidx = Win(w)->effectIndex >= 0 ? Win(w)->effectIndex : current_;
    if (eidx < 0 || eidx >= static_cast<int>(effects_.size())) return;
    const EffectDef& e = effects_[static_cast<size_t>(eidx)];
    const float radius = paramValues_.empty() ? 0.15f : paramValues_[0];
    const float minLen = std::max(
        1.0f, radius * std::min(original_.Width(), original_.Height()) * 0.02f);
    const std::string tool =
        e.fixed.empty() ? std::string("forward-warp") : e.fixed[0].second;
    const bool forwardWarp = (tool == "forward-warp");
    if (forwardWarp && len < minLen) return;
    if (!forwardWarp && len < 0.5f) return; // 单击类工具：一次按下只算一笔

    if (forwardWarp) {
        pendingDirX_ = strokeDX_;
        pendingDirY_ = strokeDY_;
        pendingZeroDir_ = false;
    } else {
        pendingDirX_ = 0.0f;
        pendingDirY_ = 0.0f;
        pendingZeroDir_ = true;
    }

    // 只累积、不渲染：把这一笔加进插件内部的置换场（很便宜），
    // 逆映射渲染交给 StrokeRender 按显示帧率做。
    // 之前这里是"同步全图渲染 + 12ms 节流"——渲染一次几十上百毫秒，鼠标事件
    // 125Hz 灌进来，输入队列越积越多；WM_PAINT 又是低优先级消息被饿死，
    // 于是拖动期间画布根本不刷新（用户报的"画的时候没变化"）。
    StrokeAccumulate(w);
    Win(w)->showApplied = false;
}

void BridgeCore::StrokeAccumulate(int winIdx) {
    const int w = Norm(winIdx);
    if (!started_ || !manager_ || !host_) return;
    UiPreview* wp = Win(w);
    const int eidx = (w != kWinMain && wp->effectIndex >= 0) ? wp->effectIndex
                                                             : current_;
    if (!EffectAvailable(eidx)) return;
    if (eidx != current_) EnsureParamsForWindow(w);
    const EffectDef& e = effects_[static_cast<size_t>(eidx)];
    if (e.group != kGroupLiquify) return;

    host_->ClearParams();
    for (const auto& kv : e.fixed) host_->SetParam(kv.first, kv.second);
    for (size_t i = 0; i < e.params.size() && i < paramValues_.size(); ++i) {
        const UiParam& p = e.params[i];
        const float v = paramValues_[i];
        if (p.isInt)
            host_->SetParam(p.key, static_cast<int>(std::lround(v)));
        else
            host_->SetParam(p.key, v);
    }
    host_->SetParam("liquify.session", sessionIds_[static_cast<size_t>(w)]);
    host_->SetParam("liquify.cx", strokeX_);
    host_->SetParam("liquify.cy", strokeY_);
    host_->SetParam("liquify.dirX", pendingZeroDir_ ? 0.0f : pendingDirX_);
    host_->SetParam("liquify.dirY", pendingZeroDir_ ? 0.0f : pendingDirY_);
    host_->SetParam("liquify.mode", std::string("accumulate"));
    if (strokeBeginPending_) {
        host_->SetParam("liquify.begin", 1);
        strokeBeginPending_ = false;
        const std::string& sid = sessionIds_[static_cast<size_t>(w)];
        if (liquifyStrokeSession_ != sid) {
            liquifyStrokeSession_ = sid;
            liquifyStrokeCount_ = 0;
        }
        ++liquifyStrokeCount_;
    }
    if (liquifyUndoPending_) {
        host_->SetParam("liquify.undo", 1);
        liquifyUndoPending_ = false;
    }

    // 累积调用不记录 Undo（一笔一个快照，由 strokeBeginPending_ 负责）
    host_->SetRecordingUndo(false);
    // 画布尺寸必须与会话原图一致，否则插件判定"换图"会重置置换场；
    // 内容无关紧要，所以复用一张替身画布而不是克隆整幅图。
    ImageBuffer& base = wp->base;
    if (!strokeScratch_.IsValid() || !base.IsValid() ||
        strokeScratch_.Width() != base.Width() ||
        strokeScratch_.Height() != base.Height() ||
        strokeScratch_.Format() != base.Format()) {
        strokeScratch_ = base.IsValid() ? base.Clone() : ImageBuffer();
    }
    if (!strokeScratch_.IsValid()) return;
    host_->SetCanvas(std::move(strokeScratch_), false);
    std::string err;
    if (!manager_->ExecutePlugin(e.pluginId, err)) {
        status_ = "执行失败: " + err;
        Log("liquify accumulate failed: " + err);
    }
    strokeScratch_ = std::move(host_->Canvas()); // 收回替身，供下一个采样点复用
    host_->SetRecordingUndo(true);
}

void BridgeCore::StrokeRender(int winIdx) {
    const int w = Norm(winIdx);
    if (!started_ || !HasImage()) return;
    UiPreview* wp = Win(w);
    if (w != kWinMain && !wp->open) return;
    // "render"：只重渲染当前置换场（不再应用新的一笔）
    UpdatePreview(w, "render");
}

void BridgeCore::StrokeEnd(int winIdx) {
    (void)winIdx;
    strokeActive_ = false;
}

bool BridgeCore::UndoStroke(int winIdx) {
    const int w = Norm(winIdx);
    if (w != kWinLiquify) return false; // 非液化窗口走应用级撤销
    UiPreview* win = Win(w);
    const int eidx = win->effectIndex >= 0 ? win->effectIndex : current_;
    if (eidx < 0 || eidx >= static_cast<int>(effects_.size()) ||
        effects_[static_cast<size_t>(eidx)].group != kGroupLiquify)
        return false;
    if (liquifyStrokeSession_ != sessionIds_[static_cast<size_t>(w)] ||
        liquifyStrokeCount_ <= 0) {
        status_ = "没有可撤销的笔画";
        return true;
    }
    --liquifyStrokeCount_;
    liquifyUndoPending_ = true;
    UpdatePreview(w);
    status_ = "已撤销上一笔";
    Log("liquify stroke undo, remaining=" +
        std::to_string(liquifyStrokeCount_));
    return true;
}

// ---------------------------------------------------------------------------
// 滤镜缩略图
// ---------------------------------------------------------------------------

const ImageBuffer& BridgeCore::ThumbSource() {
    if (!thumbSource_.IsValid() && original_.IsValid())
        thumbSource_ = Downscale(original_, kThumbSourceDim);
    return thumbSource_;
}

ImageBuffer BridgeCore::RenderEffectOn(int effectIndex, const ImageBuffer& src,
                                       float scale) {
    if (!manager_ || !host_ || effectIndex < 0 ||
        effectIndex >= static_cast<int>(effects_.size()))
        return src.Clone();
    const EffectDef& e = effects_[static_cast<size_t>(effectIndex)];

    host_->ClearParams();
    for (const auto& kv : e.fixed) host_->SetParam(kv.first, kv.second);
    for (size_t i = 0; i < e.params.size(); ++i) {
        const UiParam& p = e.params[i];
        float v = p.def;
        if (p.pixelScale) v *= (scale > 0.0f ? scale : 1.0f);
        if (p.isInt)
            host_->SetParam(p.key, static_cast<int>(std::lround(v)));
        else
            host_->SetParam(p.key, v);
    }

    host_->SetRecordingUndo(false);
    host_->SetCanvas(src.Clone(), false);
    std::string err;
    if (!manager_->ExecutePlugin(e.pluginId, err)) return src.Clone();
    host_->SetRecordingUndo(true);
    return std::move(host_->Canvas());
}

const ImageBuffer* BridgeCore::Thumbnail(int effectIndex, int dim) {
    if (effectIndex < 0 || effectIndex >= static_cast<int>(effects_.size()))
        return nullptr;
    thumbDim_ = dim;
    if (thumbDone_[static_cast<size_t>(effectIndex)])
        return &thumbs_[static_cast<size_t>(effectIndex)];

    const ImageBuffer& src = ThumbSource();
    if (!src.IsValid()) return nullptr;

    const float scale = static_cast<float>(src.Width()) /
                        static_cast<float>(std::max(1, original_.Width()));
    thumbs_[static_cast<size_t>(effectIndex)] =
        RenderEffectOn(effectIndex, src, scale);
    thumbDone_[static_cast<size_t>(effectIndex)] = true;
    return &thumbs_[static_cast<size_t>(effectIndex)];
}

ImageBuffer BridgeCore::MakeThumbnail(const ImageBuffer& src, int dim) {
    if (!src.IsValid() || dim <= 0) return ImageBuffer();
    return Downscale(src, dim);
}

// ---------------------------------------------------------------------------
// 反馈
// ---------------------------------------------------------------------------

int BridgeCore::RefreshCount() const {
    return host_ ? host_->RefreshCount() : 0;
}

std::string BridgeCore::PluginSummary() const {
    if (!manager_) return "(未启动)";
    std::string s;
    for (const auto& item : manager_->ListPlugins()) {
        if (!s.empty()) s += ", ";
        s += item.id + " " + item.version;
        s += item.loaded ? " [loaded]" : (item.enabled ? " [idle]" : " [disabled]");
    }
    return s.empty() ? "(无插件)" : s;
}

int BridgeCore::PluginLoadedCount() const {
    if (!manager_) return 0;
    int n = 0;
    for (const auto& item : manager_->ListPlugins())
        if (item.loaded && item.id != "com.sai2.helloworld") ++n; // 示例不计
    return n;
}

int BridgeCore::PluginTotalCount() const {
    if (!manager_) return 0;
    int n = 0;
    for (const auto& item : manager_->ListPlugins())
        if (item.id != "com.sai2.helloworld") ++n; // helloworld 是示例，不计
    return n;
}

void BridgeCore::Log(const std::string& line) {
    logLines_.push_back(line);
    if (logLines_.size() > 300) logLines_.erase(logLines_.begin());
    // 同步落盘（logs/bridge.log），供自动化验证读取。
    // 路径一律走 CreateFileW：logDir_ 是 UTF-8，用 CreateFileA 时
    // 含中文的目录（例如「SAI2插件测试包」）会直接写不进去。
    if (!logDir_.empty()) {
        static std::wstring logPathW;
        if (logPathW.empty()) {
            const std::string full = logDir_ + "\\bridge.log";
            const int n = MultiByteToWideChar(CP_UTF8, 0, full.c_str(),
                                              static_cast<int>(full.size()),
                                              nullptr, 0);
            if (n <= 0) return;
            logPathW.assign(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, full.c_str(),
                                static_cast<int>(full.size()), logPathW.data(),
                                n);
        }
        HANDLE f = CreateFileW(logPathW.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(f, line.c_str(), static_cast<DWORD>(line.size()), &w,
                      nullptr);
            WriteFile(f, "\r\n", 2, &w, nullptr);
            CloseHandle(f);
        }
    }
}

} // namespace bridge
} // namespace sai2
