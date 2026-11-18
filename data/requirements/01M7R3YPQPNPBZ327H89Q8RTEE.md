# 🐞S2 `ID3DXSprite::Begin(ALPHABLEND)` 没有像 D3DX 那样先设 SRCALPHA/INVSRCALPHA ⇒ 普通 sprite 变成 ONE/ZERO 不透明拷贝（透明黑 texel 写进后缓冲）

- id: REQ-01M7R3YPQPNPBZ327H89Q8RTEE
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
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

## 要做什么

1. `d3dx9::Begin`：带 `D3DXSPRITE_ALPHABLEND` 时，按真 D3DX 的语义**写设备状态**
   （至少 `kRsAlphaBlendEnable=1` / `kRsSrcBlend=5` / `kRsDestBlend=6`；其余 D3DX 会改的状态项按现有实现边界决定，
   ⛔ 不要顺手扩到无关状态），并把**原值**存进 sprite 状态块；`End` 时恢复（含 `Flush` 不恢复）。
2. 加性那条不动：引擎在 `Begin` 之后、`End` 之前写 `19=5` / `20=2` ⇒ `End` 读到的仍是 `5/2`。
3. ⛔ 不要退回"sprite 路写死 5/6"（那就又把 BUG-33 修回去）。

## 判据（守卫）

- pure/单元：`Begin(ALPHABLEND)` 之后设备状态块里 `19==5`、`20==6`；`End` 之后**恢复**到 `Begin` 前那对值
  （夹具给一对非 5/6 的初值，断言逐字回原）。
- **像素回归**：一张 16×16 源图（`alpha=0` 的黑色 texel 铺满，中间一个 `alpha=255` 的红点）画到
  纯色后缓冲上 ⇒ 背景像素**一个都不许变成黑**，只有红点被写下（修前写法靠 5/6 能过，现在必须靠 Begin 设状态）。
- 反例守护：把 `Begin` 里那几句状态写删掉 ⇒ 上面两条**当场红**（这正是本次回归的形态）。
- e2e（不进默认门禁）：SAVE05 里走廊地面 / 对话框底板**不再是黑块**，同时 120×120 黑块**仍然没有**。

## 复现（e2e 读数出处）

`docs/03-emulator/save05-black-rect.md#复现`；对照图 `.tmp/bug33-e2e/shots/{cmp-b011-vs-fx1,fx1,fx6,B06,B09}.png`。

## 边界

- 这条与 BUG-33 **不是**回退关系：一条是"因子必须来自状态"，一条是"`Begin` 必须像 D3DX 那样先把默认因子设好"。
- ⛔ 不要把"引擎没设过 19/20"当成"引擎想要 2/1" —— 真机上 D3DX 早就替它设成 5/6 了。
