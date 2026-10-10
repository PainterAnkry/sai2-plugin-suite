// ============================================================================
// ComputeRegistry.cpp - 计算后端注册表实现
// ============================================================================

#include "sai2/host/ComputeRegistry.h"

namespace sai2::host {

ComputeRegistry& ComputeRegistry::Instance() {
    static ComputeRegistry inst;
    return inst;
}

std::vector<IComputeBackend*>& ComputeRegistry::Backends() {
    if (!initialized_) {
        owned_.emplace_back(CreateDx11Backend());   // 优先 GPU
        owned_.emplace_back(CreateCpuSimdBackend()); // CPU fallback 永远可用
        for (auto& b : owned_) ordered_.push_back(b.get());
        initialized_ = true;
    }
    return ordered_;
}

IComputeBackend* ComputeRegistry::Best() {
    for (IComputeBackend* b : Backends())
        if (b->Available()) return b;
    return nullptr;
}

IComputeBackend* ComputeRegistry::Preferred(ComputeBackendType type) {
    for (IComputeBackend* b : Backends())
        if (b->Info().type == type && b->Available()) return b;
    return nullptr;
}

void ComputeRegistry::Reset() {
    owned_.clear();
    ordered_.clear();
    initialized_ = false;
}

} // namespace sai2::host
