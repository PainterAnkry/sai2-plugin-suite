add_library(liquify MODULE
    src/LiquifyPlugin.cpp
)

target_link_libraries(liquify PRIVATE sai2_sdk)
