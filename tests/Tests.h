#pragma once
// ============================================================================
// Tests.h - 极简测试框架（无第三方依赖）
// ============================================================================

#include <iostream>
#include <string>
#include <vector>

namespace sai2test {

// TestMain 传入的附加参数（plugin_lifecycle 用）
extern std::vector<std::string> g_args;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "  CHECK 失败: " #cond "  (" << __FILE__ << ":"       \
                      << __LINE__ << ")\n";                                    \
            return false;                                                      \
        }                                                                      \
    } while (0)

// 失败时附加输出上下文信息（如 error 内容）
#define CHECK_MSG(cond, msgText)                                               \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cout << "  CHECK 失败: " #cond "  (" << __FILE__ << ":"       \
                      << __LINE__ << ")\n    " << (msgText) << "\n";           \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool TestManifest();
bool TestImageBuffer();
bool TestUndo();
bool TestHostContext();
bool TestPluginLifecycle();
bool TestBlur();
bool TestLiquify();
bool TestFilterGallery();
bool TestPluginSource();
bool TestCrypto();
bool TestCryptoSign();
bool TestCompute();
bool TestLiquifySession();
bool TestGalleryView();

} // namespace sai2test
