# Image API 规格（Phase 2）

## 像素格式

| 格式 | 每通道 | 每像素 | 通道取值 |
| --- | --- | --- | --- |
| `PixelFormat::RGBA8` | 1 字节 | 32 bit | 0..255 |
| `PixelFormat::RGBA16` | 2 字节 | 64 bit | 0..65535 |
| `RGBA32F`（预留） | 4 字节 | 128 bit | 0.0..1.0 |

- 内存布局**紧凑无行填充**：`Stride = Width × 4 × BytesPerChannel`；
- 所有接口统一以 `RGBA{uint16_t r,g,b,a}` 承载通道值，写入时按格式截断；
- `ImageBuffer::Convert` 在 8↔16 位间线性缩放（255 ⇄ 65535）。

## 核心操作

```cpp
ImageBuffer buf(1920, 1080, PixelFormat::RGBA8);
buf.FillRect(10, 10, 100, 80, RGBA{0, 0xFFFF, 0, 0xFF}); // 自动裁剪
ImageBuffer copy = buf.Clone();
ImageBuffer wide = ImageBuffer::Convert(buf, PixelFormat::RGBA16);
```

## 选区（Selection）

- **矩形选区**：`Selection(Rect{x,y,w,h})`；
- **蒙版选区**：`SetMask(w, h, mask)`，8bit 灰度、非 0 即选中；
- `Contains(px, py)` 供插件逐像素判断；`Bounds()` 供局部处理（Liquify 的
  Region 更新、滤镜的 ROI 优化都基于它）；
- `GetSelection() == nullptr` 表示**全图有效**。

## Undo（规格书第七节）

- **一次完整的插件操作 = 一个 UndoGroup**，而不是每次笔刷移动生成一条撤销记录；
- 组内可压入多张快照（分步操作），撤销时**整组恢复**（恢复组内最早快照）；
- 宿主默认保留 50 组（`UndoStack(maxGroups)` 可调）；
- 插件侧约定：`BeginUndoGroup → SnapshotForUndo(修改前) → 处理 → RefreshCanvas → EndUndoGroup`。

## Preview（规格书第七/八/九节）

```
BeginPreview(maxDim) ──► 得到最近邻降采样缓冲（最长边 ≤ maxDim）
        │                    插件在预览缓冲上反复调参（30~60 FPS 目标）
        ▼
EndPreview(commit)
   ├─ commit=true : 预览结果回写主画布 + RefreshCanvas
   └─ commit=false: 放弃，主画布不动
```

- 预览阶段用 **Preview Buffer**（缩小图），确定后再对全分辨率图执行；
- 后续 GPU 加速（Phase 4 的 `IComputeBackend`）只替换实现，不动本契约。

## Canvas Refresh

- `RefreshCanvas()` 请求宿主重绘画布；`Undo` 成功后宿主自动刷新；
- 测试宿主 `MockHostContext` 用 `refreshCount_` 计数验证该契约。
