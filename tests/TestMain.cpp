#include "Tests.h"

namespace sai2test {

std::vector<std::string> g_args;

} // namespace sai2test

int main(int argc, char** argv) {
    std::cout << std::unitbuf; // 每次插入后立即刷新，崩溃时也能看到进度
    if (argc < 2) {
        std::cout << "用法: sai2_tests <suite>\n"
                  << "  suites: manifest | imagebuffer | undo | hostcontext"
                  << " | blur | liquify | filter_gallery | plugin_source"
                  << " | crypto | crypto_sign | plugin_lifecycle | liquify_shift | gallery_specs\n";
        return 2;
    }
    for (int i = 2; i < argc; ++i)
        sai2test::g_args.emplace_back(argv[i]);

    const std::string suite = argv[1];
    bool ok = false;
    if (suite == "manifest") ok = sai2test::TestManifest();
    else if (suite == "imagebuffer") ok = sai2test::TestImageBuffer();
    else if (suite == "undo") ok = sai2test::TestUndo();
    else if (suite == "hostcontext") ok = sai2test::TestHostContext();
    else if (suite == "plugin_lifecycle") ok = sai2test::TestPluginLifecycle();
    else if (suite == "blur") ok = sai2test::TestBlur();
    else if (suite == "liquify") ok = sai2test::TestLiquify();
    else if (suite == "filter_gallery") ok = sai2test::TestFilterGallery();
    else if (suite == "plugin_source") ok = sai2test::TestPluginSource();
    else if (suite == "crypto") ok = sai2test::TestCrypto();
    else if (suite == "crypto_sign") ok = sai2test::TestCryptoSign();
    else if (suite == "compute") ok = sai2test::TestCompute();
    else if (suite == "liquify_session") ok = sai2test::TestLiquifySession();
    else if (suite == "gallery_view") ok = sai2test::TestGalleryView();
    else if (suite == "liquify_shift") ok = sai2test::TestLiquifyShift();
else if (suite == "gallery_specs") ok = sai2test::TestGallerySpecs();
    else {
        std::cout << "未知套件: " << suite << "\n";
        return 2;
    }

    std::cout << (ok ? "[PASS] " : "[FAIL] ") << suite << "\n";
    return ok ? 0 : 1;
}
