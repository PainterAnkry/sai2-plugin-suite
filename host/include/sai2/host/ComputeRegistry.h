#pragma once
// ============================================================================
// ComputeRegistry.h - 计算后端注册表（host 端）
//
// 按优先级枚举后端：DirectX11 > CpuSimd；Best() 返回第一个可用后端。
// 可用 SetPreferred() 强制指定（测试 / 用户设置）。
// ============================================================================

#include "sai2/IComputeBackend.h"

#include <memory>
#include <vector>

namespace sai2::host {

class ComputeRegistry {
public:
    static ComputeRegistry& Instance();

    // 惰性创建全部后端实例并返回（顺序即优先级）
    std::vector<IComputeBackend*>& Backends();

    // 第一个 Available() 的后端；全都不可用返回 nullptr
    IComputeBackend* Best();

    // 强制指定类型；对应后端不可用时返回 nullptr
    IComputeBackend* Preferred(ComputeBackendType type);

    // 清空缓存（重建设备用，测试用）
    void Reset();

private:
    ComputeRegistry() = default;
    std::vector<std::unique_ptr<IComputeBackend>> owned_;
    std::vector<IComputeBackend*> ordered_;
    bool initialized_ = false;
};

// 后端工厂（实现见 src/compute/*.cpp）
IComputeBackend* CreateCpuSimdBackend();
IComputeBackend* CreateDx11Backend();

} // namespace sai2::host
