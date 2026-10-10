#!/usr/bin/env bash
# ============================================================================
# build-zig.sh - 使用 zig c++（clang 21, MinGW ABI）构建整个工程
# 用途：本机无 CMake/MSVC 时的验证构建；正式发布仍推荐 CMake
#
# 注意：缓存目录放在工程内（--cache-dir），这样在受限环境里也能编译
# （默认缓存目录在用户 AppData 下，某些环境不可写）。
# ============================================================================
set -e

ZIG="C:/Users/Ankry/.workbuddy/binaries/python/envs/default/Lib/site-packages/ziglang/zig.exe"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-zig"
ZIGC=("$ZIG" c++)
# zig 的全局缓存默认在用户目录（%LOCALAPPDATA%\zig）；放到工程内，
# 这样在受限环境（不能写用户目录）里也能编译
export ZIG_GLOBAL_CACHE_DIR="$BUILD/zig-cache"

# 注意：路径含空格，编译选项必须用 bash 数组
CXXFLAGS=(-std=c++20 -O2 -Wno-everything
          "-I$ROOT/sdk/include" "-I$ROOT/host/include")

SDK_SRC=("$ROOT/sdk/src/ImageBuffer.cpp" "$ROOT/sdk/src/Selection.cpp")
HOST_SRC=("$ROOT/host/src/Json.cpp" "$ROOT/host/src/Logging.cpp"
          "$ROOT/host/src/Manifest.cpp" "$ROOT/host/src/PluginRegistry.cpp"
          "$ROOT/host/src/PluginLoader.cpp" "$ROOT/host/src/PluginInstaller.cpp"
          "$ROOT/host/src/PluginSecurity.cpp" "$ROOT/host/src/PluginManager.cpp"
          "$ROOT/host/src/PluginSource.cpp" "$ROOT/host/src/Crypto.cpp"
          "$ROOT/host/src/UndoStack.cpp" "$ROOT/host/src/MockHostContext.cpp"
          "$ROOT/host/src/compute/ComputeRegistry.cpp"
          "$ROOT/host/src/compute/CpuSimdBackend.cpp"
          "$ROOT/host/src/compute/Dx11Backend.cpp")

mkdir -p "$BUILD/plugins" "$BUILD/tests" "$BUILD/tools"

echo "==> [1/7] helloworld.dll"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/helloworld.dll" \
    "$ROOT/plugins/helloworld/src/HelloWorldPlugin.cpp" "${SDK_SRC[@]}"

echo "==> [1b/7] adjust.dll（PS 风格调整）"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/adjust.dll" \
    "$ROOT/plugins/adjust/src/AdjustPlugin.cpp" "${SDK_SRC[@]}"

echo "==> [2/7] blur.dll"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/blur.dll" \
    "$ROOT/plugins/blur/src/BlurPlugin.cpp" "${SDK_SRC[@]}"

echo "==> [3/7] liquify.dll"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/liquify.dll" \
    "$ROOT/plugins/liquify/src/LiquifyPlugin.cpp" "${SDK_SRC[@]}"

echo "==> [4/7] filtergallery.dll"
"${ZIGC[@]}" "${CXXFLAGS[@]}" \
    "-I$ROOT/plugins/filter-gallery/src" \
    -shared -o "$BUILD/plugins/filtergallery.dll" \
    "$ROOT/plugins/filter-gallery/src/FilterPlugin.cpp" "${SDK_SRC[@]}"

echo "==> [5/7] sai2_tests.exe + sai2_hostdemo.exe + sai2_computebench.exe"
"${ZIGC[@]}" "${CXXFLAGS[@]}" \
    "-I$ROOT/plugins/filter-gallery/src" \
    "-I$ROOT/plugins/liquify/src" \
    -o "$BUILD/tests/sai2_tests.exe" \
    "$ROOT"/tests/*.cpp "${SDK_SRC[@]}" "${HOST_SRC[@]}"

"${ZIGC[@]}" "${CXXFLAGS[@]}" -o "$BUILD/tools/sai2_hostdemo.exe" \
    "$ROOT/tools/HostDemo.cpp" "${SDK_SRC[@]}" "${HOST_SRC[@]}"

"${ZIGC[@]}" "${CXXFLAGS[@]}" \
    "-I$ROOT/plugins/liquify/src" \
    -o "$BUILD/tools/sai2_computebench.exe" \
    "$ROOT/tools/ComputeBench.cpp" "${SDK_SRC[@]}" "${HOST_SRC[@]}"

echo "==> [6/7] sai2_bridge.exe（伴随宿主：剪贴板桥 + 主面板 + 4 个功能窗口）"
# -mwindows/-municode：GUI 子系统 + wWinMain 入口；--selftest 走 AttachConsole
"${ZIGC[@]}" "${CXXFLAGS[@]}" -municode -mwindows \
    "-I$ROOT/tools/bridge" \
    -o "$BUILD/tools/sai2_bridge.exe" \
    "$ROOT/tools/bridge/BridgeApp.cpp" \
    "$ROOT/tools/bridge/BridgeCore.cpp" \
    "$ROOT/tools/bridge/BridgeHostContext.cpp" \
    "$ROOT/tools/bridge/ClipboardImage.cpp" \
    "$ROOT/tools/bridge/BmpIO.cpp" \
    "$ROOT/tools/bridge/SelfTest.cpp" \
    "$ROOT/tools/bridge/Ui.cpp" \
    "$ROOT/tools/bridge/UiWindow.cpp" \
    "$ROOT/tools/bridge/LiquifyWnd.cpp" \
    "$ROOT/tools/bridge/FilterWnd.cpp" \
    "$ROOT/tools/bridge/BlurWnd.cpp" \
    "$ROOT/tools/bridge/AdjustWnd.cpp" \
    "$ROOT/tools/bridge/Diag.cpp" \
    "$ROOT/tools/bridge/Demo.cpp" \
    "${SDK_SRC[@]}" "${HOST_SRC[@]}" \
    -lgdi32 -luser32 -lshell32

echo "==> [6b/7] sai2_ui_probe.exe（界面探针：无人值守验证面板真的画出来了）"
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything -municode \
    -o "$BUILD/tools/sai2_ui_probe.exe" \
    "$ROOT/tools/bridge/UiProbe.cpp" \
    -luser32 -lgdi32

echo "==> [7/7] 注入链路（sai2_agent.dll + sai2_injector.exe + sai2_fakehost.exe）"
# Agent：注入进 SAI2 的观察 DLL（IAT 钩子 ExtTextOutW/DrawTextW/
# SetDIBitsToDevice/BitBlt + 同步命名管道）。
# -fms-extensions 暂留；SEH 在 windows-gnu 目标无运行时语义，
# 代码已按无 SEH 防御式编写。
"${ZIGC[@]}" -std=c++20 -O2 -fms-extensions -Wno-everything -shared \
    -o "$BUILD/tools/sai2_agent.dll" \
    "$ROOT/tools/agent/AgentMain.cpp" \
    "$ROOT/tools/agent/AgentCore.cpp" \
    "$ROOT/tools/agent/IATHook.cpp" \
    "$ROOT/tools/agent/AgentToolbar.cpp" \
    -lgdi32 -luser32

# Injector：注入/卸载/查询 CLI + FakeHost 端到端自检（selftest）
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything \
    -o "$BUILD/tools/sai2_injector.exe" \
    "$ROOT/tools/injector/InjectorMain.cpp" \
    -lgdi32 -luser32 -ladvapi32

# FakeHost：假 SAI2（ExtTextOutW 画图层行 + SetDIBitsToDevice 画缩略图），
# 供 `sai2_injector selftest` 在不碰真 SAI2 的前提下验证整条注入链路
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything -municode -mwindows \
    -o "$BUILD/tools/sai2_fakehost.exe" \
    "$ROOT/tools/agent-fakehost/FakeHost.cpp" \
    -lgdi32 -luser32

# 把 4 个插件摆到 sai2_bridge.exe 旁边，PluginRegistry::Scan() 才能自动发现
BRIDGE_PLUGINS="$BUILD/tools/plugins"
mkdir -p "$BRIDGE_PLUGINS/com.sai2.blur" \
         "$BRIDGE_PLUGINS/com.sai2.liquify" \
         "$BRIDGE_PLUGINS/com.sai2.filter-gallery" \
         "$BRIDGE_PLUGINS/com.sai2.helloworld" \
         "$BRIDGE_PLUGINS/com.sai2.adjust"
cp "$BUILD/plugins/blur.dll"          "$BRIDGE_PLUGINS/com.sai2.blur/"
cp "$BUILD/plugins/liquify.dll"       "$BRIDGE_PLUGINS/com.sai2.liquify/"
cp "$BUILD/plugins/filtergallery.dll" "$BRIDGE_PLUGINS/com.sai2.filter-gallery/"
cp "$BUILD/plugins/helloworld.dll"    "$BRIDGE_PLUGINS/com.sai2.helloworld/"
cp "$ROOT/plugins/blur/manifest.json"           "$BRIDGE_PLUGINS/com.sai2.blur/"
cp "$ROOT/plugins/liquify/manifest.json"        "$BRIDGE_PLUGINS/com.sai2.liquify/"
cp "$ROOT/plugins/filter-gallery/manifest.json" "$BRIDGE_PLUGINS/com.sai2.filter-gallery/"
cp "$ROOT/plugins/helloworld/manifest.json"     "$BRIDGE_PLUGINS/com.sai2.helloworld/"
cp "$BUILD/plugins/adjust.dll"        "$BRIDGE_PLUGINS/com.sai2.adjust/"
cp "$ROOT/plugins/adjust/manifest.json"         "$BRIDGE_PLUGINS/com.sai2.adjust/"

echo "==> 构建完成: $BUILD"
