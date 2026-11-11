# emulator-lift · GPU 覆盖账本把「入队成功」当「已覆盖」⇒ fallback=0 与像素脱钩

- id: REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/ledger-queued-vs-recorded
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#gpu_done
- severity: S2
- tags: [emulator-lift gpu coverage diagnostics]

## 一句话
GPU 覆盖账本把"**入队成功**"当成"**已覆盖**"：`d3d9_pipeline.cpp` 里 `if (ok) { gpu_done = true; ++g_gpu_draws; }`
（`ok` = rasterQueue 入队成功）；而 fallback 只在 `ctx.pixels_written > 0` 时记，GPU 路 `pixels_written` 恒 0
⇒ **`fallback=0` 与"有没有像素"彻底脱钩**，`--gpu-strict` 必然绿。
⇒ `REQ-01M73P3GXW22Y467YGYPQSM571`（GPU 臂整层丢 Live2D 角色）因此**静默**：`gpu=124/124`、`fallback=0`、`sprite 49/49`。

## 已核（A8，`.tmp/d3d9/a8-gpu-layer.md`）
- 两臂 124 个 FF 批一一对应；CPU 臂每批 `pixels=7405…79757`，GPU 臂每批 `gpu=1 why=-` 且**零像素**。
- 若按符号定位：`gpu_done = true` 与 `gpuNoteFallback`（⚠ 该文件在取证时正被别的 agent 改，**别按行号**）。

## 修法（**只让缺口可观测**，不动 `--gpu-strict` 语义）
★ 用户 2026-11-11 裁决："这里目前不重要，有记录就行" ⇒ 本单不改 strict 的通过条件。
1. 账本拆三个数：`queued`（入队）· `recorded`（真的录进了一次 render pass）· `fragments`（能拿到的片元/顶点计数）。
2. run summary 的 `gpu:` 行补 `recorded=` 与 `empty=`（`queued>0 && recorded==0` 的批数）。
3. `[draw]` 在 GPU 路也打 `rec=`（是否真的录进去），⛔ 不要只打"入队成功"。

## 守卫
- **pure**：用 `MockRaster` 让某批"入队成功但不记录" ⇒ 该批必须计入 `empty`（现在会显示成 `gpu=1` 且什么都不报）。
- **变异**：把 `empty` 恒 0 ⇒ 该用例红。

## 还没做（本单之后）
- ★ **`fragments` 这个名字是错的**：它的值实际是**顶点数**（A13 已核：`gpu_vertices`），不是片元数。
  本轮分析里 A8/A9 都曾拿它当"覆盖率"读 ⇒ 应改名 `vertices=` 或在 summary 里标 `fragments=(不可得)`；本单没改（超出范围）。

## 未做
- 不改 `--gpu-strict` 的通过条件（等用户重新裁决）。
- 不在本单修"整层丢失"本身（那是 `REQ-01M73P3GXW22Y467YGYPQSM571`）。
- 不动 `LIFT_GPU_VERIFY`（它在本 build 里 12 min 0 行，属另一个问题）。
