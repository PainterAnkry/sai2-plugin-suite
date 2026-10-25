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

#include "BmpIO.h"
#include "BridgeHostContext.h"
#include "ClipboardImage.h"
#include "sai2/host/PluginManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace sai2 {
namespace bridge {

// 双线性放大（定义在 Downscale 旁边；ParamChanged 的拖动帧要用到，
// 所以这里先声明一下）
ImageBuffer UpsampleLinear(const ImageBuffer& src, int dw, int dh);

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 单调毫秒（绝对值无所谓，只用来算间隔）
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// "按住不动持续施力"的两个手感常数。
//   间隔 40ms ≈ 25 次/秒：比显示帧略慢，肉眼是连续的，又不会把渲染队列灌满。
//   力度 0.06：插件里一次 bloat 的位移量是 strength*0.25，所以每秒累计
//     25 × 0.25 × 0.06 ≈ 0.375 —— 按住 1 秒能看到明显的吹胀，但不会瞬间炸开；
//     这个量级和 PS 的「按住不放」接近。
//   （1.28 用的是 55ms + 0.08，但那条路根本没被触发过，见 BridgeCore.h 注释。）
const int kHoldIntervalMs = 40;
const float kHoldFactor = 0.06f;

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

void BridgeCore::ParamChanged(int winIdx, bool quick) {
    const int w = Norm(winIdx);
    UiPreview* wp = Win(w);
    if (w != kWinMain && !wp->open) return;
    // 滑块类参数不改变液化置换场，因此这里重跑一次渲染即可（不重放笔画）
    const auto t0 = Clock::now();
    bool done = false;
    if (quick && wp->src.IsValid()) {
        // 拖动中的显示帧：半分辨率跑一遍，再双线性拉回原尺寸。
        // 实测滤镜库 900px 从 57ms 降到 ~15ms 渲染 + ~5ms 拉回；
        // 松手（WM_LBUTTONUP）会用 quick=false 再跑一帧真正的结果。
        ImageBuffer small =
            RenderEffect(w, wp->scale, false, nullptr, nullptr, 2);
        if (small.IsValid()) {
            if (small.Width() != wp->src.Width() ||
                small.Height() != wp->src.Height()) {
                wp->display =
                    UpsampleLinear(small, wp->src.Width(), wp->src.Height());
            } else {
                wp->display = std::move(small);
            }
            done = wp->display.IsValid();
        }
    }
    if (!done) wp->display = RenderEffect(w, wp->scale, false);
    wp->ms = MsSince(t0);
    wp->commitScale = wp->scale;
    wp->showApplied = false;
    lastPreviewMs_ = wp->ms;
    // 性能取证开关：设了 SAI2_BRIDGE_PROFILE，每次预览都往 logs/bridge.log
    // 记一行（quick = 拖动中的半分辨率帧，full = 松手/离散操作的全帧）。
    // 界面上的"预览 N ms"会被窄窗口省略号吃掉，跑回归时读日志更可靠。
    if (std::getenv("SAI2_BRIDGE_PROFILE")) {
        Log(std::string("preview w") + std::to_string(w) +
            (quick ? " quick " : " full ") + std::to_string(wp->ms) + " ms");
    }
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

    // 新图进来：先当作"未补边的原始包围盒"，交给下面重新判定/补边。
    // 同时**作废过期的画布上下文** —— 偏移是针对某一块包围盒算出来的，
    // 换了一张尺寸不同的图（用户复制了选区、换了图层、从外部剪贴板抓图）
    // 就不能再拿它补边，否则内容会被摆到错误位置（用户看到的"位置不对"）。
    //
    // ★ 判定必须用 original_（移动后的新图），**不能用 img**：img 已被
    //   std::move 搬空，IsValid() 为 false，于是 CanvasCtxFits 恒返回 false →
    //   每次 SetImage 都把刚定位好的上下文作废。实测后果：抓取流程第 4 步刚
    //   SetCanvasContext(true, 1080,1640, …)，紧接着 SetImage 就把它丢了 →
    //   面板永远显示"⚠ 无画布尺寸"、图永远不补边（用户 21:26 那次就是这个，
    //   日志里 "canvas ctx dropped: image 760x611 != ctx source 760x611"
    //   两个尺寸一模一样却还是被丢弃）。
    baseIsCanvas_ = false;
    if (hasCanvasCtx_ && !CanvasCtxFits(original_)) {
        Log("canvas ctx dropped: image " + std::to_string(original_.Width()) +
            "x" + std::to_string(original_.Height()) + " != ctx source " +
            std::to_string(ctxSrcW_) + "x" + std::to_string(ctxSrcH_) +
            " (需重新定位)");
        SetCanvasContext(false, 0, 0, 0, 0);
    }

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
    // 诊断开关：设了 SAI2_BRIDGE_DUMP_PREVIEW 就把窗口实际显示的那张预览落盘。
    // "窗口里显示不全 / 位置不对"这类问题只有看到这张图才能判断是补边错了
    // 还是窗口拿到了旧图（界面上两者长得差不多）。
    {
        char dumpDir[512] = {0};
        if (GetEnvironmentVariableA("SAI2_BRIDGE_DUMP_PREVIEW", dumpDir, 512) > 0) {
            char path[MAX_PATH] = {0};
            std::snprintf(path, sizeof(path), "%s\\preview_w%d.bmp", dumpDir, w);
            std::string werr;
            WriteBmp32(wp->src, path, werr);
            char bp[MAX_PATH] = {0};
            std::snprintf(bp, sizeof(bp), "%s\\base_w%d.bmp", dumpDir, w);
            WriteBmp32(base, bp, werr);
            Log(std::string("preview dumped: ") + path + " " +
                std::to_string(wp->src.Width()) + "x" +
                std::to_string(wp->src.Height()) + " base=" +
                std::to_string(base.Width()) + "x" +
                std::to_string(base.Height()));
        }
    }
}

// 双线性放大（拖动期"低分辨率预览"回贴用）。
// 为什么必须拉回原尺寸：画布的适配倍率、缩放读数、平移边界全都是从
// **显示图尺寸**算出来的（CanvasViewOf）。如果直接拿半尺寸的预览去画，
// 适配显示会变成 200%（读数错），手动缩放过还会画成一半大小。
// 拉回原尺寸后，几何与读数完全不变，只有画面在拖动过程中略软一点，
// 松手立刻补一帧全分辨率（见 ParamChanged 的 quick 参数）。
ImageBuffer UpsampleLinear(const ImageBuffer& src, int dw, int dh) {
    if (!src.IsValid() || dw <= 0 || dh <= 0) return ImageBuffer();
    const int sw = src.Width(), sh = src.Height();
    if (sw <= 0 || sh <= 0) return ImageBuffer();
    if (sw == dw && sh == dh) return src.Clone();

    ImageBuffer out;
    out.Create(dw, dh, src.Format());
    const uint32_t bpc = src.BytesPerChannel();
    const uint8_t* s = src.Data();
    uint8_t* d = out.Data();
    const size_t ss = src.Stride(), ds = out.Stride();

    // 预计算每一列的源下标与权重（0..256），行同理
    std::vector<int> x0(static_cast<size_t>(dw)), x1(static_cast<size_t>(dw));
    std::vector<int> wx(static_cast<size_t>(dw));
    for (int x = 0; x < dw; ++x) {
        const double fx = (x + 0.5) * sw / dw - 0.5;
        int i0 = static_cast<int>(std::floor(fx));
        double t = fx - i0;
        if (i0 < 0) { i0 = 0; t = 0.0; }
        if (i0 >= sw - 1) { i0 = std::max(0, sw - 1); t = 0.0; }
        x0[static_cast<size_t>(x)] = i0;
        x1[static_cast<size_t>(x)] = std::min(sw - 1, i0 + 1);
        wx[static_cast<size_t>(x)] = static_cast<int>(t * 256.0 + 0.5);
    }

    const auto rd = [bpc](const uint8_t* p) -> int {
        // 16 位通道按小端取（与 ImageBuffer 的存储一致）
        return bpc == 2 ? static_cast<int>(p[0] | (p[1] << 8))
                        : static_cast<int>(p[0]);
    };
    const auto wr = [bpc](uint8_t* p, int v) {
        if (bpc == 2) {
            p[0] = static_cast<uint8_t>(v & 0xff);
            p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
        } else {
            p[0] = static_cast<uint8_t>(v);
        }
    };
    const size_t px = static_cast<size_t>(4) * bpc;

    for (int y = 0; y < dh; ++y) {
        const double fy = (y + 0.5) * sh / dh - 0.5;
        int j0 = static_cast<int>(std::floor(fy));
        double ty = fy - j0;
        if (j0 < 0) { j0 = 0; ty = 0.0; }
        if (j0 >= sh - 1) { j0 = std::max(0, sh - 1); ty = 0.0; }
        const int j1 = std::min(sh - 1, j0 + 1);
        const int wy = static_cast<int>(ty * 256.0 + 0.5);
        const uint8_t* r0 = s + static_cast<size_t>(j0) * ss;
        const uint8_t* r1 = s + static_cast<size_t>(j1) * ss;
        uint8_t* dr = d + static_cast<size_t>(y) * ds;
        for (int x = 0; x < dw; ++x) {
            const int wxx = wx[static_cast<size_t>(x)];
            const uint8_t* pl0 = r0 + static_cast<size_t>(x0[static_cast<size_t>(x)]) * px;
            const uint8_t* pr0 = r0 + static_cast<size_t>(x1[static_cast<size_t>(x)]) * px;
            const uint8_t* pl1 = r1 + static_cast<size_t>(x0[static_cast<size_t>(x)]) * px;
            const uint8_t* pr1 = r1 + static_cast<size_t>(x1[static_cast<size_t>(x)]) * px;
            uint8_t* dp = dr + static_cast<size_t>(x) * px;
            for (int c = 0; c < 4; ++c) {
                const size_t co = static_cast<size_t>(c) * bpc;
                const int a = rd(pl0 + co), b = rd(pr0 + co);
                const int cc = rd(pl1 + co), dd = rd(pr1 + co);
                const int t = (a * (256 - wxx) + b * wxx + 128) >> 8;
                const int u = (cc * (256 - wxx) + dd * wxx + 128) >> 8;
                wr(dp + co, (t * (256 - wy) + u * wy + 128) >> 8);
            }
        }
    }
    return out;
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
    const uint32_t bpc = src.BytesPerChannel();
    // ★ 采样盒必须按**浮点比例**算，不能 w/nw 整除。
    //   原来写的是 bx = max(1, w/nw)：1080x1640 缩到 592x900 时
    //   bx=by=1（整数除法），于是"降采样"退化成**取左上角 592x900 的裁剪**——
    //   预览只显示画布左上角那一块，窗口里看着就是"图像显示不全 / 内容被切掉"，
    //   而图像本身、补边、回贴都没问题（用户反馈的正是这个现象）。
    //   液化预览（按像素预算算 limit）同样中招。
    for (int y = 0; y < nh; ++y) {
        const int sy0 = static_cast<int>(static_cast<long long>(y) * h / nh);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(
            static_cast<long long>(y + 1) * h / nh));
        for (int x = 0; x < nw; ++x) {
            const int sx0 = static_cast<int>(static_cast<long long>(x) * w / nw);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(
                static_cast<long long>(x + 1) * w / nw));
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = sy0; sy < std::min(h, sy1); ++sy) {
                for (int sx = sx0; sx < std::min(w, sx1); ++sx) {
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
                                     const char* liqMode, int quickDiv) {
    const int wi = Norm(winIdx);
    UiPreview* w = Win(wi);
    const bool isTool = (wi != kWinMain);
    // 以谁为准：
    //   * 窗口开着 → 以窗口选中的效果为准（用户看到的那个）；
    //   * 窗口没开 → 必须用引擎的 current_。窗口没开时 w->effectIndex 还停在
    //     窗口上次的取值（或组内第一个），照它渲染就是"点了方框模糊却出了
    //     高斯模糊"——自检里 8 种模糊输出逐字节相同、47 个滤镜库滤镜一模一样，
    //     根因就在这（e.fixed 里的 blur.kind / filter.id 一直是错的）。
    const int eidx = (isTool && w->open && w->effectIndex >= 0) ? w->effectIndex
                                                               : current_;
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
        // quickDiv > 1：拖动中的显示帧再降一档分辨率（像素量按平方下降）。
        // 这里必须走 w->src 的副本而不能动 w->src 本身，否则松手后
        // 全分辨率那一帧也会跟着变糊。
        if (quickDiv > 1) {
            const int longest = std::max(w->src.Width(), w->src.Height());
            base = Downscale(w->src, std::max(1, longest / quickDiv));
        } else {
            base = w->src.Clone();
        }
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
    for (const auto& kv : extraParams_) host_->SetParam(kv.first, kv.second);
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
    // 按字节预算淘汰（见 kHistoryBudgetBytes 的注释）。以前只按张数（20）限制，
    // 大画布上 20 张全尺寸快照能吃到 GB 级内存。
    const auto bytesOf = [](const ImageBuffer& img) -> size_t {
        return img.IsValid() ? img.Stride() * static_cast<size_t>(img.Height())
                             : 0;
    };
    size_t total = 0;
    for (const auto& h : history_) total += bytesOf(h);
    size_t dropped = 0;
    while (history_.size() > kHistoryMinDepth && total > kHistoryBudgetBytes) {
        total -= bytesOf(history_.front());
        history_.erase(history_.begin());
        ++dropped;
    }
    while (history_.size() > kHistoryMaxDepth) {
        total -= bytesOf(history_.front());
        history_.erase(history_.begin());
        ++dropped;
    }
    if (dropped) {
        Log("history trimmed: kept " + std::to_string(history_.size()) +
            " / " + std::to_string(total / (1024 * 1024)) + " MB, dropped " +
            std::to_string(dropped));
    }
}

void BridgeCore::SetCanvasContext(bool has, int canvasW, int canvasH,
                                  int offX, int offY, int srcW, int srcH) {
    hasCanvasCtx_ = has && canvasW > 0 && canvasH > 0;
    canvasW_ = canvasW;
    canvasH_ = canvasH;
    canvasX0_ = offX;
    canvasY0_ = offY;
    ctxSrcW_ = hasCanvasCtx_ ? srcW : 0;
    ctxSrcH_ = hasCanvasCtx_ ? srcH : 0;
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
    // 定位结果与上次补边用的偏移不同（同一图层重新定位、或用户换了层）：
    // 把**已经补好的画布图整体平移**纠正 —— 比"作废上下文重抓"好得多：
    // 后者会让只走「监听剪贴板」抓图的用户永远拿不到整幅画布显示。
    if (hasCanvasCtx_ && baseIsCanvas_ && original_.IsValid() &&
        original_.Width() == canvasW_ && original_.Height() == canvasH_ &&
        (canvasX0_ != paddedAtX_ || canvasY0_ != paddedAtY_)) {
        ShiftBaseBy(canvasX0_ - paddedAtX_, canvasY0_ - paddedAtY_);
        paddedAtX_ = canvasX0_;
        paddedAtY_ = canvasY0_;
    }
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
        baseIsCanvas_ = true;   // 本身已是画布尺寸：按画布坐标对待
        paddedAtX_ = canvasX0_;
        paddedAtY_ = canvasY0_;
        return;
    }
    const int ox = canvasX0_, oy = canvasY0_;
    original_ = PadImageAt(original_, canvasW_, canvasH_, ox, oy);
    if (applied_.IsValid())
        applied_ = PadImageAt(applied_, canvasW_, canvasH_, ox, oy);
    // ★ 不清零 canvasX0_/canvasY0_：它们描述"被抓图层在画布上的位置"，
    //   下一次抓图（下一个 900x506 的包围盒）还要用它来补边。
    baseIsCanvas_ = true;
    paddedAtX_ = ox;
    paddedAtY_ = oy;
    Log("base padded to canvas " + std::to_string(canvasW_) + "x" +
        std::to_string(canvasH_) + " (was at " + std::to_string(ox) + "," +
        std::to_string(oy) + ")");
}

void BridgeCore::ShiftBaseBy(int dx, int dy) {
    if ((dx == 0 && dy == 0) || !original_.IsValid()) return;
    const int W = original_.Width(), H = original_.Height();
    auto shiftOne = [&](const ImageBuffer& src) {
        ImageBuffer dst(W, H, src.Format());
        dst.Clear(RGBA{0, 0, 0, 0});
        for (int y = 0; y < H; ++y) {
            const int sy = y - dy;
            if (sy < 0 || sy >= H) continue;
            int sx = 0, sdx = 0, cnt = W;
            if (dx > 0) { sdx = dx; cnt = W - dx; }
            else if (dx < 0) { sx = -dx; cnt = W + dx; }
            if (cnt <= 0) continue;
            std::memcpy(dst.Data() + dst.Stride() * static_cast<size_t>(y) +
                            static_cast<size_t>(sdx) * 4,
                        src.Data() + src.Stride() * static_cast<size_t>(sy) +
                            static_cast<size_t>(sx) * 4,
                        static_cast<size_t>(cnt) * 4);
        }
        return dst;
    };
    original_ = shiftOne(original_);
    if (applied_.IsValid() && applied_.Width() == W && applied_.Height() == H)
        applied_ = shiftOne(applied_);
    Log("base shifted by (" + std::to_string(dx) + "," + std::to_string(dy) +
        ") to match canvas ctx");
}

ImageBuffer BridgeCore::PadToCanvas(const ImageBuffer& img) const {
    if (baseIsCanvas_) return img.Clone();   // 已是画布坐标：补边即恒等
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
    for (const auto& kv : extraParams_) host_->SetParam(kv.first, kv.second);
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
    // 单击也算一笔（膨胀/收缩/旋转不需要拖动方向）：渲染首帧。
    // 首帧这一笔是全强度的（和 PS 按下即有一笔一致），记下时刻，
    // 让紧随其后的 StrokeHold 从"下一次间隔"开始计。
    UpdatePreview(w);
    lastDabMs_ = NowMs();
}

bool BridgeCore::StrokeMove(float nx, float ny, int winIdx) {
    if (!strokeActive_ || !HasImage()) return false;
    const int w = Norm(winIdx);
    if (w != strokeWin_) return false;

    const float px = strokeX_, py = strokeY_;
    strokeX_ = std::min(1.0f, std::max(0.0f, nx));
    strokeY_ = std::min(1.0f, std::max(0.0f, ny));

    strokeDX_ = (strokeX_ - px) * original_.Width();
    strokeDY_ = (strokeY_ - py) * original_.Height();

    const float len = std::sqrt(strokeDX_ * strokeDX_ + strokeDY_ * strokeDY_);
    const int eidx = Win(w)->effectIndex >= 0 ? Win(w)->effectIndex : current_;
    if (eidx < 0 || eidx >= static_cast<int>(effects_.size())) return false;
    const EffectDef& e = effects_[static_cast<size_t>(eidx)];
    const float radius = paramValues_.empty() ? 0.15f : paramValues_[0];
    const float minLen = std::max(
        1.0f, radius * std::min(original_.Width(), original_.Height()) * 0.02f);
    const std::string tool =
        e.fixed.empty() ? std::string("forward-warp") : e.fixed[0].second;
    // 需要"拖动方向"的工具：向前变形 / 左推（左推用方向的垂线）
    const bool dirTool = (tool == "forward-warp" || tool == "push");
    if (dirTool && len < minLen) return false;
    if (!dirTool && len < 0.5f) return false; // 单击类工具：位移交给 StrokeHold

    if (dirTool) {
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
    lastDabMs_ = NowMs();
    Win(w)->showApplied = false;
    return true;
}

bool BridgeCore::StrokeHold(int winIdx) {
    if (!strokeActive_ || !HasImage()) return false;
    const int w = Norm(winIdx);
    if (w != strokeWin_) return false;

    // 时间节流：鼠标移动（125Hz+）和定时器补点都会调到这里，
    // 只有"距上次真正落笔 >= kHoldIntervalMs"才施力 —— 否则两条路径
    // 叠加会把膨胀瞬间吹炸；同时也保证施力速率与事件频率无关。
    const long long now = NowMs();
    if (now - lastDabMs_ < kHoldIntervalMs) return false;

    const int eidx =
        (w != kWinMain && Win(w)->effectIndex >= 0) ? Win(w)->effectIndex
                                                    : current_;
    if (eidx < 0 || eidx >= static_cast<int>(effects_.size())) return false;
    const EffectDef& e = effects_[static_cast<size_t>(eidx)];
    if (e.group != kGroupLiquify) return false;
    const std::string tool =
        e.fixed.empty() ? std::string("forward-warp") : e.fixed[0].second;
    // 向前变形 / 左推只认位移方向：静止按住不该有任何变化（PS 同样如此）。
    if (tool == "forward-warp" || tool == "push") return false;

    // 笔刷中心不变、方向为 0 —— 正是 StrokeMove 里"步进过小直接 return"
    // 的那条路，所以这里不能复用 StrokeMove，必须直接累积。
    pendingDirX_ = 0.0f;
    pendingDirY_ = 0.0f;
    pendingZeroDir_ = true;
    holdFactor_ = kHoldFactor;
    StrokeAccumulate(w);
    holdFactor_ = 1.0f;
    lastDabMs_ = now;
    Win(w)->showApplied = false;
    return true;
}

void BridgeCore::StrokeAccumulate(int winIdx) {    const int w = Norm(winIdx);
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
    for (const auto& kv : extraParams_) host_->SetParam(kv.first, kv.second);
    for (size_t i = 0; i < e.params.size() && i < paramValues_.size(); ++i) {
        const UiParam& p = e.params[i];
        float v = paramValues_[i];
        // 按住不动的持续施力：只削"每次叠加的力度/角度"，不改用户设定值
        if (holdFactor_ != 1.0f &&
            (p.key == "liquify.strength" || p.key == "liquify.angle"))
            v *= holdFactor_;
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
    for (const auto& kv : extraParams_) host_->SetParam(kv.first, kv.second);
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
