# M2 P2 · GDI 字体/文本（宿主 Amayui CN + 三档 GGO + 自洽）

- id: REQ-01M6KHDTBREGT6Z90E4EHPKE6M
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gdi.cpp#extent-advance-self-consistency
- tags: [emulator-lift, m2]

## 一句话

在 `apps/emulator-lift` 上把 **GDI 字体 / 文本** 从 stub 变成"语义对、能跑、有会红的守卫"：
GDI 对象表（DC / 字体）走现有 `HandleTable`，字形一律来自宿主 **Amayui CN**（FreeType 桥），
`GetGlyphOutlineA` 支持 1 位 / 17 级 / 65 级三档，`extent == Σ gmCellIncX == TextOutA 推进` 走同一条路径。

## 做了什么

- **对象与状态**（`src/shims/gdi32_object.cpp` + `src/runtime/font.{h,cpp}`）：
  `CreateICA` / `GetStockObject` / `SelectObject` / `DeleteObject` / `DeleteDC` / `GetCurrentObject` /
  `SetBkMode` / `SetTextColor`。DC 有"当前字体（值 + 句柄）/ 文字色 / 背景模式 / 目标表面 / 屏幕 DC 记账"；
  `SelectObject` 返回旧对象、再选回去能恢复；`DeleteObject` 对仍被选中的对象返回 FALSE 且**不释放**。
- **字体 / 度量 / 字形 / 文本**（`src/shims/gdi32_font.cpp` + `src/runtime/font.cpp`）：
  `CreateFontIndirectA` / `GetTextMetricsA` / `GetGlyphOutlineA` / `GetTextExtentPoint32A` / `TextOutA` /
  `EnumFontFamiliesExA`（guest 回调走 `GuestCallStack`，宿主字体一条）/ `AddFontMemResourceEx` /
  `RemoveFontMemResourceEx`。
- **宿主字体策略**（照 `apps/emulator-cxx/SCOPE.md` §0 第 1 条 / §1.1）：总是 Amayui CN，
  忽略引擎字面名与 `AddFontMemResourceEx` 的内存；基线补偿 **+2px**（`--baseline-lift`，0 = 关；
  宿主视觉策略，登记在 `apps/emulator-lift/SCOPE.md` §5）；CLI 新增
  `--font` / `--baseline-lift` / `--font-engine-name`（A/B 对照档只影响上报族名）。
- **GGO 格式算术**：`GGO_BITMAP` 1bpp、行 `((w+31)>>5)<<2`；`GGO_GRAY4/8` 每像素 1 字节、
  行 `(w+3)&~3`、值域 0..16 / 0..64；`cbBuffer == 0` 返回所需字节数（同时照常写 `GLYPHMETRICS`）。
- **与 P1 的接缝**：`fontAttachDcSurface(hdc, surface_ea, SurfaceArena*)` / `fontDetachDc(hdc)`
  在 `src/runtime/font.h`；P1 的 `GetDC`/`ReleaseDC` 调它，`TextOutA` 就写进那块 guest surface。

## 偏差

- 1px 级栅格差（FT vs 真机 GDI）按平台 discard；`TextOutA` 用 FT 连续覆盖率（GDI 默认 1 位）。
- `--font-engine-name` 在本平台只到"上报族名"：lift 没有按系统族名查字的入口，字形仍来自宿主文件。
- `AddFontMemResourceEx` 忽略内存但按真机合同返回非 0 句柄 + `*pNumFonts = 1`（宿主字体确实可用，日志明说）。
- 屏幕 DC 不落宿主窗口（确定性记账：调用 / 字形 / 像素数）。

## 剩余

- `GGO_NATIVE` / `GGO_GRAY2` / `GGO_LCD` / 非单位 MAT2：如实拒绝（`GDI_ERROR` + 日志），未实现。
- `CreateCompatibleDC` / `CreateDIBSection` / `BitBlt` / `GetDeviceCaps` 仍 `addUnimplemented`（P1 / P4）。
- 8bpp 调色板表面不建模。
