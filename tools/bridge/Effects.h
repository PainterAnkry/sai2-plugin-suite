#pragma once
// ============================================================================
// Effects.h - 面板效果清单与参数模型（伴随宿主）
//
// 每个条目描述"面板上的一项效果"：属于哪个插件、下发哪些参数键，
// 以及每个滑块的范围/默认值。UI 与自检模式共用这一份模型，保证
// "面板上能点的"与"自检覆盖的"完全一致。
//
// 参数键约定（插件侧通过 IHostContext::GetParam 读取，见 ParamHelpers.h）：
//   blur.kind / blur.radius / blur.angle / blur.distance / blur.amount
//   filter.id / filter.param.<name>
//   liquify.tool / liquify.cx / liquify.cy / liquify.radius
//   liquify.strength / liquify.dirX / liquify.dirY / liquify.angle
//   liquify.session   —— 会话标识；变化即代表"换了一张图"，插件须重置置换场
// ============================================================================

#include <string>
#include <utility>
#include <vector>

namespace sai2::bridge {

struct UiParam {
    std::string key;      // 宿主参数键
    std::string label;    // 显示名
    std::string unit;     // 单位后缀（可为空）
    float min = 0.0f;
    float max = 1.0f;
    float def = 0.0f;
    bool isInt = false;
    bool pixelScale = false; // 预览降采样时按比例缩放（像素半径类参数）
};

struct EffectDef {
    std::string id;       // 稳定标识（自检/日志用）
    std::string group;    // 分组：模糊 / 滤镜 / 液化
    std::string name;     // 显示名
    std::string pluginId; // 目标插件
    std::vector<UiParam> params;
    std::vector<std::pair<std::string, std::string>> fixed; // 固定参数
    // 液化必须全分辨率预览：置换场与像素尺度绑定，且插件跨笔刷累积状态
    bool fullResPreview = false;
    // 支持在预览上拖动施力（液化类）
    bool stroke = false;
};

// 构建全部效果定义（顺序即面板显示顺序）
inline std::vector<EffectDef> BuildEffects() {
    std::vector<EffectDef> v;

    // ---------------- 模糊（com.sai2.blur） ----------------
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
        e.params.push_back({"blur.radius", "半径", "px", 0.5f, 40.0f, 4.0f, false, true});
        v.push_back(e);
    }
    {
        EffectDef e = blur("blur.box", "方框模糊", "box");
        e.params.push_back({"blur.radius", "半径", "px", 0.5f, 40.0f, 4.0f, false, true});
        v.push_back(e);
    }
    {
        EffectDef e = blur("blur.motion", "动感模糊", "motion");
        e.params.push_back({"blur.distance", "距离", "px", 1.0f, 80.0f, 16.0f, false, true});
        e.params.push_back({"blur.angle", "角度", "°", 0.0f, 360.0f, 0.0f, true, false});
        v.push_back(e);
    }
    {
        EffectDef e = blur("blur.radial", "径向模糊", "radial");
        e.params.push_back({"blur.amount", "强度", "", 0.05f, 1.0f, 0.3f, false, false});
        v.push_back(e);
    }

    // ---------------- 滤镜（com.sai2.filter-gallery） ----------------
    struct FP { const char* name; const char* label; float min; float max; float def; bool isInt; bool px; };
    struct FE { const char* id; const char* name; const char* fid; std::vector<FP> ps; };

    const std::vector<FE> filters = {
        {"filter.oil-paint", "油画", "com.sai2.filter.oil-paint",
         {{"radius", "笔刷半径", 1, 8, 3, true, true}, {"levels", "强度级数", 2, 64, 12, true, false}}},
        {"filter.watercolor", "水彩", "com.sai2.filter.watercolor",
         {{"radius", "柔化半径", 1, 10, 4, true, true}, {"saturation", "饱和度", 0, 2, 0.35f, false, false}, {"edgeDarken", "边缘沉淀", 0, 1, 0.45f, false, false}}},
        {"filter.rough-pastel", "粗糙蜡笔", "com.sai2.filter.rough-pastel",
         {{"cellSize", "块面大小", 2, 32, 6, true, true}, {"levels", "色阶数", 2, 16, 6, true, false}, {"texture", "纹理强度", 0, 1, 0.25f, false, false}}},
        {"filter.pencil-sketch", "素描", "com.sai2.filter.pencil-sketch",
         {{"radius", "描边半径", 2, 20, 8, true, true}}},
        {"filter.charcoal", "炭笔", "com.sai2.filter.charcoal",
         {{"radius", "描边半径", 2, 20, 6, true, true}, {"grain", "炭粒强度", 0, 1, 0.18f, false, false}}},
        {"filter.chalk", "粉笔", "com.sai2.filter.chalk",
         {{"strength", "力度", 0.2f, 2, 1.0f, false, false}}},
        {"filter.texturizer", "纹理化", "com.sai2.filter.texturizer",
         {{"scale", "纹理缩放", 2, 64, 8, true, true}, {"relief", "浮雕度", 0, 1, 0.5f, false, false}}},
        {"filter.grain", "颗粒", "com.sai2.filter.grain",
         {{"amount", "颗粒量", 0, 1, 0.2f, false, false}, {"monochrome", "单色", 0, 1, 1, true, false}}},
        {"filter.emboss", "浮雕", "com.sai2.filter.emboss",
         {{"strength", "强度", 0, 3, 1.0f, false, false}}},
        {"filter.find-edges", "查找边缘", "com.sai2.filter.find-edges",
         {{"invert", "反相", 0, 1, 0, true, false}}},
    };

    for (const FE& f : filters) {
        EffectDef e;
        e.id = f.id;
        e.group = "滤镜";
        e.name = f.name;
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
        e.params.push_back({"adjust.brightness", "亮度", "", -100.0f, 100.0f, 0.0f, false, false});
        e.params.push_back({"adjust.contrast", "对比度", "", -100.0f, 100.0f, 0.0f, false, false});
        v.push_back(e);
    }
    {
        EffectDef e;
        e.id = "adjust.hue-saturation";
        e.group = "调整";
        e.name = "色相/饱和度";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "hue-saturation"});
        e.params.push_back({"adjust.hue", "色相", "°", -180.0f, 180.0f, 0.0f, false, false});
        e.params.push_back({"adjust.saturation", "饱和度", "", -100.0f, 100.0f, 0.0f, false, false});
        e.params.push_back({"adjust.lightness", "明度", "", -100.0f, 100.0f, 0.0f, false, false});
        v.push_back(e);
    }
    {
        EffectDef e;
        e.id = "adjust.levels";
        e.group = "调整";
        e.name = "色阶";
        e.pluginId = "com.sai2.adjust";
        e.fixed.push_back({"adjust.kind", "levels"});
        e.params.push_back({"adjust.inBlack", "输入黑场", "", 0.0f, 254.0f, 0.0f, true, false});
        e.params.push_back({"adjust.inWhite", "输入白场", "", 1.0f, 255.0f, 255.0f, true, false});
        e.params.push_back({"adjust.gamma", "Gamma", "", 0.1f, 3.0f, 1.0f, false, false});
        v.push_back(e);
    }

    // ---------------- 液化（com.sai2.liquify） ----------------
    // 全分辨率预览 + 预览上拖动施力；笔刷位置由拖动决定
    auto liq = [&](const char* id, const char* name, const char* tool) {
        EffectDef e;
        e.id = id;
        e.group = "液化";
        e.name = name;
        e.pluginId = "com.sai2.liquify";
        e.fixed.push_back({"liquify.tool", tool});
        e.fullResPreview = true;
        e.stroke = true;
        e.params.push_back({"liquify.radius", "笔刷大小", "", 0.03f, 0.60f, 0.15f, false, false});
        return e;
    };

    {
        EffectDef e = liq("liquify.forward-warp", "向前变形", "forward-warp");
        e.params.push_back({"liquify.strength", "强度", "", 0.1f, 3.0f, 1.0f, false, false});
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.bloat", "膨胀", "bloat");
        e.params.push_back({"liquify.strength", "强度", "", 0.1f, 3.0f, 1.0f, false, false});
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.pinch", "收缩", "pinch");
        e.params.push_back({"liquify.strength", "强度", "", 0.1f, 3.0f, 1.0f, false, false});
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.twirl-cw", "顺时针旋转", "twirl-cw");
        e.params.push_back({"liquify.angle", "角度", "°", 5.0f, 180.0f, 45.0f, true, false});
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.twirl-ccw", "逆时针旋转", "twirl-ccw");
        e.params.push_back({"liquify.angle", "角度", "°", 5.0f, 180.0f, 45.0f, true, false});
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.smooth", "平滑", "smooth");
        v.push_back(e);
    }
    {
        EffectDef e = liq("liquify.reconstruct", "重建", "reconstruct");
        e.params.push_back({"liquify.strength", "强度", "", 0.1f, 1.0f, 0.5f, false, false});
        v.push_back(e);
    }

    return v;
}

} // namespace sai2::bridge
