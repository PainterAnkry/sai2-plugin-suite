#pragma once
// ============================================================================
// IComputeBackend.h - GPU/CPU 计算后端抽象（规格书第十一节）
//
// 设计原则：
//   - 插件算法不与具体 GPU API 强耦合，全部通过本抽象层调用；
//   - Windows 第一版优先 DirectX 11（compute shader），CPU SIMD 作为 fallback；
//   - 后端不可用/不支持时返回 false，由调用方回退（CPU 路径始终可用）。
//
// 第一版能力：可分离 Gaussian Blur（RGBA8）。
// 后续扩展：Box/Motion/Radial Blur、液化置换场渲染、RGBA32F。
// ============================================================================

#include "sai2/ImageBuffer.h"
#include "sai2/Selection.h"

#include <string>

namespace sai2 {

enum class ComputeBackendType {
    None = 0,
    CpuSimd = 1,     // CPU（编译器自动向量化 / 后续 intrinsics）
    DirectX11 = 2,   // D3D11 compute shader（含 WARP 软件光栅）
};

struct ComputeBackendInfo {
    ComputeBackendType type = ComputeBackendType::None;
    std::string name;    // 如 "DirectX 11 (WARP)"
    std::string device;  // 适配器描述 / "software"
};

class IComputeBackend {
public:
    virtual ~IComputeBackend() = default;

    virtual ComputeBackendInfo Info() const = 0;
    // 惰性初始化成功且设备可用
    virtual bool Available() const = 0;

    // 可分离 Gaussian Blur。
    // 支持 RGBA8；后端不支持时返回 false 并给出 error（调用方回退 CPU）。
    // sigma <= 0 或图像无效返回 false。
    virtual bool GaussianBlur(ImageBuffer& img, float sigma,
                              std::string& error) = 0;
};

} // namespace sai2
