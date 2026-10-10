# SAI2 Plugin System

## 快速开始（免构建）

到 [Releases](../../releases) 下载 `sai2-plugin-suite-win64-*.zip`，解压后：

1. 先启动 SAI2 并打开一幅画；
2. 双击 `启动插件面板.bat`（或直接运行 `sai2_bridge.exe`）；
3. 点「连接注入」→ 图层列表出现后，点「液化 / 模糊 / 滤镜 / 调整」任意功能按钮，
   面板会自动抓取 SAI2 当前图层并弹出对应功能窗口；
4. 处理完点「应用 → 剪贴板」，面板会自动切回 SAI2 粘贴回原图层。

环境要求：Windows 10/11 x64（Win11 上窗口为系统圆角）。无需安装任何运行库。

## 目录结构

```
SAI2PluginSystem/
├── sdk/                  # Plugin SDK（宿主与插件共享）
│   ├── include/sai2/     # PluginAPI / IPlugin / IHostContext / ImageBuffer / Selection / IFilter / IComputeBackend / ParamHelpers
│   └── src/              # ImageBuffer / Selection 实现
```
├── host/                 # 宿主端
│   ├── PluginManager     # 插件管理器（异常隔离：插件崩溃 ≠ 宿主崩溃）
│   ├── PluginRegistry    # 已安装插件注册表（registry.json 持久化启用状态）
│   ├── PluginLoader      # 动态库加载 + API 版本校验
│   ├── PluginInstaller   # .sai-plugin(ZIP) / 目录安装、卸载、签名验签
│   ├── PluginSecurity    # SHA-256 完整性校验
│   ├── PluginSource      # 在线插件源（index.json/搜索/安装/更新/签名验签）
│   ├── Crypto            # BigInt + RSA PKCS#1 v1.5 签名（零依赖）
│   ├── ComputeRegistry   # GPU/CPU 计算后端注册表（DX11 优先）
│   │   └── compute/      # Dx11Backend（compute shader）/ CpuSimdBackend
│   ├── UndoStack         # 快照式撤销（UndoGroup）
│   └── MockHostContext   # 测试/演示宿主（Preview / Refresh / Undo / 计算后端）
├── plugins/
│   ├── helloworld/       # Phase 1 验证插件：绘制色块，演示完整链路
│   ├── blur/             # Phase 3：BlurKernels.h（4 种模糊）+ BlurPlugin.cpp（GPU 优先）
│   ├── liquify/          # Phase 4：LiquifyCore.h + LiquifySession.h（工作流引擎）+ LiquifyPlugin.cpp
│   └── filter-gallery/   # Phase 5：Filters.h + FilterSet.h + GalleryViewModel.h（缩略图墙）+ FilterPlugin.cpp
├── tools/                # sai2_hostdemo 演示宿主 + sai2_computebench 性能基准
│   ├── bridge/           # sai2_bridge 伴随宿主（剪贴板桥 + 主面板 + 4 个独立功能窗口 + 自检）
│   ├── agent/            # sai2_agent.dll 注入式观察代理（IAT 钩子 + 行重建 + 管道服务）
│   ├── injector/         # sai2_injector.exe 注入/卸载/查询 CLI（含 FakeHost 端到端自检）
│   └── agent-fakehost/   # sai2_fakehost.exe 假 SAI2（自检用，不碰真 SAI2）
├── tests/                # 14 套测试（CTest / build-zig 两种方式运行）
├── scripts/build-zig.sh  # zig c++ 一键构建脚本（无需 CMake）
└── docs/                 # PluginManifest / PluginSDK / ImageAPI / DeveloperGuide / SAI2Integration / SAI2Injection
```

## 构建与测试

### 方式 A：zig c++（免安装 CMake/MSVC，当前机器已验证）

机器上已有 zig 0.16.0（`pip` 包 ziglang，含 clang 21 前端，MinGW ABI）：

```bash
bash scripts/build-zig.sh          # 构建全部：4 个插件 DLL + 测试 + 演示宿主 + 伴随宿主
cd build-zig/tests
./sai2_tests.exe manifest
./sai2_tests.exe imagebuffer
./sai2_tests.exe undo
./sai2_tests.exe hostcontext
./sai2_tests.exe blur
./sai2_tests.exe liquify
./sai2_tests.exe filter_gallery
./sai2_tests.exe plugin_source
./sai2_tests.exe crypto
./sai2_tests.exe crypto_sign
./sai2_tests.exe compute
./sai2_tests.exe liquify_session
./sai2_tests.exe gallery_view
./sai2_tests.exe plugin_lifecycle ../plugins/helloworld.dll ../../plugins/helloworld/manifest.json

# 伴随宿主端到端自检（无需 SAI2，退出码 = 失败项数）
../tools/sai2_bridge.exe --selftest --out ../selftest-out
```

### 方式 B：CMake + MSVC / MinGW（正式构建）

```bash
cmake -S . -B build
cmake --build build --config Release
cd build && ctest --output-on-failure -C Release && cd ..
```

## 接入 SAI2（伴随宿主 `sai2_bridge.exe`）

**前置验证结论：SAI2 没有官方插件 SDK / 公开扩展接口 / 外部调用机制**
（实测 `sai2.exe` 无导出表、无插件相关字符串、不启动外部进程、不监控目录；
唯一的"Plugin"字样是 Wacom 数位板接口；官方扩展面只有 `init\` 资源文件夹）。
因此按规格书补充章节走**兼容层**路线——插件跑在伴随宿主里，通过 Windows 标准
剪贴板与 SAI2 交换图像（实测 SAI2 导入 `OpenClipboard`/`SetClipboardData` 等全套
剪贴板 API，这条通道真实可用）。完整证据与协议见
**[docs/SAI2Integration.md](docs/SAI2Integration.md)**。

```bash
./build-zig/tools/sai2_bridge.exe            # 启动主面板（插件已在旁边的 plugins\）
```

使用流程（一功能一窗口）：

1. 点主面板上的功能按钮「模糊 / 滤镜 / 调整 / 液化」——
   面板会**自动在 SAI2 里选中当前图层并抓取**（走注入代理点击图层行 + 编辑菜单
   全选/拷贝），抓到后立刻弹出该功能的独立窗口，窗口标题显示图像尺寸；
   也可以照旧在 SAI2 里框选 → `Ctrl+C`，面板监听到剪贴板就自动抓图。
2. 在功能窗口里干活：
   - **液化窗口**：左侧竖排 7 个工具（向前变形/膨胀/收缩/顺逆时针旋转/平滑/重建），
     画布内**按住拖动涂抹**逐笔累积，`[` `]` 调笔刷大小，右键拖动平移、滚轮缩放；
   - **滤镜窗口**：左侧缩略图墙（按可见区域惰性生成），点缩略图换滤镜，右侧拖滑块；
   - **模糊 / 调整窗口**：顶部效果按钮切换，右侧参数面板；
   - 所有窗口都有数值输入框（PS 风格：滑杆 + 可输入数值）。
3. 点功能窗口里的「应用 → 剪贴板」→ 自动切回 SAI2 并发送 `Ctrl+V`；
   不想要自动粘贴可在主面板取消勾选。

主面板仍然是"素材与入口"：预览、SAI2 图层列表（点行 = 在 SAI2 选中该层）、
「连接注入 / 刷新图层 / 抓取选中图层」、以及「叠加处理 / 自动粘贴 / 监听剪贴板 /
合并回原层」等选项。

全局快捷键：`Ctrl+Alt+G` 抓取 / `Ctrl+Alt+A` 应用 / `Ctrl+Alt+Z` 撤销 / `Ctrl+Alt+R` 复位。

命令行（GUI 相关）：

```bash
sai2_bridge.exe --uitest        # UI 冒烟测试：打开全部 4 个功能窗口，数秒后自动退出
```

## SAI2 注入路线（图层识别，路线 C）

剪贴板桥解决了"像素进出"，**注入路线解决"状态识别"**——读取 SAI2 的图层面板
（图层名、行位置），这是复制粘贴画面无法提供的信息：

```
sai2_injector.exe ──LoadLibraryW──► sai2.exe ──► sai2_agent.dll（注入）
     ▲                                              │ IAT 钩子（只读观察）
     │        \\.\pipe\sai2agent（单行 JSON）        │ ExtTextOutW/DrawTextW/SetDIBitsToDevice/BitBlt
     └──────── sai2_bridge.exe 图层面板 ◄────────────┘ 文字↔位块配对 → 行重建
```

- **原理**：SAI2 的 UI 文字经 `ExtTextOutW`/`DrawTextW` 画进内存表面、再由
  `SetDIBitsToDevice`/`BitBlt` 贴到屏幕。Agent 只改主模块 IAT 的函数指针
  （不碰代码、不改参数与返回值），把"画了什么字、贴到哪"记录下来重建图层行。
  相比硬编码内存偏移，跨版本稳定性好得多（详见
  [docs/SAI2Injection.md](docs/SAI2Injection.md)）。
- **实测**（SAI2 Preview.2023.04.05 x64）：8 个图层全部识别（名/混合模式/
  不透明度，位置与截图逐像素吻合）；菜单/工具提示/状态栏一并可捕获；
  卸载后 SAI2 不受影响。
- **使用**：
  ```bash
  ./build-zig/tools/sai2_injector.exe selftest   # 端到端自检 8/8（FakeHost，不碰 SAI2）
  ./build-zig/tools/sai2_injector.exe inject     # 注入（SAI2 须已运行）
  ./build-zig/tools/sai2_injector.exe layers     # 查询图层 JSON
  ./build-zig/tools/sai2_injector.exe eject      # 卸载（恢复 IAT）
  ```
  桥面板新增「图层（SAI2 注入代理）」区：实时列表 + 刷新 + **点击行 = 在 SAI2
  选中该图层并自动抓取**（点行 → Ctrl+A → Ctrl+C 接续原有剪贴板工作流）。
- **注意**：重编译 `sai2_agent.dll` 前先 `eject`；面板只在内容变化时重绘，
  注入后点一下图层行即可出数据。已知限制见文档第七节。
- **SAI2 内插件入口（浮窗按钮条）**：注入后在 SAI2 菜单栏右侧出现一排功能按钮
  「模糊 | 滤镜 | 调整 | 液化」，点哪个按钮桥就打开哪个**独立功能窗口**
  （自动选中并抓取当前图层），不再需要在下拉菜单里翻。
- **PS 风格调整插件**（com.sai2.adjust）：反相/去色/亮度对比度/色相饱和度/色阶，
  在「调整」窗口里切换，参数带数值输入框。
- **图层单独调整**：勾选「应用后合并回原层」后，点图层行 → 选中抓取 → 应用 →
  粘贴 → 自动"向下转写"合并回原图层，调整只作用于目标层。

| 项目 | 说明 |
| --- | --- |
| 插件 | 4 个插件全部可用（模糊 4 种 / 滤镜 10 个 / 液化 7 种工具），高斯走 DX11 |
| 界面 | 主面板 + 4 个独立功能窗口（液化/滤镜/模糊/调整），各有独立预览会话与窗口位置记忆 |
| 剪贴板 | 写 `CF_DIBV5`（带 alpha）+ `CF_DIB`；自检实测往返 `maxDiff=0` |
| 互操作 | 用 .NET/GDI+ 作独立生产者交叉验证：不透明图 **30000/30000 像素逐字节一致**；`--dump-clipboard` 可复核 |
| 参数通道 | 新增 `IHostContext::GetParam`（API 1.1，带默认实现，**1.0 插件无需改动**） |
| 自检 | `sai2_bridge.exe --selftest --out <dir>`：**PASS=58 FAIL=0**，导出每步 BMP 供复核 |
| UI 冒烟 | `sai2_bridge.exe --uitest`：4 个功能窗口全部创建并完整绘制，退出码 0 |

## 端到端演示（4 插件）

```bash
# 准备 plugins 目录：每个插件 = manifest.json + 编译出的 dll
demo/plugins/com.sai2.helloworld/{manifest.json, helloworld.dll}
demo/plugins/com.sai2.blur/{manifest.json, blur.dll}
demo/plugins/com.sai2.liquify/{manifest.json, liquify.dll}
demo/plugins/com.sai2.filter-gallery/{manifest.json, filtergallery.dll}

./build-zig/tools/sai2_hostdemo.exe demo/plugins demo/logs
```

演示宿主将依次：加载全部插件 → 逐个执行（Blur → Hello 色块 → Liquify → FilterGallery 默认滤镜）→
每个操作后 Undo → 禁用/启用/卸载；日志在 `demo/logs/plugin.log`。

## Phase 6 插件源格式（index.json）

```json
{
  "plugins": [
    {
      "id": "com.sai2.blur",
      "name": "Blur",
      "version": "1.1.0",
      "download": "https://example.com/pkg/blur-1.1.0.sai-plugin",
      "description": "Gaussian/Box/Motion/Radial blur",
      "icon": "blur.png",
      "sha256": "<64 位小写十六进制>",
      "minApiVersion": "1.0"
    }
  ]
}
```

安装/更新流程（`PluginSource::InstallOrUpdate`）：
minApiVersion 检查 → 已装版本比较（更新必须更新）→ 下载 → SHA-256 校验 →
PluginInstaller 安装（内部再校验 Manifest + API 版本 + 入口 DLL）。
生产环境使用 `HttpDownloader`（WinHTTP）；测试/离线用 `FileDownloader`。

## 插件签名（第十六节）

manifest.json / index.json 均可扩展两个字段：

```json
{
  "sha256": "<入口 DLL 或安装包的 SHA-256>",
  "signature": "<RSA PKCS#1 v1.5 (SHA-256) 签名 hex>",
  "signerPubKey": "SAI2RSA1.<modulusHex>.<exponentHex>"
}
```

- 被签名消息：`SAI2-SIGN1|<id>|<version>|<sha256>`
- signature 与 signerPubKey **同时存在时强制验签**，只给其一直接拒绝
- 签名覆盖 SHA-256，篡改安装包或 DLL 都会验签失败
- 实现为自带 BigInt（modpow + Miller-Rabin 密钥生成），零第三方依赖；
  生产建议 2048 位密钥离线签名，运行时验签（e=65537）毫秒级

## 性能基准（第十一节：30~60 FPS 目标）

```bash
./build-zig/tools/sai2_computebench.exe
```

实测（本机，2026-10-04）：

| 场景 | 后端 | 结果 |
| --- | --- | --- |
| 4K 降采样预览（1024px）高斯 σ=4 | DirectX 11（hardware） | **5.91 ms/帧 ≈ 169 FPS** ✅ |
| 同上 | CPU SIMD fallback | 166 ms/帧 ≈ 6 FPS（fallback 保底） |
| 4K 液化笔刷（r=80）增量 ROI 预览 | CPU（ROI 仅笔刷区域） | **0.68 ms/帧 ≈ 1474 FPS** ✅ |

## 已修复的关键问题（验证过程中发现）

1. **Logger 死锁**：`Init()` 持有 `std::mutex` 时调用 `Write()`（非递归锁）→ 进程挂起。
   已改为先更新成员、锁外写日志。
2. **EnablePlugin 逻辑缺陷**：禁用后再启用时只改注册表不重新加载 DLL。已修复为
   对已停止实例重新 `StartInstance`。
3. **PluginSource 索引健壮性**：单个无效条目跳过而非整体失败；error 在每次
   InstallOrUpdate 开始时清空，避免陈旧错误信息误导排查。
4. **BigInt 求逆错误**：扩展欧几里得的中间结果可能为负，无符号 Sub 下溢回绕 →
   d 计算错误。已改为全程规约到 [0, m) 的模减法。
5. **HLSL cbuffer 布局**：float4 数组的打包规则对齐（16 字节边界），与 C++ 结构体
   逐字节对齐后 CPU/GPU 结果一致（容差 ≤3）。
6. **FastBoxBlur 从不创建目标缓冲区**（接入阶段由伴随宿主自检发现）：
   两个调用方（`Watercolor`、`SketchDodge`）传入的都是默认构造的空 buffer，
   而 `FastBoxBlur` 只往 `dst` 里 `SetPixel`、从不 `Create`，于是越界写入全部
   静默失败 —— `PencilSketch` 输出与输入**逐字节相同**（完全失效），
   `Watercolor` 退化成"单纯压暗 40%"。已改为目标缓冲区尺寸/格式不符时按需创建；
   修复后素描/水彩恢复正确行为，14/14 测试套件与自检全部通过（当前 PASS=58）。
   附带修正 `DisplacementField::IsValid()` 的自比较笔误（`dx_.size() == dx_.size()`）。
7. **剪贴板 DIB 像素起点算错 12 字节**（接入阶段用**独立生产者**交叉验证发现）：
   GDI 在 `BI_BITFIELDS` 下会把 3 个颜色掩码**再写一遍**在 `BITMAPV5HEADER`
   之后（像素起点 136），而本工程自己写的是"V5 头(124) + 像素"（起点 124）。
   早期实现只认后者，遇到 GDI 的布局就整体右移 3 个像素并环绕。
   已改为**用 `GlobalSize - stride×height` 反推像素起点**（剪贴板 DIB 没有多余尾巴，
   推导确定），落在合理区间外才回退。
   教训：**自家的 writer/reader 闭环测不出这类缺陷**（两边错得一致）；
   必须用第三方生产者（这里是 .NET/GDI+ 的 `Clipboard.SetImage`）做交叉验证。
   修复后：不透明测试图 **30000/30000 像素逐字节一致**；含 alpha 的图，
   源图不透明区域 **27399/27399 逐字节一致**，剩余差异全部来自生产者经
   `CF_BITMAP` 丢失 alpha（生产者侧限制，非读取端缺陷）。

## 全阶段验证清单（规格书第十八节）

| 检查项 | 状态 |
| --- | --- |
| Manifest 解析/校验/API 版本兼容 | ✅ tests: manifest |
| DLL 加载、Initialize/Shutdown/卸载 | ✅ tests: plugin_lifecycle |
| 启用/禁用/卸载、registry.json 持久化 | ✅ tests: plugin_lifecycle |
| 插件异常隔离（crash.log + 自动禁用） | ✅ PluginManager try/catch 隔离 |
| ImageBuffer RGBA8/RGBA16、转换、裁剪 | ✅ tests: imagebuffer |
| Selection 矩形/蒙版 | ✅ tests: imagebuffer |
| UndoGroup 快照式撤销 | ✅ tests: undo / hostcontext |
| Preview 降采样/提交回写、Canvas Refresh | ✅ tests: hostcontext |
| HelloWorld 插件端到端 | ✅ tests: plugin_lifecycle + hostdemo |
| Gaussian Blur 可分离卷积（能量守恒/对称性） | ✅ tests: blur |
| Box/Motion/Radial Blur + Selection | ✅ tests: blur |
| Liquify 置换场/逆映射/双线性采样 | ✅ tests: liquify |
| Forward Warp/Bloat/Pinch/Twirl/Smooth/Reconstruct | ✅ tests: liquify |
| **FilterRegistry 注册/查找/四分类** | ✅ tests: filter_gallery |
| **声明式参数模型（clamp/重置/JSON 描述）** | ✅ tests: filter_gallery |
| **10 个滤镜算法性质（含确定性噪声复现）** | ✅ tests: filter_gallery |
| **index.json 解析/搜索/坏条目跳过** | ✅ tests: plugin_source |
| **下载→SHA-256→API 检查→安装→更新全流程** | ✅ tests: plugin_source |
| **RSA 签名/验签 + 篡改检测 + 安装链路验签** | ✅ tests: crypto / crypto_sign |
| **IComputeBackend（DX11 CS + WARP + CPU fallback）** | ✅ tests: compute |
| **GPU/CPU 结果一致性（容差 ≤3）** | ✅ tests: compute |
| **液化工作流引擎（Stroke/ROI 预览/Commit/Cancel）** | ✅ tests: liquify_session |
| **缩略图墙（10 滤镜缩略图/分类树/参数面板 JSON）** | ✅ tests: gallery_view |
| **性能：4K Blur 预览 169 FPS / 液化 1474 FPS** | ✅ computebench 实测 |
| **4 插件端到端（加载→执行→撤销→卸载）** | ✅ hostdemo 实测通过 |
| **SAI2 插件接口可行性验证（PE 导出表/导入表/字符串/目录）** | ✅ 结论：无官方 SDK，见 docs/SAI2Integration.md |
| **剪贴板桥（CF_DIBV5+CF_DIB 写入/读回，含 alpha）** | ✅ 自检往返 maxDiff=0（像素级无损） |
| **宿主参数通道 IHostContext::GetParam（API 1.1 向后兼容）** | ✅ 1.0 插件零改动可运行；14/14 套件不回归 |
| **伴随宿主端到端（19 效果 + 液化 7 工具 + 撤销/复位/叠加）** | ✅ 自检 PASS=58 FAIL=0 |
| **Win32 功能窗口（独立窗口 + 各自预览会话 + 液化涂抹/滤镜缩略图墙）** | ✅ --uitest 4 窗口全通过 |
| **Win32 主面板（图层列表/剪贴板监听/全局热键/自动粘贴）** | ✅ 实机启动 + 5 插件加载验证 |
| **注入路线：IAT 钩子 + 文字/位块配对 + 行重建 + 管道服务** | ✅ 真机 SAI2 实测：8 图层全部识别，位置逐像素吻合 |
| **注入路线：FakeHost 端到端自检 + 卸载/重注入循环** | ✅ 自检 8/8（与真 SAI2 agent 同时在线互不干扰） |
| 编译通过（zig c++ 0.16.0 / clang 21，MinGW ABI） | ✅ 2026-10-04 |

## 后续方向（可选增强）

- **资源包接入（路线 A）**：把滤镜/程序化纹理导出成 SAI2 原生
  `init\papertex` / `brushtex` / `scatter` 资源，与本伴随宿主互补
- ~~独立窗口 UI~~ ✅ 已完成（本轮）：液化窗口 / 滤镜窗口 / 模糊窗口 / 调整窗口
  都是 Win32 自绘的独立窗口（`tools/bridge/*Wnd.cpp` + `Ui.{h,cpp}` 公共层），
  每个窗口独立预览会话、窗口位置记忆
- **功能窗口记住参数 / 预设**：目前重开窗口按默认值起步，可加"上次参数 + 预设保存"
- **液化 ROI 提交**：给液化插件加 `liquify.roi*` 参数，让大画布涂抹只重渲染笔刷区域
- **GPU 扩展**：液化置换场渲染迁移 DX11；Box/Motion/Radial 的 CS 实现
- **RGBA32F**：ImageBuffer 第三种格式（规格书第七节预留）
- **签名基础设施**：发布者证书体系、密钥轮换、CRL

详细文档见 `docs/`（接入 SAI2 见 [docs/SAI2Integration.md](docs/SAI2Integration.md)）。
