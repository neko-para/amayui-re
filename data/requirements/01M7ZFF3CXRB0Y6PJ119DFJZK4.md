# 🐞S2 D3D9 半像素：屏幕→NDC 少 +0.5 ⇒ 切立绘 2 帧整层偏 (−0.48,−0.46) 并变糊

- id: REQ-01M7ZFF3CXRB0Y6PJ119DFJZK4
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/rt-roundtrip-identity
- repro: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/sprite-vs-rhw-quad
- severity: S2
- done_reason: 判据 = 「1:1 RT 往返必须恒等」这条数学必然：CTest d3d9-pipeline-gpu 的用例 d3d9p/rt-roundtrip-identity（直画 vs 画进同尺寸 RT 再全屏合成 ⇒ 逐像素相同且都等于源 texel）+ CTest d3dx9-gpu 的用例 d3dx9/sprite-vs-rhw-quad（sprite 路 vs XYZRHW 路同像素）；变异 kD3d9PixelCenterBias 0.5→0 已进 tools/mutate-check.mjs 并实测红（CTest 退出码 8）。场景读数：adj_chg 49145/49045 → 0、亚像素 (−0.480,−0.460) → (0,0)、锐度不再双峰（15.77→15.74）、稳定帧修前/修后逐字节相同。
- tags: [emulator-lift, d3d9, half-pixel, gpu]

## 一句话

用户报的「ADV 切立绘那一下偏 (1,1) + 变糊」= 恰好 **2 帧**的整层偏移 `(−0.480,−0.460)` px + 变糊；根因是**屏幕→光栅器视口的换算少了 D3D9 的半像素**（与 sprite 采样器那条 `BUG-37` 是两件事，见末节）。

## 现象（读数）

- 那 2 帧是整跑**唯一**走 `ID3DXEffect` 的帧（`[gpu-submits] submits=15 targets=4`；同一段日志有 `SetTechnique "Fade"` / `SetFloat "Alpha"=0` / `Tex0`+`Tex1` / 1 次 `DrawPrimitive` FVF=0x244），其余帧是 `ID3DXSprite` 直画（`submits=3 targets=0`）。
- 立绘窗内 `mean|Laplacian|`：稳定帧 `15.77` → 异常帧 `9.94`，第 3 帧起**逐字节**回到稳定帧。

## 根因（MSDN 定点，不是"看起来像"）

- MSDN `Coordinate Systems (Direct3D 10)` 差异段原文：「… **In Direct3D 9, pixel centers are at integer locations.**」；而 SDL_GPU 的光栅器（D3D12 / Metal / Vulkan）像素中心在 `(i+0.5, j+0.5)`。
- 探针 ①（`submitPrimitives` 入口）：guest 的 Fade quad 四角 = `(−0.5,−0.5) … (1279.5,719.5)`、UV `0..1`、`RHW=1` ⇒ D3D9 的**全屏 quad idiom**（像素边界 −0.5..size−0.5）。
- 探针 ②（`rasterQueueEffect`）：`tgt=1280x720 vp=0,0,1280,720`、`tex0/tex1=1280x720`、`sampler LINEAR/CLAMP` ⇒ 这是 **1:1 往返**，数学上**必须恒等**。按 D3D9 口径：像素 `i` 的 `u=(i+0.5)/1280` ⇒ 硬件采样坐标 `u·1280−0.5 = i` = texel `i` 中心 ⇒ 恒等；少 +0.5 时 `u=(i+1)/1280` ⇒ `i+0.5` = texel **边界** ⇒ LINEAR 50/50（糊）+ 半 texel 偏移。
- 为什么稳定帧看不出来：sprite 直画只有一跳，且 `ID3DXSprite` 自己把四角减 0.5 ⇒ 与缺失的 +0.5 **正好相消**；多一跳的 Fade / RTT 路把误差加倍到 1 px 才暴露。

## 修法（提交 `a46a929` / 2026-11-22）

- `repo:apps/emulator-lift/src/runtime/d3d9_gpu.cpp`：`kD3d9PixelCenterBias = 0.5f` + `d3d9ScreenToNdcX/Y()` = 屏幕坐标（D3D9 像素中心）→ 光栅器视口的**唯一**换算；flush 的**两处**顶点转换（effect 批 / 固定功能+sprite 批）与 `[gpu-ff-tex]` 诊断都改走它。
- `repo:apps/emulator-lift/src/runtime/d3dx9_sprite.cpp`：`kD3dxSpriteVertexBias = 0.5` —— D3DX 自己的 −0.5（`position` 是像素、四角是边界），加在**变换之后**。★ 与上一条**成对**，光栅器侧相消 ⇒ sprite 路像素**逐字节不变**（实测 maxdiff=0），只有 RT / Fade 路变恒等。
- 连带把 3 条**编码了旧相位**的期望按新口径重算并写明推导（`d3d9p/min-filter` 两条、`d3d9p/perspective-interpolation` 参考像素中心 `x+0.5`→`x`、`d3dx9/sprite-sampler-pixels` 的推导注释）。

## 判据（守卫 + 读数）

- CTest `d3d9-pipeline-gpu` 的用例 `d3d9p/rt-roundtrip-identity`：(a) 直画 vs (b) 画进同尺寸 RT 再全屏合成 ⇒ 逐像素相同，且都等于源 texel（容差 1 吸收 unorm 舍入）。
- CTest `d3dx9-gpu` 的用例 `d3dx9/sprite-vs-rhw-quad`：同一纹理 / 位置 / 1:1 的 sprite 路 vs XYZRHW 路 ⇒ 同像素。
- 变异：`kD3d9PixelCenterBias` 0.5→0 ⇒ ① 红（4096/4096 不等）；`kD3dxSpriteVertexBias` 0.5→0 ⇒ ② 红（63/64 不等）。前一条已进 `repo:tools/mutate-check.mjs`（`--match kD3d9PixelCenterBias` 实测 CTest 退出码 8）；后一条**未**进（手工实测）。
- 场景 A/B（win32 / 2026-11-22 / `--scale 1.0|1.5`）：进入 / 离开 `adj_chg` **49145/49045 → 0**；亚像素 `(−0.480,−0.460)` → **(0,0)**；锐度**不再双峰**（15.7726 → 15.7504 → 15.7389，差 ≤0.03）；稳定帧修前 / 修后**逐字节相同**（3 对 maxdiff=0）。独立旁证：`d3d9p/perspective-interpolation` 改参考像素中心后最大误差 **207 LSB → 0/1 LSB**。门禁：默认 `ctest` 137/137、`pnpm test` 614/614。

## 未做（同族、guest 可见语义 ⇒ 另立 open 单）

`expand_point` / `expand_line` 选像素仍是 `floor`（D3D9 口径应为 `floor(c+0.5)`）、`clipScreen` 比 D3D9 的像素范围 `[vp−0.5, vp+size−0.5]` 左 / 上各紧半像素 ⇒ 见 `REQ-01M7ZFEVQP8YNSN46Z3GT0P516`（「D3D9 点/线的像素选择…」那条 open 单；⛔ 不要顺手改）。overlay 混合路（宿主 UI）**刻意**保持边界口径。

## 复现（能照抄）

```bash
cd apps/emulator-lift
ctest --test-dir out -R 'd3d9-pipeline-gpu|d3dx9-gpu' --output-on-failure
```

场景配方 = `.tmp/jitter/save08-sc0010.plan`（⚠ gitignored、**不在仓内**）；命令逐字见 `repo:docs/03-emulator/defects.md` 的 BUG-38「场景 A/B」段（`--dump-frame` + `--frame-log` + `--max-frames 300` + env `LIFT_GPU_SUBMITS=1`）。
