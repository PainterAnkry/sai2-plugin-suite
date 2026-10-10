cmake_minimum_required(VERSION 3.20)
project(SAI2PluginSystem
    VERSION 0.1.0
    DESCRIPTION "SAI2 Plugin System - Plugin SDK / Host / Plugins"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---- 核心 SDK（头文件 + ImageBuffer/Selection 实现，宿主与插件共享） ----
add_subdirectory(sdk)

# ---- 宿主：插件管理器 / 注册表 / 加载器 / 安装器 / 安全 / 日志 / Undo ----
add_subdirectory(host)

# ---- 插件：HelloWorld（Phase 1 验证插件） / Blur（Phase 3） / Liquify（Phase 4） ----
add_subdirectory(plugins/helloworld)
add_subdirectory(plugins/blur)
add_subdirectory(plugins/liquify)
add_subdirectory(plugins/filter-gallery)
add_subdirectory(plugins/adjust)

# ---- 工具：宿主演示程序（手动验证整条链路） ----
add_subdirectory(tools)

# ---- 测试 ----
enable_testing()
add_subdirectory(tests)
