add_executable(sai2_tests
    TestMain.cpp
    test_manifest.cpp
    test_imagebuffer.cpp
    test_undo.cpp
    test_hostcontext.cpp
    test_plugin_lifecycle.cpp
    test_blur.cpp
    test_liquify.cpp
    test_filtergallery.cpp
    test_plugin_source.cpp
    test_crypto.cpp
    test_compute.cpp
    test_liquifysession.cpp
    test_galleryview.cpp
)

target_link_libraries(sai2_tests PRIVATE sai2_host)

# CTest：每个套件一个 test case
add_test(NAME manifest COMMAND sai2_tests manifest)
add_test(NAME imagebuffer COMMAND sai2_tests imagebuffer)
add_test(NAME undo COMMAND sai2_tests undo)
add_test(NAME hostcontext COMMAND sai2_tests hostcontext)
add_test(NAME plugin_lifecycle COMMAND sai2_tests plugin_lifecycle
         "$<TARGET_FILE:helloworld>"
         "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/helloworld/manifest.json")
add_test(NAME blur COMMAND sai2_tests blur)
add_test(NAME liquify COMMAND sai2_tests liquify)
add_test(NAME filter_gallery COMMAND sai2_tests filter_gallery)
add_test(NAME plugin_source COMMAND sai2_tests plugin_source)
add_test(NAME crypto COMMAND sai2_tests crypto)
add_test(NAME crypto_sign COMMAND sai2_tests crypto_sign)
add_test(NAME compute COMMAND sai2_tests compute)
add_test(NAME liquify_session COMMAND sai2_tests liquify_session)
add_test(NAME gallery_view COMMAND sai2_tests gallery_view)

target_include_directories(sai2_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/filter-gallery/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/liquify/src)
