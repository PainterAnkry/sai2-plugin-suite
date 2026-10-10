# Plugin SDK 开发指南

## 插件生命周期

```
LoadLibrary(sai2_CreatePlugin)
        │
        ▼
Initialize(IHostContext*)   ── 返回 false 视为失败，宿主卸载并禁用
        │
        ▼
Execute() / CreateUI()      ── 可多次调用
        │
        ▼
Shutdown()                  ── 卸载前调用一次
        │
        ▼
FreeLibrary
```

## 最小插件

```cpp
#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"

namespace {
using namespace sai2;

class MyPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.myplugin";
        info.name = "My Plugin";
        info.version = "1.0.0";
        info.type = "image-effect";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        host_ = host;
        return true;
    }
    void Shutdown() override { host_ = nullptr; }

    bool Execute() override {
        ImageBuffer* canvas = host_->GetImageBuffer();
        if (!canvas) return false;

        // 一次完整操作 = 一个 UndoGroup（规格书第七节）
        host_->BeginUndoGroup("My Operation");
        host_->SnapshotForUndo("My Operation");

        // ... 在 canvas 上做图像处理（遵守 Selection）...

        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

private:
    IHostContext* host_ = nullptr;
};
} // namespace

SAI2_DEFINE_PLUGIN(MyPlugin)   // 导出 sai2_CreatePlugin / sai2_GetPluginAPIVersion
```

## CMake（插件工程）

```cmake
add_library(myplugin MODULE src/MyPlugin.cpp)
target_link_libraries(myplugin PRIVATE sai2_sdk)
```

打包为 `.sai-plugin`（ZIP，STORED 方式）：

```bash
zip -0 -r myplugin.sai-plugin manifest.json myplugin.dll
```

> 当前安装器支持 STORED（无压缩）条目并做 CRC32 校验；DEFLATE 支持接入 zlib 后开放。

## 必须遵守的规则（规格书第四/十八节）

1. **不得直接修改 SAI2 内部对象**，一切通过 `IHostContext`；
2. 修改图像前必须 `SnapshotForUndo`，一次完整用户操作 = 一个 `UndoGroup`；
3. 所有插件操作必须支持 **Preview 与 Undo**；
4. 插件异常不得逃逸出导出函数（宿主也会 catch 并自动禁用插件）；
5. API 面向向后兼容设计：主版本不变时，新增接口只能以次版本递增。

## IHostContext 能力速查

| 方法 | 用途 |
| --- | --- |
| `GetImageBuffer()` | 当前画布（RGBA8/RGBA16） |
| `GetSelection()` | 当前选区（nullptr = 全图） |
| `BeginUndoGroup / EndUndoGroup` | 划定一次可撤销操作 |
| `SnapshotForUndo` | 修改前压入快照 |
| `Undo` | 撤销最近一个组 |
| `BeginPreview / EndPreview` | 降采样实时预览；确认后提交 |
| `RefreshCanvas` | 请求画布重绘 |
| `Log / ShowMessage` | 日志与用户提示 |
