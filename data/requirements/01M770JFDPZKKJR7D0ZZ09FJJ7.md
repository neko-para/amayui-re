# M0.1 验收读数（Windows/D3D12）：Present 内 ~250 ms 与 fill_max 的 A/B

- id: REQ-01M770JFDPZKKJR7D0ZZ09FJJ7
- type: req
- status: open
- parent: REQ-01M7614GMRFD7DNE4DHNBEWZHW
- tags: [emulator-lift, audio, token, windows]

## 为什么需要这张单
`REQ-01M7614GMRFD7DNE4DHNBEWZHW`（M0.1）的结构性修复已落：Present 路径上 7 处 `SDL_AcquireGPUCommandBuffer` /
`SDL_SubmitGPUCommandBuffer` 已移进 `GuestToken::YieldGuard`。但它的验收判据（`holdhist >50ms` 个位数、
`fill_max_ms ≤250 ms`、`fill_slow=0`）来自 **Windows / D3D12** 的读数；本机 macOS / Metal 的 Present 只有
4–5 ms，**250 ms 不复现**（Metal 的提交是异步 `[commandBuffer commit]`，D3D12 的 `IDXGISwapChain_Present`
才在提交里同步等）。⇒ 必须在有 D3D12 的那台设备上跑一次 A/B，才能判 M0.1 是否达标。

## 命令（`apps/emulator-lift`，RelWithDebInfo；记 `git log -1`）
```powershell
New-Item -ItemType Directory -Force ..\..\.tmp\m01 | Out-Null
$env:LIFT_AUDIO_TRACE='1'; $env:LIFT_FRAME_TRACE='1'
# A 臂：GPU 直出（要看的因果臂）
.\out\emulator-lift.exe --game-dir ..\..\links\install --appdata ..\..\.tmp\m01\appdata-A `
  --present window --present-vsync 0 --present-hidden --raster gpu `
  --native-overrides all --skip-logo --audio sdl --interaction-graph ..\..\graphs\se-eof.graph `
  > ..\..\.tmp\m01\A.log 2>&1
# B 臂：完全没有 SDL_GPU（renderer × software）
.\out\emulator-lift.exe <同上> --appdata ..\..\.tmp\m01\appdata-B --raster cpu --present-driver software > ..\..\.tmp\m01\B.log 2>&1
# C 臂：不建 gpu_raster，但仍走 SDL_Renderer 的 GPU 呈现
.\out\emulator-lift.exe <同上> --appdata ..\..\.tmp\m01\appdata-C --raster cpu > ..\..\.tmp\m01\C.log 2>&1
```
取数：
```powershell
Select-String ..\..\.tmp\m01\A.log -Pattern '^tokenhold|^holdhist|^waithist|^token |^tokenwait|^audio |^shimmax|^\s+\d+\. ' | % Line
$m = Select-String ..\..\.tmp\m01\A.log -Pattern 'pres : readback=([\d.]+)ms hash=([\d.]+)ms stats=([\d.]+)ms sink=([\d.]+)ms'
"sink max     = " + (($m | % {[double]$_.Matches[0].Groups[4].Value}) | Measure-Object -Maximum).Maximum
"readback max = " + (($m | % {[double]$_.Matches[0].Groups[1].Value}) | Measure-Object -Maximum).Maximum
```

## 判据
- A 臂：`holdhist : main … >50ms` 从 36（2026-11-12 读数）降到 **个位数**；`fill_max_ms ≤250`；`fill_slow=0`；
  `waithist : main >50ms` **不显著增大**（防"持有变等待"）。
- `shimmax` 里 `IDirect3DDevice9::Present` 的 max 明显下降（或掉出前 5）。
- `pres : sink=` 是长帧的主桶 ⇒ 250 ms 在 `:1264`（已让出），A 臂应下降；`readback=` 主桶 ⇒ 在 `:816/:925/:456`。
- **A/B/C 三臂都 ≈250 ms** ⇒ 与 `d3d9_gpu` 无关 ⇒ 停下报告（M0.1 重开，改查别处）；⛔ 不要为了绿改判据。

## 交付
把 A/B/C 的 `tokenhold/holdhist/waithist/shimmax/audio` 行与 `pres :` 两个 max 贴回本单（或父级），
并写日期 / 构建 / 平台。⛔ 不改任何产品代码；这是读数单。
