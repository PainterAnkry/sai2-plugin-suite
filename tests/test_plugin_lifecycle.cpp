// ============================================================================
// test_plugin_lifecycle.cpp - 插件生命周期端到端测试（Phase 1 核心）
//
// 流程：把编译出的 helloworld 插件（manifest + dll）复制到临时 plugins 目录，
//       用 PluginManager 完成 启动 -> 执行 -> 撤销 -> 禁用/启用 -> 卸载。
//
// 由 CTest 传入：
//   g_args[0] = helloworld 动态库路径
//   g_args[1] = helloworld manifest.json 路径
// ============================================================================

#include "Tests.h"
#include "sai2/host/MockHostContext.h"
#include "sai2/host/PluginManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace sai2test {

bool TestPluginLifecycle() {
    using namespace sai2;
    using namespace sai2::host;

    CHECK(g_args.size() >= 2);
    const std::string dllPath = g_args[0];
    const std::string manifestPath = g_args[1];
    CHECK(fs::exists(dllPath));
    CHECK(fs::exists(manifestPath));

    // ---- 准备临时插件目录（放在当前目录下，避免依赖系统临时目录权限） ----
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path workDir =
        fs::current_path() / ("sai2_test_" + std::to_string(stamp));
    const fs::path pluginsDir = workDir / "plugins";
    const fs::path pluginDir = pluginsDir / "com.sai2.helloworld";
    std::error_code ec;
    fs::create_directories(pluginDir, ec);
    CHECK(!ec);
    fs::copy_file(manifestPath, pluginDir / "manifest.json",
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    fs::copy_file(dllPath, pluginDir / "helloworld.dll",
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    // ---- 宿主 + 管理器 ----
    MockHostContext host;
    host.SetCanvasSize(64, 64, PixelFormat::RGBA8);

    PluginManager manager(pluginsDir.string(), (workDir / "logs").string(),
                          kPluginApiVersion, &host);
    std::string error;
    CHECK(manager.Startup(error));

    // ---- 已安装且已加载 ----
    auto items = manager.ListPlugins();
    CHECK(items.size() == 1);
    CHECK(items[0].id == "com.sai2.helloworld");
    CHECK(items[0].enabled);
    CHECK(items[0].loaded);

    // ---- 执行插件：画布左上角出现色块 ----
    CHECK(manager.ExecutePlugin("com.sai2.helloworld", error));
    ImageBuffer* canvas = host.GetImageBuffer();
    RGBA got;
    CHECK(canvas->GetPixel(10, 10, got));
    CHECK(got.r == 0x20 && got.g == 0xC0 && got.b == 0xE0);
    CHECK(host.refreshCount_ > 0);

    // ---- Undo 恢复 ----
    CHECK(host.Undo());
    CHECK(canvas->GetPixel(10, 10, got) && got.r == 0 && got.g == 0);

    // ---- 禁用 -> 未加载但保留安装 ----
    CHECK(manager.DisablePlugin("com.sai2.helloworld", error));
    items = manager.ListPlugins();
    CHECK(items.size() == 1 && !items[0].enabled && !items[0].loaded);
    CHECK(!manager.ExecutePlugin("com.sai2.helloworld", error)); // 已停止

    // ---- 重新启用 ----
    CHECK(manager.EnablePlugin("com.sai2.helloworld", error));
    items = manager.ListPlugins();
    CHECK(items.size() == 1 && items[0].enabled && items[0].loaded);
    CHECK(manager.ExecutePlugin("com.sai2.helloworld", error));

    // ---- 卸载 ----
    CHECK(manager.UninstallPlugin("com.sai2.helloworld", error));
    items = manager.ListPlugins();
    CHECK(items.empty());
    CHECK(!fs::exists(pluginDir));

    manager.Shutdown();

    // ---- 清理临时目录 ----
    fs::remove_all(workDir, ec);

    std::cout << "  lifecycle: 启动/执行/撤销/禁用/启用/卸载 全部通过\n";
    return true;
}

} // namespace sai2test
