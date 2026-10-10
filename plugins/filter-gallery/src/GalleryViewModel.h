#pragma once
// ============================================================================
// GalleryViewModel.h - Filter Gallery 视图模型（规格书第九节 Thumbnail）
//
// 为 Filter Gallery 窗口提供数据：
//   - ThumbnailGenerator：在规范样本图上预渲染每个滤镜的小缩略图
//     （默认参数，64px；缩略图在首次访问时惰性生成并缓存）
//   - GalleryViewModel：分类树 + 每项（id/name/缩略图索引）+ 参数面板 JSON
//     UI（Qt 6 / Win32 自绘）只消费该模型，不接触滤镜算法。
// ============================================================================

#include "FilterBase.h"
#include "FilterSet.h"

#include <memory>
#include <vector>

namespace sai2::fg {

// ---------------------------------------------------------------------------
// ThumbnailGenerator：规范样本图 + 缩略图生成/缓存
// ---------------------------------------------------------------------------
class ThumbnailGenerator {
public:
    explicit ThumbnailGenerator(int thumbSize = 64) : size_(thumbSize) {}

    int Size() const { return size_; }

    // 生成（或命中缓存）某个滤镜的缩略图；返回索引
    int GetThumbnail(FilterBase* filter) {
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].id == filter->GetInfo().id)
                return static_cast<int>(i);
        Entry e;
        e.id = filter->GetInfo().id;
        e.image = Render(filter);
        entries_.push_back(std::move(e));
        return static_cast<int>(entries_.size()) - 1;
    }

    const ImageBuffer* Thumbnail(int index) const {
        return index >= 0 && static_cast<size_t>(index) < entries_.size()
                   ? &entries_[index].image
                   : nullptr;
    }

    // 规范样本图：渐变背景 + 几何图形，能让不同滤镜呈现区分度
    static ImageBuffer MakeSample(int size) {
        ImageBuffer img(size, size, PixelFormat::RGBA8);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) {
                RGBA c;
                c.r = static_cast<uint16_t>(x * 255 / size);
                c.g = static_cast<uint16_t>(y * 255 / size);
                c.b = static_cast<uint16_t>(128 + 100 *
                            ((x + y) % 2)); // 棋盘细节供纹理类滤镜表现
                c.a = 255;
                // 中心亮圆（供边缘/素描类滤镜表现）
                const int dx = x - size / 2, dy = y - size / 2;
                if (dx * dx + dy * dy < (size / 4) * (size / 4))
                    c.r = c.g = c.b = 255;
                img.SetPixel(x, y, c);
            }
        return img;
    }

private:
    ImageBuffer Render(FilterBase* filter) {
        ImageBuffer work = MakeSample(std::max(64, size_ * 2)); // 稍大再降采样
        filter->ResetParams();
        filter->Apply(work, nullptr);
        // 简单最近邻降采样到 size_
        ImageBuffer thumb(size_, size_, PixelFormat::RGBA8);
        const int srcW = work.Width();
        for (int y = 0; y < size_; ++y)
            for (int x = 0; x < size_; ++x) {
                RGBA c;
                work.GetPixel(x * srcW / size_, y * srcW / size_, c);
                thumb.SetPixel(x, y, c);
            }
        return thumb;
    }

    struct Entry {
        std::string id;
        ImageBuffer image;
    };
    int size_;
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------
// GalleryViewModel：分类树 + 滤镜项 + 参数面板
// ---------------------------------------------------------------------------
class GalleryViewModel {
public:
    struct Item {
        std::string id;
        std::string name;
        std::string category;
        int thumbIndex = -1; // ThumbnailGenerator 中的索引
    };

    GalleryViewModel() {
        // 注册全部滤镜（与插件 DLL 内一致的集合）
        registry_.Add<OilPaintFilter>();
        registry_.Add<WatercolorFilter>();
        registry_.Add<RoughPastelFilter>();
        registry_.Add<PencilSketchFilter>();
        registry_.Add<CharcoalFilter>();
        registry_.Add<ChalkFilter>();
        registry_.Add<TexturizerFilter>();
        registry_.Add<GrainFilter>();
        registry_.Add<EmbossFilter>();
        registry_.Add<FindEdgesFilter>();
    }

    // 惰性生成全部缩略图并填充条目（UI 打开窗口时调用一次）
    void Build(ThumbnailGenerator& thumbs) {
        items_.clear();
        for (const std::string& cat : registry_.Categories()) {
            for (FilterBase* f : registry_.ByCategory(cat)) {
                Item item;
                item.id = f->GetInfo().id;
                item.name = f->GetInfo().name;
                item.category = cat;
                item.thumbIndex = thumbs.GetThumbnail(f);
                items_.push_back(std::move(item));
            }
        }
    }

    // 分类树（保持注册顺序）
    std::vector<std::string> Categories() const {
        std::vector<std::string> cats;
        for (const Item& it : items_)
            if (std::find(cats.begin(), cats.end(), it.category) == cats.end())
                cats.push_back(it.category);
        return cats;
    }

    // 某分类下的条目（category 空 = 全部）
    std::vector<const Item*> Items(const std::string& category) const {
        std::vector<const Item*> out;
        for (const Item& it : items_)
            if (category.empty() || it.category == category) out.push_back(&it);
        return out;
    }

    const Item* FindItem(const std::string& id) const {
        for (const Item& it : items_)
            if (it.id == id) return &it;
        return nullptr;
    }

    // 选中滤镜后：参数面板描述（宿主据此动态生成滑块）
    std::string ParamPanelJson(const std::string& filterId) const {
        FilterBase* f = registry_.Find(filterId);
        return f ? f->ParamDefsJson() : "[]";
    }

    // 修改选中滤镜参数（UI 滑块回调）；再调用 Preview 管线实时预览
    bool SetParam(const std::string& filterId, const std::string& param,
                  float value) const {
        FilterBase* f = registry_.Find(filterId);
        return f && f->SetParam(param, value);
    }

    // 把滤镜按当前参数应用到图像（确定后由宿主走 Undo/回写流程）
    bool Apply(const std::string& filterId, ImageBuffer& img) const {
        FilterBase* f = registry_.Find(filterId);
        return f && f->Apply(img, nullptr);
    }

    size_t ItemCount() const { return items_.size(); }

private:
    FilterRegistry registry_;
    std::vector<Item> items_;
};

} // namespace sai2::fg
