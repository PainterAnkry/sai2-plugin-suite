#pragma once
// ============================================================================
// IATHook.h - 进程内 IAT（导入地址表）钩子引擎
//
// 只处理"主模块"（即 sai2.exe / fakehost.exe 自己）：EXE 里对导入函数的
// 调用全部经由本模块 IAT 间接寻址（MSVC x64: call qword ptr [rip+...]），
// 因此替换 IAT 表项即可拦截主程序发起的全部调用。
//
// 选择 IAT 而不是改函数序言（trampoline）的原因：
//   * 不需要指令长度解码，不碰任何代码字节 —— 出错面小一个数量级；
//   * 不需要为每个 Windows 版本准备跳板布局；
//   * 卸载只需把表项写回。
// 局限：只拦截"经 IAT 的调用"（主程序静态导入）；系统 DLL 内部互调不受影响
//   —— 对本 Agent 恰好是优点（只看 SAI2 自己画的东西）。
// ============================================================================

#include <map>
#include <string>
#include <windows.h>

namespace sai2agent {

class IATHook {
public:
    // 在主模块 IAT 里把 `funcName`（属于 `dllName`，如 "GDI32.dll"）的表项
    // 替换为 replacement；返回原函数指针。失败返回 nullptr。
    void* Install(const char* dllName, const char* funcName,
                  void* replacement);

    // 查原函数（Hook 实现里调用原功能用）
    void* Original(void* replacement) const;

    // 卸载全部钩子（恢复表项），返回恢复的个数
    int RestoreAll();

private:
    // 遍历主模块导入表找到匹配表项的虚拟地址
    void** FindIATSlot(const char* dllName, const char* funcName);

    std::map<void*, void*> replToOriginal_; // replacement → original
};

} // namespace sai2agent
