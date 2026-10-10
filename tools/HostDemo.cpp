// ============================================================================
// HostDemo.cpp - 宿主演示程序
//
// 手动验证整条插件链路：
//   1. 在 plugins 目录安装（复制）helloworld 插件目录与编译出的 dll
//   2. ./sai2_hostdemo <pluginsDir> <logDir>
//   3. 自动列出 / 执行 / 撤销 / 禁用 / 启用 / 卸载
// ============================================================================

#include "sai2/host/PluginManager.h"
#include "sai2/host/UndoStack.h"

#include <iostream>

namespace {

using namespace sai2;
using namespace sai2::host;

// 演示用宿主：一块 128x96 的画布
class DemoHost final : public IHostContext {
public:
    ImageBuffer* GetImageBuffer() override { return &canvas_; }
    const Selection* GetSelection() const override { return nullptr; }

    void BeginUndoGroup(const char* name) override { undo.BeginGroup(name); }
    void EndUndoGroup() override { undo.EndGroup(); }
    void SnapshotForUndo(const char* label) override {
        undo.PushSnapshot(label, canvas_);
    }
    bool Undo() override { return undo.Undo(canvas_); }

    ImageBuffer* BeginPreview(int) override { return nullptr; }
    void EndPreview(bool) override {}

    void RefreshCanvas() override { ++refreshes; }
    void ShowMessage(const char* m) override {
        std::cout << "  [message] " << m << "\n";
    }
    void Log(LogLevel level, const char* m) override {
        std::cout << "  [" << LogLevelName(level) << "] " << m << "\n";
    }

    ImageBuffer canvas_;
    UndoStack undo{20};
    int refreshes = 0;
};

} // namespace

int main(int argc, char** argv) {
    const std::string pluginsDir = argc > 1 ? argv[1] : "plugins";
    const std::string logDir = argc > 2 ? argv[2] : "logs";

    std::cout << "=== SAI2 Host Demo ===\n";

    DemoHost host;
    // 初始化一块渐变画布
    host.canvas_.Create(128, 96, PixelFormat::RGBA8);
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 128; ++x) {
            RGBA c;
            c.r = static_cast<uint16_t>(x * 2);
            c.g = static_cast<uint16_t>(y * 2);
            c.b = 0x40;
            c.a = 0xFF;
            host.canvas_.SetPixel(x, y, c);
        }

    PluginManager manager(pluginsDir, logDir, kPluginApiVersion, &host);
    std::string error;
    if (!manager.Startup(error)) {
        std::cerr << "启动失败: " << error << "\n";
        return 1;
    }

    std::cout << "\n-- 已安装插件 --\n";
    for (const auto& item : manager.ListPlugins())
        std::cout << "  " << item.id << "  v" << item.version
                  << (item.enabled ? "  [enabled]" : "  [disabled]")
                  << (item.loaded ? "  [loaded]" : "") << "\n";

    // 执行所有已加载的插件（每个插件在一次完整的画布操作中验证链路）
    for (const auto& item : manager.ListPlugins()) {
        if (!item.loaded) continue;
        std::cout << "\n-- 执行 " << item.id << " --\n";
        std::string execError;
        if (manager.ExecutePlugin(item.id, execError))
            std::cout << "  执行成功, 画布刷新 " << host.refreshes << " 次\n";
        else
            std::cout << "  执行失败: " << execError << "\n";
        // 每个插件执行后撤销，恢复画布供下一个插件使用
        std::cout << (host.Undo() ? "  已撤销\n" : "  没有可撤销的操作\n");
    }

    const std::string helloId = "com.sai2.helloworld";
    std::cout << "\n-- 禁用 / 启用 --\n";
    if (manager.DisablePlugin(helloId, error))
        std::cout << "  已禁用\n";
    if (manager.EnablePlugin(helloId, error))
        std::cout << "  已重新启用\n";

    std::cout << "\n-- 卸载 --\n";
    if (manager.UninstallPlugin(helloId, error))
        std::cout << "  已卸载（可重新安装目录后重试）\n";
    else
        std::cout << "  卸载失败: " << error << "\n";

    manager.Shutdown();
    std::cout << "\n日志目录: " << logDir << "\n";
    return 0;
}
