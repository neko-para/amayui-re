# emulator-lift · M2 · 2D 画面与文本（窗口/消息 · DirectDraw · GDI 字体·停手点 A）

- id: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- type: req
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [emulator-lift m2]

## 一句话
在 M1 的 shim 框架之上补齐 **2D 画面 + 文本**：窗口 / 消息 / 区域 · DirectDraw 2D 表面 · GDI 字体度量与栅格化。
★ 这是 `platform-effort.md` §4 的**停手点 A**（最重要的验收点，也是这条线的 go/no-go）。

## 范围（真源：`repo:docs/03-emulator/platform-effort.md`）
- **§3.2 窗口 / 消息 / region**：USER32 62 导入 / 261 站点 / 102 caller。菜单栏按 `SCOPE.md` 整体 no-op（`LoadMenuA` 返非空假句柄、`SetMenu` TRUE、`TrackPopupMenu` 立即返回 0）；**窗口创建 / 消息泵 / 输入 / region / 计时器必须真做**。
- **§3.4 DirectDraw 2D 表面**：`CreateSurface` / `Blt` / `BltFast` / `Flip` / `Lock` / `Unlock` / `GetDC` / `ReleaseDC` / `SetDisplayMode` / `SetCooperativeLevel`；颜色键、越界裁剪、`Lock` 的 pitch 与通道掩码。
- **§3.3 GDI 字体**：`SelectObject` / `GetTextMetricsA` / `GetGlyphOutlineA`（`GGO_BITMAP` 1 位 / `GGO_GRAY4` 17 级 / `GGO_GRAY8` 65 级）/ `GetTextExtentPoint32A` / `TextOutA`。
- **资源族（4 条）也归 M2**（用户 2026-11-04：资源等 M2）：`FindResourceA` / `SizeofResource` / `LoadResource` / `LockResource`。
  ★ 实测它们**不在 PE 导入表、`.lst` 里也没有调用点** —— guest 从不直呼它们；入口卡点 `USER32!LoadIconA`（EA `0x004BA9AA`）要的是
  **读 PE 资源目录**（`RT_GROUP_ICON` / `RT_ICON`），那可以由 `LoadIconA` 的实现直接做。⇒ M2 开工时二选一：把四条真做出来给内部用，
  或删掉这四条"没人调用"的注册（删要走 `pnpm tools lift sync --write`）。M1 期间它们保持 `addUnimplemented`（不判红）。
- **模态（有限，用户 2026-11-04）**：只做**关闭窗口的二次确认**（`MessageBoxA` 的 OK / Cancel 语义）；
  ⛔ 不做 AGERC 提供的复杂设置对话框（AGERC 整体仍按 stub）。
- **COM / VARIANT（用户 2026-11-04 裁决归 M2）**：`ole32!CoCreateInstance` / `PropVariantClear` +
  `oleaut32!SysStringLen` / `VariantClear` / `VariantInit`（5 条）；实现随 M2 的 AGERC / 图像路径。
- **小族（用户 2026-11-04 裁决：正常跟随 M2，不是永久偏差）**：
  `version` 3（`GetFileVersionInfoSizeA` / `GetFileVersionInfoA` / `VerQueryValueA`：读 PE `VS_VERSIONINFO`，
  比较 AGE / AGERC 版本）· `shell32!SHGetSpecialFolderPathA`（特殊目录：我的文档 / AppData，5 个调用点）·
  `shlwapi!PathRemoveFileSpecA`（路径去文件名，3 个调用点）· `imagehlp!MakeSureDirectoryPathExists`
  （递归建目录，3 个调用点）· `imm32!ImmAssociateContext`（窗口 IME，1 个调用点）。
- 可复用的是**知识**不是代码：`repo:apps/emulator-cxx/src/core/text_alpha.h`（17/65 级量化）、`repo:apps/emulator-cxx/SCOPE.md` §1.1（基线补偿实测值）。

## 依赖与硬约束
- 依赖 M1 已交付：shim ABI / 句柄表 / guest 堆 / 虚拟时钟 / 文件与路径族 / **覆盖清单**（新桩必须有地方登记 reason）。
- ★ **字体簇不能单独替换**（§3.3 第 4 点）：度量跑在 `CreateICA("DISPLAY")` 的**屏幕 DC**，绘制跑在 **DDraw 表面 DC**（表面 vtable **+68**，错误串「GetDC エラー」作证）⇒ **DDraw 必须先到**；两个 DC 的字体 realization 必须一致，否则「量出来的宽 ≠ 画出来的宽」。
- 显示模式 / 全屏只能走 DDraw（本作**没有** `ChangeDisplaySettings*` 导入）。

## 判据（开工时写实；现在只给轮廓）
- 文本自洽：`GetTextExtentPoint32A` 的宽 = `GetGlyphOutlineA` 的 advance 之和 = 实际绘制宽度。
- 画面：颜色键正确（透明处不变黑框）、越界裁剪正确、`Lock` 的 pitch/掩码能让存档缩略图不花。
- **停手点 A**：这两簇实测工期若超 ≈3–5 人月 ⇒ 「复现 Windows 平台语义」比按 spec 重写更贵，退回 emulator / 串流。

## 不做
菜单栏交互、注册表、摇杆（`SCOPE.md` §1：**永久 no-op**）；3D 与 Live2D（M3）。
