# Developer Guide

## 环境要求

- CMake ≥ 3.20
- MSVC（Visual Studio 2022，勾选“C++ 桌面开发”）或 MinGW-w64（GCC ≥ 10）
- 无第三方库依赖（JSON / SHA-256 / ZIP / CRC32 均为内置实现）

## 构建

```powershell
# MSVC（推荐在 "x64 Native Tools Command Prompt" 中执行）
cmake -S . -B build
cmake --build build --config Release

# MinGW
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

免 CMake 的替代方案（当前机器已验证）：`bash scripts/build-zig.sh`，
使用 zig 0.16.0（pip 包 ziglang，clang 21 前端，MinGW ABI）一键构建全部
插件 DLL、测试与演示宿主，产物在 `build-zig/`。

## 测试

```powershell
cd build
ctest --output-on-failure -C Release
```

| 套件 | 覆盖内容 |
| --- | --- |
| `manifest` | Manifest 解析、校验、版本比较、API 兼容策略 |
| `imagebuffer` | RGBA8/RGBA16 布局、像素读写、裁剪、克隆、格式转换、Selection |
| `undo` | 快照撤销、UndoGroup 组内多步、组外自动单步、多组 LIFO |
| `hostcontext` | Host 契约：UndoGroup / Preview 降采样与提交 / RefreshCanvas |
| `plugin_lifecycle` | 端到端：复制插件→启动→执行→撤销→禁用→启用→卸载 |
| `blur` | Gaussian 能量守恒/对称、Box 均匀不变、Motion 方向扩散、Radial 旋转涂抹、Selection |
| `liquify` | 零场恒等、双线性采样、Forward Warp 内容位移、Bloat/Pinch 方向、Twirl、Smooth、Reconstruct |
| `filter_gallery` | Registry 注册/查找/四分类、参数模型（clamp/重置/JSON）、10 个滤镜算法性质、蒙版尊重、RGBA16、确定性噪声复现 |
| `plugin_source` | index.json 解析/搜索/坏条目跳过、CheckUpdates、下载→SHA-256→API 检查→安装→更新全流程、SHA 不匹配/API 不满足/重复安装拒绝 |
| `crypto` | BigInt 加减乘除模幂、Miller-Rabin 素性、hex 往返 |
| `crypto_sign` | RSA 密钥生成、PKCS#1 v1.5 签名/验签往返、篡改检测、换钥失败、带签名 manifest 安装链路 |
| `compute` | CPU 后端正确性（能量/均匀不变/参数校验）、注册表优先级、DX11（WARP 兜底）可用性与 CPU/GPU 交叉一致性（容差 ≤3） |
| `liquify_session` | 工作流引擎：Stroke 累积置换场、增量 ROI 预览、Commit 回写+Undo、Cancel 无副作用、ReconstructAll |
| `gallery_view` | 缩略图生成/缓存/非退化、分类树、参数面板 JSON、参数 clamp + Apply |

CMake 下还有一个不在 `sai2_tests` 里的测试目标 `bridge_selftest`：伴随宿主
端到端自检（剪贴板往返无损、19 个效果逐个全分辨率应用、液化 7 工具、
撤销/复位/叠加语义），也可手动运行 `sai2_bridge.exe --selftest --out <dir>`；
界面部分另有 `sai2_bridge.exe --uitest`（打开全部功能窗口做绘制冒烟，数秒后退出）。

## 接入 SAI2：伴随宿主（`tools/bridge`）

SAI2 没有官方插件接口（验证证据见 `SAI2Integration.md`），所以插件通过**伴随宿主 +
剪贴板桥**作用于 SAI2 的画：

- `ClipboardImage.{h,cpp}`：Win32 剪贴板 DIB 读写（`CF_DIBV5` + `CF_DIB`，
  保留 alpha，处理行 4 字节对齐与自下而上/自上而下差异）
- `BridgeHostContext.{h,cpp}`：真正的 `IHostContext` 实现（画布 / 快照式 Undo /
  参数通道 / 计算后端）；`SetRecordingUndo(false)` 让预览不污染撤销栈
- `BridgeCore.{h,cpp}`：无界面核心流程（抓取 / 预览 / 全分辨率应用 / 撤销 / 复位 /
  液化笔画会话）。**每个窗口一份预览会话**（`BridgeCore::UiPreview`：基准图、显示图、
  显示倍率），预览对降采样图执行并按比例缩放像素半径类参数；**液化用全分辨率基准**
  （置换场与像素坐标绑定，且插件跨笔累积状态），显示图按像素预算降采样但提交恒为
  全分辨率，因此"窗口里看到的形变"="提交得到的形变"，且**从不重放笔画**
- `Effects.h`：效果与参数模型（所有窗口与自检共用同一份，保证"能点的"="测过的"）
- `Ui.{h,cpp}`：功能窗口公共层（绘制原语、命中结构、标准布局与骨架、画布绘制/
  缩放/平移、参数滑杆与数值框、窗口位置记忆）
- `UiWindow.cpp`：功能窗口的类注册、创建/关闭、消息处理与公共命中分发
- `LiquifyWnd.cpp` / `FilterWnd.cpp` / `BlurWnd.cpp` / `AdjustWnd.cpp`：
  四个功能窗口各自的内容与命中处理（液化 = 左侧竖排 7 工具 + 涂抹；滤镜 =
  缩略图墙 + 参数面板；模糊/调整 = 效果条 + 参数面板）
- `BridgeApp.cpp`：主面板（预览 + SAI2 图层列表 + 四个功能按钮 + 全局热键 +
  自动粘贴 + 后台"选中并抓取当前图层"状态机）+ `--selftest` / `--uitest` 入口
- `SelfTest.cpp` / `BmpIO.{h,cpp}`：无界面端到端自检与 BMP 导出

### 界面结构（一功能一窗口）

```
sai2_bridge.exe（主面板 = 启动器）
  ├ 预览 / SAI2 图层列表 / 连接注入 / 选项（叠加·自动粘贴·合并回层）
  └ 功能按钮 ─┬─ 模糊   → BlurWnd    （4 种模糊，参数面板 + 实时预览）
              ├─ 滤镜   → FilterWnd  （缩略图墙 + 参数面板）
              ├─ 调整   → AdjustWnd  （5 项调整，参数面板 + 实时预览）
              └─ 液化   → LiquifyWnd （7 工具 + 画布涂抹，PS 液化风格）
```

点功能按钮的默认流程：`RequestToolWindow` → 后台线程在 SAI2 里点图层行 →
编辑菜单"全选"→"拷贝"→ 等剪贴板更新 → `GrabFromClipboard` → 打开对应窗口；
没有注入代理时退化为"已有图像就直接开窗，否则提示先 Ctrl+C"。

### 新增插件时如何接入面板

1. 插件从 `IHostContext::GetParam`（配合 `sai2/ParamHelpers.h`）读取参数，
   键缺失时**必须**回落到自身默认值 —— 这样 1.0 宿主与测试环境行为不变；
2. 在 `Effects.h` 的 `BuildEffects()` 里加一条 `EffectDef`
   （`pluginId` / `group`（决定进哪个功能窗口）/ `fixed` 固定参数 / `params` 滑块定义，
   像素半径类参数标 `pixelScale=true`）；新分组则同时加 `group` 到
   `BridgeCore::kGroupNames`、`kWin*` 索引，并仿照 `BlurWnd.cpp` 加一个窗口文件；
3. 插件放进 `plugins/<id>/`（DLL + manifest.json），`PluginRegistry::Scan()`
   会自动发现，无需手写 registry.json。

## Phase 5：Filter Gallery 插件架构

- `plugins/filter-gallery/src/Filters.h`：10 个滤镜的**纯算法**实现（油画强度直方图投票、
  水彩柔化+饱和度+边缘沉淀、蜡笔量化+纸纹、素描/炭笔 Color Dodge、粉笔颗粒、
  画布纹理、颗粒噪声、3x3 卷积浮雕、Sobel 查找边缘），全部尊重 Selection、
  支持 RGBA8/RGBA16、噪声使用确定性 hash（测试可复现）。
- `FilterBase.h`：声明式参数模型（`ParamDef` 名称/标签/默认值/范围 + clamp），
  `ParamDefsJson()` 供宿主动态生成参数面板。
- `FilterSet.h`：10 个滤镜包装类，连接算法与参数。
- `FilterPlugin.cpp`：`FilterRegistry` 注册全部滤镜；无 UI 场景 `Execute`
  应用默认滤镜并走完整 UndoGroup/RefreshCanvas；`CreateUI` 返回分类+参数 JSON。

新增滤镜只需三步：在 `Filters.h` 写纯算法 → `FilterSet.h` 加包装类（定义参数）→
`FilterPlugin.cpp` 构造函数中 `registry_.Add<...>()`。

## Phase 6：在线插件源

- `host/include/sai2/host/PluginSource.h`：`SourceEntry`（id/name/version/download/
  description/icon/sha256/minApiVersion）、`IDownloader` 抽象、`PluginSource`。
- index.json 格式见 README；无效条目（缺 id/version/download 或版本不可解析）跳过。
- `InstallOrUpdate` 流程：minApiVersion 检查（主版本相等、次版本 <= 宿主）→
  已装版本比较 → 下载到缓存目录 → SHA-256 校验（声明时）→ `PluginInstaller`
  安装（再校验 Manifest + API 版本 + 入口 DLL）→ 清理缓存包。
- 下载器实现：
  - `FileDownloader`：本地路径 / file:// 复制（测试与离线环境）。
  - `HttpDownloader`（`host/src/net/HttpDownloader.cpp`）：WinHTTP 流式下载，
    需要链接 winhttp（MSVC `#pragma` 或 `-lwinhttp`）；未纳入测试构建以避免链接依赖。

## 单独运行某个套件

```powershell
.\build\tests\Release\sai2_tests.exe manifest
.\build\tests\Release\sai2_tests.exe plugin_lifecycle <helloworld.dll 路径> <manifest.json 路径>
```

## 宿主演示

```powershell
# 准备 plugins 目录
mkdir demo\plugins\com.sai2.helloworld
copy plugins\helloworld\manifest.json demo\plugins\com.sai2.helloworld\
copy build\plugins\helloworld\Release\helloworld.dll demo\plugins\com.sai2.helloworld\

# 运行
.\build\tools\Release\sai2_hostdemo.exe demo\plugins demo\logs
```

日志输出在 `demo/logs/plugin.log`；若插件抛异常，还会写入 `demo/logs/crash.log`
并将该插件自动禁用（规格书第五节：插件崩溃 ≠ SAI2 崩溃）。

## 工程约定

- C++20，无异常边界：宿主对所有插件回调做 `try/catch` 隔离；
- SDK（`sdk/`）只放契约与纯算法，不依赖宿主实现；
- Host（`host/`）负责加载、注册、安装、安全、日志、Undo、Preview；
- 每个插件是**独立 DLL** + `manifest.json`，禁止把多个插件写进一个大类；
- 所有面向插件的接口变更必须评估 `SAI2_PLUGIN_API_VERSION` 兼容性。

## 后续方向（MVP 之外，现已全部完成）

全部六个规格书阶段（插件框架 / Image API / Blur / Liquify / Filter Gallery /
插件源）+ MVP 后四项增强已完成并通过 14/14 测试套件：

1. ✅ GPU 加速：`IComputeBackend` 抽象（`host/src/compute/`）——DX11 compute
   shader 优先、WARP 兜底、CPU SIMD fallback。实测 4K 预览高斯 169 FPS、
   液化笔刷 ROI 预览 1474 FPS（目标 30~60）。插件经
   `IHostContext::GetComputeBackend()` 使用，不与 GPU API 耦合；
2. ✅ 液化工作流引擎：`plugins/liquify/src/LiquifySession.h`（笔刷
   大小/压力/速度、增量 ROI 预览、Commit/Cancel、整体重建），独立窗口
   UI 可直接驱动；
3. ✅ 缩略图墙：`plugins/filter-gallery/src/GalleryViewModel.h`——
   ThumbnailGenerator 惰性缓存 + 分类树 + 参数面板 JSON；
4. ✅ 插件签名：`host/Crypto`（自带 BigInt + RSA PKCS#1 v1.5/SHA-256），
   manifest 与 index.json 均支持 `signature`/`signerPubKey`，安装链路强制验签。

剩余可选项：独立窗口 UI（Qt 6 / Win32 自绘）、GPU 端液化渲染、RGBA32F、
发布者证书体系。
