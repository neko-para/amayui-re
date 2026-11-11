# emulator-lift · D3D9 SetRenderTarget 不回写 viewport：渲染到纹理那一段按旧视口映射/裁剪

- id: REQ-01M7372FBT0WBXAS2Z1NESBRFG
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/set-render-target-viewport
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#setRenderTarget
- severity: S2
- tags: [emulator-lift d3d9 render-target]

## 一句话
`SetRenderTarget` 只写 `kPlColorSurface` / `kPlColorArena`，**不回写 viewport**；D3D9 文档要求
"setting a new render target will cause the viewport to be set to the full size of the new render target"。
引擎每次渲染到纹理（`0x4A778B` / `0x4A515E` / `0x4A521B`）都靠这条，而它自己只在 Reset 后设一次全屏
viewport（`0x4988D1-0x498910`）⇒ RT 尺寸 ≠ 后缓冲尺寸时，NDC→屏幕映射与 XYZRHW 的视口裁剪仍按旧 viewport，
渲染到纹理那一段**整块错位/被裁**。

## 已核
- 实现只写两个字段：`runtime/d3d9_pipeline.cpp:1932-1934`（整函数 1821-1944）。
- 引擎三处切 RT 站点见上；viewport 只在 `0x4988D1-0x498910` 设过。

## 修法（最小）
绑定成功后把 `kPlViewport` 的 X/Y/W/H 写成新目标尺寸（`MinZ/MaxZ` 不动）；`target == 0` 用后缓冲尺寸。

## 守卫（修前必须红）
`apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/set-render-target-viewport`：
先 `SetViewport(0,0,64,64)`，再 `SetRenderTarget(纹理表面)`，`GetViewport` 必须回 `(0,0,纹理宽,纹理高)`；
删掉这条新代码 ⇒ 读回仍是 64×64 ⇒ 红。

## 交付（2026-11-11）
- `d3d9_pipeline.cpp#setRenderTarget`：绑定成功后 `writeViewportFullTarget()` 回写 `kPlViewport` 的 X/Y/W/H；
  `target==0` 用 `DeviceInfo`、纹理/后缓冲 surface 用 `surfaceInfoOfState` 的同源尺寸；MinZ/MaxZ 不动；`w/h==0` 不写。
- 设计复审的反证已处理：RT > 后缓冲时深度缓冲只有后缓冲尺寸 ⇒ `rasterTriangle` 的 z 读/写只在**深度矩形**内生效
  （颜色照写），避免越界写 SurfaceArena。
- 守卫：`d3d9p/set-render-target-viewport`（纹理 32×16 / 重复设同一 target / NULL / GetBackBuffer 的后缓冲 surface）；
  变异：`node tools/mutate-check.mjs --match set-render-target-viewport`。
- 门禁：build 0 · `ctest` **76/76** · `pnpm test` 546/546 · 变异 **1/1 红** · `requirements validate` 6/6。
- 没做：MinZ/MaxZ 是否随 RT 重置未取证；无 RT>后缓冲的**像素**用例（只有代码 + 尺寸核算）；
  GPU/sprite 只读代码未实跑。

## 未做
- `pNewZStencil` 仍忽略（引擎两处站点都 `push 0`，见 `0x4A5217` / `0x4A515B`），不在本单。
- GPU 后端的 target 切换不动（它按 `kPlColorSurface` 判身份，本单只补 viewport）。
