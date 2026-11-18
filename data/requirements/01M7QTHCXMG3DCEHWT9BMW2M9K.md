# 🐞S2 `--scale` 被第一帧 Present 抹掉：两处 SDL_SetWindowSize 用帧尺寸而非 ×scale（auto 建窗变大、进界面回 1×）

- id: REQ-01M7QTHCXMG3DCEHWT9BMW2M9K
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/present_window.cpp#present-window/scale-window-follows-frame
- repro: docs/03-emulator/window-scale.md#windowSizeForFrame
- severity: S2
- done_reason: 两处 SDL_SetWindowSize 改用 windowSizeForFrame(frame×scale) 后修好：改前用例红（expected 1920 got 1280）、改后绿；全量 ctest 116/116。裁决 = c-i（窗口跟帧保倍率），scale==1.0 逐字节不变。
- tags: [emulator-lift, present, window-scale, win32]

## 现象（用户口径 2026-11-19）

`--scale auto` 在**建窗那一刻**生效（窗口变大），但**进入界面后又缩回 1×**（1280×720）。

## 根因（已核）

`--scale` 只在 `SDL_CreateWindow`（窗口档 init）用了一次；**第一帧 Present 之后**，呈现汇点把窗口直接设成
guest 后缓冲尺寸：

- `present_sdl.cpp` 的 SDL_Renderer 路径（`SDL_SetWindowSize`）；
- `present_sdl.cpp` 的 GPU 直出路径（生产走这条）。

两处条件 `!impl_->sized` 缺省为 false ⇒ **第一帧必进**。全仓只有这 2 处 `SDL_SetWindowSize`；guest 的
`SetDisplayMode` 只写 guest 状态页，但它改帧尺寸 ⇒ 经 `size_changed` 间接触发同样两处。
⇒ `--scale` 被静默抹掉，且**所有既有读数都抓不到**：启动 readout 打在 `host.run` 之前，只证明建窗那一刻。

## 裁决与落地（2026-11-19）

- **c-i：窗口 = 帧尺寸 × scale**（图像尺寸跟 guest，倍率由 `--scale` 唯一决定）。理由：`--scale` 是倍率；
  这也是唯一**不推翻** `present-window` 既有"窗口跟随帧尺寸"判据的方案。
- 唯一换算 = `window_scale.h` 的 `windowSizeForFrame(frame_w, frame_h, scale)`（一维只调既有的
  `scaleWindowDimension`：远离 0 取整、≥1 px、≥INT_MAX 钳住；`scale` 非法 / 非正 ⇒ 按 1.0）。
- 两处 `SDL_SetWindowSize` 都改用它；`scale == 1.0` ⇒ 恒等于帧尺寸（**逐字节不变**）。
- 直出路径的 `direct_w` / `direct_h` 仍存**帧**尺寸（判"要不要重设"用帧尺寸变化，窗口尺寸由 scale 唯一决定）。

## 读数（2026-11-19 / win32 / clang-cl / RelWithDebInfo）

- 改前（只加用例、产品代码未改）：`146 checks, 4 failures` —— `expected 1920, got 1280 (ww)` /
  `expected 1080, got 720` / `expected 960, got 640` / `expected 720, got 480`，exit 1。
- 改后：`present-window/scale-window-follows-frame` 绿（1.5×1280×720 ⇒ 1920×1080；帧变 640×480 ⇒ 960×720），
  既有判据**一字未改**；`ctest -R '^(present-window|window-scale)$'` = 2/2；全量 ctest 116/116。
- 变异（把调用点乘子换回 `kDefaultScale`）⇒ CTest 退出码 8（红）→ 还原 → 绿，工具最终 exit 0（含收尾全量重建）。

## 未做 / 边界

- GPU 直出那一路与 Renderer 同构，但用例只覆盖 Renderer 路径（`present-window` 未设 `raster_gpu`）⇒
  补断言需真 GPU device 的用例。
- `--present auto` 回退建的隐藏窗口尺寸与 readout 的一致性、DPI 点 vs 像素 —— 本轮都未碰。
