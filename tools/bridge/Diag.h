#pragma once
// ============================================================================
// Diag.h - 伴随宿主的启动/绘制诊断
//
// 用途：界面出现"整块空白"这类问题时，能确定到底是"没画"还是"画的时候崩了"。
//
// 默认**不写文件**（避免正常使用时也留日志）；只有设置环境变量
//   SAI2_BRIDGE_DIAG=1
// 时才写 <exe目录>\logs\ui-diag.log，并安装"未捕获异常/访问冲突"终止处理器。
// 排查界面问题的标准动作：
//   set SAI2_BRIDGE_DIAG=1
//   sai2_bridge.exe
// 然后看 logs\ui-diag.log 里最后一条 "enter ..." 停在哪一步。
// ============================================================================

#include <string>

namespace sai2::bridge::ui {

// 安装终止处理器（在任何窗口创建之前调用一次）
void InstallCrashDiagnostics(const std::string& exeDir);

// 写一行诊断（自动加毫秒时间戳；内部自己保证不抛异常）
void Diag(const std::string& line);

// 记录一次"绘制阶段"的进入/离开；leave 需要传 stage
void DiagEnter(const char* stage);
void DiagLeave(const char* stage);

// 当前是否在绘制中（崩溃处理器据此判断是不是绘制阶段挂的）
bool InPaint();

// 便捷宏：进入作用域写一条，离开作用域再写一条
struct DiagScope {
    explicit DiagScope(const char* stage) : stage_(stage) { DiagEnter(stage_); }
    ~DiagScope() { DiagLeave(stage_); }
    const char* stage_;
};

} // namespace sai2::bridge::ui
