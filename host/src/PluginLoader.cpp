#include "sai2/host/PluginLoader.h"
#include "sai2/host/Logging.h"
#include "sai2/host/Manifest.h"

#include <filesystem>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace sai2::host {

using CreatePluginFn = IPlugin* (*)();
using GetApiVersionFn = uint32_t (*)();

namespace {

#if defined(_WIN32)
void* OpenLibrary(const std::string& path) {
    // UTF-8 -> UTF-16
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1,
                                         nullptr, 0);
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    HMODULE h = LoadLibraryW(wpath.c_str());
    return h ? reinterpret_cast<void*>(h) : nullptr;
}
void CloseLibrary(void* handle) {
    if (handle) FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
void* GetSymbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}
#else
void* OpenLibrary(const std::string& path) {
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}
void CloseLibrary(void* handle) {
    if (handle) dlclose(handle);
}
void* GetSymbol(void* handle, const char* name) {
    return dlsym(handle, name);
}
#endif

} // namespace

bool PluginLoader::Load(const PluginRecord& record, Loaded& out,
                        std::string& error) {
    out = Loaded{};
    if (!record.manifest.valid) {
        error = "manifest 无效";
        return false;
    }
    // record.directory 是 UTF-8：先转宽字符拼路径，再回到 UTF-8 交给
    // OpenLibrary（它内部再转 UTF-16 调 LoadLibraryW）
    const fs::path dllFsPath =
        PathFromUtf8(record.directory) /
        PathFromUtf8(record.manifest.entry);
    const std::string dllPath = PathToUtf8(dllFsPath);
    std::error_code ec;
    if (!fs::exists(dllFsPath, ec)) {
        error = "入口文件不存在: " + dllPath;
        return false;
    }

    void* handle = OpenLibrary(dllPath);
    if (!handle) {
        error = "动态库加载失败: " + dllPath;
        return false;
    }

    // API 版本校验（可选导出，但推荐）
    const auto getVersion =
        reinterpret_cast<GetApiVersionFn>(GetSymbol(handle, "sai2_GetPluginAPIVersion"));
    if (getVersion) {
        const uint32_t pluginApi = getVersion();
        if (ApiVersionMajor(pluginApi) != ApiVersionMajor(kPluginApiVersion) ||
            ApiVersionMinor(pluginApi) > ApiVersionMinor(kPluginApiVersion)) {
            error = "插件 API 版本不兼容: plugin=" +
                    std::to_string(ApiVersionMajor(pluginApi)) + "." +
                    std::to_string(ApiVersionMinor(pluginApi)) +
                    " host=" + std::to_string(ApiVersionMajor(kPluginApiVersion)) +
                    "." + std::to_string(ApiVersionMinor(kPluginApiVersion));
            CloseLibrary(handle);
            return false;
        }
    } else if (!IsApiVersionCompatible(record.manifest.apiVersion,
                                       kPluginApiVersion)) {
        error = "manifest apiVersion 与宿主不兼容: " + record.manifest.apiVersion;
        CloseLibrary(handle);
        return false;
    }

    const auto create =
        reinterpret_cast<CreatePluginFn>(GetSymbol(handle, "sai2_CreatePlugin"));
    if (!create) {
        error = "缺少导出函数 sai2_CreatePlugin";
        CloseLibrary(handle);
        return false;
    }

    IPlugin* plugin = create();
    if (!plugin) {
        error = "sai2_CreatePlugin 返回空指针";
        CloseLibrary(handle);
        return false;
    }

    out.id = record.manifest.id;
    out.handle = handle;
    out.plugin = plugin;
    Logger::Instance().Write(LogLevel::Info, "插件加载成功: " + out.id);
    return true;
}

void PluginLoader::Unload(Loaded& loaded) {
    if (loaded.handle) {
        CloseLibrary(loaded.handle);
        Logger::Instance().Write(LogLevel::Info, "插件卸载: " + loaded.id);
    }
    loaded.handle = nullptr;
    loaded.plugin = nullptr;
}

} // namespace sai2::host
