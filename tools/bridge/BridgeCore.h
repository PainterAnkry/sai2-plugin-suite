#pragma once
// ============================================================================
// BridgeCore.h - 伴随宿主的无界面核心
//
// 把"抓取 → 跑插件 → 回写剪贴板"整条链路从 Win32 界面里剥离出来，
// 于是同一套逻辑既能被面板/功能窗口驱动，也能被自检模式（--selftest）驱动。
//
// 数据模型：
//   original_  本次从 SAI2 抓到的原图（全分辨率，画质基准）
//   applied_   当前"已应用"的结果（全分辨率，就是会被粘贴回 SAI2 的内容）
//   撤销 = applied_ 的历史栈；复位 = 回到 original_
//
// 【多窗口会话】每个功能窗口（主面板 / 模糊 / 滤镜 / 调整 / 液化）各持有一份
// 独立预览会话（UiPreview）：独立的显示图、显示倍率与预览基准。
//   * 模糊 / 滤镜 / 调整窗口：预览跑在降采样图上（像素半径类参数按倍率缩放），
//     拖动滑块保持流畅；提交时才按全分辨率跑一遍。
//   * 液化窗口：预览跑在"像素预算内"的图上，但置换场一律在**全分辨率坐标**
//     下累积（插件按归一化坐标 × 图像尺寸求笔刷几何），因此预览里的形变与
//     提交结果一致；液化也**从不重放笔画**，只重复一次渲染。
// ============================================================================

#include "Effects.h"
#include "sai2/ImageBuffer.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sai2 {

namespace host { class PluginManager; }

namespace bridge {

class BridgeHostContext;

class BridgeCore {
public:
    // ---- 分组（功能窗口按此划分） ---------------------------------------
    static constexpr const char* kGroupBlur = "模糊";
    static constexpr const char* kGroupFilter = "滤镜";
    static constexpr const char* kGroupAdjust = "调整";
    static constexpr const char* kGroupLiquify = "液化";
    static constexpr const char* kLiquifyGroup = kGroupLiquify;
    static constexpr const char* kGroupNames[4] = {kGroupBlur, kGroupFilter,
                                                   kGroupAdjust, kGroupLiquify};

    // 功能窗口索引：0 主面板，1..4 分别对应上面 4 个分组
    static constexpr int kWinMain = 0;
    static constexpr int kWinBlur = 1;
    static constexpr int kWinFilter = 2;
    static constexpr int kWinAdjust = 3;
    static constexpr int kWinLiquify = 4;
    static constexpr int kWinCount = 5;

    static constexpr int kMainPreviewMaxDim = 900;   // 主面板预览降采样最长边
    static constexpr int kLiquifyPreviewPixels = 4 * 1024 * 1024; // 液化预览预算
    static constexpr int kThumbSourceDim = 192;      // 滤镜缩略图源图最长边

    BridgeCore(std::string pluginsDir, std::string logDir);
    ~BridgeCore();

    // 启动插件管理器（扫描 registry.json 并加载已启用插件）
    bool Startup(std::string& error);
    void Shutdown();

    // ---- 效果与参数 -----------------------------------------------------
    const std::vector<EffectDef>& Effects() const { return effects_; }
    bool EffectAvailable(int index) const;
    // 按分组取该功能窗口里的效果下标（保持定义顺序）
    std::vector<int> EffectsInGroup(const char* group) const;
    // 稳定 id 反查（窗口记住上次选的效果；-1 = 没找到）
    int FindEffectById(const std::string& id) const;

    int CurrentEffect() const { return current_; }
    // 载入该效果的默认参数并刷新该窗口的预览
    void SetCurrentEffect(int index, int winIdx = kWinMain);

    // 参数是"按效果持久保存"的：每个效果有自己的滑块值，切换效果/窗口不会
    // 互相覆盖（否则多窗口同时开着时，A 窗口会显示 B 窗口的参数）。
    // winIdx 缺省 = 主面板。
    std::vector<float> ParamValues(int winIdx = kWinMain) const;
    void SetParamValue(size_t index, float value, int winIdx = kWinMain);
    // 参数变更后刷新该窗口预览（不触发全分辨率渲染）
    void ParamChanged(int winIdx);

    // ---- 图像与状态 -----------------------------------------------------
    bool HasImage() const { return original_.IsValid(); }
    const ImageBuffer& OriginalImage() const { return original_; }
    const ImageBuffer& AppliedImage() const { return applied_; }
    // 当前 applied_ 补边到画布尺寸后的结果（无画布上下文时就是 applied_ 本身）。
    // 多图层一起液化时每层都要自己摆位，测/诊断也要拿到补边图，故公开。
    ImageBuffer PaddedApplied() const {
        if (baseIsCanvas_) return applied_.Clone();   // 已补边：补边即恒等
        return hasCanvasCtx_ ? PadToCanvas(applied_) : applied_.Clone();
    }
    // 窗口当前应当绘制的图像（预览结果或已应用结果）
    const ImageBuffer& DisplayImage(int winIdx) const;

    // ---- 多窗口预览会话 -------------------------------------------------
    bool HasWindow(int winIdx) const;
    // 打开功能窗口：建立该窗口的预览会话与液化会话
    void OpenWindow(int winIdx);
    void CloseWindow(int winIdx);
    // 关闭全部功能窗口
    void CloseAllToolWindows();

    // 窗口显示倍率（显示像素 / 图像像素；主面板为 1 时不缩放）
    float RenderScale(int winIdx) const;
    // 该窗口的效果是否"跨笔累积引擎状态"（液化：不能按倍率重放）
    bool WindowSnapshotsEngineState(int winIdx) const;
    // 该窗口的显示图（全分辨率基准）
    ImageBuffer WindowBase(int winIdx) const;
    // 该窗口当前是否显示的是全分辨率结果（提交时可直接复用）
    bool DisplayIsFullRes(int winIdx) const;

    // ---- 主流程 ---------------------------------------------------------
    // 接收一张新抓取的图（剪贴板或自检注入）
    void SetImage(ImageBuffer img, const std::string& sourceLabel);
    bool GrabFromClipboard(std::string& error);

    // 重算预览（滑块变化 / 换效果 / 笔画后调用）；winIdx 缺省 = 主面板
    // liqMode 非空时透传给液化插件（"render" = 只重渲染置换场，不再应用新一笔，
    // 供涂抹过程中的显示帧使用）。
    void UpdatePreview(int winIdx = kWinMain, const char* liqMode = nullptr);

    // 按给定倍率跑一遍当前效果（scale >= 1 = 全分辨率）。
    // explicitBase 非空时用它当基准（提交路径必须显式传基准，避免用到
    // 某个还没建立会话的窗口基准）。
    ImageBuffer RenderEffect(int winIdx, float scale, bool recordUndo = false,
                             const ImageBuffer* explicitBase = nullptr,
                             const char* liqMode = nullptr);

    // 全分辨率应用当前效果并写回剪贴板
    struct ApplyResult {
        bool ok = false;
        bool clipboardWritten = false;
        std::string error;           // 失败原因（空 = 成功）
        std::string clipboardFormat; // 成功时写入的剪贴板格式
    };
    ApplyResult ApplyToClipboard();

    // 撤销上一次"应用"（并同步剪贴板）
    bool Undo(std::string& error);
    // 复位到抓取到的原图（并同步剪贴板）
    bool ResetToOriginal(std::string& error);

    // 把当前 applied_ 再写一次剪贴板（不重新计算）
    bool WriteClipboard(std::string& error);

    // ---- 画布上下文（精确回贴） -----------------------------------------
    // 整层抓取时由抓取流程设置：SAI2 画布尺寸与抓取内容在画布内的偏移。
    // 设置后，写剪贴板的都是"补边到画布尺寸"的图 —— SAI2 的 Ctrl+V 按
    // 画布 (0,0) 对齐，补边后内容正好落回原位置，不再依赖 Ctrl+Shift+V。
    void SetCanvasContext(bool has, int canvasW, int canvasH, int offX,
                          int offY);
    // 把 original_/applied_ 补边成**整幅画布**（已知画布上下文时），
    // 维持"图 = 画布坐标"的不变量 —— 所有功能窗口因此以画布为编辑面，
    // 用户才能把内容液化/模糊到图层包围盒之外（SAI2 的 Ctrl+C 只给包围盒）。
    void PadBaseToCanvas();
    bool HasCanvasContext() const { return hasCanvasCtx_; }
    int CanvasW() const { return canvasW_; }
    int CanvasH() const { return canvasH_; }
    // 基准层（抓取时那层）内容在画布内的偏移。多图层一起液化时，
    // 其余层要用 "本层偏移 − 基准层偏移" 作为置换场的采样偏移。
    // 基准图是否**已经是画布坐标**（已补边成画布尺寸）。
    // 补边**不清零** canvasX0_/canvasY0_ —— 那两个值描述"被抓图层在画布上的
    // 位置"，下一次抓图还要用它；清零过一次的后果是下一层被按 (0,0) 摆进画布，
    // 内容整体偏移（实测：偏移 351,84 的层被摆到 0,0）。
    bool IsBaseCanvas() const { return baseIsCanvas_; }
    int CanvasX0() const { return baseIsCanvas_ ? 0 : canvasX0_; }
    int CanvasY0() const { return baseIsCanvas_ ? 0 : canvasY0_; }

    // 把任意一张"已经是画布尺寸"的图写进剪贴板（多图层逐层回贴用：
    // 每层结果由 RenderLiquifyLayerToCanvas 补边好，这里是最后一步）。
    bool WriteImageToClipboard(const ImageBuffer& img, std::string& error);
    // 把当前 applied_ 补边成画布尺寸后写剪贴板 —— 这就是产品"精确回贴"的
    // 写入路径。公开出来是为了让 `--pad-clipboard` 诊断复跑**同一段**代码，
    // 而不是另写一份（否则测的就不是产品路径了）。
    bool WriteAppliedToClipboard(std::string& error);

    // ---- 多图层一起液化（方案 B）------------------------------------------
    // 用「液化会话里已经建立的同一个置换场」把**另一层**渲染出来，并补边到
    // 画布尺寸（供写剪贴板 → Ctrl+V → 向下合并回该层）。
    //   layerSrc  该层刚抓到的图（1:1 文档像素）
    //   sourceId  该层的源图身份；变化只会让插件重新缓存源图，**置换场保留**
    //   cw,ch     画布尺寸（所有层相同）
    //   offX,offY 该层内容在画布内的偏移
    //   shiftX/Y  该层偏移 − 基准层偏移（置换场采样偏移；基准层为 0）
    // 失败返回无效图。所有层因此被**同一套形变**处理，而分层结构不丢。
    ImageBuffer RenderLiquifyLayerToCanvas(const ImageBuffer& layerSrc,
                                           const std::string& sourceId,
                                           int cw, int ch, int offX, int offY,
                                           int shiftX, int shiftY);

    // ---- 液化笔画 -------------------------------------------------------
    // 按"上一采样点 → 本次点"提交一笔（笔刷中心 + 拖动方向）。
    // 注意：StrokeMove 只做"廉价累积"（把这一笔加进置换场，不做全图逆映射渲染），
    // 所以可以每个鼠标采样点都调；要看到画面得另外调 StrokeRender（按显示帧率）。
    // 默认窗口 = 液化窗口；自检/无界面调用可直接省略 winIdx。
    void StrokeBegin(float nx, float ny, int winIdx = kWinLiquify);
    void StrokeMove(float nx, float ny, int winIdx = kWinLiquify);
    // 只渲染当前置换场（不再应用新的一笔）→ 生成这一显示帧的画面
    void StrokeRender(int winIdx = kWinLiquify);
    void StrokeEnd(int winIdx = kWinLiquify);
    // 撤销液化窗口里的上一笔（插件侧置换场快照回退）。
    // 仅对液化窗口生效（返回 false = 非液化窗口，调用方可回退到 Undo）。
    bool UndoStroke(int winIdx = kWinLiquify);
    bool StrokeActive() const { return strokeActive_; }
    // 供界面绘制笔刷指示圈（归一化坐标 + 半径比例）
    float StrokeX() const { return strokeX_; }
    float StrokeY() const { return strokeY_; }
    float StrokeRadiusNorm() const {
        return paramValues_.empty() ? 0.15f : paramValues_[0];
    }
    // 当前效果是否是液化类（画笔涂抹）
    bool CurrentEffectStrokes() const;

    // ---- 滤镜缩略图 -----------------------------------------------------
    // 缩略图源图（最长边 kThumbSourceDim）
    const ImageBuffer& ThumbSource();
    // 缩略图（按需生成并缓存；dim 为缩略图边长）
    const ImageBuffer* Thumbnail(int effectIndex, int dim);
    // 把插件跑在一张给定的小图上（缩略图生成用，不改变窗口会话）
    ImageBuffer RenderEffectOn(int effectIndex, const ImageBuffer& src,
                               float scale);
    static ImageBuffer MakeThumbnail(const ImageBuffer& src, int dim);

    // ---- 选项 -----------------------------------------------------------
    void SetChain(bool on);   // 叠加处理：基于上一次结果继续处理
    bool Chain() const { return chain_; }

    // ---- 反馈 -----------------------------------------------------------
    const std::string& Status() const { return status_; }
    void SetStatus(std::string s) { status_ = std::move(s); }
    const std::vector<std::string>& LogLines() const { return logLines_; }
    int RefreshCount() const;
    double LastPreviewMs() const { return lastPreviewMs_; }
    double LastApplyMs() const { return lastApplyMs_; }
    // "com.sai2.blur 1.1.0 [loaded], ..." 形式的一行摘要
    std::string PluginSummary() const;
    // 主面板状态栏用：已加载/应加载 的插件数量（应加载 = registry 扫到的全部）
    int PluginLoadedCount() const;
    int PluginTotalCount() const;

    void Log(const std::string& line);

private:
    struct UiPreview;
    int Norm(int winIdx) const {
        return (winIdx >= 0 && winIdx < kWinCount) ? winIdx : kWinMain;
    }
    // 注意：Win() 只在 BridgeCore.cpp 里定义 —— 那里 UiPreview 才是完整类型；
    // 头文件里若 inline 定义，vector<UiPreview>::operator[] 会要求完整类型。
    UiPreview* Win(int winIdx);
    const UiPreview* Win(int winIdx) const;

    // 载入某效果的参数到"当前活动参数"（并把引擎当前效果切过去）；
    // 首次见到的效果用它的默认值初始化
    void LoadParams(int effectIndex);
    // 保证 paramValues_ 就是该窗口选中效果的参数（需要时从 effectParams_ 取）
    void EnsureParamsForWindow(int winIdx) const;
    // 液化：只把当前这一笔累加进插件置换场（不渲染，开销极小）
    void StrokeAccumulate(int winIdx);
    // 预览基准（全分辨率，叠加模式下 = 当前已应用结果）
    ImageBuffer PreviewBase(int winIdx);
    void RebuildPreviewSource(int winIdx);
    void PushAppliedHistory();
    static ImageBuffer Downscale(const ImageBuffer& src, int maxDim);

    std::string pluginsDir_;
    std::string logDir_;

    std::unique_ptr<BridgeHostContext> host_;
    std::unique_ptr<sai2::host::PluginManager> manager_;

    std::vector<EffectDef> effects_;
    std::vector<bool> available_;
    mutable int current_ = 0;  // 引擎当前效果（最后被碰过的那个；const 接口
                               // 也会切换它，见 EnsureParamsForWindow）
    int mainEffect_ = 0;    // 主面板选中的效果
    // 活动效果的参数（scratch）与"按效果持久保存"的参数表。
    // 标 mutable：ParamValues(winIdx) 是 const 接口，但内部需要把 scratch 对齐
    // 到"该窗口选中的效果"（见 EnsureParamsForWindow）。
    mutable std::vector<float> paramValues_;
    mutable std::map<std::string, std::vector<float>> effectParams_;

    ImageBuffer original_;
    ImageBuffer applied_;
    std::vector<ImageBuffer> history_;

    std::vector<UiPreview> wins_;

    // 滤镜缩略图
    ImageBuffer thumbSource_;
    std::vector<ImageBuffer> thumbs_;
    std::vector<bool> thumbDone_;
    int thumbDim_ = 88;

    bool chain_ = false;
    bool started_ = false;

    // 液化会话：每个窗口一个标识（变化即代表"换了一张图/复位"）
    std::vector<std::string> sessionIds_;    int sessionCounter_ = 0;
    bool strokeActive_ = false;
    int strokeWin_ = kWinLiquify;
    float strokeX_ = 0.5f, strokeY_ = 0.5f;
    float strokeDX_ = 0.0f, strokeDY_ = 0.0f;
    float pendingDirX_ = 0.0f, pendingDirY_ = 0.0f;
    bool pendingZeroDir_ = true;
    // 笔级撤销：下一帧液化渲染时告知插件「本笔开始（先快照）/ 回退一笔」
    bool strokeBeginPending_ = false;
    bool liquifyUndoPending_ = false;
    int liquifyStrokeCount_ = 0;          // 当前会话内已开始的笔画数
    std::string liquifyStrokeSession_;    // 笔画计数所属的会话
    // 累积调用用的"替身画布"：液化插件要求画布尺寸与会话原图一致（否则判定换图
    // 会重置置换场），而累积又不需要真实像素 —— 复用这一张，避免每个采样点
    // 克隆 8MP 大图（35MB 拷贝）。
    ImageBuffer strokeScratch_;


    // 把 img 摆到画布坐标 (canvasX0_, canvasY0_) 处、补边成画布尺寸
    ImageBuffer PadToCanvas(const ImageBuffer& img) const;

    // 画布上下文（0 = 无；抓取自外部剪贴板/匹配失败时无效）
    bool hasCanvasCtx_ = false;
    bool baseIsCanvas_ = false;
    int canvasW_ = 0, canvasH_ = 0, canvasX0_ = 0, canvasY0_ = 0;

    std::string status_;
    std::vector<std::string> logLines_;
    double lastPreviewMs_ = 0.0;
    double lastApplyMs_ = 0.0;
};

} // namespace bridge

} // namespace sai2
