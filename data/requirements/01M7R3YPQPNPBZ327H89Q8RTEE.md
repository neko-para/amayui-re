# 🐞S2 `ID3DXSprite::Begin(ALPHABLEND)` 没有像 D3DX 那样先设 SRCALPHA/INVSRCALPHA ⇒ 普通 sprite 变成 ONE/ZERO 不透明拷贝（透明黑 texel 写进后缓冲）

- id: REQ-01M7R3YPQPNPBZ327H89Q8RTEE
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3dx9_sprite_blend.cpp#d3dx9-sprite-blend
- repro: docs/03-emulator/save05-black-rect.md#复现
- severity: S2
- tags: [emulator-lift, d3dx9, sprite, blend, regression]

## 现象（2026-11-19 · 修 BUG-33 之后暴露）

`ID3DXSprite` 的批次现在按**设备渲染状态**取混合因子（BUG-33 的修法），于是普通 sprite 批次读到的是
引擎留在设备里的 `SRCBLEND=ONE(2)` / `DESTBLEND=ZERO(1)` ⇒ **不透明拷贝**：
源图里 alpha=0 的黑色 texel 被原样写进 guest 后缓冲。

可见后果（e2e 实测，`.tmp/bug33-e2e/report.md`）：走廊石地、教程暗化页、对话框底板**全黑**
—— 老对话框有边框花纹（1.23% 黑），新截图对话框 **33.70% 黑**（同一 `MES` 帧 `lastHash` 两次 run 一致）。

## 已核事实（读数）

- 普查（每条批次 `End` 时生效的 `D3DRS_SRCBLEND/DESTBLEND`）：修后对话框帧 **383/383 批次 = 2/1**、
  特效帧 370/371 = 2/1（唯一 5/2 是那条加性特效）⇒ 引擎本来就一直把 `2/1` 留在设备里。
- **修前**（`.tmp/save05-black/slice_f48/f49`）普查**完全相同**（370/371 = 2/1）⇒ 修前是
  "sprite 路写死 5/6" 碰巧对；它从来没读过状态。
- 真 D3DX9 的语义：`ID3DXSprite::Begin(D3DXSPRITE_ALPHABLEND)` **自己**把
  `ALPHABLENDENABLE` / `SRCBLEND=SRCALPHA(5)` / `DESTBLEND=INVSRCALPHA(6)` 设好（并在 `End` 保存/恢复它改过的那批状态）。

## 根因

我们把"混合因子来自设备状态"实现对了（BUG-33），但**漏了 D3DX 自己在 `Begin` 里改状态这一步**：
引擎不会替 sprite 设 `5/6`，它只会在需要加性的那一条之前补 `5/2`。

## 修法（2026-11-19 落地，dev `a0fd69e`）

- `d3d9_pipeline.{h,cpp}`：新增写入口 `writeRenderState(X86&, dev_state, state, value, why)`
  （`pipelineOf` + `setRs` 同源，⛔ 不另写偏移）。
- `d3dx9_sprite.cpp`：sprite 状态块末尾追加 `saved_valid` / `saved_alpha_blend_enable` /
  `saved_src_blend` / `saved_dst_blend`（`kSpBytes` 128 → 140；这块是**我们自己**分配的对象，不是 guest ABI）。
  `Begin(ALPHABLEND)`：读设备 `27/19/20` 落盘原值 → 写 `27=1 / 19=5 / 20=6`；`End` 恢复，`Flush` 不恢复。
  取不到设备 / 没有流水线状态块 ⇒ `saved_valid=0` + 一条告警、**不改** Begin 的返回码。
- 因子仍然只从设备状态**读**（`readBlendState` + `gpuBlendStateFor` 一行未改）⇒ BUG-33 没有回退。
- 边界（本轮不动）：`D3DXSPRITE_DO_NOT_SAVE_STATE` / `DO_NOT_MODIFY_RENDER_STATE` 仍"记录并忽略"；
  Begin/End 之间销毁 sprite 或 `OnLostDevice` 不做恢复；⛔ 不扩到 `ALPHATESTENABLE` / `ZENABLE` / `LIGHTING`。

## 守卫 / 读数（2026-11-19 / win32 / RelWithDebInfo）

- CTest `d3dx9-sprite-blend`（**400 checks**）新增两条子用例：
  `d3dx9-sprite-blend/begin-restores-blend-state`（Begin 后 27/19/20 = 1/5/6；End 逐字回夹具初值 0/3/9；
  第二次可重复）、`d3dx9-sprite-blend/pixel-transparent-texel`（16×16 源图 = alpha0 黑铺满 + 中心 a255 红点，
  按 D3D9 混合方程作用到纯色后缓冲：`blackened==0` / `unchanged==255` / `red_written==1`，附 ONE/ZERO 负例）。
  `d3dx9-sprite-blend/wiring` 同名改判据：引擎在 Begin 之后写 5/2 ⇒ 仍加性。
- 变异 `sprite-begin-alphablend-state-writes`（禁掉 Begin 的三句写）⇒ **400 checks / 12 failures**，当场红；
  还原后 400 / 0。全量 ctest **127/127**（139.99 s）；`pnpm test` 587/587。

## e2e（SAVE05，私有 appdata，`saves/` 未动；warn 档 frames=808，正常结局）

| 帧段 | 全屏黑 | 对照 |
|---|---|---|
| 对话框（`MES` 772–808） | **1.23%** | 回归时 33.70% / 修前 1.23%；**有边框花纹** |
| 特效（`FIELD 0x3E118`，742–766） | 1.14–1.15%，搜索区 1.0–1.18% | **120×120 黑块不在**，白星在宝藏上可见；走廊石地可见 |

截图：dev `.tmp/sprite-begin-e2e/shots/f3.png`（特效）/ `f8.png`（对话框）。
⚠ 本轮 warn 档，未重做逐批次 19/20 普查；机制判据是单测（含红读数）。

## 边界

- 这条与 BUG-33 **不是**回退关系：一条是"因子必须来自状态"，一条是"`Begin` 必须像 D3DX 那样先把默认因子设好"。
- ⛔ 不要把"引擎没设过 19/20"当成"引擎想要 2/1" —— 真机上 D3DX 早就替它设成 5/6 了。
