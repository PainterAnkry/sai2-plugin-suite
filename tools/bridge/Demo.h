#pragma once
#include <string>

// 演示模式：程序内自动编排一套完整操作流程（抓取 → 模糊调参/应用 →
// 液化涂抹 → 滤镜库 → 调整 → 主题切换），同时由截图线程把"当前演示窗口"
// 每 200ms 抓一帧（PrintWindow），等比合成到 900x900 画布导出 BMP 序列。
// 跑完后自动退出进程（与 --uitest 同一退出通道）。outDir 需已存在。
void RunDemo(int seconds, const std::string& outDir);
