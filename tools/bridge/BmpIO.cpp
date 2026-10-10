// ============================================================================
// BmpIO.cpp
// ============================================================================

#include "BmpIO.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace sai2::bridge {

namespace {

// UTF-8 路径 → 宽字符 FILE*：Windows 上 fopen 按 ANSI 代码页解释窄路径，
// 所在目录名含中文（例如「SAI2插件测试包」）时会直接打不开文件。
FILE* OpenFileUtf8(const std::string& path, const wchar_t* mode) {
#if defined(_WIN32)
    if (path.empty()) return nullptr;
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                      static_cast<int>(path.size()), nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                        w.data(), n);
    return _wfopen(w.c_str(), mode);
#else
    (void)mode;
    return nullptr;
#endif
}

} // namespace

namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t bfType = 0x4D42; // "BM"
    uint32_t bfSize = 0;
    uint16_t bfReserved1 = 0;
    uint16_t bfReserved2 = 0;
    uint32_t bfOffBits = 0;
};
struct BmpInfoHeader {
    uint32_t biSize = 40;
    int32_t biWidth = 0;
    int32_t biHeight = 0;
    uint16_t biPlanes = 1;
    uint16_t biBitCount = 32;
    uint32_t biCompression = 0;
    uint32_t biSizeImage = 0;
    int32_t biXPelsPerMeter = 2835;
    int32_t biYPelsPerMeter = 2835;
    uint32_t biClrUsed = 0;
    uint32_t biClrImportant = 0;
};
#pragma pack(pop)

} // namespace

bool WriteBmp32(const ImageBuffer& img, const std::string& path,
                std::string& error) {
    error.clear();
    if (!img.IsValid()) {
        error = "图像无效";
        return false;
    }
    const ImageBuffer src = img.Format() == PixelFormat::RGBA8
                                ? img
                                : ImageBuffer::Convert(img, PixelFormat::RGBA8);

    const int w = src.Width(), h = src.Height();
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    const size_t pixelBytes = rowBytes * h;

    BmpFileHeader fh;
    BmpInfoHeader ih;
    fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize = static_cast<uint32_t>(fh.bfOffBits + pixelBytes);
    ih.biWidth = w;
    ih.biHeight = h; // 正数 = 自下而上
    ih.biSizeImage = static_cast<uint32_t>(pixelBytes);

    FILE* f = OpenFileUtf8(path, L"wb");
    if (!f) {
        error = "无法写入文件: " + path;
        return false;
    }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);

    std::vector<uint8_t> row(rowBytes);
    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* p = src.Data() + static_cast<size_t>(y) * src.Stride();
        for (int x = 0; x < w; ++x) {
            row[x * 4 + 0] = p[x * 4 + 2]; // B
            row[x * 4 + 1] = p[x * 4 + 1]; // G
            row[x * 4 + 2] = p[x * 4 + 0]; // R
            row[x * 4 + 3] = p[x * 4 + 3]; // A
        }
        std::fwrite(row.data(), rowBytes, 1, f);
    }
    std::fclose(f);
    return true;
}

bool ReadBmp32(const std::string& path, ImageBuffer& out, std::string& error) {
    error.clear();
    FILE* f = OpenFileUtf8(path, L"rb");
    if (!f) {
        error = "无法打开文件: " + path;
        return false;
    }
    BmpFileHeader fh{};
    BmpInfoHeader ih{};
    if (std::fread(&fh, sizeof(fh), 1, f) != 1 ||
        std::fread(&ih, sizeof(ih), 1, f) != 1 || fh.bfType != 0x4D42) {
        std::fclose(f);
        error = "BMP 头部无效";
        return false;
    }
    if (ih.biBitCount != 24 && ih.biBitCount != 32) {
        std::fclose(f);
        error = "仅支持 24/32bpp BMP";
        return false;
    }
    const int w = ih.biWidth;
    const int h = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    const bool topDown = ih.biHeight < 0;
    const int bpp = ih.biBitCount / 8;
    const size_t stride = ((static_cast<size_t>(w) * ih.biBitCount + 31) / 32) * 4;

    out.Create(w, h, PixelFormat::RGBA8);
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < h; ++y) {
        if (std::fread(row.data(), stride, 1, f) != 1) break;
        const int dstY = topDown ? y : (h - 1 - y);
        uint8_t* dst = out.Data() + static_cast<size_t>(dstY) * out.Stride();
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * bpp + 2];
            dst[x * 4 + 1] = row[x * bpp + 1];
            dst[x * 4 + 2] = row[x * bpp + 0];
            dst[x * 4 + 3] = (bpp == 4) ? row[x * 4 + 3] : 255;
        }
    }
    std::fclose(f);
    return true;
}

} // namespace sai2::bridge
