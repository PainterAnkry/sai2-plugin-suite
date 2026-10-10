add_library(blur MODULE
    src/BlurPlugin.cpp
)

target_link_libraries(blur PRIVATE sai2_sdk)
