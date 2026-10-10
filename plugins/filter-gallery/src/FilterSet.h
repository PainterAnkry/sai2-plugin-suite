#pragma once
// ============================================================================
// FilterSet.h - Filter Gallery 的 10 个滤镜包装类（Phase 5）
//
// 每个类 = 算法（Filters.h）+ 声明式参数定义（FilterBase.h）。
// Artistic:  油画 / 水彩 / 粗糙蜡笔
// Sketch:    素描 / 炭笔 / 粉笔
// Texture:   纹理化 / 颗粒 / 浮雕
// Stylize:   查找边缘
// ============================================================================

#include "FilterBase.h"
#include "Filters.h"

namespace sai2::fg {

#define FG_INFO(idStr, nameStr, catStr)                                        \
    info_.id = idStr;                                                          \
    info_.name = nameStr;                                                      \
    info_.category = catStr;                                                   \
    info_.version = "1.0.0";

class OilPaintFilter final : public FilterBase {
public:
    OilPaintFilter() { FG_INFO("com.sai2.filter.oil-paint", "Oil Paint", "Artistic")
        DefParam("radius", "笔刷半径", 3, 1, 8, true);
        DefParam("levels", "强度级数", 12, 2, 64, true);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        OilPaint(b, Pi("radius"), Pi("levels"), s);
        return true;
    }
};

class WatercolorFilter final : public FilterBase {
public:
    WatercolorFilter() { FG_INFO("com.sai2.filter.watercolor", "Watercolor", "Artistic")
        DefParam("radius", "柔化半径", 4, 1, 10, true);
        DefParam("saturation", "饱和度", 0.35f, 0.0f, 2.0f);
        DefParam("edgeDarken", "边缘沉淀", 0.45f, 0.0f, 1.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Watercolor(b, Pi("radius"), P("saturation"), P("edgeDarken"), s);
        return true;
    }
};

class RoughPastelFilter final : public FilterBase {
public:
    RoughPastelFilter() { FG_INFO("com.sai2.filter.rough-pastel", "Rough Pastel", "Artistic")
        DefParam("cellSize", "块面大小", 6, 2, 32, true);
        DefParam("levels", "色阶数", 6, 2, 16, true);
        DefParam("texture", "纹理强度", 0.25f, 0.0f, 1.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        RoughPastel(b, Pi("cellSize"), Pi("levels"), P("texture"), s);
        return true;
    }
};

class PencilSketchFilter final : public FilterBase {
public:
    PencilSketchFilter() { FG_INFO("com.sai2.filter.pencil-sketch", "Pencil Sketch", "Sketch")
        DefParam("radius", "描边半径", 8, 2, 20, true);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        PencilSketch(b, Pi("radius"), s);
        return true;
    }
};

class CharcoalFilter final : public FilterBase {
public:
    CharcoalFilter() { FG_INFO("com.sai2.filter.charcoal", "Charcoal", "Sketch")
        DefParam("radius", "描边半径", 6, 2, 20, true);
        DefParam("grain", "炭粒强度", 0.18f, 0.0f, 1.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Charcoal(b, Pi("radius"), P("grain"), s);
        return true;
    }
};

class ChalkFilter final : public FilterBase {
public:
    ChalkFilter() { FG_INFO("com.sai2.filter.chalk", "Chalk", "Sketch")
        DefParam("strength", "力度", 1.0f, 0.2f, 2.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Chalk(b, P("strength"), s);
        return true;
    }
};

class TexturizerFilter final : public FilterBase {
public:
    TexturizerFilter() { FG_INFO("com.sai2.filter.texturizer", "Texturizer", "Texture")
        DefParam("scale", "纹理缩放", 8, 2, 64, true);
        DefParam("relief", "浮雕度", 0.5f, 0.0f, 1.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Texturizer(b, Pi("scale"), P("relief"), s);
        return true;
    }
};

class GrainFilter final : public FilterBase {
public:
    GrainFilter() { FG_INFO("com.sai2.filter.grain", "Grain", "Texture")
        DefParam("amount", "颗粒量", 0.2f, 0.0f, 1.0f);
        DefParam("monochrome", "单色", 1, 0, 1, true);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Grain(b, P("amount"), Pi("monochrome"), s);
        return true;
    }
};

class EmbossFilter final : public FilterBase {
public:
    EmbossFilter() { FG_INFO("com.sai2.filter.emboss", "Emboss", "Texture")
        DefParam("strength", "强度", 1.0f, 0.0f, 3.0f);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        Emboss(b, P("strength"), s);
        return true;
    }
};

class FindEdgesFilter final : public FilterBase {
public:
    FindEdgesFilter() { FG_INFO("com.sai2.filter.find-edges", "Find Edges", "Stylize")
        DefParam("invert", "反相", 0, 0, 1, true);
    }
    bool Apply(ImageBuffer& b, const Selection* s) override {
        FindEdges(b, Pi("invert"), s);
        return true;
    }
};

#undef FG_INFO

} // namespace sai2::fg
