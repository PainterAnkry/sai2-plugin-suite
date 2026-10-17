// ============================================================================
// SelfTest.cpp - 伴随宿主端到端自检
// ============================================================================

#include "SelfTest.h"

#include "BmpIO.h"
#include "BridgeCore.h"
#include "CanvasLocateTest.h"
#include "ClipboardImage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sai2::bridge {
namespace {

// UTF-8 路径 → FILE*（Windows 上 fopen 按 ANSI 解释窄路径，中文目录会失败）
FILE* OpenUtf8(const std::string& path, const wchar_t* mode) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                      static_cast<int>(path.size()), nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                        w.data(), n);
    return _wfopen(w.c_str(), mode);
}

} // namespace
namespace {

// ---------------------------------------------------------------------------
// 报告
// ---------------------------------------------------------------------------
struct Report {
    std::vector<std::string> lines;
    int failures = 0;
    int passes = 0;
    int skips = 0;

    void Add(const std::string& s) {
        lines.push_back(s);
        std::printf("%s\n", s.c_str());
    }
    void Pass(const std::string& what, const std::string& detail) {
        ++passes;
        Add("PASS  " + what + "   " + detail);
    }
    void Fail(const std::string& what, const std::string& detail) {
        ++failures;
        Add("FAIL  " + what + "   " + detail);
    }
    void Skip(const std::string& what, const std::string& detail) {
        ++skips;
        Add("SKIP  " + what + "   " + detail);
    }
    void Check(bool ok, const std::string& what, const std::string& detail) {
        if (ok) Pass(what, detail); else Fail(what, detail);
    }
};

// ---------------------------------------------------------------------------
// 合成测试图：渐变 + 圆形 + 高频条纹 + 半透明角（覆盖 alpha 与边缘类算法）
// ---------------------------------------------------------------------------
ImageBuffer MakeTestImage(int w, int h) {
    ImageBuffer img;
    img.Create(w, h, PixelFormat::RGBA8);
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float r2 = (h * 0.28f) * (h * 0.28f);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            RGBA c;
            c.r = static_cast<uint16_t>(x * 255 / std::max(1, w - 1));
            c.g = static_cast<uint16_t>(y * 255 / std::max(1, h - 1));
            c.b = static_cast<uint16_t>(((x / 8 + y / 8) % 2) ? 200 : 60);
            c.a = 255;

            const float dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy < r2) {
                c.r = 250; c.g = 240; c.b = 40;
            }
            if (x % 7 == 0) c.b = 255;          // 高频竖纹（边缘/浮雕类）
            if (y % 11 == 0) c.g = 255;
            if (x < w / 8 && y < h / 8) c.a = 128; // 半透明角
            img.SetPixel(x, y, c);
        }
    }
    return img;
}

// ---------------------------------------------------------------------------
// 差异度量
// ---------------------------------------------------------------------------
struct Diff {
    double meanAbs = 0.0; // 四通道平均绝对差
    int maxAbs = 0;
};

Diff Compare(const ImageBuffer& a, const ImageBuffer& b) {
    Diff d;
    if (!a.IsValid() || !b.IsValid() || a.Width() != b.Width() ||
        a.Height() != b.Height()) {
        d.meanAbs = -1.0;
        d.maxAbs = -1;
        return d;
    }
    long long sum = 0;
    const long long n = static_cast<long long>(a.Width()) * a.Height() * 4;
    for (int y = 0; y < a.Height(); ++y) {
        for (int x = 0; x < a.Width(); ++x) {
            RGBA ca, cb;
            a.GetPixel(x, y, ca);
            b.GetPixel(x, y, cb);
            const int va[4] = {ca.r, ca.g, ca.b, ca.a};
            const int vb[4] = {cb.r, cb.g, cb.b, cb.a};
            for (int c = 0; c < 4; ++c) {
                const int diff = std::abs(va[c] - vb[c]);
                sum += diff;
                d.maxAbs = std::max(d.maxAbs, diff);
            }
        }
    }
    d.meanAbs = static_cast<double>(sum) / static_cast<double>(n);
    return d;
}

std::string Fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    return buf;
}

// 应用当前效果并核对"剪贴板往返无损"
void CheckApplyAndClipboard(Report& rep, BridgeCore& core,
                            const std::string& tag, const std::string& outDir,
                            double minChange) {
    core.UpdatePreview();
    const BridgeCore::ApplyResult r = core.ApplyToClipboard();
    if (!r.ok) {
        rep.Fail(tag + " 应用", r.error);
        return;
    }
    if (!r.clipboardWritten) {
        rep.Fail(tag + " 写剪贴板", r.error);
        return;
    }

    const ImageBuffer& applied = core.AppliedImage();
    const Diff vsOriginal = Compare(core.OriginalImage(), applied);

    // 从剪贴板读回，验证桥的写/读闭环
    ImageBuffer back;
    std::string err;
    if (!ReadClipboardImage(back, err)) {
        rep.Fail(tag + " 读回剪贴板", err);
    } else {
        const Diff rt = Compare(applied, back);
        rep.Check(rt.maxAbs == 0, tag + " 剪贴板往返无损",
                  "maxDiff=" + std::to_string(rt.maxAbs));
    }

    std::string werr;
    WriteBmp32(applied, outDir + "\\effect_" + tag + ".bmp", werr);

    rep.Check(vsOriginal.meanAbs > minChange, tag + " 产生预期变化",
              "meanDiff=" + Fmt(vsOriginal.meanAbs) +
                  " (阈值>" + Fmt(minChange) + ")  " +
                  std::to_string(static_cast<int>(core.LastApplyMs())) + "ms");
}

} // namespace

int RunSelfTest(const std::string& outDir, const std::string& pluginsDir,
                const std::string& logDir) {
    std::printf("=== SAI2 Bridge 自检 ===\n");
    std::printf("plugins=%s\nlogs=%s\nout=%s\n\n", pluginsDir.c_str(),
                logDir.c_str(), outDir.c_str());

    // outDir 是 UTF-8：用 CreateDirectoryW，否则中文路径下建不出目录
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, outDir.c_str(),
                                          static_cast<int>(outDir.size()),
                                          nullptr, 0);
        if (n > 0) {
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, outDir.c_str(),
                                static_cast<int>(outDir.size()), w.data(), n);
            CreateDirectoryW(w.c_str(), nullptr);
        }
    }

    Report rep;
    rep.Add("SAI2 Bridge 端到端自检报告");
    rep.Add("插件 API 版本: " + std::to_string(ApiVersionMajor(kPluginApiVersion)) +
            "." + std::to_string(ApiVersionMinor(kPluginApiVersion)));
    rep.Add("");

    BridgeCore core(pluginsDir, logDir);
    std::string err;
    if (!core.Startup(err)) {
        rep.Fail("启动插件管理器", err);
        rep.Add("");
        rep.Add("插件摘要: " + core.PluginSummary());
        FILE* f = OpenUtf8(outDir + "\\report.txt", L"wb");
        if (f) {
            for (const auto& l : rep.lines) std::fprintf(f, "%s\n", l.c_str());
            std::fclose(f);
        }
        return rep.failures;
    }
    rep.Pass("启动插件管理器", core.PluginSummary());
    rep.Add("");

    const ImageBuffer test = MakeTestImage(256, 192);
    std::string werr;
    WriteBmp32(test, outDir + "\\00_original.bmp", werr);

    // ---------------- 1. 剪贴板往返 ----------------
    rep.Add("--- 1. 剪贴板 DIB 往返 ---");
    if (!WriteClipboardImage(test, err)) {
        rep.Fail("写入剪贴板", err);
    } else {
        ImageBuffer back;
        std::string fmt;
        if (!ReadClipboardImage(back, err, &fmt)) {
            rep.Fail("读回剪贴板", err);
        } else {
            const Diff d = Compare(test, back);
            rep.Pass("剪贴板往返", "格式=" + fmt + " 尺寸=" +
                                       std::to_string(back.Width()) + "x" +
                                       std::to_string(back.Height()) +
                                       " maxDiff=" + std::to_string(d.maxAbs));
            rep.Check(d.maxAbs == 0, "剪贴板像素无损", "maxDiff=" +
                                                          std::to_string(d.maxAbs));
        }
    }
    rep.Add("");

    // ---------------- 2. 非液化效果 ----------------
    rep.Add("--- 2. 模糊与滤镜（逐个全分辨率应用 + 剪贴板回读）---");
    const auto& effects = core.Effects();
    int nonLiquifyCount = 0;
    for (size_t i = 0; i < effects.size(); ++i) {
        const EffectDef& e = effects[i];
        if (e.group == BridgeCore::kLiquifyGroup) continue;
        if (!core.EffectAvailable(static_cast<int>(i))) {
            rep.Skip(e.id, "插件未加载");
            continue;
        }
        ++nonLiquifyCount;
        core.SetImage(test.Clone(), "selftest");
        core.SetCurrentEffect(static_cast<int>(i));
        // 恒等默认参数的调整效果：给非默认参数，否则"应产生变化"断言无意义
        if (e.id == "adjust.brightness-contrast") {
            core.SetParamValue(0, 40.0f);
            core.SetParamValue(1, 30.0f);
        } else if (e.id == "adjust.hue-saturation") {
            core.SetParamValue(0, 60.0f);
            core.SetParamValue(1, 40.0f);
            core.SetParamValue(2, 10.0f);
        } else if (e.id == "adjust.levels") {
            core.SetParamValue(0, 30.0f);
            core.SetParamValue(2, 1.4f);
        }
        CheckApplyAndClipboard(rep, core, e.id, outDir, 0.2);
    }
    rep.Add("");

    // ---------------- 3. 液化（同一会话内逐工具）----------------
    rep.Add("--- 3. 液化（全分辨率会话 + 逐笔累积）---");
    int liquifySeeded = 0;

    // 定位 liquify.forward-warp（用它先建立非空置换场）
    int seedIndex = -1;
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].id == "liquify.forward-warp") {
            seedIndex = static_cast<int>(i);
            break;
        }
    }

    if (seedIndex < 0) {
        rep.Skip("液化", "效果表里没有 liquify.forward-warp");
    } else if (!core.EffectAvailable(seedIndex)) {
        rep.Skip("液化", "com.sai2.liquify 未加载");
    } else {
        core.SetImage(test.Clone(), "selftest-liquid");
        // 先做一笔向前变形，建立非空置换场（此后 smooth/reconstruct 才有东西可作用）
        core.SetCurrentEffect(seedIndex);
        core.StrokeBegin(0.30f, 0.50f);
        core.StrokeMove(0.55f, 0.50f);
        core.StrokeEnd();
        const BridgeCore::ApplyResult r = core.ApplyToClipboard();
        if (r.ok) {
            ++liquifySeeded;
            const Diff d = Compare(core.OriginalImage(), core.AppliedImage());
            rep.Check(d.meanAbs > 0.2, "liquify.forward-warp 产生预期变化",
                      "meanDiff=" + Fmt(d.meanAbs));
        } else {
            rep.Fail("liquify.forward-warp", r.error);
        }

        if (liquifySeeded > 0) {
            for (size_t i = 0; i < effects.size(); ++i) {
                const EffectDef& e = effects[i];
                if (e.group != BridgeCore::kLiquifyGroup) continue;
                if (e.id == "liquify.forward-warp") continue; // 已测
                if (!core.EffectAvailable(static_cast<int>(i))) {
                    rep.Skip(e.id, "插件未加载");
                    continue;
                }
                core.SetCurrentEffect(static_cast<int>(i));
                core.StrokeBegin(0.62f, 0.42f);
                core.StrokeMove(0.74f, 0.50f);
                core.StrokeEnd();
                CheckApplyAndClipboard(rep, core, e.id, outDir, 0.01);
            }
        }
    }
    rep.Add("");

    // ---------------- 4. 撤销 / 复位语义 ----------------
    rep.Add("--- 4. 撤销与复位 ---");
    core.SetImage(test.Clone(), "selftest-undo");
    // 找一个模糊效果做撤销测试
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].id == "blur.gaussian" && core.EffectAvailable((int)i)) {
            core.SetCurrentEffect(static_cast<int>(i));
            const BridgeCore::ApplyResult r = core.ApplyToClipboard();
            const Diff changed = Compare(core.OriginalImage(), core.AppliedImage());
            rep.Check(r.ok && changed.meanAbs > 0.2, "撤销前状态已改变",
                      "meanDiff=" + Fmt(changed.meanAbs));
            std::string uerr;
            const bool undone = core.Undo(uerr);
            const Diff after = Compare(core.OriginalImage(), core.AppliedImage());
            rep.Check(undone && after.maxAbs == 0, "撤销恢复原图",
                      "maxDiff=" + std::to_string(after.maxAbs));
            break;
        }
    }
    {
        core.SetImage(test.Clone(), "selftest-reset");
        for (size_t i = 0; i < effects.size(); ++i) {
            if (effects[i].id == "filter.emboss" && core.EffectAvailable((int)i)) {
                core.SetCurrentEffect(static_cast<int>(i));
                core.ApplyToClipboard();
                std::string rerr;
                core.ResetToOriginal(rerr);
                const Diff d = Compare(core.OriginalImage(), core.AppliedImage());
                rep.Check(d.maxAbs == 0, "复位回到原图",
                          "maxDiff=" + std::to_string(d.maxAbs));
                break;
            }
        }
    }
    rep.Add("");

    // ---------------- 5. 叠加处理 ----------------
    rep.Add("--- 5. 叠加处理（chain）---");
    for (size_t i = 0; i < effects.size(); ++i) {
        if (effects[i].id == "blur.gaussian" && core.EffectAvailable((int)i)) {
            core.SetImage(test.Clone(), "selftest-chain");
            core.SetChain(false);
            core.SetCurrentEffect(static_cast<int>(i));
            core.ApplyToClipboard();
            const ImageBuffer single = core.AppliedImage().Clone();
            core.SetChain(true);
            core.SetCurrentEffect(static_cast<int>(i));
            core.ApplyToClipboard();
            const Diff d = Compare(single, core.AppliedImage());
            rep.Check(d.meanAbs > 0.05, "叠加处理结果不同于单次",
                      "meanDiff=" + Fmt(d.meanAbs));
            core.SetChain(false);
            break;
        }
    }

    rep.Add("");

    // ---------------- 6. 画布定位（精确回贴的成败全在这一段）----------------
    // 用户反馈的三条现象（抓到的图不是画布尺寸 / 窗口里位置与原层不一致 /
    // 回贴错位）都来自这里。用纯内存合成图固定判据：纯色重叠、歧义、遮挡、
    // 贴边五种情形必须分别"命中真值"或"明确拒绝"。
    rep.Add("--- 6. 画布定位（合成回归）---");
    {
        int locFails = 0;
        const std::vector<std::string> lines =
            ui::RunCanvasLocateTests(locFails);
        for (const std::string& l : lines) {
            if (l.rfind("PASS", 0) == 0) {
                ++rep.passes;
                rep.Add(l);
            } else {
                ++rep.failures;
                rep.Add(l);
            }
        }
        if (lines.empty()) rep.Skip("画布定位回归", "用例集为空");
        (void)locFails;
    }

    rep.Add("");
    rep.Add("---------------------------------------------");
    rep.Add("汇总: PASS=" + std::to_string(rep.passes) +
            "  FAIL=" + std::to_string(rep.failures) +
            "  SKIP=" + std::to_string(rep.skips) +
            "   (非液化效果 " + std::to_string(nonLiquifyCount) + " 项)");
    rep.Add("日志尾部:");
    const auto& logs = core.LogLines();
    const size_t start = logs.size() > 12 ? logs.size() - 12 : 0;
    for (size_t i = start; i < logs.size(); ++i) rep.Add("  " + logs[i]);

    core.Shutdown();

    FILE* f = OpenUtf8(outDir + "\\report.txt", L"wb");
    if (f) {
        for (const auto& l : rep.lines) std::fprintf(f, "%s\n", l.c_str());
        std::fclose(f);
    }
    std::printf("\n%s\n", ("报告: " + outDir + "\\report.txt").c_str());
    return rep.failures;
}

} // namespace sai2::bridge
