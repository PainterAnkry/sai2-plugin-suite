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

ICON="$ROOT/resources/sai2_suite.ico"
# rc 文件里写的是**字面路径**，resinator 不认 MSYS 风格 /f/... ；
# 用 cygpath -m 转成 F:/... （正斜杠 Windows 路径，既能被 rc 打开，
# 又不会在 sed 替换串里被当成转义）
if command -v cygpath >/dev/null 2>&1; then ICON="$(cygpath -m "$ROOT/resources/sai2_suite.ico")"; fi
# 版本资源：每个随包二进制都带 VERSIONINFO（无版本信息的裸二进制
# 最容易被杀软启发式引擎误报）。模板见 resources/version.rc.in。
VER="1.23.0"
VER4="${VER//./,},0"          # 1.23.0 -> 1,23,0,0（VERSIONINFO 要逗号分隔四段）
RES="$BUILD/res"
mkdir -p "$RES"
# make_res <描述> <内部名> <原始文件名> <VFT_APP|VFT_DLL> <输出.res> [图标路径]
# 图标只给 sai2_bridge（面板 + 4 个功能窗口共用）：此前没有任何图标资源，
# 任务栏/标题栏显示的是 Windows"未知应用"默认图标（用户反馈图标位置不齐）。
make_res() {
    local iconline=""
    if [ -n "$6" ]; then iconline="101 ICON \"$6\""; fi
    sed -e "s|@DESC@|$1|g" -e "s|@NAME@|$2|g" -e "s|@FILE@|$3|g" \
        -e "s|@TYPE@|$4|g" -e "s|@VER@|$VER|g" -e "s|@VER4@|$VER4|g" \
        -e "s|@ICONLINE@|$iconline|g" \
        "$ROOT/resources/version.rc.in" > "$RES/$2.rc"
    # /c 65001：模板是 UTF-8，不带此参数中文会被按 cp1252 解析成乱码；
    # 写成 //c 是防止 Git Bash 把 /c 当 POSIX 路径转换（MSYS 折叠 // → /）
    "$ZIG" rc //c 65001 "$RES/$2.rc" "$5"
}

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
make_res "SAI2 插件：示例（Hello World）" helloworld helloworld.dll VFT_DLL "$RES/helloworld.res"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/helloworld.dll" \
    "$ROOT/plugins/helloworld/src/HelloWorldPlugin.cpp" "${SDK_SRC[@]}" "$RES/helloworld.res"

echo "==> [1b/7] adjust.dll（PS 风格调整）"
make_res "SAI2 插件：图像调整" adjust adjust.dll VFT_DLL "$RES/adjust.res"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/adjust.dll" \
    "$ROOT/plugins/adjust/src/AdjustPlugin.cpp" "${SDK_SRC[@]}" "$RES/adjust.res"

echo "==> [2/7] blur.dll"
make_res "SAI2 插件：高斯模糊" blur blur.dll VFT_DLL "$RES/blur.res"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/blur.dll" \
    "$ROOT/plugins/blur/src/BlurPlugin.cpp" "${SDK_SRC[@]}" "$RES/blur.res"

echo "==> [3/7] liquify.dll"
make_res "SAI2 插件：液化" liquify liquify.dll VFT_DLL "$RES/liquify.res"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -shared -o "$BUILD/plugins/liquify.dll" \
    "$ROOT/plugins/liquify/src/LiquifyPlugin.cpp" "${SDK_SRC[@]}" "$RES/liquify.res"

echo "==> [4/7] filtergallery.dll"
make_res "SAI2 插件：滤镜库" filtergallery filtergallery.dll VFT_DLL "$RES/filtergallery.res"
"${ZIGC[@]}" "${CXXFLAGS[@]}" \
    "-I$ROOT/plugins/filter-gallery/src" \
    -shared -o "$BUILD/plugins/filtergallery.dll" \
    "$ROOT/plugins/filter-gallery/src/FilterPlugin.cpp" "${SDK_SRC[@]}" "$RES/filtergallery.res"

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
# 保持控制台子系统（--selftest/--uitest/--demo 的输出要能被测试管道捕获）。
# 双击/.bat 启动时由 wWinMain 主动隐藏控制台窗口（见 BridgeApp.cpp）。
make_res "SAI2 伴随宿主面板（图层抓取 + 液化/滤镜/模糊/调整）" sai2_bridge sai2_bridge.exe VFT_APP "$RES/sai2_bridge.res" "$ICON"
"${ZIGC[@]}" "${CXXFLAGS[@]}" -municode \
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
    -lgdi32 -luser32 -lshell32 "$RES/sai2_bridge.res"

echo "==> [6b/7] sai2_ui_probe.exe（界面探针：无人值守验证面板真的画出来了）"
make_res "SAI2 界面探针" sai2_ui_probe sai2_ui_probe.exe VFT_APP "$RES/sai2_ui_probe.res"
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything -municode \
    -o "$BUILD/tools/sai2_ui_probe.exe" \
    "$ROOT/tools/bridge/UiProbe.cpp" \
    -luser32 -lgdi32 "$RES/sai2_ui_probe.res"

echo "==> [7/7] 注入链路（sai2_agent.dll + sai2_injector.exe + sai2_fakehost.exe）"
# Agent：注入进 SAI2 的观察 DLL（IAT 钩子 ExtTextOutW/DrawTextW/
# SetDIBitsToDevice/BitBlt + 同步命名管道）。
# -fms-extensions 暂留；SEH 在 windows-gnu 目标无运行时语义，
# 代码已按无 SEH 防御式编写。
make_res "SAI2 画布代理（注入观察库）" sai2_agent sai2_agent.dll VFT_DLL "$RES/sai2_agent.res"
"${ZIGC[@]}" -std=c++20 -O2 -fms-extensions -Wno-everything -shared \
    -o "$BUILD/tools/sai2_agent.dll" \
    "$ROOT/tools/agent/AgentMain.cpp" \
    "$ROOT/tools/agent/AgentCore.cpp" \
    "$ROOT/tools/agent/IATHook.cpp" \
    "$ROOT/tools/agent/AgentToolbar.cpp" \
    -lgdi32 -luser32 "$RES/sai2_agent.res"

# Injector：注入/卸载/查询 CLI + FakeHost 端到端自检（selftest）
make_res "SAI2 插件注入器" sai2_injector sai2_injector.exe VFT_APP "$RES/sai2_injector.res"
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything \
    -o "$BUILD/tools/sai2_injector.exe" \
    "$ROOT/tools/injector/InjectorMain.cpp" \
    -lgdi32 -luser32 -ladvapi32 "$RES/sai2_injector.res"

# FakeHost：假 SAI2（ExtTextOutW 画图层行 + SetDIBitsToDevice 画缩略图），
# 供 `sai2_injector selftest` 在不碰真 SAI2 的前提下验证整条注入链路
make_res "SAI2 注入链路自检宿主" sai2_fakehost sai2_fakehost.exe VFT_APP "$RES/sai2_fakehost.res"
"${ZIGC[@]}" -std=c++20 -O2 -Wno-everything -municode -Wl,--subsystem,windows \
    -o "$BUILD/tools/sai2_fakehost.exe" \
    "$ROOT/tools/agent-fakehost/FakeHost.cpp" \
    -lgdi32 -luser32 "$RES/sai2_fakehost.res"

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
