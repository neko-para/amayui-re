# 退休 CPU 光栅：GPU 成唯一光栅实现（前置=GPU 支持渲染到纹理），无 GPU 拒绝启动、允许功能回退

- id: REQ-01M78BF1FNNH6K8N8GEY6GQYXX
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#lift/no-gpu-refuses-start
- done_reason: 2026-11-14 收口（八条逐条核，只读核查见 .tmp/cpu-migration/m78bf1-audit.md）：①删 --raster cpu 已落地（src/main.cpp:556-559 未知参数 ⇒ exit 2；守卫 lift/raster-flag-gone 顶层注册）；②删 --raster-sprites 已落地（零命中；sprite 的 CPU 逐像素合成已删，见 REQ-01M7AAXNJY2PACYJHYVRV620Q5 已 done）；③删 --gpu-strict 已落地（只剩一句"已删"历史注释）；④无 GPU ⇒ 拒绝启动已落地（main.cpp:743-755 明确文案 + return 2；守卫 lift/no-gpu-refuses-start 默认档、有变异条目；例外 --report / --smoke 不建光栅已文档化）；⑤文档口径已落地（README / SCOPE / external-docs.md:62 三处"参考后端 / 像素真源"全清）；⑥feeder_seg_e2e 的 --raster cpu 已落地（走"改成要求 GPU"这条，LIFT_SKIP_IF_NO_GPU）；⑦按单的措辞已满足（原文允许"在批的计划阶段算、或在批提交时统计"，实现是后者：d3d9_pipeline.cpp:4275-4286 在 rasterQueue 之后按 gpu_vertices 统计），但口径从"逐像素混合 α"降级为"逐顶点 diffuse α" ⇒ 欠账移交 REQ-01M7B7591WHEVZMYFZ1Q2D8SHQ（本批已改挂 M2）；⑧external-docs.md:62 已落地。关键反证：整族删除后 rg "rasterTriangle|rasterizePoint|rasterizeLineSegment|writeShadedPixel" apps/emulator-lift 零命中；真实场景 130 帧 draws 7969 / tri 358551 / skipped 0，逐帧后缓冲 hash 与改前完全相同（130/130、600/600）。判据①按字面即满足：2026-11-14 复核 rg -n -- "--raster cpu|raster-sprites|参考后端|像素真源" apps/emulator-lift 零命中（审计曾预警负向夹具 tests/gpu_batch.cpp 必须含 --raster cpu 字面量、会让该条按字面永远红；该字面量已在 2026-11-14 03:46 的测试重构中消失，故按语义与按字面今天都满足）。裁决口径：允许功能回退（逐步实现中的工程，不要求"保持可用"）；若删完发现产品可见画面只能靠 CPU 光栅才画得对 ⇒ 停下报告、扩 GPU 覆盖，⛔ 不恢复第二实现 —— 本次无此情形。
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

渲染到纹理已**单独拆单**：`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`（GPU 纹理作 RT + 读回）。
本单在它落地后开工；STH 的非零断言挂在 `REQ-01M780VKKRFHJEX6VM74DRCYBM`，⛔ 不重复计入本单判据。

## 要做

1. ~~GPU 支持渲染到纹理~~ → 已拆到 `REQ-01M79MSMWGFEKF8J7C7VYWS4YP`（本单只做"删第二实现"）。
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
- ⛔ 不许为过门禁保留任何"第二实现"分支，包括"空光栅/no-op 光栅"。
- **删 CPU 前必须先把四处"靠 CPU 兜底"的旁路改成显式拒绝 + 记账**（否则删掉后静默不画）：
  点/线只走 CPU、`invw != 1`、`MIPFILTER=NONE` 的缩小、GPU 失败后的 CPU 重放。
  守卫 = 各自一条 `recordUnsupported` 的用例，而不是"删完看不出来"。
- `[draw] fa=` / `[draw] texa=` 移到 GPU 批（计划阶段或提交时统计），并有守卫保证非 0 参与 α 统计可见。

## 反证条件

- 若删完发现某处**产品可见**画面只能靠 CPU 光栅才画得对 ⇒ **停下报告**：那说明 GPU 覆盖还差一块，
  按覆盖面扩 GPU，⛔ 不恢复第二实现。
- 若"纹理 RT 在 GPU 上根本拿不到像素"（读回路径缺失）⇒ 先补读回，不要把纹理 RT 改回 CPU。

## 与其它单的交界（2026-11-13）

- **`ValidateDevice` 的翻面条件就是本单**：现在恒 `S_OK,1`（本线无多 pass 装备、引擎无 `.lst` 站点）是事实；
  本单（CPU 兜底删除）落地后，若要让 `ValidateDevice` 变成真的校验，用状态侧判据 + `D3DERR_CONFLICTINGRENDERSTATE` 且不写出参。
  RTT 落地**不是**翻面条件（那条已在 `REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 完成）。
- 本单删除 CPU 兜底时，**点/线、`invw != 1`、`MIPFILTER=NONE` 缩小、GPU 失败 CPU 重放** 四处必须逐条处置：
  命中率 > 0（boot→TITLE 实测）⇒ 在 GPU 上补；否则显式拒绝 + 记账。⛔ 不许"删完静默不画"。

## 进展（2026-11-13 初版 · 2026-11-14 收口）
- 已落地：CLI `--raster` / `--raster-sprites` / `--gpu-strict` 已删；无 GPU ⇒ 拒绝启动（exit 2 + 文案 + 指路）；`rasterTriangle` 一族随 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 全族删除（4 个符号 `rg` 零命中）；守卫 `lift/no-gpu-refuses-start` / `lift/raster-flag-gone`（各带变异）。
- ⚠ 第 7 条：诊断已挪到 GPU 批提交阶段，但 `fa=` 口径从「逐像素混合 α」降级为「逐顶点 diffuse α」且该字段没有守卫 ⇒ 欠账移交 `REQ-01M7B7591WHEVZMYFZ1Q2D8SHQ`。
