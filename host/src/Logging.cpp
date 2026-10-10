#include "sai2/host/Logging.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace sai2::host {
namespace {

std::string Timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms =
        duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << ms;
    return os.str();
}

void AppendLine(const std::filesystem::path& path, const std::string& line) {
    std::ofstream f(path, std::ios::app);
    if (f) f << line << '\n';
}

} // namespace

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

void Logger::Init(const std::filesystem::path& logDir) {
    // 注意：不能在持有 mtx_ 时调用 Write()（非递归锁会死锁）
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);
        // 这里必须用宽字符路径：logDir 可能含非 ASCII（见 PathUtil.h）
        logPath_ = logDir / std::filesystem::path(L"plugin.log");
        crashPath_ = logDir / std::filesystem::path(L"crash.log");
        enabled_ = !ec;
    }
    Write(LogLevel::Info, "---- logger initialized ----");
}

void Logger::Write(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!enabled_ || logPath_.empty()) return;
    std::ostringstream os;
    os << '[' << Timestamp() << "] [" << LogLevelName(level) << "] " << msg;
    AppendLine(logPath_, os.str());
}

void Logger::WriteCrash(const std::string& msg) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!enabled_ || crashPath_.empty()) return;
    std::ostringstream os;
    os << '[' << Timestamp() << "] " << msg;
    AppendLine(crashPath_, os.str());
    // 同时写一条到普通日志
    std::ostringstream os2;
    os2 << '[' << Timestamp() << "] [ERROR] " << msg;
    AppendLine(logPath_, os2.str());
}

} // namespace sai2::host
