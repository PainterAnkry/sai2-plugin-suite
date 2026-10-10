#pragma once
// ============================================================================
// BmpIO.h - 最简 BMP 读写（自检模式导出结果图用，无第三方依赖）
//
// 统一按 32bpp BI_RGB 自下而上写出，Windows 照片查看器 / 画图可直接打开。
// ============================================================================

#include "sai2/ImageBuffer.h"

#include <string>

namespace sai2::bridge {

// 写出 32bpp BMP；失败返回 false
bool WriteBmp32(const ImageBuffer& img, const std::string& path,
                std::string& error);

// 读入 24/32bpp BMP（自检里核对导出的文件）；统一转 RGBA8
bool ReadBmp32(const std::string& path, ImageBuffer& out, std::string& error);

} // namespace sai2::bridge
