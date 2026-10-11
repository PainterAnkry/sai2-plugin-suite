#pragma once
#include <string>

// 演示模式：程序内自动编排一套完整操作流程（抓取 → 模糊调参/应用 →
// 液化涂抹 → 滤镜库 → 调整 → 主题切换），同时由截图线程把"当前演示窗口"
// 每 200ms 抓一帧（PrintWindow），等比合成到 900x900 画布导出 BMP 序列。
// 跑完后自动退出进程（与 --uitest 同一退出通道）。outDir 需已存在。
void RunDemo(int seconds, const std::string& outDir);

// 笔刷流畅度自检（--brushbench）：造一张 WxH 测试图 → 开液化窗口 → 从脚本线程
// 往窗口消息队列里灌一段"真实鼠标拖动"（125Hz，与真手速一致）→ 同时屏抓画布，
// 统计拖动期间画面真的刷新了几次、首帧延迟、松手后多久才稳定。
// 用来量化"涂抹时画面不动、切换工具才看到变化"这个问题。
void RunBrushBench(int imgW, int imgH, const std::string& outDir);

// 真机 SAI2 演示（--sai2demo）：走应用自己的真实链路录素材 ——
// 刷新注入图层 → 点"功能按钮"抓当前图层并弹液化窗口 → 向窗口投递鼠标消息真涂抹
// → 点"应用 → 剪贴板"（自动粘贴回 SAI2）。全程不注入外部输入，便于在受限环境里
// 也能量产"真机 SAI2 + 注入图层"的演示素材。
void RunSai2Demo(int seconds, const std::string& outDir);
