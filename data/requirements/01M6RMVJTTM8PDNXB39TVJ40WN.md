# emulator-lift · GPU 光栅不镜像 guest 深度但 depth test 准入仍放行

- id: REQ-01M6RMVJTTM8PDNXB39TVJ40WN
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#gpuEligible
- severity: S2
- tags: [emulator-lift, gpu, d3d9, depth]

## 现象（代码判据，尚未构造出真实分歧帧）
GPU 光栅的深度目标是自己的 D32 纹理，`rasterSyncFromGuest` 只同步颜色，不复制 guest 深度；`rasterSyncFromGuest` 还会把 GPU 深度清成 1.0。与此同时 `gpuEligible()` 允许 `z_enable` / `z_write`：
- guest 侧 `Clear(ZBUFFER, z != 1.0)` 后，CPU 参考读 guest 深度，GPU 读自己的 1.0；
- CPU 回退写过的深度、或 GPU 写过的深度，都没有在 guest 与 GPU 之间同步；
- `--gpu-strict` 只统计 CPU fallback，不比较深度，所以这类批次可以被算作“已覆盖”，像素却可能静默不同。
TITLE 当前实测同批 `diff_pixels=0`，说明 TITLE 本身没有触发；这是非 TITLE 场景的潜在缺陷。
## 复现/判据
- 入口：`d3d9::gpuEligible()` 的 `p.state.depth_test = ctx.z_enable` / `p.state.depth_write = ctx.z_write`；GPU 深度 target 见 `d3d9_gpu.cpp` 的 `rasterPrepare` / `rasterSyncFromGuest`；CPU 参考深度见 `rasterTriangle`。
- 需要一个能红的机械用例：同一后缓冲上 `Clear(ZBUFFER, 0.5)` → 画两个深度不同的三角形 → CPU `--raster cpu --dump-frame` 与 GPU `--raster gpu --dump-frame` 对比，或 `LIFT_GPU_VERIFY` 扩成同时比深度。
- 当前 `LIFT_GPU_VERIFY` 只比颜色，不能证明深度等价。
## 处置方向（二选一，不要静默）
1. **镜像深度**：`rasterSyncFromGuest` 同步 guest 深度到 GPU，CPU 回退/读回时同步回来；或
2. **降级 fallback**：只要 guest 深度自上次 `Clear(Z, 1.0)` 起被写过、或当前深度不是可证明的 1.0，就把 `z_enable` 批次记成 fallback（`fallback_reasons` 可见），`--gpu-strict` 随之红。
不建议继续“GPU 深度独立、准入仍放行”。
## 验收
1. 上述复现用例在修复前能红（CPU/GPU 像素或深度差）；修复后差异消失，或该批次明确计入 fallback 且 `--gpu-strict` 拒绝。
2. `LIFT_GPU_VERIFY` 至少能比较颜色 + 深度（或明确写明只比颜色，不能作为深度判据）。
3. TITLE 同批像素仍为 0 diff，`--gpu-strict` 仍 exit 0；`ctest` 全绿。
4. README §4.2b 的“深度不镜像”从风险描述变成可执行判据/回退口径。
## 非目标
- 不要求本单实现完整 stencil/多 render target；只处理当前已准入的 depth test/write。
