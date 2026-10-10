// ============================================================================
// HelloWorldPlugin.cpp - Phase 1 验证插件
//
// 演示插件开发完整契约：
//   Initialize -> Execute（UndoGroup + SnapshotForUndo + 修改图像 + RefreshCanvas）
//   -> Shutdown
// ============================================================================

#include "sai2/IHostContext.h"
#include "sai2/IPlugin.h"

#include <cstring>
#include <string>

namespace {

using namespace sai2;

class HelloWorldPlugin final : public IPlugin {
public:
    PluginInfo GetInfo() const override {
        PluginInfo info;
        info.id = "com.sai2.helloworld";
        info.name = "Hello World";
        info.version = "1.0.0";
        info.author = "SAI2 Plugin Team";
        info.type = "image-effect";
        info.description = "Paints a demo swatch; exercises the full plugin "
                           "lifecycle (UndoGroup / Snapshot / Canvas refresh).";
        return info;
    }

    bool Initialize(IHostContext* host) override {
        if (!host) return false;
        host_ = host;
        host_->Log(LogLevel::Info, "HelloWorldPlugin initialized");
        return true;
    }

    void Shutdown() override {
        if (host_) {
            host_->Log(LogLevel::Info, "HelloWorldPlugin shutdown");
            host_ = nullptr;
        }
    }

    bool Execute() override {
        if (!host_) return false;
        ImageBuffer* canvas = host_->GetImageBuffer();
        if (!canvas || !canvas->IsValid()) {
            host_->Log(LogLevel::Error, "没有可用的画布");
            return false;
        }

        // 一次完整操作 = 一个 UndoGroup
        host_->BeginUndoGroup("Hello World Swatch");
        host_->SnapshotForUndo("Hello World Swatch");

        // 在画布左上角绘制一块 48x48 的演示色块（半透明青色）
        RGBA c;
        c.r = 0x20;
        c.g = 0xC0;
        c.b = 0xE0;
        c.a = canvas->Format() == PixelFormat::RGBA16 ? 0xFFFF : 0xFF;
        canvas->FillRect(8, 8, 48, 48, c);

        host_->Log(LogLevel::Info, "Hello World! swatch painted on canvas");
        host_->RefreshCanvas();
        host_->EndUndoGroup();
        return true;
    }

private:
    IHostContext* host_ = nullptr;
};

} // namespace

SAI2_DEFINE_PLUGIN(HelloWorldPlugin)
