# emulator-lift · TITLE direct GPU present（swapchain 直出）

- id: REQ-01M6RMSWFQBFSD9MBQXPD3M0EC
- type: req
- status: done
- parent: REQ-01M6RH9CDPM8PG2SFEG0PYT46T
- verify: apps/emulator-lift/tests/gpu_direct_present.cpp#gpu-direct-present
- tags: [emulator-lift, gpu, present, performance, title]

## 现状（2026-11-06 实测）
TITLE 的 GPU 四边形已走 GPU（`--gpu-strict` fallback=0），但 `SdlWindow::submitFrame` 仍用 `SDL_UpdateTexture` + `SDL_Renderer` 呈现，Present 中位数 82–108 ms，TITLE wall 155–228 ms。
## 交付
`--present window --raster gpu` 现在直接呈现 GPU target：
- `SDL_ClaimWindowForGPUDevice` + `SDL_WaitAndAcquireGPUSwapchainTexture` + `SDL_BlitGPUTexture`（target→swapchain，letterbox `LOADOP_CLEAR` + NEAREST）+ `SDL_SubmitGPUCommandBuffer`；
- **没有** `SDL_UpdateTexture` / `SDL_RenderTexture` / `SDL_RenderPresent`；
- 实测 `SDL_ClaimWindowForGPUDevice` 与 `SDL_CreateRenderer` 不能同窗口共存 ⇒ claim 成功时不建 renderer；claim 失败（dummy/驱动不支持）打印实测错误并回退 renderer 上传路径，不假装直出；
- `d3d9::Present` 显式传“target 是否最新”（`directSetTargetCurrent`）：GPU dirty 时先读回 guest 再 blit；无 GPU 绘制时 sink 先把 guest 上传 target；DDraw Flip / 直接 submitFrame 走上传分支；
- 截图/捕获从 target 回读并套同一份 letterbox；16bpp/非 BGRA8 guest 后缓冲按 `pixel_format` 掩码转 BGRA8；hidden 窗口可用；
- `present-vsync` 映射 swapchain present mode（IMMEDIATE/MAILBOX/VSYNC，实测不支持会打印回退）。
## 实测结果
TITLE 段用 draw 签名判定（`prim=5 verts=4 tris=2 tex=1 sep_alpha=1 stage0=4/4(2/2) blend=5/6 gpu=1`），不写死帧号：
| 项 | 改造前 | 直出后 |
|---|---:|---:|
| wall median | 119.8 ms | **58.5 ms** |
| Present median | 64.4 ms | **7.5 ms** |
| SpriteEnd median | 20.8 ms | 18.4 ms |
| DrawPrim median | 9.0 ms | 7.9 ms |
Present 拆解：readback 1.7 / hash 4.8 / sink 1.0 / stats 0.0（默认 warn 下直方图门控）。
- `ctest` 56/56；新增 `gpu-direct-present` 19 断言（32bpp/alpha=0/16bpp/尺寸变化/连续两帧）。
- `LIFT_GPU_VERIFY` TITLE 22 批全部 `diff_pixels=0 max_channel_diff=0`。
- `--present-screenshot` 与 `--dump-frame` 均有效，逐像素 0 diff（1:1）。
## 非目标/残余
- 按需回读、guest hash 每帧仍做（Present ~10%）；sprite 合批未做（TITLE 第二大项 18.4 ms）。这些留在子单跟踪，不把 58.5 ms 写成 33 ms。
- DDraw Flip 直出验收、16bpp 真 boot 覆盖未做。
