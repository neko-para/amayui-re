# emulator-lift · 存档缩略图 sub_4A42C0（ShrinkTexture）函数级 native override

- id: REQ-01M7BY4FMA9VPETYEEV4V7RF2Y
- type: req
- status: open
- parent: REQ-01M7CZQZK3QQJBP5XCX88B4CTE
- tags: [emulator-lift, native-override, save-load]

# emulator-lift · 存档缩略图 `sub_4A42C0`（ShrinkTexture）函数级 native override

确认保存窗里**最大的一项**（`OBS-20261114-07`）：`sub_4A42C0` self **2,767.7 ms** / incl **7,599.4 ms** / **calls=1**
（占比 18.3% / 50.2%）。它只被调一次就把 1280×720 缩成 320×180，然后交给 `UpdateSurface`。

## 身份（锚 EA）

| EA | 原文 | 结论 |
|---|---|---|
| `0x4A42C0` | `sub_4A42C0 proc near`；三处 `retn 10h`（`0x4A4368` / `0x4A481B` / `0x4A4874`） | `__thiscall`，**4 个栈参**（源纹理 id、目标纹理 id、源 rect、目标 rect）；函数级 `arg_bytes = 16` |
| `0x4A4315` / `0x4A4379` | `call sub_4A0430` | 两次取 surface（`GetDesc`+`CreateOffscreenPlainSurface`+`GetRenderTargetData`） |
| `0x4A432F` / `0x4A4393` | `call eax/edx`（虚表 `+0x34`） | 两次 `LockRect` |
| `0x4A44F8` | `call sub_49EE60` | **唯一采样点**，在最内层循环；`sub_49EE60` = 8 条指令的"取一个源像素"（32bpp 直读 / 16bpp A4R4G4B4 展开），**无边界检查** |
| `0x4A43F4..0x4A4441` / `0x4A465A..0x4A46D8` | 各 4 次 `call __ftol2_sse` | **每个目标像素 8 次** guest↔native 往返（`__ftol2_sse` 已是函数级 override，但每次仍付 dispatcher + x87 同步） |
| `0x4A476A` / `0x4A4775` / `0x4A479D` | `UnlockRect` ×2 / `UpdateSurface`（设备 `+0x78`） | 收口 |
| `0x4A47D7..0x4A4867` | 8 处 `Release` | 错误路径与收口 |

调用量（观测）：`sub_49EE60 calls=921,600` = 320×180×16（每目标像素 4×4 源样本）；
`__ftol2_sse` 应为 57,600×8 = 460,800 次（**未从该窗函数表读出，见"未取证"**）。

## 判据

1. **进表**：函数级，`arg_bytes = 16`、`GuestConv::Thiscall`、只接管**主路径**
   （32bpp 源 + 32bpp 目标 + 整数缩放 4×4）；其余一律 Declined 回退。
2. **整数缩放 ⇒ 逐字节可验收**（本候选唯一让它可验收的性质）：
   `srcW % dstW == 0 && srcH % dstH == 0` 时每个跨度权重恰好 1.0、权重和恰好 16.0、各通道和是整数、
   `(int)(Σ/16 + 0.5)` **恰好等于** `(Σ + 8) >> 4` ⇒ 纯整数实现，**不做数值容忍**。
   缩略图像素必须与解释器路径**逐字节相同**（`SAVE??.STH` 的 sha256 相同）。
3. **不写 guest 内存**（除目标 surface 的 `LockRect` 区域 —— 那是本函数的产物）；
   不建表、不分配、不回调 guest。
4. **闸门在任何 D3D 调用 / 任何写之前**：源与目标都是 32bpp、rect 在界内、整数缩放、
   `ddraw::displayMode` 与 texture desc 一致；任一不满足 ⇒ Declined。
5. **读数**：`all` vs `all,-sub_4A42C0` 两臂的 `present-tail tail_wall_ms`（保存段）。

## 未取证（动它之前必须先补）

- `incl 7,599.4 − self 2,767.7 − sub_49EE60 496.1 − sub_4A0430 2.5 ≈ 4,333 ms` **去哪了**：可能是 8 处 `Release` /
  2 处 `LockRect` / `UpdateSurface`（都走 COM 虚表 + 令牌），也可能是影子栈 `unbalanced`。
- `__ftol2_sse` 在保存窗里的 `calls` / `self` **没读**（应 ≈460,800；若不是，说明目标像素数不是 320×180）。
- 16bpp 目标路径（`dword_5518C0 == 16`）**没有样本** ⇒ 闸门只收 32bpp，16bpp 回退。
- `sub_49EE60` 无边界检查 ⇒ native 实现**必须自己夹**源坐标（否则行为与解释器不同：解释器会读相邻内存）。
  这一条要先裁决"夹"还是"照抄越界读"，否则逐字节判据会红。

## 为什么它排第二

收益比 CRC 大，但闸门多、x87 语义多、`sub_49EE60` 的无界行为要单独裁决 ⇒ 风险高一个档。
`sub_436D00` / `sub_436D50` 那两条是纯查表、零写、可判定回退，先做。
