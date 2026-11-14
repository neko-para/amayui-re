# 把 4 个 d3d9* 用例的像素断言迁出 CPU oracle，然后删 rasterTriangle 一族（CPU 光栅的最后一环）

- id: REQ-01M7A74YG08R65NJBFXF7DEJDK
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/skip-ledger
- done_reason: ① 四条判据全部落地：像素断言 139→54（tests/d3d9.cpp 69→34、tests/d3d9_pipeline.cpp 70→20，删 82 条含整条 d3d9/linelist-writes-pixels 与 d3d9/line-axes-and-direction，因为那些期望值编码了已删 CPU 实现的细节）、依赖光栅只剩 22 条、不经光栅保留 32 条；rg "rasterTriangle|rasterizePoint|rasterizeLineSegment|writeShadedPixel" apps/emulator-lift 零命中（2026-11-14 核）；--raster 系列 flag 全删 + 无 GPU 拒绝启动 + headless 走隐藏窗口；默认档 ctest 82/82 · 36.8 s，d3d9-gpu / d3d9-pipeline-gpu / d3dx9-gpu 三条 GPU 臂顶层注册 + SKIP_RETURN_CODE 77（无 GPU 时写原因 LIFT_SKIP(77)，不假绿）；② 改判据的行为：gpuEligible 拒批 ⇒ ++draw_calls + noteGpuSkip（skipped_draws + skipped_reasons + recordUnsupported 具名位）+ return false ⇒ Draw 返回 D3DERR_INVALIDCALL；删掉"回 CPU 前 rasterSyncToGuest"那段；③ 真实场景反证：130 帧 draws 7969 / tri 358551 / fallback 0 / skipped 0，200 与 600 帧 skipped=0；sprite 的 CPU 合成删除后逐帧后缓冲 hash 与改前完全相同（130/130、600/600）；④ ⛔ 边界不许隐藏：判据第 2 条是条件句、本轮未命中（600 帧 skipped=0）；ALPHATESTENABLE=1 的批今天整批被拒且 caps 仍声明 ALPHACMP ⇒ 该项已单独立单 REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4（本批已把它改挂 M2）；本单收口不代表"所有拒批类别都已实现"；另 [draw] fa= 的口径随 CPU 光栅退休降级为顶点 α（欠账在 REQ-01M7B7591WHEVZMYFZ1Q2D8SHQ）；⑤ 裁决口径：逐步实现中的工程，允许功能回退（⛔ 不要求每个中间态都保持可用）。
- tags: [emulator-lift, gpu, tests, migration]

## 背景

`REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 的**后半**已删掉 CLI 的 `--raster`（无 GPU ⇒ 拒绝启动、headless 走隐藏窗口）。
本单接手的是它的**最后一环**：`src/runtime/d3d9_pipeline.cpp` 的 `rasterTriangle` 一族与 `ctx.gpu == nullptr` 的 CPU 像素路径
曾是 4 个 pure CTest 的逐像素 oracle —— 这些断言**已全部迁出**（结构 / GPU 臂 / 删除并写明被谁覆盖），该族**已删除**。

- 曾经的 4 个消费者：`tests/d3d9.cpp`（69 条像素断言）、`tests/d3d9_pipeline.cpp`（70 条）、`tests/d3dx9.cpp`（拆成 pure 臂 + `--gpu` 臂，期望值未改）、`tests/d3d9_mip.cpp`（纯数学、无像素）。
- 它们**不建 `SdlWindow`、不装 `GpuRaster`**，只走 CPU 像素路径；`MockRaster`（`tests/gpu_batch.cpp`）只记调用序、不产像素。

⇒ 它曾是 CPU 光栅作为**测试 oracle** 的最后 4 个消费者；产品路径早已 GPU-only。

## 判据（**四条均已落地**，读数见 `done_reason`）

- 四个文件的像素断言各有归宿（逐条选一种并写明）—— 已落地：像素断言 **139 → 54**；
  1. **结构断言**（MockRaster：段/账本/状态映射）——适用于"只要证明这批进了 GPU、状态被正确解释"；
  2. **GPU 臂**（隐藏窗口 + `GetRenderTargetData` 读回，粗容差）——适用于必须证明像素结果的（blend/depth/alpha/mip）；
  3. 删掉（该断言已被更合适的守卫覆盖，写明被谁覆盖）。
- `rasterTriangle` 一族与 `ctx.gpu == nullptr` 分支**已删除**，`rg "rasterTriangle|rasterizePoint|rasterizeLineSegment|writeShadedPixel" apps/emulator-lift` 零命中（2026-11-14 核）；
- 产品二进制里不再有 CPU 光栅（`--present headless` 走隐藏窗口的 GPU 路径）—— 已落地（`--raster` 系列 flag 全删 + 无 GPU 拒绝启动）；
- `ctest` 全绿；无 GPU 的机器上 GPU 臂 `LIFT_SKIP(77)` 并写原因（⛔ 不假绿）—— 已落地（默认档 `ctest` 82/82 · 36.82 s；`d3d9-gpu` / `d3d9-pipeline-gpu` / `d3dx9-gpu` 顶层注册 + `SKIP_RETURN_CODE 77`）。

## 非目标

- 不新增像素一致性/黄金图判据；不追求 diamond-exit 或逐位一致。

## 新增范围（2026-11-13 复审否证）

复审发现 CPU 路径**不只测试可达**：`gpuEligible` 拒批时 `d3d9_pipeline.cpp:4719` 把 `gpu` 置空 ⇒ 生产路径在 `:4747/:4917/:4928` 直接走
`rasterTriangle` / `rasterizePoint` / `rasterizeLineSegment`；`LIFT_GPU_VERIFY=1` 也走 `:5028`。也就是"GPU 表达不了的那批"当时仍被 CPU **静默画出来**（`--gpu-strict` 已删 ⇒ 连账都没有）。

⇒ 本单还必须做（**1–4 均已落地**）：
1. **改判据**：`gpuEligible` 拒批 ⇒ `recordUnsupported(具名位) + 跳过该批`（⛔ 不 CPU 渲染），并在摘要里可见（`fallback` 计数保留为"未画"）；
2. 若某类拒批让用户可见画面缺失 ⇒ 按 `REQ-01M78BF1…` 的反证条件**扩 GPU 覆盖面**，⛔ 不恢复 CPU —— 本轮实测 **未命中**（600 帧 `skipped=0`）；
3. 删 `LIFT_GPU_VERIFY` 的 CPU oracle 分支（随删族一并消失）；
4. 上面 1–3 与"测试迁移"**一起**才删掉了 `rasterTriangle` 一族。

## 设计（2026-11-13 · `.tmp/cpu-migration/design.md`）

- **前提再修正**：`tests/d3dx9.cpp` 的像素**不来自** `rasterTriangle`，来自 `d3dx9_sprite.cpp` 的 **sprite CPU 合成器**（0 个 `Draw*`）；
  `d3d9_mip.cpp` 纯数学、0 像素。真正被删族卡住的只有 `tests/d3d9.cpp` + `tests/d3d9_pipeline.cpp`。
  ⇒ sprite 合成器已另开单 `REQ-01M7AAXNJY2PACYJHYVRV620Q5`；本单判据只能写"`rasterTriangle` 族为空"，⛔ 不能写"产品无 CPU 像素"。
- **真实场景拒批 = 0**（boot→TITLE / ADV 1493 帧 / OPTION 155 帧 / SAVE 4459 帧，`fallback=0`、`fallback top` 无）⇒ 改成"记账+跳过"**不会少像素**。
- **300 条像素断言分档**（设计估算）：结构 57 / GPU 臂 85 / 已被别处覆盖 16 / 该删 119 / 不经光栅保留 23 —— 落地后实际：像素断言 139 → 54、依赖光栅 22、不经光栅保留 32（见 `done_reason`）。
- 工作量：测试 600–750 行（净减 ≈400）；src −770/+150（删族 −650）。档位：4 个 pure 臂保留 + 新增 `d3d9-gpu` / `d3dx9-gpu` / `d3d9-pipeline-gpu`（顶层注册 + SKIP 77）。
- **顺序**：M1 账本（不改像素）→ 四场景复核 → 翻"跳过" → 删 `LIFT_GPU_VERIFY` → 结构断言 9 条先行 → GPU 臂 → 删族。
  M1 = `REQ-01M7AAXVGY9SG75XCREHPX641Z`。
- ⚠ 既有断言只翻 2 处（`d3d9_pipeline.cpp:2859/:2910`，`D3DERR_INVALIDCALL` 与 `gpu_batch/reject-accounting` 先例一致），其它像素期望不许放宽。

## 边界（⛔ 收口时不许隐藏）

判据第 2 条是**条件句**、本轮**未命中**（600 帧 `skipped=0`）；`ALPHATESTENABLE=1` 的批今天整批被拒、caps 仍声明 `ALPHACMP`
⇒ 该能力缺口已单独立单 `REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4`（本批已改挂 M2）。本单收口**不代表"所有拒批类别都已实现"**。
