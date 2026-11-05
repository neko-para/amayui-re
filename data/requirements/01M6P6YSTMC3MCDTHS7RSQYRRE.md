# emulator-lift · 可切换呈现后端：headless/CPU（缺省）与 SDL/GPU（显式）

- id: REQ-01M6P6YSTMC3MCDTHS7RSQYRRE
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/CMakeLists.txt#present-window-gpu
- done_reason: present-sink 40 checks（headless 不变 / sink 失败 latch 后仍 pump+可关窗 / stop→Stopped）、frame-blit 9（六种 masks）、present-window 31（dummy/software；565/alpha=0/33x17 16bpp 非紧凑 pitch/同一窗口 texture 重建/关窗事件）、present-window-gpu 31（driver=gpu/gpu=direct3d12）；真实 boot --max-frames 1 --present window --present-hidden：首帧 hash 0xEBF024FEBE998B05 与 headless 相同，窗口 BMP 与 guest BMP 抽样 RGB 0 差分；--present headless 不初始化 SDL 视频。ctest 47/47。
- tags: [emulator-lift present sdl gpu]

## 目标

把 `emulator-lift` 的呈现做成**运行时可选的两种后端**，缺省行为逐字节不变：

- `--present headless`（缺省）：今天的 `HeadlessPresenter`（hash / frameCount / `--dump-frame` / `--max-frames`）。
- `--present window`：真实 SDL 窗口 + GPU API 路径（`SDL_CreateRenderer` 的逗号驱动列表，
  缺省 `gpu,direct3d12,direct3d11,vulkan,metal,opengl`；不含 software）。`gpu` 命中以
  `SDL_GetGPURendererDevice() != NULL` 为正证据。

DDraw **不在验收内**（用户 2026-11-05：AGE 默认不走 DDraw）。D3D9 软件光栅保持原样；GPU 只做呈现。

## 设计要点

- `present.h` 保持 SDL-free：`HeadlessPresenter` 仍是唯一记录点，新增可选 `PresentSink`；
  sink 由 `Machine` 以 `unique_ptr` 拥有（生命周期不依赖 main 的声明顺序）。
- `present()` 先 hash/++frameCount，再 best-effort sink，再 `dumpBmp`；sink 失败只记账/latch，
  **不改变 guest 的 Present 结果**。
- `SdlWindow`（pimpl，头文件不暴露 SDL 类型）：`SDL_Init(VIDEO)` → 窗口 → `SDL_CreateRenderer(驱动列表)`
  → `SDL_SetRenderVSync(0)`；每帧按 `SurfaceFormat` 的 **masks** 选 SDL 像素格式（六种 D3D9 后缓冲格式），
  `SDL_UpdateTexture(..., pitch)`，`SDL_SetTextureBlendMode(NONE)`（否则 alpha=0 的帧整窗黑），
  `SDL_RenderClear`/`SDL_RenderTexture`/可选 present 前回读截图/`SDL_RenderPresent`；
  事件用 `SDL_PollEvent`（不是 `SDL_PumpEvents`）。
- 逻辑呈现尺寸按**当前帧**设置（不是固定 1280x720）；w/h/format 变化重建 texture；
  首帧按 surface 尺寸设窗口大小；resize 用 `LETTERBOX`。
- `Machine::run` 在块边界每 4096 条 pump + 检查 `stopRequested()`；关窗必须设
  `r.reason = StopReason::Stopped` + `detail`（否则 `r.ok()` 为假 ⇒ exit 2）。
- 窗口模式自动 `real_time = true`；`--bench` + 窗口给“不可比”警告。

## CLI

```text
--present headless|window|auto   缺省 headless（auto 仅调试：window 失败回 headless）
--present-driver <list>          逗号列表；缺省 gpu,direct3d12,direct3d11,vulkan,metal,opengl
--present-vsync 0|1              缺省 0
--present-hidden                 隐藏窗口（CTest）
--present-screenshot <path>      窗口像素 BMP；支持 {frame}；present 前回读
```

- `--present headless` + 任何窗口参数 ⇒ exit 2；显式 `--present-driver software` 允许但打“不是 GPU 路径”。

## 验收

- `present-sink`（pure）：sink 调用次数/参数/只读；`frameCount/lastHash` 与无 sink 一致；sink 失败 latch；
  headless 跑完 `SDL_WasInit(VIDEO)==0`；sink 的 stop ⇒ `Machine::run` 返回 `Stopped`（关窗不是 exit 2）。
- `frame-blit`（pure）：六种格式 + 非紧凑 pitch + 未知 mask。
- `present-window`（顶层，SKIP 77）：`SDL_VIDEODRIVER=dummy` + `--present-driver software`，
  窗口/上传/渲染/回读精确 RGB；含 565、33x17 pitch、alpha=0 帧、关窗事件。
- `present-window-gpu`（顶层，SKIP 77）：默认 GPU 列表；`SDL_GetGPURendererDevice()!=NULL` 才通过；
  打印 driver/gpuDevice。
- 真实 boot 证据（人工/脚本，不入 CTest）：`--max-frames 1 --present window --present-hidden`
  + `--present-screenshot`；窗口 BMP 与 `--dump-frame` 的 guest BMP 在 1:1 采样点一致；
  guest 帧 hash 与 headless 相同。

## 边界 / 风险

- 本阶段**不做**输入映射与 `GetMessageA` 宿主事件泵；若 guest 走到空 `GetMessageA` 仍会 abort，
  交互主循环另开单（阶段 2）。
- `gpu` 不可用时驱动列表可能落 `direct3d11`（可能 WARP）：日志如实写，不宣称“硬件加速”。
- 帧率无上限：看画面用 `--real-time` / `--max-frames`；`--bench + window` 不可比。
- 设备丢失只打日志 + latch，不自动重建。
