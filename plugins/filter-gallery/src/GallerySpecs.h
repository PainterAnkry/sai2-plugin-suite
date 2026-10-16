#pragma once
// ==========================================================================
// GallerySpecs.h - 自动生成，请勿手改
// 生成器: plugins/filter-gallery/tools/gen_gallery_specs.py
// 来源:   photocraft crates/algo/src/artistic.rs 的 gallery!{...}
//         （Photoshop 滤镜库的 key / 显示名 / 分类 / 参数记法）
// 参数记法: {"名":min..max=默认, "名":"选项a|选项b", "名":bool}
// ==========================================================================

namespace sai2::fg {

struct GallerySpec {
    const char* ident;     // 源标识符（调试用）
    const char* key;       // camelCase key；id = com.sai2.filter.gallery.<key>
    const char* name;      // 显示名
    const char* category;  // 分类
    const char* params;    // 参数记法
};

// 源规格表条目数。源文件或生成器改动导致条目数变化时编译期即报错，
// 防止"悄悄少生成几个滤镜"。
inline constexpr int kGallerySpecCount = 47;

inline const GallerySpec* GallerySpecs(int& count) {
    static const GallerySpec kSpecs[] = {
        {"ColoredPencil", "coloredPencil", "Colored Pencil", "Artistic", "{\"pencilWidth\":1..24=4,\"strokePressure\":0..15=8,\"paperBrightness\":0..50=25}"},
        {"Cutout", "cutout", "Cutout", "Artistic", "{\"numberOfLevels\":2..8=4,\"edgeSimplicity\":0..10=4,\"edgeFidelity\":1..3=2}"},
        {"DryBrush", "dryBrush", "Dry Brush", "Artistic", "{\"brushSize\":0..10=2,\"brushDetail\":0..10=8,\"texture\":1..3=1}"},
        {"FilmGrain", "filmGrain", "Film Grain", "Artistic", "{\"grain\":0..20=4,\"highlightArea\":0..20=0,\"intensity\":0..10=10}"},
        {"Fresco", "fresco", "Fresco", "Artistic", "{\"brushSize\":0..10=2,\"brushDetail\":0..10=8,\"texture\":1..3=1}"},
        {"NeonGlow", "neonGlow", "Neon Glow", "Artistic", "{\"glowSize\":-24..24=5,\"glowBrightness\":0..50=15,\"glowColor\":json}"},
        {"PaintDaubs", "paintDaubs", "Paint Daubs", "Artistic", "{\"brushSize\":1..50=8,\"sharpness\":0..40=7,\"brushType\":\"simple|lightRough|darkRough|wideSharp|wideBlurry|sparkle\"}"},
        {"PaletteKnife", "paletteKnife", "Palette Knife", "Artistic", "{\"strokeSize\":1..50=25,\"strokeDetail\":1..3=3,\"softness\":0..10=0}"},
        {"PlasticWrap", "plasticWrap", "Plastic Wrap", "Artistic", "{\"highlightStrength\":0..20=15,\"detail\":1..15=9,\"smoothness\":1..15=7}"},
        {"PosterEdges", "posterEdges", "Poster Edges", "Artistic", "{\"edgeThickness\":0..10=2,\"edgeIntensity\":0..10=1,\"posterization\":0..6=2}"},
        {"RoughPastels", "roughPastels", "Rough Pastels", "Artistic", "{\"strokeLength\":0..40=6,\"strokeDetail\":1..20=4,\"texture\":\"canvas|brick|burlap|sandstone\",\"scaling\":50..200=100,\"relief\":0..50=20,\"light\":\"bottom|bottomLeft|left|topLeft|top|topRight|right|bottomRight\",\"invert\":bool}"},
        {"SmudgeStick", "smudgeStick", "Smudge Stick", "Artistic", "{\"strokeLength\":0..10=2,\"highlightArea\":0..20=0,\"intensity\":0..10=10}"},
        {"Sponge", "sponge", "Sponge", "Artistic", "{\"brushSize\":0..10=2,\"definition\":0..25=12,\"smoothness\":1..15=5}"},
        {"Underpainting", "underpainting", "Underpainting", "Artistic", "{\"brushSize\":0..40=6,\"textureCoverage\":0..40=16,\"texture\":\"canvas|brick|burlap|sandstone\",\"scaling\":50..200=100,\"relief\":0..50=4,\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool}"},
        {"Watercolor", "watercolor", "Watercolor", "Artistic", "{\"brushDetail\":1..14=9,\"shadowIntensity\":0..10=1,\"texture\":1..3=1}"},
        {"AccentedEdges", "accentedEdges", "Accented Edges", "Brush Strokes", "{\"edgeWidth\":1..14=2,\"edgeBrightness\":0..50=38,\"smoothness\":1..15=5}"},
        {"AngledStrokes", "angledStrokes", "Angled Strokes", "Brush Strokes", "{\"directionBalance\":0..100=50,\"strokeLength\":3..50=15,\"sharpness\":0..10=3}"},
        {"Crosshatch", "crosshatch", "Crosshatch", "Brush Strokes", "{\"strokeLength\":3..50=9,\"sharpness\":0..20=6,\"strength\":1..3=1}"},
        {"DarkStrokes", "darkStrokes", "Dark Strokes", "Brush Strokes", "{\"balance\":0..10=5,\"blackIntensity\":0..10=6,\"whiteIntensity\":0..10=2}"},
        {"InkOutlines", "inkOutlines", "Ink Outlines", "Brush Strokes", "{\"strokeLength\":1..50=4,\"darkIntensity\":0..50=20,\"lightIntensity\":0..50=10}"},
        {"Spatter", "spatter", "Spatter", "Brush Strokes", "{\"sprayRadius\":0..25=10,\"smoothness\":1..15=5}"},
        {"SprayedStrokes", "sprayedStrokes", "Sprayed Strokes", "Brush Strokes", "{\"strokeLength\":0..20=12,\"sprayRadius\":0..25=7,\"strokeDirection\":\"rightDiagonal|horizontal|leftDiagonal|vertical\"}"},
        {"SumiE", "sumiE", "Sumi-e", "Brush Strokes", "{\"strokeWidth\":3..15=10,\"strokePressure\":0..15=2,\"contrast\":0..40=16}"},
        {"DiffuseGlow", "diffuseGlow", "Diffuse Glow", "Distort", "{\"graininess\":0..10=6,\"glowAmount\":0..20=10,\"clearAmount\":0..20=15,\"background\":json}"},
        {"Glass", "glass", "Glass", "Distort", "{\"distortion\":0..20=5,\"smoothness\":1..15=3,\"texture\":\"frosted|blocks|canvas|tinyLens\",\"scaling\":50..200=100,\"invert\":bool}"},
        {"OceanRipple", "oceanRipple", "Ocean Ripple", "Distort", "{\"rippleSize\":1..15=9,\"rippleMagnitude\":0..20=9}"},
        {"BasRelief", "basRelief", "Bas Relief", "Sketch", "{\"detail\":1..15=13,\"smoothness\":1..15=3,\"light\":\"bottom|bottomLeft|left|topLeft|top|topRight|right|bottomRight\",\"foreground\":json,\"background\":json}"},
        {"ChalkCharcoal", "chalkCharcoal", "Chalk & Charcoal", "Sketch", "{\"charcoalArea\":0..20=6,\"chalkArea\":0..20=6,\"strokePressure\":0..5=1,\"foreground\":json,\"background\":json}"},
        {"Charcoal", "charcoal", "Charcoal", "Sketch", "{\"charcoalThickness\":1..7=1,\"detail\":0..5=5,\"lightDarkBalance\":0..100=50,\"foreground\":json,\"background\":json}"},
        {"Chrome", "chrome", "Chrome", "Sketch", "{\"detail\":0..10=4,\"smoothness\":0..10=7}"},
        {"ConteCrayon", "conteCrayon", "Conté Crayon", "Sketch", "{\"foregroundLevel\":1..15=11,\"backgroundLevel\":1..15=7,\"texture\":\"canvas|brick|burlap|sandstone\",\"scaling\":50..200=100,\"relief\":0..50=4,\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool,\"foreground\":json,\"background\":json}"},
        {"GraphicPen", "graphicPen", "Graphic Pen", "Sketch", "{\"strokeLength\":1..15=15,\"lightDarkBalance\":0..100=50,\"strokeDirection\":\"rightDiagonal|horizontal|leftDiagonal|vertical\",\"foreground\":json,\"background\":json}"},
        {"HalftonePattern", "halftonePattern", "Halftone Pattern", "Sketch", "{\"size\":1..12=1,\"contrast\":0..50=5,\"patternType\":\"dot|circle|line\",\"foreground\":json,\"background\":json}"},
        {"NotePaper", "notePaper", "Note Paper", "Sketch", "{\"imageBalance\":0..50=25,\"graininess\":0..20=10,\"relief\":0..25=11,\"foreground\":json,\"background\":json}"},
        {"Photocopy", "photocopy", "Photocopy", "Sketch", "{\"detail\":1..24=7,\"darkness\":1..50=8,\"foreground\":json,\"background\":json}"},
        {"Plaster", "plaster", "Plaster", "Sketch", "{\"imageBalance\":0..50=20,\"smoothness\":1..15=2,\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"foreground\":json,\"background\":json}"},
        {"Reticulation", "reticulation", "Reticulation", "Sketch", "{\"density\":0..50=12,\"foregroundLevel\":0..50=40,\"backgroundLevel\":0..50=5,\"foreground\":json,\"background\":json}"},
        {"Stamp", "stamp", "Stamp", "Sketch", "{\"lightDarkBalance\":0..50=25,\"smoothness\":1..50=5,\"foreground\":json,\"background\":json}"},
        {"TornEdges", "tornEdges", "Torn Edges", "Sketch", "{\"imageBalance\":0..50=25,\"smoothness\":1..15=11,\"contrast\":1..25=17,\"foreground\":json,\"background\":json}"},
        {"WaterPaper", "waterPaper", "Water Paper", "Sketch", "{\"fiberLength\":3..50=15,\"brightness\":0..100=60,\"contrast\":0..100=80}"},
        {"GlowingEdges", "glowingEdges", "Glowing Edges", "Stylize", "{\"edgeWidth\":1..14=2,\"edgeBrightness\":0..20=6,\"smoothness\":1..15=5}"},
        {"Craquelure", "craquelure", "Craquelure", "Texture", "{\"crackSpacing\":2..100=15,\"crackDepth\":0..10=6,\"crackBrightness\":0..10=9}"},
        {"Grain", "grain", "Grain", "Texture", "{\"intensity\":0..100=40,\"contrast\":0..100=50,\"grainType\":\"regular|soft|sprinkles|clumped|contrasty|enlarged|stippled|horizontal|vertical|speckle\",\"foreground\":json,\"background\":json}"},
        {"MosaicTiles", "mosaicTiles", "Mosaic Tiles", "Texture", "{\"tileSize\":2..100=12,\"groutWidth\":1..15=3,\"lightenGrout\":0..10=9}"},
        {"Patchwork", "patchwork", "Patchwork", "Texture", "{\"squareSize\":0..10=4,\"relief\":0..25=8}"},
        {"StainedGlass", "stainedGlass", "Stained Glass", "Texture", "{\"cellSize\":2..50=10,\"borderThickness\":1..20=4,\"lightIntensity\":0..10=3,\"foreground\":json}"},
        {"Texturizer", "texturizer", "Texturizer", "Texture", "{\"texture\":\"canvas|brick|burlap|sandstone\",\"scaling\":50..200=100,\"relief\":0..50=4,\"light\":\"top|topRight|right|bottomRight|bottom|bottomLeft|left|topLeft\",\"invert\":bool}"},
    };
    count = kGallerySpecCount;
    return kSpecs;
}

} // namespace sai2::fg
