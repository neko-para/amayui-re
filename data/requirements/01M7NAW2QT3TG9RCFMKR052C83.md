# A · 202（Snow）落地：SetMatrix/SetVector + CPU vs_1_0 + ps_1_0 SnowPS + binding 202

- id: REQ-01M7NAW2QT3TG9RCFMKR052C83
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, effect]

## 一句话

让 **202（`SnowPS` + `technique Snow`，`vs_1_0 + ps_1_0`）真的能画**：补 `ID3DXEffect` 的 `SetMatrix("Proj")` /
`SetVector("Color")` 两槽并记录状态，CPU 侧兑现 `vs_1_0`（`Pos = mul(pos, Proj)`），GPU 侧兑现 `ps_1_0`
（`tex2D(TexSmp, UV) * Color`），并把 202 进 `kBindings`。**A 落地之前 P4 不能开。**

## 为什么

- caps 报 2.0 / 1.0 之后，引擎会建的第 4 个 effect 就是 202；当前真实失败顺序是：
  `SetMatrix("Proj")` / `SetVector("Color")` **没有槽 ⇒ `comUnsupported`** → 补槽后撞 `has_vertex_shader` 的
  无条件具名拒绝（bit 44）→ 再往后是缺 `ps_1_0` 程序。三步都必须翻面。
- 用户口径（2026-11-18）：「**要做的就是正确提供其需要的 feature，为此需要 shim 侧开发，而不是继续尝试告诉引擎不支持**」。
  ⇒ ⛔ 不把 202 登记成"已知缺失 / 已知偏离"来让 P4 变绿。

## 要做

0. **N3 前置读数（读代码，不用跑）**：`Proj` 到底是哪一组矩阵（构造时第一个 4×4，`[renderer+418h]` 的语义：
   view×proj 还是 world×view×proj）与行/列序。它决定 CPU `vs_1_0` 的实现形状。
   ⛔ 参考实现**不许**照着"以为的 Proj"写 —— 否则实现与参考会犯同一个错而双双绿。
1. **状态槽（`+0x88 SetVector` / `+0x98 SetMatrix`）**：照 `SetFloat`/`SetTexture` 的形状加两个 shim；状态块扩容，
   **矩阵 16 float 用独立 64 B 区**（⛔ 不许塞进 `constants[8]`，会把 203 的常量表挤爆；`Color` 4 float 另表）。
   同名覆盖；**没设过 ⇒ 全 0**（D3D uniform 初值就是 0，⛔ 不要默认成单位矩阵 —— 那会掩盖"引擎没设"的 bug）。
   同步 `createEffectState` / `destroyEffectState` / `stateOk` 的 `committed` 尺寸与 `EffectInfo` 断言。
2. **`EffectDrawProgram.has_vs`**：把"有 VS pass ⇒ 一律 unimplemented"改成按 **(vertex_entry, vertex_profile) 查表**；
   202 ⇒ `implemented=true, has_vs=true, program=SnowPs10`；未知 VS profile（`vs_2_0`/`vs_3_0`）⇒ 仍具名拒绝。
   ⛔ 不要"凡 `vs_1_0` 都放行"。
3. **CPU `vs_1_0`**：新增一个独立变换实现（⛔ **不改** fixed-function 的 `transformVertex` 语义）：
   跳过光照、`clip = transformVec4D3D(Proj, (x,y,z,1))`、uv 直通、位置直接写 clip（裁剪/屏幕映射仍在 `submitPrimitives`）。
   只接**两个**变换实现点（`gatherVertices` / `transformVertexOnce`）与**四条** draw 路径
   （`drawPrimitive` / `drawIndexedPrimitive` / `drawPrimitiveUP` / `drawIndexedPrimitiveUP`）。
   ⚠ `transformVertexOnce` 的每 Draw 去重缓存键是 `vertex_index` ⇒ VS 路必须把矩阵并入键或禁用该路缓存。
   ⚠ `pretransformed`（XYZRHW）与 VS 路互斥，要显式分流。
4. **`ps_1_0 SnowPS`**：MSL + DXBC 双源，语义 `return tex2D(TexSmp, UV) * Color;`；`Color` 是 **uniform float4**
   （不是顶点色），`Tex` 1 张、`uv_set=0`；push constant 32 B 足够（矩阵在 CPU 用掉，⛔ 不进 uniform）。
5. **binding**：`kBindings` 加 202（technique `Snow` / entry `SnowPS` / profile `ps_1_0`），
   `EffectProgram` 加 `SnowPs10`，pipeline 的 program 映射加一条。先确认 `gpuEligible` 是否真的需要改（⛔ 别凭计划改）。
6. **测试 + 变异**：见下节。`fx-parse` 已覆盖 202 的 3 常量 / 2 shader / `has_vertex_shader` / `vs_1_0`，⛔ 不加抄文本的断言。

## 判据

- **顶点性质**（`snow-vertices`，mock raster 记录顶点）：单位 `Proj` ⇒ 输出 == 输入顶点（逐 float）；
  两档不同 `Proj` ⇒ 两组不同落点；光照开着也不改顶点色（VS 路无光照）。
- **像素 oracle**（`snow-pixels`，真 GPU + 纹理 RT 读回）：已知 16×16 纹理 × 两档 `Color`
  （`{1,1,1,1}` / `{0.5,0.25,0.75,1}`）⇒ 逐通道 `texel*Color`，容差 ≤1 LSB；`Color={0,0,0,1}` ⇒ 全 0（控制行）。
- **记录 + 负控**（`effect-draw-snow-recorded`，由现有 `effect-draw-unimplemented` 翻面）：202 ⇒ recorded 且
  矩阵 / `Color` 进 program；`vs_2_0` 的 pass ⇒ 仍具名拒绝。
- **不回归**：`fade-pixels` / `blur-pixels` / `d3d9p/*` 全绿；fixed-function 路径逐字节不变。
- **变异实测红**（`tools/mutate-check.mjs --match`）：矩阵槽截断成 4 float · 漏乘 `Color` · 删 202 binding。
- `ctest` 全绿。

## 前置 / 未做

- 前置：N3（`Proj` 语义）。N1（`--until 0x00418340` 看雪景可达）**不阻塞**本单。
- E2E 时序：A 的 E2E（以及 N1）在 **P4 之前**做 —— 那时 `LIFT_D3D9_PS_VERSION=0x200` 这个 opt-in 还在；
  P4 一合就只剩默认 caps。
- 未做：雪景的真实 run 级验收（取决于 N1）；`Proj` 若 N3 读不出结论，先登记 `## 待核` 再实现。
