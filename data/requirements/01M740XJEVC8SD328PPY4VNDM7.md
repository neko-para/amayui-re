# emulator-lift · ID3DXSprite 无 ALPHABLEND 时按覆盖写像素（真机按设备混合状态）

- id: REQ-01M740XJEVC8SD328PPY4VNDM7
- type: bug
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/d3dx9_sprite.cpp#compositeBatch
- severity: S2
- dropped_reason: 实测不触发：1013/1013 次 Begin 全传 0x10 (D3DXSPRITE_ALPHABLEND)，无一次缺 0x10 ⇒ 混合分支已覆盖全部调用。初稿判据写成 Flags & 1 是笔误（0x1=DONOTSAVESTATE、ALPHABLEND=0x10），已在正文更正并留 .lst 佐证。不修、不加守卫（无触发面；为当前 flags 取值设断言属脆守卫）。
- tags: [emulator-lift d3dx9 sprite blend]

## 一句话（原假设）
`ID3DXSprite::Begin(flags)` 在本线只认识 `ALPHABLEND`；原假设：`Begin(0)`（无 `ALPHABLEND`）时真机按设备当前混合状态混合，
我们却按"不透明覆盖"写像素。

## 实测结论：**不触发**（2026-11-11）
在当前 `out/emulator-lift.exe` 上读 `[d3dx9] Begin: Flags=…`：
- `--skip-logo --max-frames 8`：201 次 Begin，**201×0x10**
- `--skip-logo --max-frames 30`：751 次，**751×0x10**
- 含 logo `--max-frames 20`：61 次，**61×0x10**

合计 **1013/1013 全为 `0x10`**，其余位恒 0；Begin/End 一一配对 ⇒ **混合分支覆盖了全部调用**，不存在"无 ALPHABLEND"的批。

## ★ 顺带更正一个判据错误（本单初稿写错）
`D3DXSPRITE_ALPHABLEND = 0x10`，而 `0x1 = D3DXSPRITE_DONOTSAVESTATE`。
初稿写的 `Flags & 1` 会把实测的 `0x10` 误判成"无 ALPHABLEND"（假触发）。
佐证：`.lst` `0x4A30E5 mov edx,[ecx+20h]` / `0x4A30E8 push 10h` / `0x4A30EB call edx`；仓库既有 `01M6MJRC…md:28` 也记的是 `0x10`。

## 处置
按"**不是缺陷**"收口：
- 不改代码、不加守卫（没有触发面，加守卫只会锁住"当前 flags 取值"，属 AGENTS §10 禁的脆断言）；
- 若将来出现 `Flags & 0x10 == 0` 的批，**那时**按"设备混合状态"实现；届时判据已在这里写清（`& 0x10`）。

## 未做
- `SAVE_STATE` 的保存/还原：我们从不改设备状态 ⇒ 无需还原（"空真"）。
- `SORT_*`：排序只影响绘制顺序，本线按调用序。
