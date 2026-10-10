add_executable(sai2_hostdemo
    HostDemo.cpp
)

target_link_libraries(sai2_hostdemo PRIVATE sai2_host)

# GPU/液化性能基准（规格书第十一节：30~60 FPS 目标验证）
add_executable(sai2_computebench
    ComputeBench.cpp
)

target_link_libraries(sai2_computebench PRIVATE sai2_host)
target_include_directories(sai2_computebench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/liquify/src)

# ---- SAI2 伴随宿主：剪贴板桥 + Win32 自绘面板 ----
# 规格书第十八节的"兼容层"落地：SAI2 没有官方插件 SDK，因此把插件跑在
# 伴随宿主里，通过 Windows 剪贴板与 SAI2 交换图像。
add_executable(sai2_bridge WIN32
    bridge/BridgeApp.cpp
    bridge/BridgeCore.cpp
    bridge/BridgeHostContext.cpp
    bridge/ClipboardImage.cpp
    bridge/BmpIO.cpp
    bridge/SelfTest.cpp
)

target_include_directories(sai2_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge)

target_link_libraries(sai2_bridge PRIVATE sai2_host gdi32 user32 shell32)

# 把 4 个插件（DLL + manifest.json）放到 sai2_bridge.exe 旁边的 plugins/，
# 于是 bridge 的 PluginRegistry::Scan() 能自动发现并加载它们。
set(BRIDGE_PLUGINS_DIR $<TARGET_FILE_DIR:sai2_bridge>/plugins)

add_custom_command(TARGET sai2_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${BRIDGE_PLUGINS_DIR}/com.sai2.blur
            ${BRIDGE_PLUGINS_DIR}/com.sai2.liquify
            ${BRIDGE_PLUGINS_DIR}/com.sai2.filter-gallery
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:blur>
            ${BRIDGE_PLUGINS_DIR}/com.sai2.blur/blur.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/plugins/blur/manifest.json
            ${BRIDGE_PLUGINS_DIR}/com.sai2.blur/manifest.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:liquify>
            ${BRIDGE_PLUGINS_DIR}/com.sai2.liquify/liquify.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/plugins/liquify/manifest.json
            ${BRIDGE_PLUGINS_DIR}/com.sai2.liquify/manifest.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:filtergallery>
            ${BRIDGE_PLUGINS_DIR}/com.sai2.filter-gallery/filtergallery.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/plugins/filter-gallery/manifest.json
            ${BRIDGE_PLUGINS_DIR}/com.sai2.filter-gallery/manifest.json
    COMMENT "Staging plugins next to sai2_bridge.exe"
)

# 自检目标：无需 SAI2 即可端到端验证（退出码 = 失败项数）
add_test(NAME bridge_selftest
    COMMAND sai2_bridge --selftest
            --out ${CMAKE_CURRENT_BINARY_DIR}/selftest-out)

# ---- SAI2 注入链路（路线 C：IAT 钩子观察代理）----
# sai2_agent：注入进 SAI2 进程的只读观察 DLL —— 钩 ExtTextOutW/DrawTextW/
# SetDIBitsToDevice/BitBlt，重建图层面板行，经命名管道对外提供 JSON 查询。
# 注意：agent 源码不依赖 SDK/host，单独成库即可。
add_library(sai2_agent SHARED
    agent/AgentMain.cpp
    agent/AgentCore.cpp
    agent/IATHook.cpp
)
target_link_libraries(sai2_agent PRIVATE gdi32 user32)

# sai2_injector：注入/卸载/查询 CLI + FakeHost 端到端自检（sai2_injector selftest）
add_executable(sai2_injector
    injector/InjectorMain.cpp
)
target_link_libraries(sai2_injector PRIVATE gdi32 user32 advapi32)

# sai2_fakehost：假 SAI2（ExtTextOutW 画图层行 + SetDIBitsToDevice 画缩略图），
# 供注入自检在不碰真 SAI2 的前提下验证整条链路
add_executable(sai2_fakehost WIN32
    agent-fakehost/FakeHost.cpp
)
target_link_libraries(sai2_fakehost PRIVATE gdi32 user32)

# 注入自检（需要先构建 sai2_fakehost / sai2_agent）
add_test(NAME agent_selftest
    COMMAND sai2_injector selftest)

# 让 bridge 能找到 AgentClient.h
target_include_directories(sai2_bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/agent)
