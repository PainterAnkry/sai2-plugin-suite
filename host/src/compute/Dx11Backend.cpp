// ============================================================================
// Dx11Backend.cpp - DirectX 11 compute shader 后端（规格书第十一节）
//
// 实现要点：
//   - 完全运行时动态加载 d3d11.dll / D3DCompiler_47.dll，不依赖导入库，
//     缺失或设备创建失败时 Available()=false，上层自动回退 CPU；
//   - 设备创建顺序：HARDWARE -> WARP（软件光栅，Win8+ 必有，保证可用性）;
//   - 可分离 Gaussian Blur：水平 pass（texA->texB）-> 垂直 pass（texB->texA），
//     CS 内 clamp 采样，31 tap（radius <= 15），RGBA8 UNORM。
// ============================================================================

#include "sai2/host/ComputeRegistry.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sai2::host {

namespace {

// ---------------------------------------------------------------------------
// HLSL：可分离 Gaussian blur compute shader
// ---------------------------------------------------------------------------
const char* kBlurCS = R"HLSL(
Texture2D<float4> srcTex : register(t0);
RWTexture2D<float4> dstTex : register(u0);
cbuffer BlurParams : register(b0) {
    uint2 dims;      // xy
    int  radius;     // <= 15
    int  horizontal; // 1 = 水平 pass
    float4 weights[16]; // 按 HLSL 打包规则从 32 字节处开始，w 在 .x
};
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= dims.x || tid.y >= dims.y) return;
    // RGBA 四通道一起模糊（含 alpha）——否则透明图层的透明度会在模糊后丢失
    float4 acc = float4(0, 0, 0, 0);
    for (int i = -radius; i <= radius; ++i) {
        int2 p = horizontal ? int2(tid.x + i, tid.y)
                            : int2(tid.x, tid.y + i);
        p = clamp(p, int2(0, 0), int2(dims) - int2(1, 1));
        float w = weights[abs(i)].x;
        acc += srcTex.Load(int3(p, 0)).rgba * w;
    }
    dstTex[tid.xy] = acc;
}
)HLSL";

typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_D3DCompile)(
    LPCSTR, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// GPU 侧参数块（与 HLSL cbuffer 布局一致：horizontal 结束于 16 字节，
// float4 数组从 16 开始，总 272 字节）
struct BlurParamsGPU {
    uint32_t dims[2];     // 0..7
    int32_t radius;       // 8..11
    int32_t horizontal;   // 12..15
    float weights[16][4]; // 16..271，w 存于 [i][0]
};
static_assert(sizeof(BlurParamsGPU) == 272, "cbuffer 布局必须 272 字节");

} // namespace

// ---------------------------------------------------------------------------
class Dx11Backend final : public IComputeBackend {
public:
    ComputeBackendInfo Info() const override {
        ComputeBackendInfo info;
        info.type = ComputeBackendType::DirectX11;
        info.name = "DirectX 11 (compute shader)";
        info.device = device_;
        return info;
    }

    bool Available() const override { return Init(); }

    bool GaussianBlur(ImageBuffer& img, float sigma,
                      std::string& error) override {
        if (!Init()) { error = "DirectX 11 后端不可用"; return false; }
        if (sigma <= 0.0f) { error = "sigma 必须为正"; return false; }
        if (img.Format() != PixelFormat::RGBA8) {
            error = "DirectX 11 后端仅支持 RGBA8（RGBA16 走 CPU 回退）";
            return false;
        }
        const int w = img.Width(), h = img.Height();
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            error = "图像尺寸超出支持范围";
            return false;
        }
        if (sigma > 32.0f) { error = "sigma 过大（CS 最多 31 tap）"; return false; }

        const int radius = std::max(1, std::min(15,
            static_cast<int>(std::ceil(sigma * 3.0f))));
        BlurParamsGPU params{};
        params.dims[0] = static_cast<uint32_t>(w);
        params.dims[1] = static_cast<uint32_t>(h);
        params.radius = radius;
        {
            const double denom = 2.0 * sigma * sigma;
            double sum = params.weights[0][0] = 1.0f;
            for (int i = 1; i <= radius; ++i) {
                const float v = static_cast<float>(std::exp(-(i * i) / denom));
                params.weights[i][0] = v;
                sum += 2.0 * v;
            }
            for (int i = 0; i <= radius; ++i)
                params.weights[i][0] /= static_cast<float>(sum);
        }

        // ---- 资源创建 ----
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;

        ID3D11Texture2D* texA = nullptr;   // 输入，最终结果
        ID3D11Texture2D* texB = nullptr;   // 中间结果
        ID3D11Texture2D* stageUp = nullptr;
        ID3D11Texture2D* stageDown = nullptr;
        ID3D11ShaderResourceView* srvA = nullptr;
        ID3D11ShaderResourceView* srvB = nullptr;
        ID3D11UnorderedAccessView* uavA = nullptr;
        ID3D11UnorderedAccessView* uavB = nullptr;
        ID3D11Buffer* cb = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = td.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
        uavd.Format = td.Format;
        uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;

        auto fail = [&](const std::string& msg) {
            error = msg;
            if (cb) cb->Release();
            if (uavB) uavB->Release();
            if (uavA) uavA->Release();
            if (srvB) srvB->Release();
            if (srvA) srvA->Release();
            if (stageDown) stageDown->Release();
            if (stageUp) stageUp->Release();
            if (texB) texB->Release();
            if (texA) texA->Release();
            return false;
        };

        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &texA)) ||
            FAILED(dev_->CreateTexture2D(&td, nullptr, &texB)) ||
            FAILED(dev_->CreateTexture2D(&sd, nullptr, &stageUp)) ||
            FAILED(dev_->CreateTexture2D(&sd, nullptr, &stageDown)))
            return fail("创建纹理失败");
        if (FAILED(dev_->CreateShaderResourceView(texA, &srvd, &srvA)) ||
            FAILED(dev_->CreateShaderResourceView(texB, &srvd, &srvB)) ||
            FAILED(dev_->CreateUnorderedAccessView(texA, &uavd, &uavA)) ||
            FAILED(dev_->CreateUnorderedAccessView(texB, &uavd, &uavB)))
            return fail("创建 SRV/UAV 失败");

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(BlurParamsGPU);
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT cbHr = dev_->CreateBuffer(&cbd, nullptr, &cb);
        if (FAILED(cbHr))
            return fail("创建常量缓冲失败 hr=0x" +
                        [&] {
                            char b[16];
                            snprintf(b, sizeof(b), "%08lx",
                                     static_cast<unsigned long>(cbHr));
                            return std::string(b);
                        }());

        // ---- 上传像素 ----
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx_->Map(stageUp, 0, D3D11_MAP_WRITE, 0, &mapped)))
            return fail("Map(上传) 失败");
        {
            uint8_t* base = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < h; ++y) {
                uint8_t* row = base + static_cast<size_t>(y) * mapped.RowPitch;
                for (int x = 0; x < w; ++x) {
                    RGBA c;
                    img.GetPixel(x, y, c);
                    row[static_cast<size_t>(x) * 4 + 0] = static_cast<uint8_t>(c.r);
                    row[static_cast<size_t>(x) * 4 + 1] = static_cast<uint8_t>(c.g);
                    row[static_cast<size_t>(x) * 4 + 2] = static_cast<uint8_t>(c.b);
                    row[static_cast<size_t>(x) * 4 + 3] = static_cast<uint8_t>(c.a);
                }
            }
        }
        ctx_->Unmap(stageUp, 0);
        ctx_->CopyResource(texA, stageUp);

        // ---- 两个 pass ----
        struct Pass {
            ID3D11ShaderResourceView* srv;
            ID3D11UnorderedAccessView* uav;
            int horizontal;
        };
        const Pass passes[2] = {
            {srvA, uavB, 1}, // 水平：texA -> texB
            {srvB, uavA, 0}, // 垂直：texB -> texA
        };
        for (const Pass& p : passes) {
            params.horizontal = p.horizontal;
            ctx_->UpdateSubresource(cb, 0, nullptr, &params, 0, 0);
            ctx_->CSSetShader(cs_, nullptr, 0);
            ctx_->CSSetConstantBuffers(0, 1, &cb);
            ctx_->CSSetShaderResources(0, 1, &p.srv);
            ctx_->CSSetUnorderedAccessViews(0, 1, &p.uav, nullptr);
            ctx_->Dispatch((static_cast<UINT>(w) + 7) / 8,
                           (static_cast<UINT>(h) + 7) / 8, 1);
            ID3D11ShaderResourceView* nullSRV = nullptr;
            ID3D11UnorderedAccessView* nullUAV = nullptr;
            ctx_->CSSetShaderResources(0, 1, &nullSRV);
            ctx_->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
        }
        ctx_->Flush();
        ctx_->CopyResource(stageDown, texA);

        // ---- 下载结果 ----
        if (FAILED(ctx_->Map(stageDown, 0, D3D11_MAP_READ, 0, &mapped)))
            return fail("Map(下载) 失败");
        {
            const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);
            for (int y = 0; y < h; ++y) {
                const uint8_t* row =
                    base + static_cast<size_t>(y) * mapped.RowPitch;
                for (int x = 0; x < w; ++x) {
                    RGBA c;
                    c.r = row[static_cast<size_t>(x) * 4 + 0];
                    c.g = row[static_cast<size_t>(x) * 4 + 1];
                    c.b = row[static_cast<size_t>(x) * 4 + 2];
                    c.a = row[static_cast<size_t>(x) * 4 + 3];
                    img.SetPixel(x, y, c);
                }
            }
        }
        ctx_->Unmap(stageDown, 0);

        cb->Release();
        uavB->Release(); uavA->Release();
        srvB->Release(); srvA->Release();
        stageDown->Release(); stageUp->Release();
        texB->Release(); texA->Release();
        return true;
    }

    ~Dx11Backend() override { Shutdown(); }

private:
    bool Init() const {
        std::lock_guard<std::mutex> lock(initMtx_);
        if (initState_ == State::Ready) return true;
        if (initState_ == State::Failed) return false;

        HMODULE d3d = LoadLibraryW(L"d3d11.dll");
        HMODULE compiler = d3d ? LoadLibraryW(L"D3DCompiler_47.dll") : nullptr;
        auto pCreate = d3d ? reinterpret_cast<PFN_D3D11CreateDevice>(
            GetProcAddress(d3d, "D3D11CreateDevice")) : nullptr;
        auto pCompile = compiler ? reinterpret_cast<PFN_D3DCompile>(
            GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!pCreate || !pCompile) {
            if (d3d) FreeLibrary(d3d);
            if (compiler) FreeLibrary(compiler);
            initState_ = State::Failed;
            return false;
        }

        // 创建设备：HARDWARE -> WARP
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                             nullptr, 0, D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
        if (SUCCEEDED(hr)) {
            device_ = "hardware";
        } else {
            hr = pCreate(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                         D3D11_SDK_VERSION, &dev_, &fl, &ctx_);
            device_ = "WARP (software)";
        }
        if (FAILED(hr) || !dev_ || !ctx_) {
            FreeLibrary(d3d);
            FreeLibrary(compiler);
            initState_ = State::Failed;
            return false;
        }
        libs_.push_back(d3d);
        libs_.push_back(compiler);

        // 编译 compute shader
        ID3DBlob* code = nullptr;
        ID3DBlob* errs = nullptr;
        hr = pCompile(kBlurCS, std::strlen(kBlurCS), "sai2blur", nullptr,
                      nullptr, "main", "cs_5_0", 0, 0, &code, &errs);
        if (errs) errs->Release();
        if (FAILED(hr) || !code) {
            Shutdown();
            initState_ = State::Failed;
            return false;
        }
        hr = dev_->CreateComputeShader(code->GetBufferPointer(),
                                       code->GetBufferSize(), nullptr, &cs_);
        code->Release();
        if (FAILED(hr)) {
            Shutdown();
            initState_ = State::Failed;
            return false;
        }
        initState_ = State::Ready;
        return true;
    }

    void Shutdown() const {
        if (cs_) { cs_->Release(); cs_ = nullptr; }
        if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
        if (dev_) { dev_->Release(); dev_ = nullptr; }
        for (HMODULE m : libs_) if (m) FreeLibrary(m);
        libs_.clear();
    }

    enum class State { New, Ready, Failed };
    // 惰性初始化状态（Available() const 内创建，全部 mutable）
    mutable std::mutex initMtx_;
    mutable State initState_ = State::New;
    mutable ID3D11Device* dev_ = nullptr;
    mutable ID3D11DeviceContext* ctx_ = nullptr;
    mutable ID3D11ComputeShader* cs_ = nullptr;
    mutable std::vector<HMODULE> libs_;
    mutable std::string device_;
};

IComputeBackend* CreateDx11Backend() { return new Dx11Backend(); }

} // namespace sai2::host
