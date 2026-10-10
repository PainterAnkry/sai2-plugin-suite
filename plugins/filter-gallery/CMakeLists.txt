# Filter Gallery 插件（Phase 5）
add_library(filtergallery MODULE
    src/FilterPlugin.cpp
)

target_link_libraries(filtergallery PRIVATE sai2_sdk)
target_include_directories(filtergallery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
