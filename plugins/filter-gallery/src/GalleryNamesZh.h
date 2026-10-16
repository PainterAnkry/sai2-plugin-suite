#pragma once
// ============================================================================
// GalleryNamesZh.h - 滤镜库的简体中文名（key → 中文名、分类 → 中文）
//
// 规格表（GallerySpecs.h）里的名字是 Photoshop 的英文原名，界面是中文软件，
// 所以在这里给一张对照表。名字采用 Photoshop 官方简体中文译名。
//
// 用法：const char* zh = GalleryNameZh(key);  // 返回 nullptr 表示没有译名
//       （调用方回退到规格表里的英文名，避免出现空白项）
//
// 注意：photocraft 自带的 i18n 表里**没有**滤镜条目（实测 zh-hans.tsv 里
// coloredPencil / oceanRipple / filter.gallery 命中数都是 0），所以这份表
// 是手写的；新增滤镜时记得在这里补一行。
// ============================================================================

#include <cstring>

namespace sai2::fg {

struct GalleryNameZhEntry {
    const char* key;
    const char* name;
};

inline const GalleryNameZhEntry* GalleryNameZhTable(int& count) {
    static const GalleryNameZhEntry kTable[] = {
        // ---- Artistic 艺术效果 ----
        {"coloredPencil", "彩色铅笔"},
        {"cutout", "剪贴画"},
        {"dryBrush", "干画笔"},
        {"filmGrain", "胶片颗粒"},
        {"fresco", "壁画"},
        {"neonGlow", "霓虹灯光"},
        {"paintDaubs", "绘画涂抹"},
        {"paletteKnife", "调色刀"},
        {"plasticWrap", "塑料包装"},
        {"posterEdges", "海报边缘"},
        {"roughPastels", "粗糙蜡笔"},
        {"smudgeStick", "涂抹棒"},
        {"sponge", "海绵"},
        {"underpainting", "底纹效果"},
        {"watercolor", "水彩"},
        // ---- Brush Strokes 画笔描边 ----
        {"accentedEdges", "强化的边缘"},
        {"angledStrokes", "成角的线条"},
        {"crosshatch", "交叉线"},
        {"darkStrokes", "深色线条"},
        {"inkOutlines", "油墨概况"},
        {"spatter", "喷溅"},
        {"sprayedStrokes", "喷色描边"},
        {"sumiE", "烟灰墨"},
        // ---- Distort 扭曲 ----
        {"diffuseGlow", "扩散亮光"},
        {"glass", "玻璃"},
        {"oceanRipple", "海洋波纹"},
        // ---- Sketch 素描 ----
        {"basRelief", "基底凸现"},
        {"chalkCharcoal", "粉笔和炭笔"},
        {"charcoal", "炭笔"},
        {"chrome", "铬黄"},
        {"conteCrayon", "炭精笔"},
        {"graphicPen", "绘图笔"},
        {"halftonePattern", "半调图案"},
        {"notePaper", "便条纸"},
        {"photocopy", "影印"},
        {"plaster", "塑料效果"},
        {"reticulation", "网状"},
        {"stamp", "图章"},
        {"tornEdges", "撕边"},
        {"waterPaper", "水彩纸"},
        // ---- Stylize 风格化 ----
        {"glowingEdges", "照亮边缘"},
        // ---- Texture 纹理 ----
        {"craquelure", "龟裂"},
        {"grain", "颗粒"},
        {"mosaicTiles", "马赛克拼贴"},
        {"patchwork", "拼缀图"},
        {"stainedGlass", "染色玻璃"},
        {"texturizer", "纹理化"},
    };
    count = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    return kTable;
}

// key → 中文名；找不到返回 nullptr
inline const char* GalleryNameZh(const char* key) {
    if (!key) return nullptr;
    int n = 0;
    const GalleryNameZhEntry* t = GalleryNameZhTable(n);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(t[i].key, key) == 0) return t[i].name;
    return nullptr;
}

// 规格表里的英文分类 → 中文分类；找不到返回 nullptr
inline const char* GalleryCategoryZh(const char* category) {
    if (!category) return nullptr;
    if (std::strcmp(category, "Artistic") == 0) return "艺术效果";
    if (std::strcmp(category, "Brush Strokes") == 0) return "画笔描边";
    if (std::strcmp(category, "Distort") == 0) return "扭曲";
    if (std::strcmp(category, "Sketch") == 0) return "素描";
    if (std::strcmp(category, "Stylize") == 0) return "风格化";
    if (std::strcmp(category, "Texture") == 0) return "纹理";
    return nullptr;
}

// 便捷：中文名 + 中文分类（形如 "海洋波纹 · 扭曲"）；无译名时回退英文
inline std::string GalleryDisplayNameZh(const char* key, const char* enName,
                                        const char* category) {
    const char* nm = GalleryNameZh(key);
    const char* ct = GalleryCategoryZh(category);
    std::string out = nm ? nm : (enName ? enName : key);
    if (ct) {
        out += " · ";
        out += ct;
    } else if (category) {
        out += " · ";
        out += category;
    }
    return out;
}

} // namespace sai2::fg
