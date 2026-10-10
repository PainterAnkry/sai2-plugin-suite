add_library(sai2_host STATIC
    src/Logging.cpp
    src/Json.cpp
    src/Manifest.cpp
    src/PluginRegistry.cpp
    src/PluginLoader.cpp
    src/PluginInstaller.cpp
    src/PluginSecurity.cpp
    src/PluginManager.cpp
    src/PluginSource.cpp
    src/Crypto.cpp
    src/UndoStack.cpp
    src/MockHostContext.cpp
    src/compute/ComputeRegistry.cpp
    src/compute/CpuSimdBackend.cpp
    src/compute/Dx11Backend.cpp
)

target_include_directories(sai2_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(sai2_host PUBLIC sai2_sdk)

if (WIN32)
    target_link_libraries(sai2_host PUBLIC advapi32 d3d11)
endif()
