add_library(helloworld MODULE
    src/HelloWorldPlugin.cpp
)

target_link_libraries(helloworld PRIVATE sai2_sdk)
