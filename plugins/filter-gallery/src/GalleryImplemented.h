#pragma once
// ============================================================================
// GalleryImplemented.h - "哪些滤镜算法已经实现"的唯一清单
//
// 单独一个头（只依赖 <cstring>）是为了让**桥**也能包含它：桥的界面需要给
// 未移植的滤镜标上"（未实现）"——它们的缩略图必然是黑的（算法没实现、应用
// 直接失败），标注后就不会被误当成坏掉。
//
// ★ 这份清单必须与 GalleryFilters.h 里 GalleryApplyAll 的分派保持同步。
//   tests/gallery_specs 会用 Apply() 逐个验证同一批 key（改了一边没改另一边
//   就会被测试抓到）。
// ============================================================================

#include <cstring>

namespace sai2::fg {

inline bool GalleryKeyImplemented(const char* key) {
    static const char* kDone[] = {
        // Distort（3）
        "oceanRipple", "glass", "diffuseGlow",
        // Texture（6）
        "mosaicTiles", "patchwork", "stainedGlass", "craquelure", "grain",
        "texturizer",
        // Stylize（1）
        "glowingEdges",
        // Sketch（8）
        "basRelief", "graphicPen", "halftonePattern", "photocopy", "stamp",
        "tornEdges", "reticulation", "waterPaper",
        // Artistic（7）
        "dryBrush", "fresco", "filmGrain", "sponge", "plasticWrap",
        "posterEdges", "roughPastels",
        // Sketch 第二批（6）
        "chalkCharcoal", "charcoal", "chrome", "conteCrayon", "notePaper",
        "plaster",
        // Artistic 后 8 + Brush Strokes 8
        "coloredPencil", "cutout", "neonGlow", "paintDaubs", "paletteKnife",
        "smudgeStick", "underpainting", "watercolor", "accentedEdges",
        "angledStrokes", "crosshatch", "darkStrokes", "inkOutlines",
        "spatter", "sprayedStrokes", "sumiE",
    };
    if (!key) return false;
    for (const char* k : kDone)
        if (std::strcmp(k, key) == 0) return true;
    return false;
}

inline int GalleryImplementedCount() {
    static const char* kDone[] = {
        "oceanRipple", "glass", "diffuseGlow", "mosaicTiles", "patchwork",
        "stainedGlass", "craquelure", "grain", "texturizer", "glowingEdges",
        "basRelief", "graphicPen", "halftonePattern", "photocopy", "stamp",
        "tornEdges", "reticulation", "waterPaper", "dryBrush",
        "fresco", "filmGrain", "sponge", "plasticWrap", "posterEdges",
        "roughPastels", "chalkCharcoal", "charcoal", "chrome",
        "conteCrayon", "notePaper", "plaster",
        "coloredPencil", "cutout", "neonGlow", "paintDaubs", "paletteKnife",
        "smudgeStick", "underpainting", "watercolor", "accentedEdges",
        "angledStrokes", "crosshatch", "darkStrokes", "inkOutlines",
        "spatter", "sprayedStrokes", "sumiE"};
    return static_cast<int>(sizeof(kDone) / sizeof(kDone[0]));
}

} // namespace sai2::fg