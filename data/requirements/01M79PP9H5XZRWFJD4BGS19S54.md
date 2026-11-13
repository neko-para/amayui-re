# emulator-lift · D3D9 ValidateDevice：本轮拒绝实现（显式 1 pass + 翻面条件 + 防漂移守卫）

- id: REQ-01M79PP9H5XZRWFJD4BGS19S54
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/validate-device-one-pass
- tags: [emulator-lift, d3d9]

## 缺口（索引第 3 行拆出）

`ValidateDevice` 今天恒返回 `S_OK, *pNumPasses=1`。语义上它问的是"**当前状态组合**能否被设备渲染 / 需要几 pass"，
与"这条 draw 合不合法"无关（`BeginScene` / FVF / 流绑定不属它）。

`.lst` 复核：`[reg+11Ch]` 的 28 处命中全是对象字段 / 栈变量，**没有**"取 vtable 后两次 push"的调用形态 ⇒ 仍无引擎站点。

## 已裁决（2026-11-13）

**本轮拒绝实现**（不复用 `drawPrecheck`）：① 语义不符（会拒绝"状态没问题但还没 SetFVF"）；② 今天 `gpuEligible` 拒绝的组合
仍有 CPU 兜底**真的能画** ⇒ 挂在它上面会对真机说谎；③ `d3d9p/*` 单测无 GPU，接状态侧准入要先给 Rig 装 MockRaster，成本大于收益。

## 要做

- `validateDevice` 只补注释：1 pass 的依据（本线无多 pass 装备）+ **翻面条件**（CPU 兜底删除 / RTT 落地后改成
  "复用 `gpuEligible` 的**状态侧**判据，并按 `D3DERR_CONFLICTINGRENDERSTATE` 拒绝"）。⛔ 不 `recordUnsupported`（它不是"未实现"，是事实）。
- 守卫 `d3d9p/validate-device-one-pass`（`tests/d3d9_pipeline.cpp`）：(i) `S_OK` 且 `*pNumPasses==1`；(ii) 调用前后 256 项
  `GetRenderState` 与 `PipelineInfo` 的 `draws/draw_calls/begins/ends` 不变（无副作用）；(iii) 引擎实测的 17 个状态逐个设上后仍 `S_OK,1`。
- ★ 这条守卫**修前不会红**（防漂移，不是缺陷复现）—— 必须在用例头注明，⛔ 不许把它当"修好了"的凭据。
- 变异：`validateDevice` 写 2 ⇒ 该用例红。

## 非目标

- 不做"ValidateDevice ↔ Draw 一致性"的修前会红版本（那要把 `admitLinePointDraw` 抽成不依赖 DrawContext 的纯函数，另开单）。
