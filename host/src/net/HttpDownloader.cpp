// ============================================================================
// HttpDownloader.cpp - WinHTTP 下载实现（Phase 6 生产用）
//
// 独立文件的原因：单元测试使用 FileDownloader，避免测试链接 winhttp。
// 构建系统需要显式编译本文件并链接 winhttp（MSVC 自动 / win32 需 -lwinhttp）。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "sai2/host/PathUtil.h"
#include <winhttp.h>

#include "sai2/host/PluginSource.h"

#include <fstream>
#include <vector>

namespace sai2::host {

namespace {

std::wstring ToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// 拆分 http(s)://host[:port]/path
bool SplitUrl(const std::string& url, bool& https, std::wstring& host,
              INTERNET_PORT& port, std::wstring& path, std::string& error) {
    const std::string httpsPrefix = "https://";
    const std::string httpPrefix = "http://";
    std::string rest;
    if (url.rfind(httpsPrefix, 0) == 0) {
        https = true;
        rest = url.substr(httpsPrefix.size());
    } else if (url.rfind(httpPrefix, 0) == 0) {
        https = false;
        rest = url.substr(httpPrefix.size());
    } else {
        error = "仅支持 http/https URL: " + url;
        return false;
    }
    const auto slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = ToWide(slash == std::string::npos ? "/" : rest.substr(slash));

    port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    const auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        port = static_cast<INTERNET_PORT>(std::stoi(hostPort.substr(colon + 1)));
        hostPort = hostPort.substr(0, colon);
    }
    host = ToWide(hostPort);
    return !host.empty();
}

} // namespace

bool HttpDownloader::Download(const std::string& url,
                              const std::string& destPath,
                              std::string& error) {
    bool https = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!SplitUrl(url, https, host, port, path, error)) return false;

    bool ok = false;
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    // destPath 是 UTF-8：转宽字符（目录含中文时必需）
    std::ofstream out(PathFromUtf8(destPath), std::ios::binary);
    if (!out) {
        error = "无法写入下载目标: " + destPath;
        return false;
    }

    do {
        session = WinHttpOpen(L"SAI2-PluginSource/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { error = "WinHttpOpen 失败"; break; }

        connect = WinHttpConnect(session, host.c_str(), port, 0);
        if (!connect) { error = "WinHttpConnect 失败"; break; }

        request = WinHttpOpenRequest(
            connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!request) { error = "WinHttpOpenRequest 失败"; break; }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            error = "请求发送/接收失败";
            break;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            error = "HTTP 状态码 " + std::to_string(status);
            break;
        }

        // 流式写盘
        DWORD bytesRead = 0;
        std::vector<char> buf(64 * 1024);
        while (WinHttpReadData(request, buf.data(),
                               static_cast<DWORD>(buf.size()), &bytesRead) &&
               bytesRead > 0) {
            out.write(buf.data(), static_cast<std::streamsize>(bytesRead));
        }
        ok = out.good();
        if (!ok) error = "写盘失败";
    } while (false);

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(PathFromUtf8(destPath), ec);
    }
    return ok;
}

} // namespace sai2::host
