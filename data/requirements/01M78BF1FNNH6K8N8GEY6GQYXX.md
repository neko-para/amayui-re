# 退休 CPU 光栅：GPU 成唯一光栅实现（前置=GPU 支持渲染到纹理），无 GPU 拒绝启动、允许功能回退

- id: REQ-01M78BF1FNNH6K8N8GEY6GQYXX
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift d3d9 raster architecture]

## 决定（2026-11-13 · 用户裁决）

**不再维护两套光栅实现。** CPU 光栅与 GPU 后端都是我们对 D3D9 文档的理解，谁也没有额外权威；
同时维护两份只会带来成本 —— 实例：`SetRenderTarget` 参数按 D3D8 读（`src/shims/d3d9.cpp:782`），
因为 CPU 路一直在兜底，这个缺陷长期不可见（`REQ-01M780VKKRFHJEX6VM74DRCYBM`）。

- **GPU 是唯一光栅实现**；**无 GPU 设备 ⇒ 拒绝启动**（明确报错 + 非 0 退出码）。
- **允许功能回退**：这是逐步实现中的工程，不要求"保持可用"。
- 因此"用 CPU 当对照 / `--raster both` 双跑"这条路整体作废：guest 时间挂在墙钟上
  （`wait N wall_ms`、input-script tick、Present 节奏、音频 pump），0.5 fps 与几十 fps 到第 N 帧时
  `guest_ms` 差出几分钟 ⇒ 相位不同、差异全是噪声。绘制级捕获回放可选，但**不是**本单目标。

## 前置依赖（必须先做，否则"删了就坏"）

`0x4A778B` 是**渲染到纹理**（当前帧 → 1280×720 纹理 → `GetRenderTargetData` 读出 → 缩略图）。
GPU 准入今天拒绝 `color_surface != 0`（`src/runtime/d3d9_pipeline.cpp:4205`）⇒ 缩略图只能靠 CPU 光栅。
⇒ **GPU 必须支持"纹理当渲染目标"**（SDL_GPU 原生支持；GPU→guest 读回侧 `rasterSyncToGuest` 已有）。

## 要做

1. **GPU 支持渲染到纹理**：纹理作 RT0 + 读回 guest arena。
   守卫 = `save-graph` 的「`SAVE00.STH` payload（偏移 54 之后）非零字节 > 0」。
2. **删 `--raster cpu`**：`main.cpp` 的参数与拒绝分支、`d3d9_pipeline.cpp` 里 `ctx.gpu == nullptr` 的分支与
   "显式参考后端"注释（`:4373`）、README 里"参考/测试后端"的表述（`:631`/`:712`）。
3. **删 `--raster-sprites 0` A/B**：`main.cpp:173`、`present_sdl.cpp:1127`、`gpu_raster.h:353`。
4. **删 `--gpu-strict` 与 cpu/sprite 的矛盾用例**：`CMakeLists.txt:328/332`、`main.cpp:678-694` 的拒绝分支。
5. **无 GPU ⇒ 拒绝启动**：替掉 `main.cpp:1099` 那句"GPU 不可用 ⇒ 本轮是 CPU 光栅化"的静默降级。
6. **口径**：删掉"CPU 是参考后端 / 像素真源"（`apps/emulator-lift/README.md:631/712`、
   `docs/03-emulator/external-docs.md:62`）。CPU 光栅删除后不得留下"另一条可比的实现"的暗示。
7. **用例**：`tests/feeder_seg_e2e.cpp:201` 用 `--raster cpu` 的理由是"别让没显卡的机器 SKIP" ⇒ 改成要求 GPU，
   或改成不产像素（前提是那种"无光栅模式"不算第二实现；否则一律要求 GPU）。
8. **把只在 CPU 光栅上统计的诊断挪到 GPU 路径**：`[draw] fa=`（本批真正参与混合的 α：n/均值/最小/最大）与
   `[draw] texa=` 今天只有 `--raster cpu` 才打。删掉 CPU 光栅等于删掉"α 语义是否正确"的唯一可观测面 ——
   实例：`REQ-01M735AWXRDZKCDT4124BCFF27`（固定功能批次 α 恒 0）的判据就靠 `fa=`。
   ⇒ 这些聚合要么在 GPU 批的计划阶段算（`GpuBatchPlan` 里本来就有 blend/arg 信息），要么在批提交时统计；
   ⛔ 不许因为"CPU 路没了"就把这条判据一起丢掉。

## 现状实测（2026-11-13，支持本裁决）

- 纹理 RT 今天由 CPU 光栅回退接手：`save-graph` 真跑 `draws=6531 gpu=6529 fallback=2`（2×1280×720 ≈ 1.84 M px），
  **而 `SAVE00.STH` 的 payload 非零字节仍是 0** ⇒ 所谓"回退"并没有真正实现渲染到纹理。
  ⇒ 「CPU 光栅是兜底」这个前提**本身不成立**：它连它唯一被指定去兜的场景都没兜住。
- 另一例（同一天）：`SetRenderTarget` 按 D3D8 读参，被 CPU 路长期掩盖到 `REQ-01M780VKKRFHJEX6VM74DRCYBM` 才暴露。

## 判据

- `rg -n -- '--raster cpu|raster-sprites|参考后端|像素真源' apps/emulator-lift` 为空（注释与文档也算）；
- 无 GPU 环境启动 ⇒ 非 0 退出 + 明确文案（用注入/环境变量模拟，⛔ 不要靠真拔显卡）；
- `save-graph` 的 STH 断言绿（纹理 RT 真的画出来了）；
- ⛔ 不许为过门禁保留任何"第二实现"分支，包括"空光栅/no-op 光栅"。

## 反证条件

- 若删完发现某处**产品可见**画面只能靠 CPU 光栅才画得对 ⇒ **停下报告**：那说明 GPU 覆盖还差一块，
  按覆盖面扩 GPU，⛔ 不恢复第二实现。
- 若"纹理 RT 在 GPU 上根本拿不到像素"（读回路径缺失）⇒ 先补读回，不要把纹理 RT 改回 CPU。
