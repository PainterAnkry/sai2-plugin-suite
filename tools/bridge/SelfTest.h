#pragma once
// ============================================================================
// SelfTest.h - 无界面端到端自检
//
// 不需要 SAI2、不需要有人点鼠标，就能把整条链路跑一遍：
//   合成测试图 → 剪贴板写入/读回 → 每个效果全分辨率应用 → 再读回剪贴板
//   → 与导出 BMP 比对 → 检查撤销/复位语义
// 结果写 outDir/report.txt，并把每一步的图像导出成 BMP 供肉眼复核。
// 返回失败项数量（0 = 全部通过），可直接当进程退出码。
// ============================================================================

#include <string>

namespace sai2::bridge {

int RunSelfTest(const std::string& outDir, const std::string& pluginsDir,
                const std::string& logDir);

} // namespace sai2::bridge
