add_library(sai2_sdk STATIC
    src/ImageBuffer.cpp
    src/Selection.cpp
)

target_include_directories(sai2_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
