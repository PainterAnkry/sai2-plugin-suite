add_library(adjust MODULE
    src/AdjustPlugin.cpp
)

target_link_libraries(adjust PRIVATE sai2_sdk)
