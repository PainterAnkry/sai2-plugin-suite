#pragma once
#include <deque>
// 滤镜库规格表 + 参数记法解析器（插件侧 header-only）：界面的滤镜清单直接
// 来自插件真正支持的集合，避免维护第二份手写清单（曾经因此看不到新滤镜）。
#include "../../plugins/filter-gallery/src/GalleryParams.h"
#include "../../plugins/filter-gallery/src/GalleryImplemented.h"
#include "../../plugins/filter-gallery/src/GalleryParamNamesZh.h"
#include "../../plugins/filter-gallery/src/GalleryNamesZh.h"
#include "../../plugins/filter-gallery/src/GallerySpecs.h"
// ============================================================================
// Effects.h - 面板效果清单与参数模型（伴随宿主）
//
// 每个条目描述"面板上的一项效果"：属于哪个插件、下发哪些参数键，
// 以及每个滑块的种类/范围/默认值。UI 与自检模式共用这一份模型，保证
// "面板上能点的"与"自检覆盖的"完全一致。
//
// 参数键约定（插件侧通过 IHostContext::GetParam 读取，见 ParamHelpers.h）：
//   blur.kind / blur.radius / blur.angle / blur.distance / blur.amount /
//   blur.samples / blur.threshold / blur.method / blur.quality / blur.mode
//   filter.id / filter.param.<name>
//   adjust.kind / adjust.*
//   liquify.tool / liquify.cx / liquify.cy / liquify.radius
//   liquify.strength / liquify.dirX / liquify.dirY / liquify.angle
//   liquify.session   —— 会话标识；变化即代表"换了一张图"，插件须重置置换场
// ============================================================================

#include <string>
#include <utility>
#include <vector>

namespace sai2::bridge {

// 参数控件种类（决定 DrawParamRow 画什么、点击怎么改）
enum class ParamKind {
    Slider, // 标签 + 数值框 + 滑杆（默认）
    Choice, // 下拉框（choices 里的序号，存 float）
    Toggle  // 开关（0 / 1）
};

struct UiParam {
    std::string key;      // 宿主参数键
    std::string label;    // 显示名
    std::string unit;     // 单位后缀（可为空）
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.0f;
    bool isInt = false;
    bool pixelScale = false; // 预览降采样时按比例缩放（像素半径类参数）
    // 显示倍率（只影响"数值框里显示什么"）：液化笔刷大小内部是 0..1 的
    // 归一化半径，用户看到 0.15 毫无意义，PS 那边显示的是百分比/像素。
    // 传 100 后框里就是 15，输入 30 也按 0.30 存回去。
    float displayScale = 1.0f;
    // 下拉框选项（UTF-8，顺序即取值 0..n-1）。非空 = Choice 控件。
    // 以前把选项塞进标签文本（"纹理(0=canvas 1=brick …)"），既看不懂也点不准；
    // 现在解析器的 choices 直接变成下拉项。
    std::vector<std::string> choices;
    // 开关控件（0/1）。PS 的"反相/单色"这类二值选项用它。
    bool isBool = false;

    ParamKind Kind() const {
        if (!choices.empty()) return ParamKind::Choice;
        if (isBool) return ParamKind::Toggle;
        return ParamKind::Slider;
    }
};

struct EffectDef {
    std::string id;       // 稳定标识（自检/日志用）
    std::string group;    // 分组：模糊 / 滤镜 / 调整 / 液化
    std::string name;     // 显示名
    std::string pluginId; // 目标插件
    // 滤镜库的分类（艺术效果 / 画笔描边 / 扭曲 / 素描 / 风格化 / 纹理）；
    // 其它分组留空。滤镜窗口的左栏按它分组成可折叠的分类。
    std::string category;
    std::vector<UiParam> params;
    std::vector<std::pair<std::string, std::string>> fixed; // 固定参数
    // 液化必须全分辨率预览：置换场与像素尺度绑定，且插件跨笔刷累积状态
    bool fullResPreview = false;
    // 支持在预览上拖动施力（液化类）
    bool stroke = false;
    // 参数面板交给窗口自己画（曲线这类需要专用控件的效果）
    bool customPanel = false;
};

// 滤镜分类的中文名（规格表里是 Photoshop 的英文分类名）
inline std::string GalleryCategoryZh(const std::string& en) {
    if (en == "Artistic") return "艺术效果";
    if (en == "Brush Strokes") return "画笔描边";
    if (en == "Distort") return "扭曲";
    if (en == "Sketch") return "素描";
    if (en == "Stylize") return "风格化";
    if (en == "Texture") return "纹理";
    if (en == "Blur") return "模糊";
    if (en == "Video") return "视频";
    if (en == "Pixelate") return "像素化";
    if (en == "Noise") return "杂色";
    return en.empty() ? std::string("其它") : en;
}

// 构建全部效果定义（顺序即面板显示顺序）
inline std::vector<EffectDef> BuildEffects() {
    std::vector<EffectDef> v;

    // ---------------- 模糊（com.sai2.blur） ----------------
    // 对照 Photoshop「模糊」滤镜组的 8 个子项组织（形状模糊/镜头模糊的
    // 光圈外观类参数在本实现里用"高光"近似，不做虚假的镜头模拟）。
    auto blur = [&](const char* id, const char* name, const char* kind) {
        EffectDef e;
        e.id = id;
        e.group = "模糊";
        e.name = name;
        e.pluginId = "com.sai2.blur";
        e.fixed.push_back({"blur.kind", kind});
        return e;
    };

    {
        EffectDef e = blur("blur.gaussian", "高斯模糊", "gaussian");
        e.params.push_back({"blur.radius", "半径", "px", 0.1f, 40.0f, 4.0f, false, true});
        v.push_back(e);
    }
    {
        EffectDef e = blur("blur.box", "方框模糊", "box");
        e.params.push_back({"blur.radius", "半径", "px", 1.0f, 60.0f, 8.0f, false, true});
        v.push_back(e);
    }
    {
        EffectDef e = blur("blur.motion", "动感模糊", "motion");
        e.params.push_back({"blur.angle", "角度", "°", 0.0f, 360.0f, 0.0f, true, false});
        e.params.push_back({"blur.distance", "距离", "px", 1.0f, 200.0f, 20.0f, true, true});
        v.push_back(e);
    }
    {
        // 径向模糊：数量 / 模糊方法（旋转、缩放）/ 品质（草图、好、最好）
        EffectDef e = blur("blur.radial", "径向模糊", "radial");
        UiParam amount;
        amount.key = "blur.amount";
        amount.label = "数量";
        amount.min = 0.01f;
        amount.max = 1.0f;
        amount.def = 0.1f;
        amount.displayScale = 100.0f; // 内部 0.01..1 → 界面 1..100
        e.params.push_back(amount);
        UiParam method;
        method.key = "blur.method";
        method.label = "模糊方法";
        method.min = 0.0f;
        method.max = 1.0f;
        method.isInt = true;
        method.choices = {"旋转", "缩放"};
        e.params.push_back(method);
        UiParam quality;
        quality.key = "blur.quality";
        quality.label = "品质";
        quality.min = 0.0f;
        quality.max = 2.0f;
        quality.isInt = true;
        quality.choices = {"草图", "好", "最好"};
        quality.def = 1.0f;
        e.params.push_back(quality);
        v.push_back(e);
    }
    {
        // 表面模糊：半径 + 阈值（只模糊"相近颜色"，边缘保留）
        EffectDef e = blur("blur.surface", "表面模糊", "surface");
        e.params.push_back({"blur.radius", "半径", "px", 1.0f, 40.0f, 5.0f, true, true});
        e.params.push_back({"blur.threshold", "阈值", "", 0.0f, 255.0f, 15.0f, true, false});
        v.push_back(e);
    }
    {
        // 特殊模糊：半径 / 阈值 / 品质 / 模式（正常、仅边缘、叠加边缘）
        EffectDef e = blur("blur.smart", "特殊模糊", "smart");
        e.params.push_back({"blur.radius", "半径", "px", 1.0f, 40.0f, 5.0f, true, true});
        e.params.push_back({"blur.threshold", "阈值", "", 0.0f, 100.0f, 25.0f, true, false});
        UiParam quality;
        quality.key = "blur.quality";
        quality.label = "品质";
        quality.min = 0.0f;
        quality.max = 2.0f;
        quality.isInt = true;
        quality.choices = {"低", "中", "高"};
        quality.def = 1.0f;
        e.params.push_back(quality);
        UiParam mode;
        mode.key = "blur.mode";
        mode.label = "模式";
        mode.min = 0.0f;
        mode.max = 2.0f;
        mode.isInt = true;
        mode.choices = {"正常", "仅边缘", "叠加边缘"};
        e.params.push_back(mode);
        v.push_back(e);
    }
    {
        // 镜头模糊：半径 + 高光（高光阈值越高，越少像素被"光圈"放大成光斑）
        EffectDef e = blur("blur.lens", "镜头模糊", "lens");
        e.params.push_back({"blur.radius", "半径", "px", 1.0f, 40.0f, 12.0f, true, true});
        e.params.push_back({"blur.highlight", "高光", "", 0.0f, 255.0f, 200.0f, true, false});
        v.push_back(e);
    }
    {
        // 平均：整层填充平均色（PS 里也在这一组里，没有参数）
        EffectDef e = blur("blur.average", "平均", "average");
        v.push_back(e);
    }

    // ---------------- 滤镜（com.sai2.filter-gallery） ----------------
    struct FP { const char* name; const char* label; float min; float max; float def; bool isInt; bool px; };
    struct FE { const char* id; const char* name; const char* cat; const char* fid; std::vector<FP> ps; };

    // 早期手写的滤镜（FilterSet.h）。与滤镜库重名的（水彩/炭笔/纹理化/颗粒/
    // 粗糙蜡笔）已经删掉 —— 同一个分类里出现两个同名项只会让人以为是 bug；
    // 这 5 个滤镜库里没有对应实现，保留并各自归入 PS 的分类。
    std::vector<FE> filters = {
        {"filter.oil-paint", "油画", "艺术效果", "com.sai2.filter.oil-paint",
         {{"radius", "笔刷半径", 1, 8, 3, true, true}, {"levels", "强度级数", 2, 64, 12, true, false}}},
        {"filter.pencil-sketch", "铅笔素描", "素描", "com.sai2.filter.pencil-sketch",
         {{"radius", "描边半径", 2, 20, 8, true, true}}},
        {"filter.chalk", "粉笔", "素描", "com.sai2.filter.chalk",
         {{"strength", "力度", 0.2f, 2, 1.0f, false, false}}},
        {"filter.emboss", "浮雕效果", "风格化", "com.sai2.filter.emboss",
         {{"strength", "强度", 0, 3, 1.0f, false, false}}},
        {"filter.find-edges", "查找边缘", "风格化", "com.sai2.filter.find-edges",
         {{"invert", "反相", 0, 1, 0, true, false}}},
    };

    // ---- 从插件侧规格表自动加入 Photoshop 滤镜库（47 个）------------------
    // 踩坑：这张表原来是手写的，插件里新注册的滤镜界面里根本看不到（移植了
    // 18 个，界面里还是老 10 个）。现在读插件侧的规格表 + 参数记法解析器
    // （都是 header-only），保证插件能执行的，界面一定列得出来。
    // 文本用 deque 存：FE/FP 只保存 const char*，deque 地址稳定不会失效。
    {
        static std::deque<std::string> strs;
        int specCount = 0;
        const sai2::fg::GallerySpec* specs = sai2::fg::GallerySpecs(specCount);
        for (int i = 0; i < specCount; ++i) {
            strs.push_back(std::string("filter.gallery.") + specs[i].key);
            const char* eid = strs.back().c_str();
            // 中文名（Photoshop 官方译名）+ 中文分类；没有译名时回退英文名，
            // 保证列表里不会出现空白项。
            {
                std::string nm = sai2::fg::GalleryDisplayNameZh(
                    specs[i].key, specs[i].name, specs[i].category);
                // 尚未移植的算法：名字上明确标出来。它的缩略图会是黑的（滤镜
                // 没实现、应用直接失败），标注后就不会被误当成坏掉了。
                if (!sai2::fg::GalleryKeyImplemented(specs[i].key))
                    nm += "（未实现）";
                strs.push_back(nm);
            }
            const char* ename = strs.back().c_str();
            strs.push_back(std::string("com.sai2.filter.gallery.") + specs[i].key);
            const char* efid = strs.back().c_str();
            strs.push_back(GalleryCategoryZh(specs[i].category));
            const char* ecat = strs.back().c_str();
            FE e{eid, ename, ecat, efid, {}};
            const std::vector<sai2::fg::GalleryParamSpec> ps =
                sai2::fg::ParseGalleryParams(specs[i].params);
            for (const auto& p : ps) {
                strs.push_back(p.key);
                const char* pname = strs.back().c_str();
                // 参数名优先用中文（查不到回退英文键名）
                const char* pz = sai2::fg::GalleryParamNameZh(p.key.c_str());
                std::string lab = pz ? pz : p.key;
                strs.push_back(lab);
                const char* plabel = strs.back().c_str();
                float lo = p.min, hi = p.max, de = p.def;
                bool isInt = p.isInt;
                if (!p.choices.empty()) {
                    lo = 0.0f;
                    hi = static_cast<float>(p.choices.size() - 1);
                    de = static_cast<float>(p.choiceDefault);
                    isInt = true;
                } else if (p.kind == sai2::fg::GalleryParamSpec::Bool) {
                    lo = 0.0f; hi = 1.0f; de = p.boolDefault ? 1.0f : 0.0f; isInt = true;
                }
                const bool px = (lab.find("Size") != std::string::npos ||
                                 lab.find("Width") != std::string::npos ||
                                 lab.find("Length") != std::string::npos ||
                                 lab.find("Radius") != std::string::npos ||
                                 lab.find("Spacing") != std::string::npos);
                e.ps.push_back(FP{pname, plabel, lo, hi, de, isInt, px});
            }
            filters.push_back(std::move(e));
        }
    }

    for (const FE& f : filters) {
        EffectDef e;
        e.id = f.id;
        e.group = "滤镜";
        e.name = f.name;
        e.category = f.cat;
        e.pluginId = "com.sai2.filter-gallery";
        e.fixed.push_back({"filter.id", f.fid});
        for (const FP& p : f.ps) {
            UiParam u;
            u.key = std::string("filter.param.") + p.name;
            u.label = p.label;
            u.min = p.min;
            u.max = p.max;
            u.def = p.def;
            u.isInt = p.isInt;
            u.pixelScale = p.px;
            e.params.push_back(u);
        }
        v.push_back(e);
    }

    // ---- 规格表里的 choices / bool 补成真正的控件 --------------------------
    // （上面那一轮只搬了范围；选项名当时被拼进标签文本，这里改成下拉/开关。
    //   按 id 回查规格表比在 FE 里多塞一份 choices 更省事，也不会走样。）
    {
        int specCount = 0;
        const sai2::fg::GallerySpec* specs = sai2::fg::GallerySpecs(specCount);
        for (auto& e : v) {
            if (e.group != "滤镜" || e.id.rfind("filter.gallery.", 0) != 0) continue;
            const std::string key = e.id.substr(std::string("filter.gallery.").size());
            const sai2::fg::GallerySpec* sp = nullptr;
            for (int i = 0; i < specCount; ++i)
                if (key == specs[i].key) { sp = &specs[i]; break; }
            if (!sp) continue;
            const auto ps = sai2::fg::ParseGalleryParams(sp->params);
            for (size_t i = 0; i < ps.size() && i < e.params.size(); ++i) {
                if (!ps[i].choices.empty()) e.params[i].choices = ps[i].choices;
                else if (ps[i].kind == sai2::fg::GalleryParamSpec::Bool)
                    e.params[i].isBool = true;
            }
        }
    }

    // ---------------- 调整（com.sai2.adjust，PS 风格） ----------------
    {
        EffectDef e;
        e.id = "adjust.invert";
        e.group = "调整";
        e.name = "反相";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "invert"});
        v.push_back(e);
    }
    {
        EffectDef e;
        e.id = "adjust.grayscale";
        e.group = "调整";
        e.name = "去色";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "grayscale"});
        v.push_back(e);
    }
    {
        EffectDef e;
        e.id = "adjust.brightness-contrast";
        e.group = "调整";
        e.name = "亮度/对比度";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "brightness-contrast"});
        e.params.push_back({"adjust.brightness", "亮度", "", -100.0f, 100.0f, 0.0f, true, false});
        e.params.push_back({"adjust.contrast", "对比度", "", -100.0f, 100.0f, 0.0f, true, false});
        v.push_back(e);
    }
    {
        EffectDef e;
        e.id = "adjust.hue-saturation";
        e.group = "调整";
        e.name = "色相/饱和度";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "hue-saturation"});
        // 三档都是整数（PS 的色相是度数、饱和度/明度是百分数）：整数参数
        // 才显示成 "-180" 而不是 "-180.00"，拖动步长也才是 1 而不是 2。
        e.params.push_back({"adjust.hue", "色相", "°", -180.0f, 180.0f, 0.0f, true, false});
        e.params.push_back({"adjust.saturation", "饱和度", "", -100.0f, 100.0f, 0.0f, true, false});
        e.params.push_back({"adjust.lightness", "明度", "", -100.0f, 100.0f, 0.0f, true, false});
        v.push_back(e);
    }
    {
        // 色阶的边栏换成 PS「曲线」面板（直方图 + 可拖控制点 + 通道）。
        // 参数全在自定义面板里（customPanel），曲线本身以字符串参数下发：
        //   adjust.curve.rgb / .r / .g / .b = "x:y,x:y,…"（0..255 的输入/输出）
        EffectDef e;
        e.id = "adjust.curves";
        e.group = "调整";
        e.name = "曲线";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "curves"});
        e.customPanel = true;
        UiParam channel;
        channel.key = "adjust.channel";
        channel.label = "通道";
        channel.min = 0.0f;
        channel.max = 3.0f;
        channel.isInt = true;
        channel.choices = {"RGB", "红", "绿", "蓝"};
        e.params.push_back(channel);
        // 保留色阶的三件套（黑场/白场/Gamma）作为"输入色阶"子区域，
        // 免得用户还得切回旧面板 —— PS 的曲线里也是这三个三角块。
        e.params.push_back({"adjust.inBlack", "输入黑场", "", 0.0f, 254.0f, 0.0f, true, false});
        e.params.push_back({"adjust.inWhite", "输入白场", "", 1.0f, 255.0f, 255.0f, true, false});
        e.params.push_back({"adjust.gamma", "中间调", "", 0.1f, 3.0f, 1.0f, false, false});
        v.push_back(e);
    }

    // ---------------- 液化（com.sai2.liquify） ----------------
    // 全分辨率预览 + 预览上拖动施力；笔刷位置由拖动决定。
    // 顺序照 Photoshop 的液化工具栏：向前变形 / 重建 / 平滑 / 顺时针 /
    // 逆时针 / 褶皱 / 膨胀 / 左推（后两个是"冻结/解冻蒙版"之前的工具）。
    auto liq = [&](const char* id, const char* name, const char* tool) {
        EffectDef e;
        e.id = id;
        e.group = "液化";
        e.name = name;
        e.pluginId = "com.sai2.liquify";
        e.fixed.push_back({"liquify.tool", tool});
        e.fullResPreview = true;
        e.stroke = true;
        // 笔刷大小内部是"占图像最短边的比例"（0.03~0.60），界面按 PS 习惯
        // 显示成 3~60 的百分数，用户输入 30 也按 0.30 存回去。
        UiParam brush;
        brush.key = "liquify.radius";
        brush.label = "画笔大小";
        brush.min = 0.03f;
        brush.max = 0.60f;
        brush.def = 0.15f;
        brush.displayScale = 100.0f;
        e.params.push_back(brush);
        return e;
    };

    // 压力：内部 0.1~3.0，界面同样按百分数显示（PS 的"画笔压力"就是 0~100）
    auto liqStrength = [](EffectDef& e, float def) {
        UiParam st;
        st.key = "liquify.strength";
        st.label = "画笔压力";
        st.min = 0.1f;
        st.max = 3.0f;
        st.def = def;
        st.displayScale = 100.0f;
        e.params.push_back(st);
    };

    {
        EffectDef e = liq("liquify.forward-warp", "向前变形", "forward-warp");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.reconstruct", "重建", "reconstruct");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.smooth", "平滑", "smooth");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.twirl-cw", "顺时针扭曲", "twirl-cw");
        e.params.push_back({"liquify.angle", "角度", "°", 5.0f, 180.0f, 45.0f, true, false});
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.twirl-ccw", "逆时针扭曲", "twirl-ccw");
        e.params.push_back({"liquify.angle", "角度", "°", 5.0f, 180.0f, 45.0f, true, false});
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.pinch", "褶皱", "pinch");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.bloat", "膨胀", "bloat");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.push", "左推", "push");
        liqStrength(e, 1.0f);
        v.push_back(e);
    }

    return v;
}

} // namespace sai2::bridge
