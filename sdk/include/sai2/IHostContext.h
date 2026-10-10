#pragma once
// ============================================================================
// IHostContext.h - 宿主提供给插件的能力接口（Phase 1 定义 + Phase 2 扩展）
//
// 插件通过该接口访问图像、选区、Undo、预览与画布刷新，
// 不允许直接触碰 SAI2 内部对象。
// ============================================================================

#include "sai2/PluginAPI.h"
#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

namespace sai2 {

class IComputeBackend; // 见 IComputeBackend.h（GPU/CPU 计算后端抽象）

class IHostContext {
public:
    virtual ~IHostContext() = default;

    // ---- 图像访问 -------------------------------------------------------
    // 当前编辑中的图像缓冲区（生命周期归宿主所有，插件不得缓存指针跨 Execute 使用）
    virtual ImageBuffer* GetImageBuffer() = 0;

    // 当前选区；无选区时返回 nullptr（nullptr == 全图有效）
    virtual const Selection* GetSelection() const = 0;

    // ---- Undo（规格书第七节）-------------------------------------------
    // 开始一个 UndoGroup：组内的所有快照在撤销时作为一次操作恢复
    virtual void BeginUndoGroup(const char* name) = 0;
    virtual void EndUndoGroup() = 0;

    // 在修改图像**之前**调用，把当前图像快照压入当前 UndoGroup
    virtual void SnapshotForUndo(const char* label) = 0;

    // 撤销最近一个 UndoGroup；返回是否发生了撤销
    virtual bool Undo() = 0;

    // ---- 预览（规格书第七/八/九节）--------------------------------------
    // 请求一个降采样预览缓冲区（最长边不超过 maxPreviewDim）。
    // 插件在预览缓冲区上反复调试参数；用户确认后 EndPreview(true)，
    // 宿主负责把最终效果以合适方式回写（或要求插件对全分辨率图重新执行）。
    virtual ImageBuffer* BeginPreview(int maxPreviewDim) = 0;
    virtual void EndPreview(bool commit) = 0;

    // ---- 画布 / 消息 -----------------------------------------------------
    virtual void RefreshCanvas() = 0;
    virtual void ShowMessage(const char* message) = 0;

    // 插件日志（写入 logs/plugin.log，并带插件 id 前缀）
    virtual void Log(LogLevel level, const char* message) = 0;

    // ---- GPU 加速（规格书第十一节，API 1.1 起提供，向后兼容） ----------
    // 返回宿主计算后端（DirectX11 优先，CPU fallback），插件算法不与
    // 具体 GPU API 耦合。宿主未实现或后端全不可用时返回 nullptr，
    // 插件必须准备纯 CPU 回退路径。
    // 返回指针生命周期归宿主所有，插件不得释放或跨 Execute 缓存。
    virtual IComputeBackend* GetComputeBackend() { return nullptr; }

    // ---- 宿主参数通道（API 1.1 起提供，向后兼容）------------------------
    // 宿主通过键值字符串向插件下发参数，例如：
    //   "blur.kind"      = "gaussian" | "box" | "motion" | "radial"
    //   "blur.radius"    = "8"
    //   "filter.id"      = "com.sai2.filter.oil-paint"
    //   "filter.param.radius" = "5"
    // 键不存在时返回 nullptr；插件必须保留自身默认值，因此同一份插件在
    // 无参数通道的宿主（默认实现返回 nullptr，如测试用 MockHostContext）
    // 下行为与以往完全一致。
    // 返回指针在本轮 Execute 内保持有效，插件不得释放或跨 Execute 缓存。
    virtual const char* GetParam(const char* key) {
        (void)key;
        return nullptr;
    }
};

} // namespace sai2
