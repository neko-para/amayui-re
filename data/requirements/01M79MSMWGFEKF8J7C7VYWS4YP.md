# emulator-lift · GPU 纹理作渲染目标（RTT）+ 读回：放行颜色目标，解锁缩略图与「删 CPU 光栅」

- id: REQ-01M79MSMWGFEKF8J7C7VYWS4YP
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#d3d9p/render-to-texture
- tags: [emulator-lift, d3d9, gpu, rtt]

## 一句话
把「纹理当渲染目标」做进 GPU 光栅：`gpuEligible` 放行 `color_surface != 0`，把目标纹理的 arena 绑成 pass 的 color target，并把结果读回 guest arena。这是 2026-11-13 裁决（GPU 唯一光栅、CPU 光栅整体退休）的**硬前置**：没有它，`0x4A778B` 的缩略图路径只能靠已删除的 CPU 光栅。

## 现状（2026-11-13，只读核实）
- 准入仍显式拒绝：`repo:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `gpuEligible` 对 `color_surface != 0` 回「GPU 只支持默认后缓冲」。
- 下游已就位：`ctx.color_base` 已解纹理 arena；每批 `rasterPrepare(ctx.color_base)` 已调；SDL_GPU 后端已按基址换 target 并重建同尺寸 depth；`getRenderTargetData` 已带 sync 屏障；shim 侧按 D3D8 读参的缺陷已修（`repo:apps/emulator-lift/src/shims/d3d9.cpp`）。
- `save-graph` 今天只在 `--raster cpu` 兜底下走到 STH，payload 仍全 0 ⇒ CPU 光栅并没有兜住它。

## 要做
1. **准入**：`color_surface != 0` 且该 surface 是纹理 RT 时放行（32bpp / 无 alpha test / GOURAUD 等既有条件不变）。
2. **目标绑定**：目标纹理 arena 作为 pass 的 color target（同尺寸 depth 复用已有重建逻辑）。
3. **读回**：`GetRenderTargetData` / `LockRect` 把像素写回 guest arena（复用已有 sync 屏障）。
4. **记账**：仍不覆盖的组合继续 `recordUnsupported` + 进 fallback 账（⛔ 不许静默）。

## 判据
- 新 CTest `d3d9p/render-to-texture`：GPU 臂把一张纹理当 RT，画一个已知颜色的全屏 quad，再 `GetRenderTargetData` 读回 ⇒ 读回像素**非全 0 且颜色落在容差内**（粗粒度，⛔ 不与 CPU 逐像素比）。
- `save-graph` 的 `SAVE00.STH` payload 非零断言挂在 `REQ-01M780VKKRFHJEX6VM74DRCYBM`（本单只提供能力）。

## 不做
- 多 RT / MRT、深度纹理：引擎未用到，用到时另开单。
- CPU 光栅侧的 RTT：那条路整体退休。

## 设计复审（2026-11-13 · 结论：可行，`.tmp/rtt/design.md`）

- **挡路的只有准入一条**：`gpuEligible` 的 `ctx.color_surface != 0` 拒绝；target 绑定（`rasterPrepare(ctx.color_base)`）、
  读回（`getRenderTargetData` 屏障 / 纹理 `lockRect`）、`Clear`、`Present` 已是"是镜像目标才回读"的正确形状。
  ⇒ 现证据：`gpu-batch/barriers` ② 今天就在真后端把绿块画进纹理。
- **准入解禁要配两条新闸**：① 同批把 RT 当采样纹理（`textures[s].pixels == color_base`）⇒ 拒 + `recordUnsupported`；
  ② 纹理没有 `D3DUSAGE_RENDERTARGET` 时只 warn 放行（引擎真值未取证，不新增静默回退）。
- **必须顺手改一处**：换 target 时 `destroyTargets()` 连 `depth_` + `pipelines_` 一起毁 ⇒ RTT 让换 target 每帧 ≥2 次 ⇒
  每帧重建 D32 与全部 pipeline。改成"尺寸不变则复用 depth、pipelines 不随换 target 清"；深度内容语义（属设备、不随 target 变）一并定。
- **守卫的"修前红"只能来自准入/账本**（像素断言今天也会绿：CPU 回退正往同一块 arena 写）。
  纯段断言 `queued_draws==1 ∧ rasterTargetBase()==tex_pixels ∧ fallback_draws==0`；真后端段用粗粒度颜色容差。
- **sprite→RT 本单不做**：判据 = RT 窗口内 sprite End/Flush 次数 0（两条 sprite 路径都硬取 `dev.arena`）。
- 停止线：准入放开后 `rasterPrepare` 仍拒 / 新闸被引擎命中 / STH 仍黑但 RT 批 `recorded > 0` / 帧时间因重建变差 /
  需要 depth-stencil·MRT·mip RT ⇒ **停下报告**，⛔ 不退回 CPU 光栅。

## 实施结果（2026-11-13）

- 准入放行 + 两条闸（反馈环拒 + usage warn）、depth 按尺寸复用、`pipelines_` 不随换 target 清，全部落地；
  守卫 `d3d9p/render-to-texture`（修前红：`queued 0→1 / fallback 1→0 / base=tex_pixels`）与
  `d3d9p/render-to-texture-feedback` 绿；**真后端臂真的跑起来**（`driver=gpu-direct`，三角内 R≥240 / 外=清屏蓝），
  同尺寸换 target 的 `depths_created/pipelines_created` 差值 0。变异 2/2 红。
- ★ **停止线 3 命中**：`SAVE00.STH` payload 修前/修后都是 0（独立复核：0 / 172800）。RT 窗口（绑定→解绑）里的实测是
  2×`Clear(Z+black)` + **2 条 `DrawPrimitive`（rec=1，全屏无纹理的黑覆盖 quad）** + **77 次 `[d3dx9] End/Flush` 合成**
  （首条 921600 px = 满屏）；这些像素全进**后缓冲**（sprite 两条路径硬取 `dev.arena`）⇒ 改判方向
  `REQ-01M79V2TVJH98BKH0C0WQV2FET`（sprite→RT），本单不扩大范围。
  ⚠ 设计 §4 原写"sprite End/Flush 次数 = 0"作判据，**实测不成立**（是 77 次），不要再用那句当证据。
