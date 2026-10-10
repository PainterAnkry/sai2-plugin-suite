// ============================================================================
// UiProbe.cpp - 界面探针（诊断用，不属于插件系统本体）
//
// 用途：无头地验证 sai2_bridge 主面板到底画没画。
//   1) 启动 sai2_bridge.exe
//   2) 枚举它进程里的顶层窗口
//   3) 对每个窗口调 PrintWindow 抓成位图，统计平均亮度/非白像素占比
//   4) 把结论打到 stdout，并可选把抓到的位图导出 BMP
//
// 这样"面板是不是白屏"变成一个可复现的数字，而不是靠人眼看截图。
//
// 用法：sai2_ui_probe.exe <bridge.exe 全路径> [等待毫秒] [输出目录] [-- 透传给桥的参数…]
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Stats {
    long long sum = 0;
    long long n = 0;
    long long nonWhite = 0;
    int w = 0, h = 0;
    double mean() const { return n ? static_cast<double>(sum) / n : -1.0; }
    double nonWhitePct() const {
        return n ? 100.0 * static_cast<double>(nonWhite) / n : -1.0;
    }
};

bool GrabWindow(HWND hwnd, std::vector<uint8_t>& bgra, int& w, int& h) {
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return false;
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return false;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    // PW_RENDERFULLCONTENT = 2（Win8.1+），失败就退回 0
    BOOL ok = PrintWindow(hwnd, mem, 2);
    if (!ok) ok = PrintWindow(hwnd, mem, 0);

    bgra.assign(static_cast<size_t>(w) * h * 4, 0);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(mem, bmp, 0, static_cast<UINT>(h), bgra.data(), &bi,
              DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

Stats Analyze(const std::vector<uint8_t>& bgra) {
    Stats s;
    s.n = static_cast<long long>(bgra.size() / 4);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const int b = bgra[i], g = bgra[i + 1], r = bgra[i + 2];
        const int lum = (r * 30 + g * 59 + b * 11) / 100;
        s.sum += lum;
        if (r < 245 || g < 245 || b < 245) ++s.nonWhite;
    }
    return s;
}

bool WriteBmp(const std::string& path, const std::vector<uint8_t>& bgra, int w,
              int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t rowBytes = static_cast<uint32_t>(w) * 4;
    const uint32_t pixelBytes = rowBytes * h;
    const uint32_t off = 14 + 40;
    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    const uint32_t size = off + pixelBytes;
    std::memcpy(fh + 2, &size, 4);
    std::memcpy(fh + 10, &off, 4);
    uint8_t ih[40] = {0};
    const uint32_t hs = 40;
    std::memcpy(ih, &hs, 4);
    std::memcpy(ih + 4, &w, 4);
    std::memcpy(ih + 8, &h, 4); // 正数 = 自下而上（bgra 是自上而下，需翻转）
    const uint16_t planes = 1, bpp = 32;
    std::memcpy(ih + 12, &planes, 2);
    std::memcpy(ih + 14, &bpp, 2);
    std::memcpy(ih + 20, &pixelBytes, 4);
    std::fwrite(fh, sizeof(fh), 1, f);
    std::fwrite(ih, sizeof(ih), 1, f);
    for (int y = h - 1; y >= 0; --y)
        std::fwrite(bgra.data() + static_cast<size_t>(y) * w * 4, w * 4, 1, f);
    std::fclose(f);
    return true;
}

struct EnumCtx {
    DWORD pid = 0;
    std::vector<HWND> wins;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    EnumCtx* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    if (r.right - r.left < 40 || r.bottom - r.top < 40) return TRUE;
    c->wins.push_back(h);
    return TRUE;
}

std::wstring ExeDir(const wchar_t* argv0) {
    std::wstring s(argv0);
    const size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring(L".") : s.substr(0, p);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("用法: sai2_ui_probe.exe <bridge.exe> [等待毫秒] [输出目录]\n");
        return 2;
    }
    const std::wstring exe = argv[1];
    const int waitMs = argc > 2 ? _wtoi(argv[2]) : 2500;
    const std::wstring outDir = argc > 3 ? argv[3] : L"";

    // 第 4 个参数起透传给桥（例如 --uitest 让 4 个功能窗口一起出现）
    std::wstring cmd = exe;
    for (int i = 4; i < argc; ++i) {
        cmd += L" ";
        cmd += argv[i];
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    std::wstring exeDir = exe.substr(0, exe.find_last_of(L"\\/"));
    if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, exeDir.c_str(), &si, &pi)) {
        std::printf("CreateProcessW 失败 err=%lu\n", GetLastError());
        return 1;
    }
    std::printf("已启动 pid=%lu，等待 %d ms\n",
                static_cast<unsigned long>(pi.dwProcessId), waitMs);
    Sleep(static_cast<DWORD>(waitMs));

    EnumCtx ctx;
    ctx.pid = pi.dwProcessId;
    for (int attempt = 0; attempt < 12; ++attempt) {
        ctx.wins.clear();
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
        if (!ctx.wins.empty()) break;
        Sleep(250);
    }
    std::printf("可见顶层窗口 %d 个\n", static_cast<int>(ctx.wins.size()));

    int verdict = 0;
    for (HWND h : ctx.wins) {
        wchar_t title[256] = {0};
        GetWindowTextW(h, title, 256);
        std::vector<uint8_t> bgra;
        int w = 0, hh = 0;
        const bool ok = GrabWindow(h, bgra, w, hh);
        const Stats st = Analyze(bgra);
        char t8[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, title, -1, t8, sizeof(t8), nullptr,
                            nullptr);
        std::printf("  窗口 %p \"%s\" %dx%d 抓取=%s 平均亮度=%.1f 非白像素=%.1f%%\n",
                    reinterpret_cast<void*>(h), t8, w, hh, ok ? "OK" : "FAIL",
                    st.mean(), st.nonWhitePct());
        if (st.nonWhitePct() < 1.0) {
            std::printf("    => 判定：几乎全白（未绘制）\n");
            verdict = 3;
        } else if (st.mean() < 40.0) {
            std::printf("    => 判定：深色界面已绘制（正常）\n");
        } else {
            std::printf("    => 判定：已绘制（浅色/混合）\n");
        }
        if (!outDir.empty() && ok) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\probe_%p.bmp", outDir.c_str(),
                     reinterpret_cast<void*>(h));
            char p8[MAX_PATH * 2] = {0};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), nullptr,
                                nullptr);
            if (WriteBmp(p8, bgra, w, hh)) std::printf("    已导出 %s\n", p8);
        }
    }

    // 收尾：让桥自己退出（发 WM_CLOSE），失败就强杀
    for (HWND h : ctx.wins) PostMessageW(h, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 3000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    (void)ExeDir;
    return verdict;
}
