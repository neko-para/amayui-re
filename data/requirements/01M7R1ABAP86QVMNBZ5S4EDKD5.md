# 🐞S2 `ID3DXSprite` 批次把混合因子写死 SrcAlpha/InvSrcAlpha：引擎的加性（SRCALPHA/ONE）被丢掉 ⇒ sprite 的黑底被不透明盖成黑块

- id: REQ-01M7R1ABAP86QVMNBZ5S4EDKD5
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3dx9_sprite_blend.cpp#d3dx9-sprite-blend
- repro: docs/03-emulator/save05-black-rect.md#复现
- severity: S2
- tags: [emulator-lift, d3dx9, sprite, blend, render]

## 现象

某个 sprite 被画成 **120×120 黑块**：引擎用**加性**混合（`SRCBLEND=SRCALPHA(5)` + `DESTBLEND=ONE(2)`）
画一个 sprite，它的源图几乎全黑（225 个采样点只有 14 个非零 = 一颗白星）；本仓的 sprite 合成路把混合因子
写死成 `SrcAlpha / InvSrcAlpha` ⇒ 黑底被**不透明**盖住 ⇒ 黑块。
在 SAVE05（SC0000 第一个地图）点击宝藏 740,355 后的特效里观察到；取证全文
`docs/03-emulator/save05-black-rect.md`。

★ **措辞纪律（用户 2026-11-19）**：这件事**与"影片"无关**。同帧那条 dshow 影片只是**相关读数**
（120×120 强相关、未直读），⛔ 不得写成因果断言。

## 已核事实（锚）

- 帧 49 唯一一条加性批次：`State=19⇒0x5` + `State=20⇒0x2`，随后
  `[d3dx9] End: 合成 1 条 Draw / 14400 个覆盖格`（=120×120）；帧 48 的 `DESTBLEND=ONE` 出现 **0** 次。
- 黑在 **guest 后缓冲**里：`screenshot.source=guest-backbuffer` + `hash_still_matches=true`。
- 排除：DDraw 三帧切片命中 0 次；本轮无 HUD；`b1b6` 置 bit4（关掉 SO033A 覆盖图）后黑块仍在。

## 根因

`src/runtime/d3dx9_sprite.cpp` 的 `compositeBatch` 只从 `Begin` 的 flags 取一个 bool 写 `gs_state.blend`，
`src_blend` / `dst_blend` / `blend_op` / alpha 段**一个都没赋** ⇒ 落到 `src/runtime/gpu_raster.h` 的缺省
`SrcAlpha / InvSrcAlpha`；`rg "kRsSrcBlend|kRsDestBlend" src/runtime/d3dx9_sprite.cpp` = **0 命中**
（fixed-function 路会读那两个状态）。

## 修法（2026-11-19 落地，dev `d947cab`）

- `d3d9_pipeline.{h,cpp}`：新增 `BlendStateSnapshot` + `readBlendState()`，与 fixed-function 的
  `prepareContext` 共用**唯一**读点 `readBlendStateAt`（8 个 `D3DRS` 只在一处出现，读数逐字不变）。
- `d3dx9_sprite.{h,cpp}`：新增纯判定 `spriteBlendState()`，**只转发** `gpuBlendStateFor`（同一份映射；
  ⛔ 没有第二套 switch、⛔ 没有"没状态就用缺省因子"）。`compositeBatch` **每批读一次**设备状态，
  `GpuRasterState` 从快照拷贝后再覆盖 viewport / scissor / depth。
  `Begin` 的 `ALPHABLEND` 只决定**开不开**混合（D3DX 语义），因子 / op 一律来自设备状态。
- 读不到 / 表达不了 ⇒ 这一批每条 draw `gpuNoteSpriteNotDrawn` + 跳过，只打**一条**批次级 `spriteWarn`
  （sprite 路没有 CPU 兜底；⛔ 不退回写死的 `SrcAlpha/InvSrcAlpha`）。

## 守卫 / 读数（2026-11-19 / win32 / RelWithDebInfo）

- CTest `d3dx9-sprite-blend`（66 checks）：①加性 ②`ALPHABLEND=0`（因子不被使用）
  ③separate alpha 开/关 ④不可表达 ⇒ false + `why` ⑤**接线**（真实 `Begin/Draw/End` + mock `GpuRaster`
  截下 `rasterQueue` 的那一份状态：加性必须是 `SrcAlpha/One`，改 `DESTBLEND` 要跟着变，不可表达时**不入队**）。
- 改前红：把 `spriteBlendState` 改成忽略状态 ⇒ **66 checks / 14 failures**；改后 66 / 0。
- 全量 ctest **127/127**（127.36 s）；`pnpm test` **587/587**；变异 `sprite-blend-state-ignored` ⇒ 当场红（CTest 8）。
- `tests/d3dx9.cpp` 的两条 ALPHABLEND 像素用例改成**显式**设 `SrcAlpha/InvSrcAlpha`
  （因子现在来自设备状态，D3D9 缺省是 One/Zero）；断言与期望值**没有改弱**。

## 未定论 / 未做

- 没有一行日志直读"这条 sprite 采样的是哪张 texture"（120×120 是**强相关**，不是直读）；要钉死需要给
  `[d3dx9] Draw` / `End` 加一行 opt-in 读数（纹理对象 + src rect）。
- **e2e 未做**：预期 SAVE05 里黑块消失、看到那颗白星。同帧影片"只送 1 帧"是**另一条单**
  （`REQ-01M7R1AR688T09G2T65GY8SZGG`），与本单无因果关系，本轮不碰。
