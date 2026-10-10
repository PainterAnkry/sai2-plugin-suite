# 接入 SAI2 应用：技术可行性验证与伴随宿主方案

本文档记录**规格书补充章节（第 108-113 行）要求的那项前置验证**的实测结果，
以及据此确定的接入方案、使用方法和协议约定。

---

## 一、技术可行性验证（实测，2026-10-04）

验证对象：本机已安装的 `D:\SAI 2.0\[Saier]SAI Ver.2\sai2.exe`
（Systemax PaintTool SAI Ver.2，版本 2023-04-05，5,670,400 字节，x64 PE）。

### 1.1 结论

**SAI2 不提供任何第三方插件 SDK、公开扩展接口或稳定的外部调用机制。**
唯一被 SAI2 主动消费的扩展面是 `init\` 下的**资源文件夹**（笔刷形状/纹理/纸张等）。
因此"让 SAI2 加载本工程编译出的插件 DLL"在受支持的路径上**不可行**。

### 1.2 证据

| 检查项 | 方法 | 结果 |
| --- | --- | --- |
| 导出表 | 解析 PE Data Directory[0] | **不存在 Export Directory** —— 不对外暴露任何可调用接口 |
| 插件相关字符串 | 全镜像字符串扫描 | 无 `plugin` / `manifest` / `apiVersion` / `.sai-plugin`；唯一的 `IStylusPlugin`、`IStylusAsyncPlugin`、`MyStylusPlugin` 是 **Wacom 数位板驱动**的 COM 接口，不是通用扩展点 |
| 动态加载 | 导入表 | 有 `LoadLibraryW` / `GetProcAddress` / `FreeLibrary`（通用动态加载，无插件语义） |
| 启动外部程序 | 导入表 | **无** `CreateProcess` / `ShellExecute` / `WinExec` |
| 目录监控 | 导入表 | **无** `ReadDirectoryChangesW` / `FindFirstChangeNotification` |
| 渲染后端 | 导入表 | 仅 GDI（`SetDIBitsToDevice`），**无** D3D11 / DXGI / OpenGL |
| **剪贴板** | 导入表 | **有** `OpenClipboard` / `GetClipboardData` / `SetClipboardData` / `IsClipboardFormatAvailable` / `RegisterClipboardFormatW` |
| 安装目录 | 目录枚举 | 无 `plugins\` 目录、无扩展路径配置 |
| 配置文件 | `sai2.ini` | 仅 `[Locale]` / `[PenTablet]` 等设置，无扩展项 |
| 官方扩展面 | 目录枚举 | `init\` 下 6 个资源文件夹：`blotmap`(2) `bristle`(3) `brshape`(2) `brushtex`(2) `papertex`(4) `scatter`(2)，均为 BMP + 配套 .ini |

### 1.3 可选路线对比

| 路线 | 可行性 | 效果 | 风险 |
| --- | --- | --- | --- |
| A. 资源包接入 `init\` | 官方消费，稳定 | 只能提供纸张/笔刷纹理等**素材**，滤镜与液化算法本体进不去 | 无 |
| **B. 伴随宿主 + 剪贴板桥（本方案）** | **已验证可用** | 全部插件算法（模糊/液化/10 滤镜，含 DX11 加速）**真的作用在 SAI2 的画上** | 无（只用标准剪贴板） |
| C. DLL 注入 / API Hook | 技术上可做 | 可在 SAI2 进程内实现 IHostContext | 需逆向、随版本失效、可能损坏画作、几乎必然违反授权协议 |

规格书原文已预判此结论：*"若不存在，以上 Host API 架构更适合用于'自研宿主/兼容层'
方案，而不能直接假设可以把 DLL 注入现有 SAI2。"* 本工程选择**路线 B**。

---

## 二、方案架构

```
┌──────────────┐   Ctrl+C    ┌──────────────────────────────────────┐
│   SAI2       │ ──────────► │  sai2_bridge.exe（伴随宿主）          │
│  (sai2.exe)  │             │                                      │
│              │  剪贴板      │  ClipboardImage  ⇄ CF_DIBV5 / CF_DIB │
│  画布/选区    │             │        ↓                             │
│              │             │  BridgeHostContext (IHostContext)    │
│              │             │   ├─ 画布 ImageBuffer                 │
│              │             │   ├─ UndoStack（快照式）              │
│              │             │   ├─ 参数通道 GetParam（API 1.1）     │
│              │             │   └─ ComputeRegistry（DX11 优先）     │
│              │             │        ↓                             │
│              │             │  PluginManager ── 插件 DLL           │
│              │             │   blur / liquify / filter-gallery    │
│              │  Ctrl+V     │        ↓                             │
│              │ ◄────────── │  Win32 自绘面板（效果/参数/预览）      │
└──────────────┘  剪贴板      └──────────────────────────────────────┘
```

关键点：**插件完全不知道自己跑在伴随宿主里还是 SAI2 里**。
这正是规格书第四节"插件不得直接修改 SAI2 内部对象，应通过 Host API 操作图像和状态"
的落地方式——同一份插件 DLL，在 `sai2_hostdemo`（演示）、`sai2_tests`（测试）
与 `sai2_bridge`（真实伴随宿主）里走的是同一条 `IHostContext` 路径。

---

## 三、使用步骤

1. 构建：`bash scripts/build-zig.sh` 或 `cmake -S . -B build && cmake --build build --config Release`
   （脚本会把 4 个插件自动摆到 `sai2_bridge.exe` 旁边的 `plugins\`）
2. 运行 `sai2_bridge.exe`
3. **点主面板上的功能按钮**「模糊 / 滤镜 / 调整 / 液化」：
   面板先通过注入代理在 SAI2 里选中当前图层、走编辑菜单「全选」→「拷贝」，
   抓到像素后立刻弹出该功能的**独立窗口**（PS 风格，一功能一窗口）。
   也可以照旧：在 SAI2 里框选 → `Ctrl+C` → 面板自动监听到并抓图。
4. 在功能窗口里干活：
   - **液化窗口**：左侧竖排 7 工具（向前变形/膨胀/收缩/顺逆时针旋转/平滑/重建），
     画布内按住拖动涂抹、逐笔累积；`[` `]` 调笔刷，右键拖动平移、滚轮缩放、`Ctrl+Z` 撤销；
   - **滤镜窗口**：左侧缩略图墙（按可见区域惰性生成），点缩略图换滤镜，右侧调参；
   - **模糊 / 调整窗口**：顶部按钮切换效果，右侧参数面板；
   - 每个窗口都是滑杆 + 数值输入框（PS 风格），窗口位置会被记住；
   - 勾主面板「叠加处理」可基于上一次结果继续处理。
5. 点功能窗口的「应用 → 剪贴板」（或 `Ctrl+Alt+A`）
6. 面板会自动切回 SAI2 并发送 `Ctrl+V`，效果落到画布上
   （不想要自动粘贴可取消勾选，手动到 SAI2 按 `Ctrl+V`）

### 界面结构（一功能一窗口）

```
主面板（启动器）                          功能窗口（各自独立、互不干扰）
┌────────────────────────────┐          ┌───────────────────────────────┐
│ 预览（当前抓到的图层）       │  模糊 →  │ 效果条 + 预览 + 参数面板        │
│ 功能：[模糊][滤镜][调整][液化]│  滤镜 →  │ 缩略图墙 + 预览 + 参数面板      │
│ SAI2 图层列表 + 注入按钮     │  调整 →  │ 效果条 + 预览 + 参数面板        │
│ 选项 / 应用 / 撤销 / 复位     │  液化 →  │ 7 工具竖排 + 画布涂抹 + 参数面板 │
└────────────────────────────┘          └───────────────────────────────┘
```

每个窗口持有独立预览会话（显示倍率、平移、液化置换场）；液化窗口以全分辨率
基准求解置换场、显示图按像素预算降采样，因此"窗口里看到的形变"与"应用结果"
一致，而且**不会重放笔画**（刀笔顺序不可重放）。

### 快捷键（全局，可在其它窗口中使用）

| 快捷键 | 作用 |
| --- | --- |
| `Ctrl+Alt+G` | 抓取剪贴板 |
| `Ctrl+Alt+A` | 应用并回写剪贴板（+自动粘贴） |
| `Ctrl+Alt+Z` | 撤销上一次应用 |
| `Ctrl+Alt+R` | 复位到抓取到的原图 |

功能窗口内：`[` / `]` 调笔刷大小（液化）、`Ctrl+Z` 撤销、`Esc` 关闭窗口、
滚轮缩放画布、右键拖动平移。

### 命令行

```
sai2_bridge.exe                                   # 启动主面板
sai2_bridge.exe --plugins <dir> --logs <dir>      # 指定插件/日志目录
sai2_bridge.exe --selftest --out <dir>            # 无界面端到端自检
sai2_bridge.exe --uitest                          # UI 冒烟：打开 4 个功能窗口后自动退出
sai2_bridge.exe --dump-clipboard <out.bmp>        # 只读剪贴板位图并导出 BMP（互操作验证）
```

---

## 四、宿主参数通道协议（Plugin API 1.1）

`IHostContext` 末尾追加了一个带默认实现的虚函数，**1.0 插件无需改动即可运行**：

```cpp
virtual const char* GetParam(const char* key);  // 键不存在返回 nullptr
```

插件侧用 `sai2/ParamHelpers.h` 读取，键缺失时回落到自身默认值，
于是"没有参数通道的宿主"（如测试用 `MockHostContext`）行为与 1.0 完全一致。

### 约定键

| 键 | 取值 | 说明 |
| --- | --- | --- |
| `blur.kind` | `gaussian` \| `box` \| `motion` \| `radial` | 模糊种类 |
| `blur.radius` | 浮点，像素 | 高斯/方框半径 |
| `blur.angle` / `blur.distance` | 浮点 | 动感模糊方向 / 距离 |
| `blur.amount` | 0..1 | 径向模糊强度 |
| `blur.samples` | 整数 | 采样数 |
| `filter.id` | 滤镜 id，如 `com.sai2.filter.oil-paint` | 目标滤镜 |
| `filter.param.<name>` | 浮点 | 该滤镜的声明式参数（键名见 `FilterSet.h`） |
| `liquify.session` | 字符串 | 会话标识，**变化即代表换图**，插件据此重置缓存原图与置换场 |
| `liquify.tool` | `forward-warp` \| `bloat` \| `pinch` \| `twirl-cw` \| `twirl-ccw` \| `smooth` \| `reconstruct` | 液化工具 |
| `liquify.cx` / `liquify.cy` | 0..1 归一化 | 笔刷中心（尺度无关） |
| `liquify.radius` | 相对 `min(w,h)` 的比例 | 笔刷半径（尺度无关） |
| `liquify.strength` / `liquify.angle` / `liquify.dirX` / `liquify.dirY` | 浮点 | 强度 / 旋转角 / 拖动方向 |
| `liquify.reset` | `0`/`1` | 强制重置置换场 |

### 液化为什么强制全分辨率预览

置换场与像素尺度绑定、且插件跨笔累积状态，降采样预览会导致
"预览看到的"与"应用得到的"不一致。其余效果在预览时对降采样图执行，
并对**像素半径类参数**按比例缩放（在 `Effects.h` 里用 `pixelScale` 标记），
因此预览与最终结果观感一致，同时拖动滑块保持流畅。

---

## 五、自检（无需 SAI2）

```bash
./build-zig/tools/sai2_bridge.exe --selftest --out build-zig/selftest-out
```

自检会：合成测试图 → 剪贴板写入/读回 → 逐个效果全分辨率应用 → 再读回剪贴板
→ 与导出 BMP 比对 → 检查撤销/复位/叠加语义，并把每步图像导出为 BMP。
退出码 = 失败项数。

实测结果：**PASS=58 FAIL=0**，其中每一步的剪贴板往返均为
`maxDiff=0`（像素级无损）。

界面部分单独冒烟（会真的创建/绘制全部功能窗口，不碰 SAI2）：

```bash
./build-zig/tools/sai2_bridge.exe --uitest     # 打开 4 个功能窗口，数秒后自动退出（退出码 0）
```

自检曾发现一个真实缺陷：`FastBoxBlur` 从不创建目标缓冲区，
导致 `PencilSketch` 完全失效（输出与输入逐字节相同）、`Watercolor`
退化成单纯压暗。已修复（目标缓冲区按需 `Create`），修复后 14/14 测试套件
与自检全部通过。

---

## 五之二、剪贴板互操作性（实测记录）

### 两种真实存在的 CF_DIBV5 布局

GDI 在 `BI_BITFIELDS` 下会把三个颜色掩码**再写一遍**在头之后，于是同一个
"BITMAPV5HEADER"有两个都合法的像素起点：

| 生产者 | 布局 | 像素起点 |
| --- | --- | --- |
| GDI / GDI+（经 `CF_BITMAP` 合成，例如 .NET `Clipboard.SetImage`） | V5 头(124) + 掩码(12) + 像素 | **136** |
| 本工程 `WriteClipboardImage`（V5 头内已含掩码） | V5 头(124) + 像素 | **124** |

仅凭压缩标志无法区分。`ClipboardImage.cpp` 的解决办法是**用缓冲区总大小反推**：

```
pixelOffset = GlobalSize - stride × height
```

剪贴板里的 DIB 就是"头(+掩码) + 像素"、没有别的尾巴，所以这个推导是确定的；
推导值落在 `[headerSize, headerSize+16]` 之外时才回退到按头型计算的默认值。

> 这个缺陷是**用独立生产者做交叉验证**才暴露的：自家 writer/reader 闭环
> （自检里的往返 `maxDiff=0`）完全无法发现它——因为两边错得一致。

### 读取端交叉验证结果

用 .NET/GDI+（`System.Drawing` + `Clipboard.SetImage`）作为**独立生产者**投图到
剪贴板，再由 `sai2_bridge.exe --dump-clipboard` 读回比对：

| 用例 | 结果 |
| --- | --- |
| 完全不透明测试图（渐变 + 棋盘 + 圆 + 对角线） | **30000/30000 像素逐字节一致（0 差异）** |
| 含半透明区测试图 | 源图**完全不透明**的 27399 个像素**逐字节一致（0 差异）**；差异只出现在半透明区 |

半透明区的差异属于**生产者侧**：`.NET Clipboard.SetImage` 走的是 `CF_BITMAP`，
而 GDI 位图没有 alpha 通道，Windows 由它合成的 `CF_DIBV5` 里 `alphaMask = 0`。
读取端对此的处置是"alpha 声明存在但整幅为 0 → 按不透明处理"，
因此不会出现全透明黑洞，只是无法还原生产者本就丢掉的信息。
**SAI2 复制透明图层时发布的是带 alpha 的 DIB**，这条路径下 alpha 逐字节保真
（自检中对自家 DIBV5 的往返实测 `maxDiff=0`，含 alpha 通道）。

命令行：`sai2_bridge.exe --dump-clipboard <out.bmp>` —— 只把当前剪贴板位图导出为
BMP，用于验证"读取端能否正确解析别的程序写进来的 DIB"，不启动界面。

---

## 六、已知限制

1. **不是"在 SAI2 里面"**：效果通过剪贴板往返完成，需要一次 `Ctrl+C` / `Ctrl+V`
   （自动粘贴已默认开启）。点功能按钮时面板会自动完成"选中图层 + 全选 + 拷贝"，
   因此用户通常一次键都不用按。这是 SAI2 无扩展接口的必然结果。
2. **剪贴板格式限于 8bit DIB**：写入 `CF_DIBV5`（带 alpha）+ `CF_DIB`；
   SAI2 的透明图层 alpha 可保真。不提供 PNG 格式（工程坚持零第三方依赖，
   未内置 PNG 编解码）。
3. **选区**：SAI2 复制出来的选区在剪贴板里是**已裁切的位图**，因此插件看到的
   画布就是该选区；选区外的像素不参与处理（等价于 sel=nullptr 的全图处理）。
4. **液化需要宿主驱动**：无参数通道的旧宿主执行液化时走内置演示笔画（保持
   1.0 兼容），要得到交互式涂抹必须由 `sai2_bridge` 的液化窗口驱动。
5. **自动抓取需要注入代理在线**：点功能按钮时的"选中并抓取当前图层"依赖
   `sai2_agent.dll` 已注入（它提供图层行的屏幕坐标与菜单项定位）。没注入时
   面板退化为"已有图像就直接开窗，否则提示先在 SAI2 里 Ctrl+C"。
6. **功能窗口不保存参数**：重开窗口按当前效果的默认值起步（与 PS 对话框一致），
   只有窗口位置被记住。

## 七、SAI2 内浮窗入口（注入后）

注入 `sai2_agent.dll` 后，SAI2 菜单栏右侧会出现一排功能按钮
「模糊 | 滤镜 | 调整 | 液化」（浮窗随主窗口移动、最小化时隐藏）。点其中一个：

```
SAI2 内浮窗按钮 → agent 记录待处理请求（\\.\pipe\sai2agent）
   → sai2_bridge 轮询取走（take.request，拿到功能窗口索引 1..4）
   → 自动选中并抓取 SAI2 当前图层 → 打开对应的独立功能窗口
```

也就是说，在 SAI2 里点「液化」和在桥主面板上点「液化」的效果完全一样：
都弹出液化窗口，且都针对 SAI2 当前选中的图层。浮窗不再有下拉菜单，
每个功能一个按钮。

## 八、后续可选增强

- **资源包导出**（路线 A）：把滤镜/程序化纹理导出成 SAI2 原生
  `papertex` / `brushtex` / `scatter` 资源，作为本方案的互补。
- 每个功能窗口记住上次参数、预设保存/加载。
- 更多剪贴板格式（PNG）以兼容其它软件往返。
- 液化笔刷压力/速度曲线（`LiquifySession` 已具备引擎侧支持）。
- 液化笔画走插件侧 ROI（`liquify.roi*` 参数）以进一步降低大画布延迟。
