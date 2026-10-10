// ============================================================================
// ClipboardImage.cpp - 剪贴板 DIB 读写实现
// ============================================================================

#include "ClipboardImage.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <vector>

namespace sai2::bridge {
namespace {

// 打开剪贴板（其它程序可能短暂占用，重试若干次）
bool OpenClipboardRetry(HWND owner, int attempts = 10) {
    for (int i = 0; i < attempts; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

// DIB 解析结果：像素已是 RGBA8，自上而下
struct DibView {
    const uint8_t* bits = nullptr;
    int width = 0;
    int height = 0;      // 已取绝对值
    int bitCount = 0;
    int stride = 0;      // 源行字节数（4 字节对齐）
    bool topDown = false;
    bool hasAlpha = false;
};

// 从 DIB 头部（BITMAPINFOHEADER 系列）解析布局，返回像素数据偏移
bool ParseDib(const uint8_t* base, size_t size, DibView& v, size_t& pixelOffset) {
    if (size < sizeof(BITMAPINFOHEADER)) return false;

    uint32_t headerSize = 0;
    std::memcpy(&headerSize, base, sizeof(uint32_t));
    if (headerSize < sizeof(BITMAPINFOHEADER) || headerSize > size) return false;

    BITMAPINFOHEADER ih{};
    std::memcpy(&ih, base, sizeof(BITMAPINFOHEADER));

    if (ih.biPlanes != 1) return false;
    if (ih.biBitCount != 24 && ih.biBitCount != 32) return false;
    if (ih.biWidth <= 0 || ih.biHeight == 0) return false;

    v.width = static_cast<int>(ih.biWidth);
    v.height = ih.biHeight < 0 ? -static_cast<int>(ih.biHeight)
                               : static_cast<int>(ih.biHeight);
    v.bitCount = ih.biBitCount;
    v.topDown = ih.biHeight < 0;
    v.stride = ((v.width * v.bitCount + 31) / 32) * 4;

    if (ih.biCompression != BI_RGB && ih.biCompression != BI_BITFIELDS)
        return false; // 暂不支持 RLE 等压缩 DIB

    // ---- 像素起点（这里必须同时兼容两种真实存在的布局）------------------
    // 实测 GDI/GDI+ 经 CF_BITMAP 合成出的 CF_DIBV5 是：
    //     BITMAPV5HEADER(124) + 3 个颜色掩码(12) + 像素           -> 起点 136
    // 而本工程自己写出的 CF_DIBV5 是：
    //     BITMAPV5HEADER(124) + 像素                              -> 起点 124
    // 两者都合法（GDI 对 BI_BITFIELDS 会重复写一遍掩码）。仅靠压缩标志
    // 无法区分，因此用"缓冲区总大小 - 像素数据大小"反推起点，最可靠：
    // 剪贴板里的 DIB 就是 头(+掩码) + 像素，没有别的尾巴。
    // （早期版本无条件按"V5 不加 12"处理，遇到 GDI 的布局会让整幅图右移
    //   3 个像素并环绕 —— 12 字节 ÷ 4 字节/像素。）
    constexpr uint32_t kV4HeaderSize = 108; // 掩码已含在头内的头大小下限
    const bool masksInHeader = headerSize >= kV4HeaderSize;

    size_t fallback = headerSize;
    if (!masksInHeader && ih.biCompression == BI_BITFIELDS) fallback += 12;

    const size_t need = static_cast<size_t>(v.stride) * v.height;
    if (size < need) return false;
    const size_t derived = size - need;
    pixelOffset = (derived >= headerSize && derived <= headerSize + 16)
                      ? derived
                      : fallback;

    if (masksInHeader) {
        // V4/V5 的掩码字段偏移一致：红 40 / 绿 44 / 蓝 48 / alpha 52
        uint32_t alphaMask = 0;
        std::memcpy(&alphaMask, base + 52, sizeof(uint32_t));
        // BI_RGB 的 V4/V5 头里 alpha 通道通常无效，按不透明处理
        v.hasAlpha = (alphaMask == 0xFF000000u) && (ih.biCompression != BI_RGB);
    } else {
        v.hasAlpha = false; // 老式 DIB 无 alpha
    }

    if (pixelOffset >= size) return false;
    if (size - pixelOffset < need) return false;

    v.bits = base + pixelOffset;
    return true;
}

// 把 DIB 像素拷进 ImageBuffer（RGBA8，自上而下）
void DibToImage(const DibView& v, ImageBuffer& out) {
    out.Create(v.width, v.height, PixelFormat::RGBA8);
    uint8_t* dst = out.Data();
    if (!dst) return;

    for (int y = 0; y < v.height; ++y) {
        const int srcRow = v.topDown ? y : (v.height - 1 - y);
        const uint8_t* row = v.bits + static_cast<size_t>(srcRow) * v.stride;
        uint8_t* outRow = dst + static_cast<size_t>(y) * out.Stride();
        for (int x = 0; x < v.width; ++x) {
            const uint8_t b = row[x * (v.bitCount / 8) + 0];
            const uint8_t g = row[x * (v.bitCount / 8) + 1];
            const uint8_t r = row[x * (v.bitCount / 8) + 2];
            const uint8_t a = (v.bitCount == 32)
                                  ? row[x * 4 + 3]
                                  : static_cast<uint8_t>(255);
            outRow[x * 4 + 0] = r;
            outRow[x * 4 + 1] = g;
            outRow[x * 4 + 2] = b;
            // 无 alpha 通道，或 alpha 全 0（部分程序不填 alpha）时按不透明处理
            outRow[x * 4 + 3] = v.hasAlpha ? a : 255;
        }
    }

    // 修正：DIBV5 声明了 alpha 掩码但整幅 alpha 全 0 —— 视为不透明
    if (v.hasAlpha) {
        bool anyAlpha = false;
        const size_t n = static_cast<size_t>(v.width) * v.height;
        for (size_t i = 0; i < n; ++i) {
            if (dst[i * 4 + 3] != 0) {
                anyAlpha = true;
                break;
            }
        }
        if (!anyAlpha)
            for (size_t i = 0; i < n; ++i) dst[i * 4 + 3] = 255;
    }
}

// 构造自下而上的 BGRA 像素块（DIB 约定）
std::vector<uint8_t> BuildBgraBottomUp(const ImageBuffer& img, int& stride) {
    stride = static_cast<int>(((img.Width() * 32 + 31) / 32) * 4);
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * img.Height(), 0);
    for (int y = 0; y < img.Height(); ++y) {
        const int dstRow = img.Height() - 1 - y; // 自下而上
        const uint8_t* src = img.Data() + static_cast<size_t>(y) * img.Stride();
        uint8_t* row = buf.data() + static_cast<size_t>(dstRow) * stride;
        for (int x = 0; x < img.Width(); ++x) {
            row[x * 4 + 0] = src[x * 4 + 2]; // B
            row[x * 4 + 1] = src[x * 4 + 1]; // G
            row[x * 4 + 2] = src[x * 4 + 0]; // R
            row[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }
    return buf;
}

// 分配可放入剪贴板的全局内存并拷入数据；失败返回 nullptr
HGLOBAL AllocGlobal(const void* data, size_t bytes) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return nullptr;
    }
    std::memcpy(p, data, bytes);
    GlobalUnlock(h);
    return h;
}

} // namespace

bool ClipboardHasImage() {
    return IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB);
}

bool ReadClipboardImage(ImageBuffer& out, std::string& error,
                        std::string* formatName) {
    error.clear();
    if (!OpenClipboardRetry(nullptr)) {
        error = "打开剪贴板失败（可能被其它程序占用）";
        return false;
    }

    bool ok = false;
    const UINT formats[2] = {CF_DIBV5, CF_DIB};
    for (UINT fmt : formats) {
        if (!IsClipboardFormatAvailable(fmt)) continue;
        HANDLE h = GetClipboardData(fmt);
        if (!h) continue;
        const uint8_t* base = static_cast<const uint8_t*>(GlobalLock(h));
        if (!base) continue;
        const SIZE_T bytes = GlobalSize(h);

        DibView v;
        size_t offset = 0;
        if (ParseDib(base, static_cast<size_t>(bytes), v, offset)) {
            DibToImage(v, out);
            ok = out.IsValid();
            if (ok && formatName)
                *formatName = (fmt == CF_DIBV5) ? "CF_DIBV5" : "CF_DIB";
        } else {
            error = "剪贴板 DIB 布局不受支持（仅支持 24/32bpp 未压缩）";
        }
        GlobalUnlock(h);
        if (ok) break;
    }

    CloseClipboard();
    if (!ok && error.empty()) error = "剪贴板中没有位图";
    return ok;
}

bool WriteClipboardImage(const ImageBuffer& img, std::string& error) {
    error.clear();
    if (!img.IsValid()) {
        error = "图像无效";
        return false;
    }

    // 统一成 RGBA8 再转换（RGBA16 由调用方决定是否降位）
    ImageBuffer src = img.Format() == PixelFormat::RGBA8
                          ? img
                          : ImageBuffer::Convert(img, PixelFormat::RGBA8);
    if (!src.IsValid()) {
        error = "图像转换失败";
        return false;
    }

    int stride = 0;
    const std::vector<uint8_t> pixels = BuildBgraBottomUp(src, stride);
    const size_t pixelBytes = pixels.size();

    // --- CF_DIBV5（唯一格式）--------------------------------------------
    // 实测（2026-10-06）：SAI2 自己拷贝透明图层时写 CF_BITMAP+CF_DIB+CF_DIBV5，
    // 其 DIBV5 是 GDI 风格布局：BITMAPV5HEADER(124) + 3 个掩码(12) + 像素
    // （像素起点 136）。为最大化 SAI2 粘贴端的兼容性，本写入器完全模仿
    // 该布局。注意不要同时提供 CF_DIB：SAI2 的粘贴会优先读无 alpha 语义的
    // CF_DIB，透明像素按不透明合成 → 粘贴出来背景变黑（实测踩坑）。
    BITMAPV5HEADER v5{};
    v5.bV5Size = sizeof(BITMAPV5HEADER);
    v5.bV5Width = src.Width();
    v5.bV5Height = src.Height(); // 正数 = 自下而上
    v5.bV5Planes = 1;
    v5.bV5BitCount = 32;
    v5.bV5Compression = BI_BITFIELDS;
    v5.bV5SizeImage = static_cast<DWORD>(pixelBytes);
    v5.bV5RedMask = 0x00FF0000u;
    v5.bV5GreenMask = 0x0000FF00u;
    v5.bV5BlueMask = 0x000000FFu;
    v5.bV5AlphaMask = 0xFF000000u;
    v5.bV5CSType = 0; // SAI2 自己写的也是 0（不声明 sRGB）

    std::vector<uint8_t> blob(sizeof(BITMAPV5HEADER) + 12 + pixelBytes, 0);
    std::memcpy(blob.data(), &v5, sizeof(v5));
    // GDI 风格：头之后再写一遍三个颜色掩码（alpha 掩码不重复）
    uint32_t masks[3] = {v5.bV5RedMask, v5.bV5GreenMask, v5.bV5BlueMask};
    std::memcpy(blob.data() + sizeof(v5), masks, sizeof(masks));
    std::memcpy(blob.data() + sizeof(v5) + 12, pixels.data(), pixelBytes);

    if (!OpenClipboardRetry(nullptr)) {
        error = "打开剪贴板失败（可能被其它程序占用）";
        return false;
    }
    if (!EmptyClipboard()) {
        CloseClipboard();
        error = "清空剪贴板失败";
        return false;
    }

    bool ok = false;
    if (HGLOBAL h5 = AllocGlobal(blob.data(), blob.size())) {
        if (SetClipboardData(CF_DIBV5, h5)) {
            ok = true; // 所有权已移交系统，不可再 free
        } else {
            GlobalFree(h5);
        }
    }

    CloseClipboard();
    if (!ok) error = "写入剪贴板失败";
    return ok;
}

} // namespace sai2::bridge
