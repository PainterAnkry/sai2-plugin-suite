#include "AgentCore.h"
#include "IATHook.h"
#include "JsonText.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace sai2agent {

// ---------------------------------------------------------------------------
// Hook 跳板
// ---------------------------------------------------------------------------
namespace {

typedef BOOL(WINAPI* FnExtTextOutW)(HDC, int, int, UINT, const RECT*,
                                    LPCWSTR, int, const INT*);
typedef int(WINAPI* FnDrawTextW)(HDC, LPWSTR, int, LPRECT, UINT);
typedef int(WINAPI* FnSetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int,
                                         UINT, UINT, const void*,
                                         const BITMAPINFO*, UINT);
typedef BOOL(WINAPI* FnBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);

void* g_origExtTextOutW = nullptr;
void* g_origDrawTextW = nullptr;
void* g_origSetDIBitsToDevice = nullptr;
void* g_origBitBlt = nullptr;
IATHook* g_hooks = nullptr;
AgentCore* g_core = nullptr; // Startup 后有效

thread_local int g_inCapture = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

void CopyClassName(HWND hwnd, wchar_t (&dst)[96]) {
    dst[0] = L'\0';
    if (hwnd) GetClassNameW(hwnd, dst, 96);
}

bool IsMemoryDc(HDC hdc) {
    return GetObjectType(hdc) == OBJ_MEMDC;
}

// 把给定 32bpp DIB 数据写成 BMP（declaredHeight<0 表示 top-down，翻成正立）
bool WriteBmpFromDib32(const std::wstring& path, int w, int h,
                       LONG declaredHeight, const void* bits,
                       size_t bitsBytes) {
    if (w <= 0 || h <= 0 || !bits) return false;
    const size_t srcStride = ((static_cast<size_t>(w) * 4) + 3) & ~size_t(3);
    if (bitsBytes < srcStride * static_cast<size_t>(h)) return false;

    const uint8_t* row0 = static_cast<const uint8_t*>(bits);
    std::vector<uint8_t> flipped;
    if (declaredHeight < 0) { // top-down → 翻成 bottom-up 并正立
        flipped.resize(srcStride * static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
            std::memcpy(flipped.data() + static_cast<size_t>(y) * srcStride,
                        row0 + static_cast<size_t>(y) * srcStride, srcStride);
        }
        for (int y = 0; y < h / 2; ++y) {
            uint8_t* a = flipped.data() + static_cast<size_t>(y) * srcStride;
            uint8_t* b = flipped.data() +
                         static_cast<size_t>(h - 1 - y) * srcStride;
            for (size_t i = 0; i < srcStride; ++i) std::swap(a[i], b[i]);
        }
        row0 = flipped.data();
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const size_t dataLen = srcStride * static_cast<size_t>(h);
    const DWORD bfOffBits = 14 + 40;
    const DWORD fileSize = bfOffBits + static_cast<DWORD>(dataLen);
    uint8_t hdr[14 + 40] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    std::memcpy(hdr + 2, &fileSize, 4);
    std::memcpy(hdr + 10, &bfOffBits, 4);
    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h; // bottom-up
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    std::memcpy(hdr + 14, &bih, sizeof(bih));
    DWORD written = 0;
    const bool ok =
        WriteFile(f, hdr, sizeof(hdr), &written, nullptr) &&
        WriteFile(f, row0, static_cast<DWORD>(dataLen), &written, nullptr);
    CloseHandle(f);
    return ok;
}

void CopyPathToChar(const std::wstring& w, char (&dst)[MAX_PATH]) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, dst, MAX_PATH,
                                      nullptr, nullptr);
    if (n > 0) dst[static_cast<size_t>(n) - 1] = '\0';
    else dst[0] = '\0';
}

} // namespace

// ---------------------------------------------------------------------------
// AgentCore::Impl
// ---------------------------------------------------------------------------

struct AgentCore::Impl {
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<TextCapture> textRing;
    std::deque<BlitCapture> blitRing;

    // 图层行模型：每窗口一组按 y 升序的行
    std::unordered_map<HWND, std::vector<RowInfo> > rowsByHwnd;

    // 内存 DC 文字等待"呈现 blit"配对（SAI2 把文字画进单元表面再贴出）
    std::deque<TextCapture> pendingTexts;

    // 已确认的图层面板（粘滞：识别过一次后续查询优先选它）
    HWND stickyPanel = nullptr;

    // 插件菜单（桥推送）与用户点选请求
    std::vector<AgentCore::PluginItem> pluginItems;
    bool hasPending = false;
    int pendingIndex = -1;

    std::wstring outDir;
    std::wstring thumbDir;
    HANDLE logFile = nullptr;
    CRITICAL_SECTION logLock;
    long thumbSeq = 0;

    // ------------------------------------------------------------ 异步缩略图
    // 用户反馈：装插件后画画有延迟。原因：绘画时 SAI2 实时更新图层缩略图，
    // 命中捕获条件的 blit 会在**绘画线程上同步 CreateFileW+WriteFile**，
    // 杀软实时扫描还会再叠一层。现在钩子内只做内存拷贝（且有 per-hwnd
    // 350ms 节流），磁盘 I/O 全部移交 ThumbThreadProc 后台线程。
    struct ThumbJob {
        BlitCapture meta;
        std::vector<uint8_t> pixels;
        int w = 0, h = 0;
        LONG declaredHeight = 0;
    };
    CRITICAL_SECTION thumbLock;
    std::deque<ThumbJob> thumbJobs;
    HANDLE thumbEvent = nullptr;
    HANDLE thumbThread = nullptr;
    volatile bool thumbStop = false;
    std::unordered_map<HWND, ULONGLONG> lastThumbTick;
    static constexpr ULONGLONG kThumbIntervalMs = 350;
    static constexpr size_t kMaxThumbJobs = 3;

    void InitThumbs() {
        InitializeCriticalSection(&thumbLock);
        thumbEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    // 节流 + 入队。返回是否入队成功（false = 被节流跳过）。
    bool TryEnqueueThumb(BlitCapture& c, int w, int h, LONG declared,
                         const void* bits, size_t bytes) {
        EnterCriticalSection(&thumbLock);
        if (thumbStop) {
            LeaveCriticalSection(&thumbLock);
            return false;
        }
        const ULONGLONG now = c.tick;
        auto it = lastThumbTick.find(c.hwnd);
        if (it != lastThumbTick.end() &&
            now - it->second < kThumbIntervalMs) {
            LeaveCriticalSection(&thumbLock);
            return false;
        }
        lastThumbTick[c.hwnd] = now;
        ThumbJob job;
        job.meta = c;
        job.w = w;
        job.h = h;
        job.declaredHeight = declared;
        job.pixels.assign(static_cast<const uint8_t*>(bits),
                          static_cast<const uint8_t*>(bits) + bytes);
        while (thumbJobs.size() >= kMaxThumbJobs) thumbJobs.pop_front();
        thumbJobs.push_back(std::move(job));
        LeaveCriticalSection(&thumbLock);
        SetEvent(thumbEvent);
        return true;
    }

    void ShutdownThumbs() {
        thumbStop = true;
        if (thumbEvent) SetEvent(thumbEvent);
        if (thumbThread) {
            // 最多等 1.5s：Shutdown 可能从 DllMain 调用，不能死等
            WaitForSingleObject(thumbThread, 1500);
            CloseHandle(thumbThread);
            thumbThread = nullptr;
        }
        if (thumbEvent) {
            CloseHandle(thumbEvent);
            thumbEvent = nullptr;
        }
        DeleteCriticalSection(&thumbLock);
    }

    // ------------------------------------------------------------ 日志
    void Log(const char* line) {
        EnterCriticalSection(&logLock);
        if (logFile && logFile != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear,
                          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                          st.wMilliseconds);
            DWORD w = 0;
            WriteFile(logFile, buf, static_cast<DWORD>(std::strlen(buf)), &w,
                      nullptr);
            WriteFile(logFile, line, static_cast<DWORD>(std::strlen(line)), &w,
                      nullptr);
            WriteFile(logFile, "\r\n", 2, &w, nullptr);
        }
        LeaveCriticalSection(&logLock);
    }

    // ------------------------------------------------------------ 行合并
    // y 容差 6px：同一行重绘时抖动合并
    static constexpr int kRowMergeTol = 6;

    void UpsertRow(HWND hwnd, const TextCapture& c) {
        auto& vec = rowsByHwnd[hwnd];
        size_t lo = 0, hi = vec.size();
        // 二分找到插入点，再在邻域找容差内的既有行
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (vec[mid].y < c.y - kRowMergeTol) lo = mid + 1;
            else hi = mid;
        }
        for (size_t i = lo; i < vec.size(); ++i) {
            if (vec[i].y > c.y + kRowMergeTol) break;
            if (vec[i].y >= c.y - kRowMergeTol) {
                RowInfo& r = vec[i];
                r.y = c.y;
                r.x = c.x;
                r.w = c.w;
                r.hasScreen = c.hasScreen;
                r.screenX = c.screenX;
                r.screenY = c.screenY;
                r.tick = c.tick;
                r.count += 1;
                r.text = c.text;
                return;
            }
        }
        RowInfo r;
        r.y = c.y; r.x = c.x; r.w = c.w;
        r.hasScreen = c.hasScreen;
        r.screenX = c.screenX; r.screenY = c.screenY;
        r.tick = c.tick;
        r.count = 1;
        r.text = c.text;
        vec.insert(vec.begin() + static_cast<long>(lo), std::move(r));
        if (vec.size() > 512) vec.erase(vec.begin()); // 异常窗口防膨胀
    }

    // ------------------------------------------------------------ 捕获
    // SAI2 的 UI 文字画在内存表面（hwnd=0 的内存 DC），随后由
    // SetDIBitsToDevice/BitBlt 贴到真实子窗口。这里做文字↔位块相关性映射：
    // 文字坐标若落在某次 blit 的源矩形内，就还原出它的屏幕坐标与目标窗口。
    bool CorrelateTextToScreen(TextCapture& c) {
        // 从最近的 blit 往回找（UI 重绘都是"画文字→紧跟着贴出"）
        for (auto it = blitRing.rbegin();
             it != blitRing.rend() && it != blitRing.rbegin() + 64; ++it) {
            const BlitCapture& b = *it;
            if (!b.hasScreen || b.w <= 0 || b.h <= 0) continue;
            if (c.x >= b.srcX && c.x < b.srcX + b.w && c.y >= b.srcY &&
                c.y < b.srcY + b.h) {
                c.screenX = b.screenX + (c.x - b.srcX);
                c.screenY = b.screenY + (c.y - b.srcY);
                c.hasScreen = true;
                if (!c.hwnd && b.hwnd) {
                    c.hwnd = b.hwnd;
                    CopyClassName(c.hwnd, c.cls);
                }
                return true;
            }
        }
        return false;
    }

    void RecordText(TextCapture c) {
        // 全空白文字是噪声（SAI2 单元格占位），跳过
        {
            bool allWs = true;
            for (wchar_t ch : c.text)
                if (ch != L' ' && ch != L'	') { allWs = false; break; }
            if (allWs) return;
        }
        if (!c.hasScreen) CorrelateTextToScreen(c);
        AcquireSRWLockExclusive(&lock);
        textRing.push_back(c);
        while (textRing.size() > 4096) textRing.pop_front();
        if (c.hwnd) {
            UpsertRow(c.hwnd, c);
        } else {
            // 画在内存表面：等呈现 blit 配对出屏幕位置
            pendingTexts.push_back(std::move(c));
            while (pendingTexts.size() > 64) pendingTexts.pop_front();
        }
        ReleaseSRWLockExclusive(&lock);
    }

    // blit 到达时，把"当前积压的待配对文字"落位到本笔目标。
    //
    // SAI2 面板实测绘制模式（见 docs/SAI2Injection.md），两种都出现：
    //   a) 逐行交错：渲染第 i 行的文字 → 贴第 i 行；
    //   b) 整批：先渲染所有行的文字（按列：先所有名字、再所有混合模式…），
    //      再依次贴出行槽。
    // 统一处理：每笔 blit 消费积压的待配对文字（120ms 窗口内）；
    //   * 按"连续相同局部 y"分列、列按局部 y 升序（名字列最先）；
    //   * 行数 = 第一列（名字列）长度；
    //   * 第 j 行落到 b.screenY + j*cellH（交错模式 rows=1 → 全部在本笔位置）；
    //   * 行网格超出目标窗口 → 幻影行，跳过；>120ms 的文字丢弃（防跨事件错配）。
    void PairPendingTexts(const BlitCapture& b) {
        if (!b.hasScreen || b.w < 16 || b.h < 8 || b.h > 96) return;
        AcquireSRWLockExclusive(&lock);

        RECT wr = {};
        const bool haveWin = b.hwnd && GetWindowRect(b.hwnd, &wr);

        std::vector<TextCapture> fresh;
        for (auto it = pendingTexts.begin(); it != pendingTexts.end();) {
            if (b.tick < it->tick || b.tick - it->tick > 120) {
                it = pendingTexts.erase(it); // 太旧：丢弃，防跨事件错配
                continue;
            }
            if (it->w > b.w || it->h > b.h) { ++it; continue; }
            const int sx = b.screenX + it->x;
            const int sy = b.screenY + (it->y > 0 ? it->y : 0);
            if (haveWin && (sx < wr.left || sx >= wr.right || sy < wr.top ||
                            sy >= wr.bottom)) {
                ++it; // 映射点在目标窗口外 → 错配，跳过（不消费）
                continue;
            }
            TextCapture t = *it;
            t.screenX = sx;
            t.screenY = it->y; // 先存局部 y，行步进后再加
            t.hasScreen = false;
            fresh.push_back(t);
            it = pendingTexts.erase(it);
        }
        if (fresh.empty()) {
            ReleaseSRWLockExclusive(&lock);
            return;
        }

        // 按"连续相同局部 y"分列（列内顺序 = 行序）
        std::vector<std::vector<TextCapture> > columns;
        for (size_t k = 0; k < fresh.size(); ++k) {
            if (!columns.empty() && columns.back().front().y == fresh[k].y) {
                columns.back().push_back(fresh[k]);
            } else {
                columns.push_back(std::vector<TextCapture>(1, fresh[k]));
            }
        }
        // 列按局部 y 升序（名字列最先）
        std::stable_sort(
            columns.begin(), columns.end(),
            [](const std::vector<TextCapture>& a,
               const std::vector<TextCapture>& b2) {
                return a.front().y < b2.front().y;
            });
        const size_t rows = columns[0].size();

        wchar_t cls[96];
        CopyClassName(b.hwnd, cls);
        for (size_t c = 0; c < columns.size(); ++c) {
            for (size_t j = 0; j < columns[c].size(); ++j) {
                TextCapture t = columns[c][j];
                t.hwnd = b.hwnd;
                wcscpy_s(t.cls, cls);
                t.screenY = b.screenY + static_cast<int>(j) * b.h +
                            (t.y > 0 ? t.y : 0);
                if (haveWin && (t.screenX < wr.left || t.screenX >= wr.right ||
                                t.screenY < wr.top ||
                                t.screenY >= wr.bottom))
                    continue; // 行网格超出窗口 → 幻影行，跳过
                t.hasScreen = true;
                t.y = t.screenY; // 行键 = 文字真实屏幕行
                textRing.push_back(t);
                while (textRing.size() > 4096) textRing.pop_front();
                UpsertRow(b.hwnd, t);
            }
        }
        ReleaseSRWLockExclusive(&lock);
    }

    bool PointInAnyGroup(int sx, int sy) {
        for (const auto& kv : rowsByHwnd) {
            if (kv.second.empty()) continue;
            RECT wr;
            if (!GetWindowRect(kv.first, &wr)) continue;
            if (sx >= wr.left && sx < wr.right && sy >= wr.top && sy < wr.bottom)
                return true;
        }
        return false;
    }

    void RecordBlit(const BlitCapture& c) {
        AcquireSRWLockExclusive(&lock);
        blitRing.push_back(c);
        while (blitRing.size() > 512) blitRing.pop_front();
        ReleaseSRWLockExclusive(&lock);
        if (c.captured) {
            char buf[MAX_PATH + 96];
            std::snprintf(buf, sizeof(buf), "thumb saved: %s (%dx%d at %d,%d)",
                          c.file, c.w, c.h, c.screenX, c.screenY);
            Log(buf);
        }
    }

    void CaptureSetDibits(HDC hdc, int xDst, int yDst, int w, int h,
                          const void* bits, const BITMAPINFO* bmi,
                          UINT colorUse, UINT startScan, UINT cLines,
                          int xSrc, int ySrc) {
        (void)colorUse;
        (void)startScan;
        BlitCapture c;
        c.tick = GetTickCount64();
        c.fn = FN_SETDIBITS;
        c.hwnd = WindowFromDC(hdc);
        CopyClassName(c.hwnd, c.cls);
        c.w = w;
        c.h = h;
        c.srcX = xSrc;
        c.srcY = ySrc;
        POINT org = {0, 0};
        if (!IsMemoryDc(hdc) && GetDCOrgEx(hdc, &org)) {
            c.screenX = org.x + xDst;
            c.screenY = org.y + yDst;
            c.hasScreen = true;
        }
        // 先做文字配对（行模型由此生长），再记录 blit
        PairPendingTexts(c);
        RecordBlit(c);

        // ---- 缩略图捕获：廉价过滤在前，昂贵检查（窗口命中/入队）在后。
        // 命中后不再同步写盘，只把像素拷进内存队列交给后台线程落盘。
        if (!c.hasScreen || w < 8 || h < 8 || w > 512 || h > 512) return;
        if (!bmi || bmi->bmiHeader.biBitCount != 32 ||
            bmi->bmiHeader.biCompression != BI_RGB || !bits || cLines == 0)
            return;
        if (!PointInAnyGroup(c.screenX, c.screenY)) return;
        const LONG declared = bmi->bmiHeader.biHeight;
        const size_t stride = ((static_cast<size_t>(w) * 4) + 3) & ~size_t(3);
        TryEnqueueThumb(c, w, static_cast<int>(cLines), declared, bits,
                        stride * static_cast<size_t>(cLines));
    }

    void CaptureBitBlt(HDC dstDc, int x, int y, int w, int h, HDC srcDc,
                       int sx, int sy) {
        BlitCapture c;
        c.tick = GetTickCount64();
        c.fn = FN_BITBLT;
        c.hwnd = WindowFromDC(dstDc);
        CopyClassName(c.hwnd, c.cls);
        c.w = w;
        c.h = h;
        c.srcX = sx;
        c.srcY = sy;
        POINT org = {0, 0};
        if (!IsMemoryDc(dstDc) && GetDCOrgEx(dstDc, &org)) {
            c.screenX = org.x + x;
            c.screenY = org.y + y;
            c.hasScreen = true;
        }
        RecordBlit(c);

        // ---- 缩略图候选：同 CaptureSetDibits，昂贵检查后置 + 异步落盘
        if (!c.hasScreen || !srcDc || w < 8 || h < 8 || w > 512 || h > 512)
            return;
        // 源是 32bpp DIBSection 才拿得到像素
        HBITMAP srcBmp =
            static_cast<HBITMAP>(GetCurrentObject(srcDc, OBJ_BITMAP));
        DIBSECTION ds = {};
        if (!srcBmp || GetObjectW(srcBmp, sizeof(ds), &ds) != sizeof(ds) ||
            !ds.dsBm.bmBits || ds.dsBm.bmBitsPixel != 32 ||
            ds.dsBmih.biCompression != BI_RGB)
            return;
        if (!PointInAnyGroup(c.screenX, c.screenY)) return;
        const size_t stride =
            ((static_cast<size_t>(ds.dsBm.bmWidth) * 4) + 3) & ~size_t(3);
        TryEnqueueThumb(c, ds.dsBm.bmWidth, ds.dsBm.bmHeight,
                        ds.dsBmih.biHeight, ds.dsBm.bmBits,
                        stride * static_cast<size_t>(
                                     std::abs(ds.dsBm.bmHeight)));
    }

    // ------------------------------------------------------------ 序列化
    static void SerializeText(const TextCapture& c, std::string& out) {
        out += "{\"tick\":";
        JsonNum(static_cast<long long>(c.tick), out);
        out += ",\"fn\":"; JsonNum(c.fn, out);
        out += ",\"hwnd\":";
        JsonNum(static_cast<long long>(reinterpret_cast<uintptr_t>(c.hwnd)), out);
        out += ",\"cls\":"; JsonEscapeInto(std::wstring(c.cls), out);
        out += ",\"x\":"; JsonNum(c.x, out);
        out += ",\"y\":"; JsonNum(c.y, out);
        out += ",\"w\":"; JsonNum(c.w, out);
        out += ",\"h\":"; JsonNum(c.h, out);
        out += ",\"eto\":"; JsonNum(static_cast<long long>(c.eto), out);
        out += ",\"hasScreen\":"; out += c.hasScreen ? "true" : "false";
        out += ",\"screenX\":"; JsonNum(c.screenX, out);
        out += ",\"screenY\":"; JsonNum(c.screenY, out);
        out += ",\"text\":"; JsonEscapeInto(c.text, out);
        out += "}";
    }

    static void SerializeBlit(const BlitCapture& c, std::string& out) {
        out += "{\"tick\":";
        JsonNum(static_cast<long long>(c.tick), out);
        out += ",\"fn\":"; JsonNum(c.fn, out);
        out += ",\"hwnd\":";
        JsonNum(static_cast<long long>(reinterpret_cast<uintptr_t>(c.hwnd)), out);
        out += ",\"cls\":"; JsonEscapeInto(std::wstring(c.cls), out);
        out += ",\"hasScreen\":"; out += c.hasScreen ? "true" : "false";
        out += ",\"screenX\":"; JsonNum(c.screenX, out);
        out += ",\"screenY\":"; JsonNum(c.screenY, out);
        out += ",\"srcX\":"; JsonNum(c.srcX, out);
        out += ",\"srcY\":"; JsonNum(c.srcY, out);
        out += ",\"w\":"; JsonNum(c.w, out);
        out += ",\"h\":"; JsonNum(c.h, out);
        out += ",\"captured\":"; out += c.captured ? "true" : "false";
        out += ",\"file\":"; JsonEscapeInto(std::string(c.file), out);
        out += "}";
    }

    void SerializeLayers(std::string& out) {
        std::vector<std::pair<HWND, std::vector<RowInfo>>> snap;
        AcquireSRWLockShared(&lock);
        snap.assign(rowsByHwnd.begin(), rowsByHwnd.end());
        ReleaseSRWLockShared(&lock);

        out += "{\"ok\":true,\"groups\":[";
        long long bestScore = -1;
        HWND bestHwnd = nullptr;
        bool firstGroup = true;
        for (auto& kv : snap) {
            RECT wr = {};
            const bool haveWin = kv.first != nullptr && GetWindowRect(kv.first, &wr);
            if (kv.first != nullptr && !haveWin) continue; // 窗口已销毁
            wchar_t cls[96];
            CopyClassName(kv.first, cls);
            const bool isChild = kv.first && GetAncestor(kv.first, GA_ROOT) != kv.first;

            // panelGuess 评分：图层面板每个图层都有各自的"混合模式行 +
            // 不透明度行"，层数越多计数越多；笔刷列表/工具选项面板没有
            // 这种多重复结构。另有粘滞机制：确认过的面板后续优先。
            long long score = static_cast<long long>(kv.second.size());
            if (isChild) score += 2;
            else score -= 5; // 根窗口（菜单栏/标题）不是图层面板
            int blendCount = 0, percentCount = 0;
            bool hasLayerKw = false;
            for (const RowInfo& r : kv.second) {
                std::wstring v = r.text;
                const size_t b0 = v.find_first_not_of(L" 	");
                if (b0 == std::wstring::npos) continue;
                v = v.substr(b0, v.find_last_not_of(L" 	") - b0 + 1);
                if (v.rfind(L"图层", 0) == 0 || v.find(L"Layer") != std::wstring::npos)
                    hasLayerKw = true;
                if (v.length() <= 6 && v.back() == L'%') ++percentCount;
                if (v == L"正常" || v == L"正片叠底" || v == L"明暗" ||
                    v == L"滤色" || v == L"叠加" || v == L"发光" ||
                    v == L"阴影" || v == L"Normal" || v == L"Multiply" ||
                    v == L"Screen" || v == L"Overlay")
                    ++blendCount;
            }
            score += static_cast<long long>(std::min(percentCount, 5)) * 4;
            score += static_cast<long long>(std::min(blendCount, 5)) * 4;
            if (hasLayerKw) score += 3;
            if (kv.first == stickyPanel) score += 10; // 粘滞：确认过的面板
            if (score > bestScore && !kv.second.empty() && kv.first != nullptr) {
                bestScore = score;
                bestHwnd = kv.first;
            }

            if (!firstGroup) out += ",";
            firstGroup = false;
            out += "{\"hwnd\":";
            JsonNum(static_cast<long long>(reinterpret_cast<uintptr_t>(kv.first)),
                    out);
            out += ",\"cls\":"; JsonEscapeInto(std::wstring(cls), out);
            out += ",\"isChild\":"; out += isChild ? "true" : "false";
            out += ",\"screenRect\":[";
            JsonNum(haveWin ? wr.left : 0, out); out += ",";
            JsonNum(haveWin ? wr.top : 0, out); out += ",";
            JsonNum(haveWin ? wr.right : 0, out); out += ",";
            JsonNum(haveWin ? wr.bottom : 0, out); out += "]";
            out += ",\"rows\":[";
            bool firstRow = true;
            for (const RowInfo& r : kv.second) {
                if (!firstRow) out += ",";
                firstRow = false;
                out += "{\"y\":"; JsonNum(r.y, out);
                out += ",\"x\":"; JsonNum(r.x, out);
                out += ",\"w\":"; JsonNum(r.w, out);
                out += ",\"hasScreen\":"; out += r.hasScreen ? "true" : "false";
                out += ",\"screenX\":"; JsonNum(r.screenX, out);
                out += ",\"screenY\":"; JsonNum(r.screenY, out);
                out += ",\"tick\":";
                JsonNum(static_cast<long long>(r.tick), out);
                out += ",\"count\":"; JsonNum(r.count, out);
                out += ",\"text\":"; JsonEscapeInto(r.text, out);
                out += "}";
            }
            out += "]}";
        }
        out += "],\"panelGuess\":";
        JsonNum(static_cast<long long>(reinterpret_cast<uintptr_t>(bestHwnd)),
                out);
        out += "}";
        // 确认粘滞：面板含图层名特征 + 混合模式 + 不透明度才算数
        if (bestHwnd) {
            for (auto& kv2 : snap) {
                if (kv2.first != bestHwnd) continue;
                int pc = 0, bc = 0;
                bool kw = false;
                for (const RowInfo& r : kv2.second) {
                    std::wstring v = r.text;
                    const size_t b0 = v.find_first_not_of(L" 	");
                    if (b0 == std::wstring::npos) continue;
                    v = v.substr(b0, v.find_last_not_of(L" 	") - b0 + 1);
                    if (v.rfind(L"图层", 0) == 0) kw = true;
                    if (v.length() <= 6 && v.back() == L'%') ++pc;
                    if (v == L"正常" || v == L"正片叠底" || v == L"明暗") ++bc;
                }
                if (kw && pc >= 1 && bc >= 1) stickyPanel = bestHwnd;
                break;
            }
        }
    }

    // ------------------------------------------------------------ 堆扫描
    void ScanStrings(const std::wstring& substr, bool dump, std::string& out) {
        char logbuf[128];
        std::snprintf(logbuf, sizeof(logbuf),
                      "scan.strings substr-chars=%d dump=%d",
                      static_cast<int>(substr.size()), dump ? 1 : 0);
        Log(logbuf);

        struct Hit {
            const void* addr;
            int lenChars;
            std::wstring text;
        };
        // 页级可读性校验：堆扫描读的是宿主进程内存，先验证再解引用
        struct RangeGuard {
            static bool Readable(const void* p, size_t bytes) {
                if (!p) return false;
                const uintptr_t start = reinterpret_cast<uintptr_t>(p);
                const uintptr_t end = start + bytes;
                uintptr_t cur = start;
                MEMORY_BASIC_INFORMATION mbi = {};
                while (cur < end) {
                    if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi,
                                     sizeof(mbi)) == 0)
                        return false;
                    if (mbi.State != MEM_COMMIT) return false;
                    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
                    const uintptr_t regionEnd =
                        reinterpret_cast<uintptr_t>(mbi.BaseAddress) +
                        mbi.RegionSize;
                    if (regionEnd <= cur) return false;
                    cur = regionEnd;
                }
                return true;
            }
        };
        std::vector<Hit> hits;

        const ULONGLONG t0 = GetTickCount64();
        HANDLE heaps[64];
        const DWORD nHeaps = GetProcessHeaps(64, heaps);
        for (DWORD hi = 0; hi < nHeaps && hits.size() < 64; ++hi) {
            if (GetTickCount64() - t0 > 2000) break; // 时间兜底
            HANDLE heap = heaps[hi];
            if (!HeapLock(heap)) continue;
            PROCESS_HEAP_ENTRY e = {};
            if (HeapWalk(heap, &e)) {
                do {
                    if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
                    if (e.cbData < 4 || e.cbData > (1u << 20)) continue;
                    if (substr.empty()) continue;
                    const wchar_t* p = static_cast<const wchar_t*>(e.lpData);
                    const size_t chars = e.cbData / 2;
                    if (chars < substr.size()) continue;
                    if (!RangeGuard::Readable(p, e.cbData)) continue;
                    for (size_t off = 0; off + substr.size() <= chars; ++off) {
                        if (std::wcsncmp(p + off, substr.c_str(),
                                         substr.size()) == 0) {
                            // 往回找 NUL/不可打印边界，得到完整"疑似字符串"
                            const wchar_t* start = p + off;
                            int back = 0;
                            while (back < 256 && start > p &&
                                   start[-1] != L'\0' && start[-1] >= 0x20) {
                                --start; ++back;
                            }
                            int fwd = 0;
                            while (fwd < 512 && start[fwd] != L'\0' &&
                                   start[fwd] >= 0x20)
                                ++fwd;
                            Hit h;
                            h.addr = start;
                            h.lenChars = fwd;
                            h.text.assign(start, static_cast<size_t>(fwd));
                            hits.push_back(std::move(h));
                            break;
                        }
                    }
                } while (hits.size() < 64 && HeapWalk(heap, &e));
            }
            HeapUnlock(heap);
        }

        if (dump && !hits.empty()) {
            std::wstring scanPath = outDir + L"\\scan_strings.txt";
            HANDLE f = CreateFileW(scanPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                std::string report;
                char line[256];
                for (const Hit& h : hits) {
                    std::snprintf(line, sizeof(line),
                                  "addr=0x%p len=%d text=[", h.addr, h.lenChars);
                    report += line;
                    std::string esc;
                    JsonEscapeInto(h.text, esc);
                    report += esc.substr(1, esc.size() - 2);
                    report += "]\n";
                    const uint8_t* base =
                        static_cast<const uint8_t*>(h.addr) - 64;
                    if (!RangeGuard::Readable(base, 8 * 16)) continue;
                    for (int rowI = 0; rowI < 8; ++rowI) {
                        std::snprintf(line, sizeof(line), "  %p: ",
                                      base + rowI * 16);
                        report += line;
                        for (int b = 0; b < 16; ++b) {
                            std::snprintf(line, sizeof(line), "%02x ",
                                          base[rowI * 16 + b]);
                            report += line;
                        }
                        report += "\n";
                    }
                    report += "\n";
                }
                DWORD w = 0;
                WriteFile(f, report.data(),
                          static_cast<DWORD>(report.size()), &w, nullptr);
                CloseHandle(f);
            }
        }

        out += "{\"ok\":true,\"hits\":[";
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i) out += ",";
            out += "{\"addr\":";
            JsonNum(static_cast<long long>(
                        reinterpret_cast<uintptr_t>(hits[i].addr)),
                    out);
            out += ",\"len\":";
            JsonNum(hits[i].lenChars, out);
            out += ",\"text\":";
            JsonEscapeInto(hits[i].text, out);
            out += "}";
        }
        out += "]}";
    }
};

// ---------------------------------------------------------------------------
// Hook 实现
// ---------------------------------------------------------------------------

namespace {

// 缩略图写盘后台线程：绘画热路径上绝不碰磁盘。
// 成功落盘后补记一条 captured=true 的 blit 记录（thumbs 命令消费）。
DWORD WINAPI ThumbThreadProc(LPVOID param) {
    AgentCore::Impl* impl = static_cast<AgentCore::Impl*>(param);
    while (!impl->thumbStop) {
        WaitForSingleObject(impl->thumbEvent, 400);
        for (;;) {
            AgentCore::Impl::ThumbJob job;
            EnterCriticalSection(&impl->thumbLock);
            if (impl->thumbJobs.empty()) {
                LeaveCriticalSection(&impl->thumbLock);
                break;
            }
            job = std::move(impl->thumbJobs.front());
            impl->thumbJobs.pop_front();
            LeaveCriticalSection(&impl->thumbLock);

            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%s\\thumb_%06ld.bmp",
                     impl->thumbDir.c_str(), ++impl->thumbSeq);
            if (WriteBmpFromDib32(path, job.w, job.h, job.declaredHeight,
                                  job.pixels.data(), job.pixels.size())) {
                BlitCapture c = job.meta;
                c.captured = true;
                CopyPathToChar(path, c.file);
                impl->RecordBlit(c);
            }
        }
    }
    return 0;
}

BOOL WINAPI HookedExtTextOutW(HDC hdc, int x, int y, UINT options,
                              const RECT* lprc, LPCWSTR str, int cch,
                              const INT* dx) {
    const BOOL r = reinterpret_cast<FnExtTextOutW>(g_origExtTextOutW)(
        hdc, x, y, options, lprc, str, cch, dx);
    if (g_core && !g_core->ExitRequested() && g_inCapture == 0 && str &&
        cch > 0) {
        ++g_inCapture;
            TextCapture c;
            c.tick = GetTickCount64();
            c.fn = FN_EXTTXTOUT;
            c.eto = options;
            c.hwnd = WindowFromDC(hdc);
            CopyClassName(c.hwnd, c.cls);
            c.x = x;
            c.y = y;
            // 宽度：优先字形 advance 数组，否则量一遍
            SIZE sz = {0, 0};
            if (dx && !(options & ETO_GLYPH_INDEX)) {
                LONG acc = 0;
                for (int i = 0; i < cch; ++i) acc += dx[i];
                sz.cx = acc;
                GetTextExtentPoint32W(hdc, str, cch, &sz); // 取 cy
                sz.cx = acc;
            } else {
                GetTextExtentPoint32W(hdc, str, cch, &sz);
            }
            c.w = static_cast<int>(sz.cx);
            c.h = static_cast<int>(sz.cy);
            POINT org = {0, 0};
            if (!IsMemoryDc(hdc) && GetDCOrgEx(hdc, &org)) {
                c.hasScreen = true;
                c.screenX = org.x + x;
                c.screenY = org.y + y;
            }
            c.text.assign(str, static_cast<size_t>(cch));
            g_core->ImplPtr()->RecordText(c);
            --g_inCapture;
    }
    return r;
}

int WINAPI HookedDrawTextW(HDC hdc, LPWSTR str, int cch, LPRECT rect,
                           UINT format) {
    const int r = reinterpret_cast<FnDrawTextW>(g_origDrawTextW)(hdc, str, cch,
                                                                 rect, format);
    if (g_core && !g_core->ExitRequested() && g_inCapture == 0 && str &&
        cch != 0 && rect && !(format & DT_CALCRECT)) {
        ++g_inCapture;
            TextCapture c;
            c.tick = GetTickCount64();
            c.fn = FN_DRAWTEXT;
            c.hwnd = WindowFromDC(hdc);
            CopyClassName(c.hwnd, c.cls);
            c.x = rect->left;
            c.y = rect->top;
            c.w = rect->right - rect->left;
            c.h = rect->bottom - rect->top;
            POINT org = {0, 0};
            if (!IsMemoryDc(hdc) && GetDCOrgEx(hdc, &org)) {
                c.hasScreen = true;
                c.screenX = org.x + rect->left;
                c.screenY = org.y + rect->top;
            }
            const int len = cch < 0 ? static_cast<int>(wcslen(str)) : cch;
            if (len > 0) c.text.assign(str, static_cast<size_t>(len));
            g_core->ImplPtr()->RecordText(c);
            --g_inCapture;
    }
    return r;
}

int WINAPI HookedSetDIBitsToDevice(HDC hdc, int xDst, int yDst, DWORD cx,
                                   DWORD cy, int xSrc, int ySrc, UINT startScan,
                                   UINT cLines, const void* lpvBits,
                                   const BITMAPINFO* lpBMI, UINT colorUse) {
    const int r =
        reinterpret_cast<FnSetDIBitsToDevice>(g_origSetDIBitsToDevice)(
            hdc, xDst, yDst, cx, cy, xSrc, ySrc, startScan, cLines, lpvBits,
            lpBMI, colorUse);
    if (g_core && !g_core->ExitRequested() && g_inCapture == 0) {
        ++g_inCapture;
        g_core->ImplPtr()->CaptureSetDibits(hdc, xDst, yDst,
                                            static_cast<int>(cx),
                                            static_cast<int>(cy), lpvBits,
                                            lpBMI, colorUse, startScan,
                                            cLines, xSrc, ySrc);
        --g_inCapture;
    }
    return r;
}

BOOL WINAPI HookedBitBlt(HDC dst, int x, int y, int cx, int cy, HDC src,
                         int x1, int y1, DWORD rop) {
    const BOOL r = reinterpret_cast<FnBitBlt>(g_origBitBlt)(dst, x, y, cx, cy,
                                                            src, x1, y1, rop);
    if (g_core && !g_core->ExitRequested() && g_inCapture == 0 &&
        rop == SRCCOPY) {
        ++g_inCapture;
        g_core->ImplPtr()->CaptureBitBlt(dst, x, y, cx, cy, src, x1, y1);
        --g_inCapture;
    }
    return r;
}

} // namespace

// ---------------------------------------------------------------------------
// AgentCore 公共接口
// ---------------------------------------------------------------------------

bool AgentCore::Startup(const std::wstring& outDir, std::wstring& error) {
    if (impl_) return true;
    impl_ = new Impl();
    InitializeCriticalSection(&impl_->logLock);

    // 落盘目录：参数为空 → DLL 所在目录
    if (outDir.empty()) {
        wchar_t dllPath[MAX_PATH] = {0};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), dllPath,
                           MAX_PATH);
        std::wstring s(dllPath);
        const size_t p = s.find_last_of(L"\\/");
        impl_->outDir = (p != std::wstring::npos) ? s.substr(0, p) : L".";
    } else {
        impl_->outDir = outDir;
    }
    impl_->thumbDir = impl_->outDir + L"\\thumbs";
    CreateDirectoryW(impl_->thumbDir.c_str(), nullptr);
    impl_->InitThumbs();
    impl_->thumbThread =
        CreateThread(nullptr, 0, ThumbThreadProc, impl_, 0, nullptr);

    // 日志文件（按 PID 区分多个宿主进程）
    wchar_t logp[MAX_PATH];
    swprintf(logp, MAX_PATH, L"%s\\sai2_agent_%lu.log", impl_->outDir.c_str(),
             GetCurrentProcessId());
    logPath_ = logp;
    impl_->logFile = CreateFileW(logp, FILE_APPEND_DATA, FILE_SHARE_READ,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    impl_->Log("=== sai2_agent startup ===");

    // 安装 IAT 钩子
    static IATHook hooks;
    g_origExtTextOutW =
        hooks.Install("GDI32.dll", "ExtTextOutW",
                      reinterpret_cast<void*>(&HookedExtTextOutW));
    g_origDrawTextW =
        hooks.Install("USER32.dll", "DrawTextW",
                      reinterpret_cast<void*>(&HookedDrawTextW));
    g_origSetDIBitsToDevice =
        hooks.Install("GDI32.dll", "SetDIBitsToDevice",
                      reinterpret_cast<void*>(&HookedSetDIBitsToDevice));
    g_origBitBlt = hooks.Install("GDI32.dll", "BitBlt",
                                 reinterpret_cast<void*>(&HookedBitBlt));
    g_hooks = &hooks;
    g_core = this;

    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "hooks installed: ext=%d draw=%d setdib=%d bitblt=%d",
                  g_origExtTextOutW ? 1 : 0, g_origDrawTextW ? 1 : 0,
                  g_origSetDIBitsToDevice ? 1 : 0, g_origBitBlt ? 1 : 0);
    impl_->Log(buf);
    if (!g_origExtTextOutW) {
        error = L"ExtTextOutW IAT hook 安装失败（宿主可能不经 IAT 调用）";
    }
    return true;
}

void AgentCore::Shutdown() {
    if (!impl_) return;
    impl_->Log("=== sai2_agent shutdown ===");
    impl_->ShutdownThumbs();
    if (g_hooks) {
        g_hooks->RestoreAll();
        g_hooks = nullptr;
    }
    g_core = nullptr;
    if (impl_->logFile && impl_->logFile != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->logFile);
    DeleteCriticalSection(&impl_->logLock);
    delete impl_;
    impl_ = nullptr;
}

void AgentCore::LogText(const char* line) {
    if (impl_) impl_->Log(line);
}

void AgentCore::SetPluginItems(const std::vector<PluginItem>& items) {
    if (!impl_) return;
    AcquireSRWLockExclusive(&impl_->lock);
    impl_->pluginItems = items;
    ReleaseSRWLockExclusive(&impl_->lock);
    NotifyBarUpdated(); // 浮窗立即重绘新按钮
}

std::vector<AgentCore::PluginItem> AgentCore::CopyPluginItems() {
    if (!impl_) return {};
    AcquireSRWLockShared(&impl_->lock);
    std::vector<PluginItem> out = impl_->pluginItems;
    ReleaseSRWLockShared(&impl_->lock);
    return out;
}

void AgentCore::SetPendingRequest(int effectIndex) {
    if (!impl_) return;
    AcquireSRWLockExclusive(&impl_->lock);
    impl_->hasPending = true;
    impl_->pendingIndex = effectIndex;
    ReleaseSRWLockExclusive(&impl_->lock);
}

bool AgentCore::TakePendingRequest(int& effectIndex) {
    if (!impl_) return false;
    AcquireSRWLockExclusive(&impl_->lock);
    if (!impl_->hasPending) {
        ReleaseSRWLockExclusive(&impl_->lock);
        return false;
    }
    impl_->hasPending = false;
    effectIndex = impl_->pendingIndex;
    ReleaseSRWLockExclusive(&impl_->lock);
    return true;
}

void AgentCore::HandleCommand(const std::string& line, std::string& out) {
    // cmd 提取用字面量定位（set.plugins 的负载带嵌套数组，扁平解析器会失败）
    std::wstring cmd;
    {
        const std::string pat = "\"cmd\":\"";
        const size_t p = line.find(pat);
        if (p != std::string::npos) {
            size_t i = p + pat.size();
            std::string raw;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    ++i;
                    raw += line[i++];
                } else {
                    raw += line[i++];
                }
            }
            cmd = U8ToWide(raw);
        }
    }
    std::vector<std::pair<std::wstring, JsonVal>> kv;
    JsonParseObject(line, kv); // 其余键（扁平命令的附加参数用）

    if (cmd.empty()) {
        out = "{\"ok\":false,\"error\":\"missing cmd\"}";
        return;
    }

    if (cmd == L"ping") {
        out = "{\"ok\":true,\"agent\":\"sai2_agent/1.0\",\"pid\":";
        JsonNum(GetCurrentProcessId(), out);
        out += ",\"hooks\":{\"ExtTextOutW\":";
        out += g_origExtTextOutW ? "true" : "false";
        out += ",\"DrawTextW\":";
        out += g_origDrawTextW ? "true" : "false";
        out += ",\"SetDIBitsToDevice\":";
        out += g_origSetDIBitsToDevice ? "true" : "false";
        out += ",\"BitBlt\":";
        out += g_origBitBlt ? "true" : "false";
        out += "},\"log\":";
        JsonEscapeInto(std::wstring(logPath_), out);
        out += "}";
        return;
    }

    if (cmd == L"reset") {
        AcquireSRWLockExclusive(&impl_->lock);
        impl_->textRing.clear();
        impl_->blitRing.clear();
        impl_->rowsByHwnd.clear();
        ReleaseSRWLockExclusive(&impl_->lock);
        impl_->Log("reset by command");
        out = "{\"ok\":true}";
        return;
    }

    if (cmd == L"text.recent") {
        long long n = 400;
        for (auto& p : kv)
            if (p.first == L"n" && p.second.isNum) n = p.second.num;
        std::deque<TextCapture> snap;
        AcquireSRWLockShared(&impl_->lock);
        for (const auto& c : impl_->textRing) {
            snap.push_back(c);
            if (static_cast<long long>(snap.size()) > n) snap.pop_front();
        }
        ReleaseSRWLockShared(&impl_->lock);
        out = "{\"ok\":true,\"items\":[";
        for (size_t i = 0; i < snap.size(); ++i) {
            if (i) out += ",";
            Impl::SerializeText(snap[i], out);
        }
        out += "]}";
        return;
    }

    if (cmd == L"layers") {
        impl_->SerializeLayers(out);
        return;
    }

    if (cmd == L"thumbs") {
        std::deque<BlitCapture> snap;
        AcquireSRWLockShared(&impl_->lock);
        snap = impl_->blitRing;
        ReleaseSRWLockShared(&impl_->lock);
        out = "{\"ok\":true,\"items\":[";
        for (size_t i = 0; i < snap.size(); ++i) {
            if (i) out += ",";
            Impl::SerializeBlit(snap[i], out);
        }
        out += "]}";
        return;
    }

    if (cmd == L"set.plugins") {
        // {"cmd":"set.plugins","items":[{"i":1,"g":"模糊","n":"模糊"},...]}
        // i = 功能窗口索引（1 模糊 / 2 滤镜 / 3 调整 / 4 液化），由桥定义
        std::vector<PluginItem> items;
        for (const std::string& obj : JsonArrayObjects(line, "items")) {
            std::vector<std::pair<std::wstring, JsonVal> > kv;
            if (!JsonParseObject(obj, kv)) continue;
            PluginItem it;
            for (auto& pr : kv) {
                if (pr.first == L"i" && pr.second.isNum)
                    it.index = static_cast<int>(pr.second.num);
                else if (pr.first == L"g" && pr.second.isStr)
                    it.group = pr.second.str;
                else if (pr.first == L"n" && pr.second.isStr)
                    it.name = pr.second.str;
            }
            items.push_back(it);
        }
        SetPluginItems(items);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "set.plugins: %d items",
                      static_cast<int>(items.size()));
        impl_->Log(buf);
        out = "{\"ok\":true,\"count\":";
        JsonNum(static_cast<long long>(items.size()), out);
        out += "}";
        return;
    }

    if (cmd == L"take.request") {
        int idx = -1;
        out = "{\"ok\":true,\"request\":";
        if (TakePendingRequest(idx)) {
            out += "{\"index\":";
            JsonNum(idx, out);
            out += "}";
        } else {
            out += "null";
        }
        out += "}";
        return;
    }

    if (cmd == L"scan.strings") {
        std::wstring substr;
        bool dump = false;
        for (auto& p : kv) {
            if (p.first == L"substr" && p.second.isStr) substr = p.second.str;
            if (p.first == L"dump" && p.second.isBool) dump = p.second.boolean;
        }
        impl_->ScanStrings(substr, dump, out);
        return;
    }

    out = "{\"ok\":false,\"error\":\"unknown cmd\"}";
}

} // namespace sai2agent
