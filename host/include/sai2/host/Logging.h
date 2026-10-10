#pragma once
// ============================================================================
// Logging.h - 宿主日志（规格书第十六节）
//
//   logs/plugin.log : 宿主与插件的运行日志
//   logs/crash.log  : 插件崩溃 / 异常记录
// ============================================================================

#include "sai2/PluginAPI.h"
#include "sai2/host/PathUtil.h"

#include <filesystem>
#include <mutex>
#include <string>

namespace sai2::host {

class Logger {
public:
    static Logger& Instance();

    // 初始化日志目录（不存在则创建）；在宿主启动时调用一次。
    // 路径含非 ASCII 字符时**必须**先过 PathFromUtf8（见 PathUtil.h）
    void Init(const std::filesystem::path& logDir);

    bool IsEnabled() const { return enabled_; }

    void Write(LogLevel level, const std::string& msg);
    void WriteCrash(const std::string& msg);

private:
    Logger() = default;

    std::mutex mtx_;
    std::filesystem::path logPath_;
    std::filesystem::path crashPath_;
    bool enabled_ = false;
};

} // namespace sai2::host
