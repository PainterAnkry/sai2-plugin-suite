#pragma once
// ============================================================================
// GalleryParamNamesZh.h - 滤镜参数的简体中文名（参数键 → 中文）
//
// 规格表里的参数键是 Photoshop 的英文（graininess / glowAmount …），界面是
// 中文软件，所以给一张对照表。查不到时调用方回退英文键名（不会出现空白）。
// 名称采用 Photoshop 官方简体中文。新增滤镜参数时在这里补一行。
// ============================================================================

#include <cstring>

namespace sai2::fg {

struct GalleryParamNameZhEntry { const char* key; const char* name; };

inline const GalleryParamNameZhEntry* GalleryParamNameZhTable(int& count) {
    static const GalleryParamNameZhEntry kT[] = {
        // 通用/尺寸
        {"brushSize", "画笔大小"}, {"strokeSize", "描边大小"}, {"brushDetail", "画笔细节"},
        {"edgeWidth", "边缘宽度"}, {"strokeWidth", "描边宽度"}, {"strokeLength", "描边长度"},
        {"pencilWidth", "铅笔宽度"}, {"charcoalThickness", "炭笔粗细"}, {"tileSize", "拼贴大小"},
        {"squareSize", "方块大小"}, {"cellSize", "单元格大小"}, {"size", "大小"},
        {"fiberLength", "纤维长度"}, {"detail", "细节"}, {"edgeFidelity", "边缘精确度"},
        // 强度/数量
        {"strokePressure", "描边压力"}, {"intensity", "强度"}, {"strength", "强度"},
        {"sharpness", "锐化程度"}, {"softness", "柔和度"}, {"smoothness", "平滑度"},
        {"definition", "细节保留"}, {"relief", "浮雕"}, {"texture", "纹理"},
        {"textureCoverage", "纹理覆盖"}, {"scaling", "缩放"}, {"grain", "颗粒"},
        {"graininess", "颗粒"}, {"grainType", "颗粒类型"}, {"contrast", "对比度"},
        {"brightness", "亮度"}, {"density", "密度"}, {"blackIntensity", "黑色强度"},
        {"whiteIntensity", "白色强度"}, {"darkIntensity", "暗部强度"},
        {"lightIntensity", "光照强度"}, {"edgeIntensity", "边缘强度"},
        {"shadowIntensity", "阴影强度"}, {"glowBrightness", "发光亮度"},
        {"glowAmount", "发光量"}, {"glowSize", "发光大小"}, {"glowColor", "发光颜色"},
        {"clearAmount", "清除量"}, {"highlightStrength", "高光强度"},
        {"highlightArea", "高光区域"}, {"posterization", "色调分离"},
        {"numberOfLevels", "色阶数"}, {"edgeSimplicity", "边缘简化度"},
        {"imageBalance", "图像平衡"}, {"lightDarkBalance", "明暗平衡"},
        {"balance", "平衡"}, {"foregroundLevel", "前景色阶"}, {"backgroundLevel", "背景色阶"},
        {"paperBrightness", "纸张亮度"}, {"directionBalance", "方向平衡"},
        {"sprayRadius", "喷溅半径"}, {"distortion", "扭曲度"}, {"edgeThickness", "边缘厚度"},
        {"crackSpacing", "裂缝间距"}, {"crackDepth", "裂缝深度"}, {"crackBrightness", "裂缝亮度"},
        {"groutWidth", "缝隙宽度"}, {"lightenGrout", "缝隙亮度"}, {"borderThickness", "边框宽度"},
        {"rippleSize", "波纹大小"}, {"rippleMagnitude", "波纹幅度"},
        {"charcoalArea", "炭笔区域"}, {"chalkArea", "粉笔区域"},
        {"tileSize_x", "拼贴大小"}, {"invert", "反相"},
        {"darkness", "暗度"}, {"edgeBrightness", "边缘亮度"},
        {"strokeDetail", "描边细节"},
        // 枚举
        {"texture", "纹理"}, {"brushType", "画笔类型"}, {"light", "光照方向"},
        {"strokeDirection", "描边方向"}, {"patternType", "图案类型"},
        {"foreground", "前景色"}, {"background", "背景色"},
    };
    count = static_cast<int>(sizeof(kT) / sizeof(kT[0]));
    return kT;
}

inline const char* GalleryParamNameZh(const char* key) {
    if (!key) return nullptr;
    int n = 0;
    const GalleryParamNameZhEntry* t = GalleryParamNameZhTable(n);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(t[i].key, key) == 0) return t[i].name;
    return nullptr;
}

} // namespace sai2::fg