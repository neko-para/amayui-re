# M0.1 验收读数（Windows/D3D12）：Present 内 ~250 ms 与 fill_max 的 A/B

- id: REQ-01M770JFDPZKKJR7D0ZZ09FJJ7
- type: req
- status: done
- parent: REQ-01M7614GMRFD7DNE4DHNBEWZHW
- done_reason: Windows/D3D12 三臂读数已取并贴回单：A 臂 holdhist>50ms=2、持有 56ms、fill_max=214.74、fill_slow=0、waithist>50ms=0 ⇒ M0.1 达标；A(56ms) ≪ B/C(394/486ms) 证实 250ms 在 d3d9_gpu 路径且已修
- tags: [emulator-lift, audio, token, windows]

## 为什么需要这张单

`REQ-01M7614GMRFD7DNE4DHNBEWZHW`（M0.1）的结构性修复已落（Present 路径上 7 处
`SDL_AcquireGPUCommandBuffer` / `SDL_SubmitGPUCommandBuffer` 移进 `GuestToken::YieldGuard`），
但判据来自 **Windows / D3D12** 的读数：macOS/Metal 的 Present 只有 4–5 ms，**250 ms 不复现**
（Metal 提交是异步 `[commandBuffer commit]`，D3D12 的 `IDXGISwapChain_Present` 才在提交里同步等）。

## 实测（2026-11-12 23:30，Windows 11，RelWithDebInfo，HEAD `fe3cd30`，`out-win`）

命令见下（A = GPU 直出因果臂；B = 无 SDL_GPU；C = 不建 gpu_raster 但仍走 SDL_Renderer）。

| 指标 | A（`--raster gpu`） | B（`--raster cpu --present-driver software`） | C（`--raster cpu`） | 目标 |
|---|---|---|---|---|
| `tokenhold : main … max` | **56.23 ms** | 394.85 ms | 485.85 ms | 下降 |
| `holdhist : main … >50ms` | **2** | 123 | 121 | **个位数** ✅ |
| `waithist : main … >50ms` | **0** | 0 | 0 | 不显著增大 ✅ |
| `tokenwait` | 935 次 / 277.0 ms（avg 296 µs） | 1427 / 23419.7 ms | 1398 / 24170.8 ms | — |
| `fill_max_ms` / `fill_slow` | **214.74 / 0** | （未跑音频判据） | （未跑音频判据） | ≤250 / 0 ✅ |
| `shimmax` Present max | **56.59 ms**（旧 257.39） | 14.56 ms（仅 16 帧） | 6.63 ms（仅 16 帧） | 明显下降 ✅ |
| `pres : sink_max` / `readback_max` | **3.3 / 52.5 ms** | 14.5 / 0 ms | 6.6 / 0 ms | 250 ms 不在 sink |
| 退出 | **0**，`detail: interaction: task done` | 5（`step3 等 GAMESTART` 超时，30 s 只 16 帧） | 5（同） | A 臂 0 ✅ |

`present : driver=gpu-direct video=windows gpu=direct3d12 present=direct/immediate`（A 臂确实是 D3D12 直出）。

## 结论

- **M0.1 达标**：`holdhist >50ms` **36 → 2**、持有最长 **251 → 56 ms**、`fill_max_ms` **550 → 214.74**、`fill_slow` **0**，
  且 `waithist >50ms` 仍为 0（**没有**把"持有"换成"等待"）。
- 250 ms 原来在 `pres : sink`（swapchain 等待）；让出之后 `sink_max` 只剩 3.3 ms ⇒ **因果臂确认**。
- **A ≪ B/C**（56 vs 394/486 ms）⇒ 那 250 ms **确实**在 `d3d9_gpu` 路径里，不是别处。
- ⚠ B/C 的读数**不能当 M0.1 的反证**：`--raster cpu` 在这台机器上只有 ~0.5 fps（30 s 16 帧），
  软件光栅本身就是一次几百 ms 的**单个 shim**（无让出点）⇒ 那是另一档问题（若将来要 CPU 光栅，需要给它加让出点）。

## 后续（不在本单）

- 现在 A 臂的前 3 名变成 `Present 56.6` / `DrawIndexedPrimitive 56.2` / `D3DXCreateTextureFromFileInMemory 47.3`
  ⇒ 下一档"单次持有 ~56 ms"已经不是 Present 独占，需要新的判断（是否值得为 Draw 引入让出点）。
- `--raster cpu` 的 394–486 ms 持有是独立问题（软件光栅无让出点），与 D3D12 无关。

## 命令（可复跑）

```powershell
cd apps\emulator-lift
$env:LIFT_AUDIO_TRACE='1'; $env:LIFT_FRAME_TRACE='1'
$exe='.\out-win\emulator-lift.exe'
$common=@('--game-dir','..\..\links\install','--present','window','--present-vsync','0','--present-hidden',
          '--native-overrides','all','--skip-logo','--audio','sdl','--interaction-graph','..\..\graphs\se-eof.graph')
& $exe @common --appdata ..\..\.tmp\m01\appdata-A --raster gpu                          *> ..\..\.tmp\m01\A.log
& $exe @common --appdata ..\..\.tmp\m01\appdata-B --raster cpu --present-driver software *> ..\..\.tmp\m01\B.log
& $exe @common --appdata ..\..\.tmp\m01\appdata-C --raster cpu                           *> ..\..\.tmp\m01\C.log
```
