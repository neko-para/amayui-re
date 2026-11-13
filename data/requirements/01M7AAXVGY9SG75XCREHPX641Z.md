# CPU 迁移 M1 · 拒批账本：12 个具名位 + noteGpuSkip + 摘要 skipped/skipped top（不改像素行为）

- id: REQ-01M7AAXVGY9SG75XCREHPX641Z
- type: req
- status: done
- parent: REQ-01M7A74YG08R65NJBFXF7DEJDK
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/skip-ledger
- tags: [emulator-lift, gpu, accounting]

## 范围

`REQ-01M7A74YG08R65NJBFXF7DEJDK` 的 **M1 · 账本**（设计：`.tmp/cpu-migration/design.md` §C）。
**本步不改任何像素行为**，只让"批没画"这件事**可见**。

## 要做

1. **具名位 29..40**（12 个，逐个对应 `gpuEligible` 的拒批原因 / `invw!=1` / `MIPFILTER=NONE` 缩小 / 后端失败 / 其它），
   替掉散落的裸整数；沿用 `UnsupportedBit` + `recordUnsupported`。
2. **`noteGpuSkip`**：在 `submitPrimitives` 的**函数末尾统一记账**（不再依赖 `pixels_written>0` 判定"有没有画"——
   设计 §A.3 指出：GPU 模式下 Draw 的"写 0 个像素"是 CPU 计数，会漏记）。
3. **摘要**：`gpu :` 行加 `skipped=`，并在有跳过时打 `skipped top: [N×] <原因>`（与 `fallback top` 同形）。
   口径写进 `README`：`fallback` = CPU 真写了像素（本单后应恒 0）；`skipped` = GPU 没画（记账）。
4. **不改行为**：本步 `gpuEligible` 拒批仍走现有路径；只保证账本正确（改前/改后四场景的 `draws/gpu` 不变）。

## 判据

- 四场景（boot→TITLE / `se-eof` ADV / OPTION / save）复核：`fallback=0`、`skipped=0`（真实场景不拒批）；
- 夹具：Mock 让一批被拒 ⇒ `skipped=1` + `skipped top` 出现该原因；修前无该字段 ⇒ 红；
- 变异：把 `noteGpuSkip` 删掉 ⇒ 夹具红。
- `ctest` 全绿（本步不应改任何像素断言）。

## 非目标

- 不翻"跳过"语义（那是 M3）；不迁测试（M2）；不删 `rasterTriangle`。
