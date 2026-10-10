#pragma once
// ============================================================================
// ClipboardImage.h - SAI2 剪贴板图像桥（Win32 标准剪贴板，无第三方依赖）
//
// SAI2 通过 Windows 标准剪贴板交换位图（实测 sai2.exe 导入 OpenClipboard /
// GetClipboardData / SetClipboardData / RegisterClipboardFormatW）。因此桥接
// 通道采用最通用的 DIB 系列格式：
//
//   读：优先 CF_DIBV5（带 alpha，SAI2 透明图层可保真）
//       其次 CF_DIB（24/32bpp）
//   写：同时投放 CF_DIBV5 + CF_DIB，最大化 SAI2 与其它软件的兼容性
//
// 像素转换：DIB 为 BGRA + 行 4 字节对齐 + 默认自下而上；ImageBuffer 为
// 紧凑 RGBA8 自上而下。
// ============================================================================

#include "sai2/ImageBuffer.h"

#include <string>

namespace sai2::bridge {

// 剪贴板当前是否含受支持的图像格式
bool ClipboardHasImage();

// 从剪贴板读取图像到 out（统一转换为 RGBA8）。
// 失败时返回 false 并填写 error；*formatName（可选）返回命中的格式名。
bool ReadClipboardImage(ImageBuffer& out, std::string& error,
                        std::string* formatName = nullptr);

// 把图像写入剪贴板（内部转换任意像素格式为 RGBA8）。
// 成功后剪贴板同时持有 CF_DIBV5 与 CF_DIB。
bool WriteClipboardImage(const ImageBuffer& img, std::string& error);

} // namespace sai2::bridge
