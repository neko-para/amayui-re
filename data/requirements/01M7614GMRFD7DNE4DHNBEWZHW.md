# M0.1：Present 内仍占令牌 ~250 ms 的残留（fill_max 550→≤250 ms）

- id: REQ-01M7614GMRFD7DNE4DHNBEWZHW
- type: req
- status: done
- parent: REQ-01M75WWFGT8ZEBGEPENQHSYTB8
- done_reason: 结构性修复已落（Present 路径 7 处让出）；Windows/D3D12 验收读数在 REQ-01M770JFDPZKKJR7D0ZZ09FJJ7：holdhist>50ms 36→2、持有 251→56ms、fill_max 550→214.74、fill_slow=0，且 waithist>50ms 仍为 0（没把持有换成等待）
- tags: [emulator-lift audio token]

## 背景

M0 已落地（4 项里 3 项：Present 的 swapchain wait / `downloadInto` fence / `SDL_RenderPresent` /
DS shim 等 `mutex_` / harness tick；`fread` 按纪律不做）。实测收益明显：

| 指标 | 旧 | M0 后（run7 / run8） |
|---|---|---|
| `fill_max_ms` | 1743.8 | **550.4 / 518.7** |
| `fill_slow` | 4 | **0 / 0** ✅ |
| `tokenhold main max` | （未测） | **251.3 / 274.7 ms** |

**但验收线是 `fill_max_ms ≈ 200 ms`**，现在 550 ms，还差 **2.5×**。

## 现在的账（`LIFT_AUDIO_TRACE=1`，run7）

- `holdhist`：`<1ms` 45.9M / `1–5ms` 2707 / `5–20ms` 41 / `20–50ms` 32 / **`>50ms` 36**；
- `waithist`（other 侧）：`>50ms` **29** 次 ⇒ 交接延迟仍有长尾；
- `shimmax top 8`：

| # | shim | max | calls |
|---|---|---|---|
| 1 | `com!IDirect3DDevice9::Present` | **257.4 ms** | 547 |
| 2 | `com!IDirectSoundBuffer8::Stop` | **100.6 ms** | 9 |
| 3 | `com!IDirect3DDevice9::DrawIndexedPrimitive` | 51.3 ms | 2460 |
| 4 | `d3dx9_43!D3DXCreateTextureFromFileInMemory` | 39.2 ms | 3 |
| 5 | `com!ID3DXSprite::End` | 25.0 ms | 1711 |
| 6 | `com!IDirect3DDevice9::Clear` | 15.8 ms | 551 |

⇒ **剩余最长的一格在 `Present` 里**（M0 只移走了 swapchain wait，里面还有别的：fence / probe / readback /
`rasterFlush` / `uploadGuest` 的 Acquire）。第 2 名 `DS::Stop` 100 ms 也值得单独看（它可能与本单的音频等待叠加）。

## 要做

1. 找出 `com!IDirect3DDevice9::Present` 里**仍然占着令牌 ~250 ms** 的那一段（`文件:行`），
   并判定"这一段为什么不需要令牌 / 或为什么必须持"。
2. 同样处理 `DS::Stop` 的 100 ms（⛔ 注意锁序仍是 令牌 → `mutex_` → `pending_mutex_`）。
3. 判据：`holdhist` 的 `>50ms` 桶从 36/30 降到**个位数**；`fill_max_ms` 从 550 降到 **≤250 ms**；
   `fill_slow` 保持 **0**。

## 已裁决（⛔ 不要重新裁决）

- **A) `fread` 的句柄生命周期改造：不做。** 依据：`shimmax` 前 8 名里**没有 `kernel32!ReadFile`**
  （最长的"安静"项是 `VirtualAlloc` 5.2 ms）⇒ 它**不是** 43 ms 的元凶，不值得为它改句柄生命周期。
- **B) `WindowManager` 的 `check()` 覆盖：排进 M1 前置**（M0 实测它一个 `check()` 都没有 ⇒ `--assert-guest-token` 抓不到这一类）。
- ⛔ 不重复 M0 已做过的 7 处 Present 让出；⛔ 不改 `GuestToken` 本身。

## 读数口径（⛔）

- 验收**不**看 `tokenwait` 总量：`YieldGuard` 的**取回**被挡也会记进 `waits_` ⇒ M0 之后它**会变大**。
- 看 `tokenhold max` / `holdhist` / `[audio] feeder fill … shimtok=` / `fill_max_ms` / `fill_slow`。
- 报数前先贴 `feeder=host-thread` 与 `device=ok` 两行（否则可能量到 pump 臂）。

## 状态（2026-11-12 10:37，网络中断后复核）

- 上一轮派出的实现者**因网络故障失败、没有留下任何产出**；
  复核 `git status` + 文件 mtime（`d3d9_gpu.cpp` 03:14 / `audio.cpp` 03:15 / `machine.cpp` 03:16 / `present_sdl.cpp` 03:20，
  均早于失败时刻 03:34）⇒ **本单没有落地产出**，工作树仍等于 M0 完成时的状态（全量 `ctest` 已复验，见报告）。
- 本单**未开工**，下一步就是原样重派：定位 `Present` 里仍持令牌的那 ~250 ms。
- 判据不变：`holdhist >50ms` 从 36/30 → **个位数**；`fill_max_ms` 550 → **≤250 ms**；`fill_slow` 保持 **0**。

## 2026-11-12 实现与复审（macOS 侧）
- 7 处 `Acquire`/`Submit` 已包进 `YieldGuard`（`d3d9_gpu.cpp` 改动前站点 `:456/:816/:925/:1198/:1216/:1225/:1264`）。
  `:1264` 是**唯一有后端源码依据**的站点（D3D12 `IDXGISwapChain_Present` 在该提交里），其余为同类覆盖。
- 设计复审 + 变更复审均 `approve with conditions`（`.tmp/audio-token/m01-{analysis,design-review,change-review}.md`）；
  两处注释事实错误已改；`ctest` 82/82（`overlay-window-smoke` 单跑 31.7 s Passed，属 flake）；`--assert-guest-token` violations=0。
- 读数（macOS/Metal，**不是口径A**，共享负载 load 18–22）：`holdhist >50ms = 0`、`waithist main >50ms = 0`、
  `Present` shim max **17–116 ms**（名次 #1–#6，随负载漂）、`fill_max 143–181 / fill_slow 0`。
  **Windows 的 250 ms 本机不复现** ⇒ 判据只能由 `REQ-01M770JFDPZKKJR7D0ZZ09FJJ7`（Windows A/B）定。
- 本变更**没有行为级守卫**（复审变异实测：删任一 yield / 计数挪前 / `:1225` 翻转都全套绿）⇒ 只能靠代码复审 + 读数。
- 未做：`:1549/:1562`（acquire 不判空 / submit 失败被吞 = 行为变更，另单）、诊断/叠层/截图路径、`DS::Stop`（不同根因）。
