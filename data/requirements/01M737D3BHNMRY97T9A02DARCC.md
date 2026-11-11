# emulator-lift · 渲染后端口径：CPU=oracle / GPU=product；GPU 准入保守化 + 黄金图回归

- id: REQ-01M737D3BHNMRY97T9A02DARCC
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/sampler-filter-eligibility
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#gpuEligible
- severity: S2
- tags: [emulator-lift gpu d3d9 consistency]

## 一句话
两处"GPU 与 CPU **都成功**、但像素不同"的静默差，外加验证工具本身的覆盖缺口：
1. `gpuEligible` **不检查采样器过滤** ⇒ `MAG=POINT + MIN=LINEAR` 时 CPU 用"任一 LINEAR ⇒ 双线性"（`:3986-3990`），
   GPU 各用各的（`:3158-3166`）⇒ 同一批**整批像素级不同**，而 `--gpu-strict` 看不见（两边都"成功"，不是 fallback）。
2. `LIFT_GPU_VERIFY` 只对**带纹理的批**比**颜色**（`:3541-3576`）⇒ 不带纹理的批、以及深度，都不在验证面里。

## 口径（2026-11-11 用户指示后**重裁**，取代本单原来的"CPU 是真源"）
- **CPU 光栅 = oracle / 调试实现**（慢到不能交付）；**GPU 光栅 = product**；真源是 **D3D9 语义 + 引擎行为**。
- 一致性三层：**能力层**（`gpuEligible` 保守 + 回退记账）· **夹具层**（GPU 黄金图，另单）· **调试层**（`LIFT_GPU_VERIFY` 按需，不作门禁）。
- ⛔ 取消"每帧逐像素等于 CPU"的全局契约。

## 交付（2026-11-11，**第一半 = 能力层**）
- `gpuEligible`（`d3d9_pipeline.cpp:3799-3806`）：`mag_filter != min_filter` ⇒ false，
  `why="GPU 采样要求 MAG==MIN（CPU 参考按任一 LINEAR 取双线性；当前 MAG=%u MIN=%u）"`；回退走既有 `gpuNoteFallback` 记账。
- 守卫落点**偏离原计划**：`tests/gpu_mapping.cpp` 在本树**不存在**（被 `5e4fca3 refactor: clean tests` 删掉）⇒
  落进既有 pure 目标 `tests/gpu_batch.cpp`，用例 **`gpu-batch/sampler-filter-eligibility`**（CTest `gpu-batch-order`，`MockRaster` 走真 `submitPrimitives`）。
  (a) `MAG=POINT/MIN=LINEAR` ⇒ 入队 0 + fallback=1 + 原因含 `MAG==MIN`；(b)/(c) `MAG==MIN` ⇒ 进 GPU（隔离"仅过滤项"）。
- 变异：`--match sampler-filter-eligibility` ⇒ **1/1 红**（CTest 退出码 8）。修前红：删准入 ⇒ 6 条红（182 checks → 6 failures）。
- 门禁：build OK · 全量 `ctest` **76/76** · `pnpm test` 546/0 · `validate` 6/6 · `diff --check` 0。

## 没做（已另开单）
- **GPU 黄金图回归集** ⇒ `REQ-01M73G49T01W1CQDSME83ES0YD`（第二层；本单不再包含）。
- `LIFT_GPU_VERIFY` 的深度字段 · `--gpu-strict` 语义（已记录现状，不改）。
- 复审的低危建议：`ANISOTROPIC↔LINEAR` 混合与 TFACTOR 路两侧本可等价却被拒（**只掉性能**）；
  收窄成 `gpuFilterFor(mag) != gpuFilterFor(min)` 属新单，不在本单。
