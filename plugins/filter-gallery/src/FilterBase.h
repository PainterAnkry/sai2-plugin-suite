#pragma once
// ============================================================================
// FilterBase.h - 滤镜参数模型 + 滤镜注册表（Phase 5 Filter Gallery）
//
// 规格书第九节：
//   - 统一 IFilter 接口（SDK 已定义 FilterInfo / Apply / CreateParameterUI）
//   - Filter Registry：按 id 注册/查找、按分类列出
//   - Parameters：声明式参数定义（名称/标签/默认值/范围），宿主据此渲染 UI
//   - Thumbnail / Preview：缩略图由预览管线（BeginPreview 降采样）生成
// ============================================================================

#include "sai2/IFilter.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sai2::fg {

// 单个参数定义（宿主参数面板的渲染依据）
struct ParamDef {
    std::string name;   // 程序名，如 "radius"
    std::string label;  // 显示名，如 "半径"
    float def = 0.0f;
    float min = 0.0f;
    float max = 1.0f;
    bool isInt = false; // 整数参数（滑块步进 1）
};

// ---------------------------------------------------------------------------
// FilterBase：IFilter + 声明式参数
// ---------------------------------------------------------------------------
class FilterBase : public IFilter {
public:
    // ---- IFilter ----
    FilterInfo GetInfo() const override {
        FilterInfo info;
        info.id = info_.id.c_str();
        info.name = info_.name.c_str();
        info.category = info_.category.c_str();
        info.version = info_.version.c_str();
        return info;
    }

    // ---- 参数模型 ----
    const std::vector<ParamDef>& ParamDefs() const { return defs_; }

    bool SetParam(const std::string& name, float value) {
        auto it = values_.find(name);
        if (it == values_.end()) return false;
        const ParamDef* d = FindDef(name);
        if (d) value = std::min(d->max, std::max(d->min, value));
        it->second = value;
        return true;
    }
    float P(const char* name) const {
        auto it = values_.find(name);
        return it != values_.end() ? it->second : 0.0f;
    }

    int Pi(const char* name) const {
        return static_cast<int>(P(name) + 0.5f);
    }

    void ResetParams() {
        for (const auto& d : defs_) values_[d.name] = d.def;
    }

    // JSON 风格参数描述（供宿主动态生成参数面板）
    std::string ParamDefsJson() const {
        std::string s = "[";
        for (size_t i = 0; i < defs_.size(); ++i) {
            const ParamDef& d = defs_[i];
            if (i) s += ",";
            s += "{\"name\":\"" + d.name + "\",\"label\":\"" + d.label +
                 "\",\"default\":" + std::to_string(d.def) +
                 ",\"min\":" + std::to_string(d.min) +
                 ",\"max\":" + std::to_string(d.max) +
                 ",\"int\":" + std::string(d.isInt ? "true" : "false") + "}";
        }
        s += "]";
        return s;
    }

protected:
    void DefParam(const char* name, const char* label, float def,
                  float min, float max, bool isInt = false) {
        defs_.push_back({name, label, def, min, max, isInt});
        values_[name] = def;
    }

    const ParamDef* FindDef(const std::string& name) const {
        for (const auto& d : defs_)
            if (d.name == name) return &d;
        return nullptr;
    }

    // 用可变存储承载常量字符串（GetInfo 返回 c_str）
    struct InfoStr {
        std::string id, name, category, version;
    } info_;
    std::vector<ParamDef> defs_;
    std::map<std::string, float> values_;
};

// ---------------------------------------------------------------------------
// FilterRegistry：注册 / 按 id 查找 / 按分类列出
// ---------------------------------------------------------------------------
class FilterRegistry {
public:
    template <typename T>
    T* Add() {
        static_assert(std::is_base_of_v<FilterBase, T>,
                      "T must derive from FilterBase");
        auto f = std::make_unique<T>();
        T* raw = f.get();
        filters_.push_back(std::move(f));
        return raw;
    }

    size_t Count() const { return filters_.size(); }

    FilterBase* At(size_t i) {
        return i < filters_.size() ? filters_[i].get() : nullptr;
    }

    FilterBase* Find(const std::string& id) const {
        for (auto& f : filters_) {
            FilterInfo info = f->GetInfo();
            if (id == info.id) return f.get();
        }
        return nullptr;
    }

    // 按分类列出（category 为空 = 全部）
    std::vector<FilterBase*> ByCategory(const std::string& category) const {
        std::vector<FilterBase*> out;
        for (auto& f : filters_) {
            FilterInfo info = f->GetInfo();
            if (category.empty() || category == info.category)
                out.push_back(f.get());
        }
        return out;
    }

    // 分类清单（去重，保持注册顺序）
    std::vector<std::string> Categories() const {
        std::vector<std::string> cats;
        for (auto& f : filters_) {
            FilterInfo info = f->GetInfo();
            if (std::find(cats.begin(), cats.end(), info.category) == cats.end())
                cats.push_back(info.category);
        }
        return cats;
    }

private:
    std::vector<std::unique_ptr<FilterBase>> filters_;
};

} // namespace sai2::fg
