# emulator-lift · Clear(ZBUFFER) 的 z 值没有传到底（硬编码 1.0）：接口保真 + 先 flush 再 arm

- id: REQ-01M6RMVJTTM8PDNXB39TVJ40WN
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, gpu, d3d9, depth]

## 现象（2026-11-13 收窄为「接口保真」）

guest 侧 `Clear(ZBUFFER, z)` 的 **z 值没有传到底**：`rasterRequestDepthClear()` 不带参数
（`repo:apps/emulator-lift/src/runtime/gpu_raster.h`、`repo:apps/emulator-lift/src/shims/d3d9.cpp` 的 Clear 分支），
GPU 侧硬编码清 1.0（`repo:apps/emulator-lift/src/runtime/d3d9_gpu.cpp`）。⇒ 引擎清非 1.0 时，
深度起点与 guest 记录的不一致，后续 `z_enable` 批次可能按错的初值判深度。

★ 2026-11-13 裁决后**原单的两半作废**：
- "GPU 深度不镜像 guest 深度" ⇒ **不再追**：没有消费者（`GetDepthStencilSurface` 无站点），且不做像素一致 / CPU 对照；
- "CPU↔GPU 深度对比" ⇒ 随 CPU 光栅退休一并作废（⛔ 不恢复第二实现）。
只留"z 传值"这一条接口保真缺口；本单因此从 bug 改为 req（没有可观测分歧，只有接口没落地）。

## 要做
1. `rasterRequestDepthClear(float z)` 全链传值：接口、Clear shim、GPU pass 的清除值。
2. 顺序：**先 flush 再 arm**（同一处与 `REQ-01M73XEB594ZY8TA76S0EX4512` 一起改，别分两次动同一段）。
3. `z` 落在 [0,1] 之外 ⇒ 显式拒绝 + 记账（不静默 clamp）。

## 判据
- 新 CTest `gpu-batch/depth-clear-value`：`Clear(ZBUFFER, 0.25)` 之后入队 draw 的 pass 清除值是 0.25（看计划层/记账，不要求像素）。
- `Clear(ZBUFFER, 1.0)` 仍与今天等价（TITLE 路径不退化）。

## 非目标
- 深度纹理当 RT / stencil / 多 RT：引擎未用到，用到时另开单。
- 不复原 CPU 光栅侧的深度对照。
