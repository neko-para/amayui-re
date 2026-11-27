# 音频（AOT 臂）：feeder 线程 / 延迟 / 暂停口径

> 只写**口径与实测**；缺陷史在 `defects.md`（BUG-22 共用 TEB、BUG-29 AOT 恒 D 泵），
> 需求判据在 `REQ-01M7MHEZRQJSMAAWPESVW44DSK`。

## 1. 唯一喂料路线 = feeder 线程

| | 口径 |
|---|---|
| 谁填 guest 环 | **feeder 线程**（guest 的 `0x004B5AA0` 线程体；`ResumeThread` 起真宿主线程） |
| 预读 lead | `kThreadLeadBytes` = **100 ms**（19200 B @ 48k/16/2ch；唯一一份在 `runtime/audio_policy.h`） |
| 谁混音 / 推设备 | 独立 **mixer 线程**（`AudioEngine::mixLoop`，5 ms 周期） |
| 主线程做什么 | 只执行 guest；块边界只看 `feederFailed()`（**没有主线程泵**） |

★ `lead` 是**预读字节数**，不是设备缓冲：它决定"这一刀 fill 之前先垫多少声音"，也就直接决定音效从 `Play` 到听见的间隔。

★ `--audio` **只决定输出位置**：

| `--audio` | 有没有输出 | 谁起 feeder | 摘要里的 `device=` |
|---|---|---|---|
| `off`（缺省） | 没有 | 不起（guest 的 DS 调用照跑，但没有 sink） | 不打 `audio` 行 |
| `sdl` | SDL3 真设备 | 起（guest `CreateThread` + `ResumeThread` 触发） | `ok`；设备打不开 ⇒ `none`（没有输出、不跑混音） |
| `--audio-dump <path>` | **设备无关的 WAV**（不是档位：`--audio` 仍是 off/sdl） | 起（同上） | `none`（dump 走文件 sink） |

⛔ **没有第二条喂料路线**：`virtual` 档 / 主线程 D 泵 / 600 ms 的 D 档 lead 已于 2026-11-27 整档删除
（用户裁决：「移除虚拟时钟，这个功能一直在和各个业务和测试打架」「无论是否 sdl，都需要 feeder 线程，
只是产出的音频是否有输出位置」）。⇒ 无论 headless 还是窗口，音频喂料走同一条 feeder 线程。

## 2. 暂停与音频（★ 用户裁决 2026-11-17）

**暂停（控制层 `pause` / 配方驱动的停住）不暂停音频的宿主线程**：允许音频继续按旧状态 loop、允许它继续出声/错拍。
⇒ ⛔ 不要给 mixer / feeder 加"跟随 guest 暂停"的钩子，⛔ 不要把"暂停期间还在响"记成缺陷。
判据只在"不暂停时"取：延迟、underrun、`fills` / `fill_bytes` 的读数。

## 2.1 现状

AOT 臂 `--audio sdl` 与 `--audio-dump` 都是**真 feeder 线程**（启动 readout / 摘要写 `feeder=host-thread`，
lead 19200 B）。"能起 feeder"由 `attach(..., const GuestLayout&)` 的引用入参在编译期钉死
（⛔ 不是运行时能力位）；布局不可用 ⇒ `begin()` **响亮失败**、运行期起不来 ⇒ `feederFailed()` 让执行循环停下并点名。

| 量 | 改前（AOT 恒 D 泵） | 改后（feeder 线程） |
|---|---|---|
| `pushed - played` | 115200 B = **600.0 ms** | **19200 B = 100.0 ms**（4/4 次精确同值） |
| 一刀 fill 墙钟 | 13.87 / 13.95 ms | 11.86 / 12.32 ms（启动段抖动时 96–109 ms，仍 < 500 的 quarter） |

## 3. 怎么量（读数口径）

- `--audio sdl` 在 `--log-level info` 打启动行 `[audio] mode=… feeder=host-thread … lead=… B`。
- `LIFT_AUDIO_TRACE=1`：`play state=… bytes=… loop=…`（起播）+ 每个 buffer 的状态行 + 每刀 fill 的墙钟分解。
- `--audio-dump <path>`：这一跑**真交付**的 PCM = **44 字节 WAV 头 + 真交付字节**；`data=0` 就是没交付。
  ⚠ **abort / 异常退出后文件头不回填**（`AudioEngine::end()` 没跑到）：那次 WAV 的 `RIFF`/`data` 长度是
  占位值 ⇒ **不可信**，⛔ 不要拿它当 PCM 判据。
- 设备侧队列：`SDL_OpenAudioDeviceStream` 的 `spec.samples` 未设置 ⇒ 设备缓冲由 SDL 缺省决定；改它属于**设备延迟**，与 lead 是两笔账，别混。

### 3.1 一次可复制的 run（mac arm64 / RelWithDebInfo；父级 2026-11-17 复跑同值）

```bash
cd apps/emulator-lift
LIFT_AOT_AUDIO_TRACE=1 ./out/emulator-lift-aot --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
  --game-dir ../../links/install --appdata ../../.tmp/audio-feeder/appdata \
  --skip-logo --max-frames 12 --audio sdl --sound --time-limit 120 --log-level info
```

要看的行（`--log-level info` 才有；⛔ 缺省 `warn` 档一行都不打）：

```
[audio] mode=sdl 48000 Hz / 16 bit / 2 ch（lead=19200 B）；游标=SDL 设备已消费字节；feeder=host-thread；muted=0（--sound）；PCM 由 guest 解码器产生，宿主只做混音与输出
[audio] 统计：played=409600 mixed=428800 pushed=428800 notify=3 fills=3 …
audio     : mode=sdl device=ok muted=0 fills=3 fill_bytes=288000 pushed=428800 played=409600 …
```

★ 判据 = `pushed - played`（稳态等于预读 lead）⇒ 上例 `428800 - 409600 = 19200 B = 100.0 ms`。
★ 不再有 `audio-pump:` / `calls=` 行（主线程泵已删除）。
◆ 设备打不开 ⇒ **没有音频输出**：不跑混音，摘要写 `mode=off device=none muted=1`（`--audio-dump`
另给时 dump 这一路继续，仍报 `device=none`）。⛔ 不要放宽谓词，
⛔ 也不要再找一个"设备无关的档位"来顶替（原 `virtual` 档已于 2026-11-22 移除）。

## 4. 吞吐读数（2026-11-17 / mac arm64 M3 / 转场 90 s / `--audio sdl --sound`）

```
fills=186  fill_bytes=15561900  pushed=17304320  played=17285120  notify=186
underruns=2  underrun_silence_events=2  underrun_silence_bytes=124672
fill_max_ms=1016.04  fill_slow=1  fill_overlapped=5
```

- **平均 ≈ 484 ms 一刀**（每刀产出 ~0.5 s 音频）⇒ 平均利用率 ~95–100%，**没有余量**。
- **最坏 1.0 s 一刀** ⇒ 那两刀 underrun，各插入 ~0.32 s 静音；机制是"插静音、**时间轴不漂**"（`underrun_silence_*`），
  所以**音乐整体节奏不会变**，只是偶发空档。
- 对照：解释器时代（含已退役的 per-EA override）是 **200–250 ms/刀**。
- 归因未做（feeder 没有 per-thread profile）⇒ 单子 `REQ-01M7N0G…`（先测再优化）。

### 4.1 那 1.0 s 一刀最可能是什么：**令牌头阻塞**（不是 decode 慢）

`guest_token.h` 的口径：令牌是**容器锁**（GuestHeap / HandleTable / Vfs / ComRegistry / SurfaceArena / 内存账本），
**不是执行锁** —— 所以音频 feeder 与渲染/业务主线程会在 **shim 边界**上互相等。

历史读数（解释器时代，`REQ-01M75VZXV4SDMVVM1Z72G009XJ`，已 dropped）：
"feeder 在 shim 边界被主线程长时间挡在令牌外（**单刀 1.3–1.7 s 全是 shimtok**）⇒ 静音晚到 ⇒ 尾声重播 2–3 次"。
它的 `dropped_reason` 写明"AOT 接音频后若再现，按'与暂停/令牌的配合'重新立单" ⇒ 现在 AOT 有 feeder、读数形状一致 ⇒ 已重开
（`REQ-01M7N0G…`，判据：先量 `t_shim_token_wait_ns` 占那 1.0 s 的比例）。

⇒ 在量出这个比例之前，⛔ 不要先优化 Vorbis 解码（`REQ-01M7N0G…` 与解码吞吐单分开）。

## 5. win32 读数（2026-11-18 / win32 Ryzen 7 7735H / `--audio sdl --sound` / 加载配方 240 s）

> ★ 与 §4 的 mac 读数**不是同一个场景、也不是同一次构建**：本节 = win32、`LIFT_AOT_SHAPE="split8-O2"`（LTO OFF）、
> 配方 `.tmp/sc0000/load01.plan`（TITLE → 加载 01 槽 → `SN0000` → 点中央 → `SC0000`，到达后继续跑到 `--time-limit`）。
> ⛔ 不改 §4 的任何数字；⛔ 两节相减**不是**"平台差"（理由见 §5.5）。
> ★ 本节四行 Phase-0 在 AOT 臂上是 2026-11-18 才接的（`AotHost::run` 调 `guestToken().arm()` + 两臂共用
> `runtime/token_phase0_report.cpp`）；此前只有解释器臂打印 —— §4 的 mac run 没这四行**不代表** mac 上没记账。

### 5.1 一条可复制的 run

```powershell
cd E:\Projects\amayui-re
Copy-Item -Recurse -Force .tmp\appdata .tmp\audio-token\appdata-r1      # 每次一份新副本（⛔ 不写 .tmp/appdata）
$env:LIFT_AUDIO_TRACE = '1'
.\apps\emulator-lift\out\emulator-lift-aot.exe --present headless --skip-logo --audio sdl --sound `
  --game-dir .\links\install --appdata .tmp\audio-token\appdata-r1 `
  --input-plan .tmp\sc0000\load01.plan --time-limit 240 --max-frames 0 *> .tmp\audio-token\run1.log
Remove-Item Env:LIFT_AUDIO_TRACE
```

- 3 跑全部：`input-plan: steps=12 fired=4 pending=0` + `stop: time-limit @ eip=004904E0（正常结局）`
  ⇒ 配方走完、到得了 `SC0000`。
- 3 跑全部：`audio     : mode=sdl device=ok … fills=…`（§5.2 的表就是它的字段；`calls=` 那一行已随主线程泵删除）。

### 5.2 摘要（3 跑，`--time-limit 240`）

| run | fills | fill_bytes | pushed | underruns | `fill_max_ms` | `fill_slow` | `fill_overlapped` | `device` |
|---|---|---|---|---|---|---|---|---|
| run1 | 387 | 34,857,900 | 46,097,280 | 0 | 72.63 | 0 | 6 | ok |
| run2 | 413 | 37,353,900 | 46,097,280 | 0 | 62.11 | 0 | 6 | ok |
| run3 | 391 | 35,241,900 | 46,097,280 | 0 | 52.66 | 0 | 6 | ok |

### 5.3 逐刀 `total` 分布与最坏刀（`LIFT_AUDIO_TRACE` 的 `feeder fill` 行）

| run | median | p90 | p99 | max | mean | 整跑 `Σshimtok / Σtotal` |
|---|---|---|---|---|---|---|
| run1 | 23.40 | 28.36 | 53.51 | 72.63 | 22.38 | **2.07%** |
| run2 | 22.85 | 26.74 | 38.64 | 62.11 | 21.56 | **1.35%** |
| run3 | 22.36 | 25.11 | 37.16 | 52.66 | 20.69 | **1.48%** |

（单位 ms。每刀交付的音频 ≈ `pushed / fills` = 0.62 / 0.58 / 0.61 s；逐刀**间隔** = 240 s / fills = 620 / 581 / 614 ms。）

最坏那一刀的完整分项（`wait` = 入口等令牌，`guest` 含 `shimtok`，`total` = 一刀墙钟）：

| run | total | wait | pre | guest | shimtok | shimblk | `shimtok/total` |
|---|---|---|---|---|---|---|---|
| run1 | 72.63 | 0.00 | 0.05 | 72.63 | **0.00** | 1 | 0.0% |
| run2 | 62.11 | 2.00 | 0.01 | 62.11 | **0.00** | 1 | 0.0% |
| run3 | 52.66 | 12.01 | 0.01 | 52.65 | 30.91 | 2 | 58.7% |

- 全部 1,191 刀：`total > 100 ms` **0 刀**、`shimtok > 50 ms` **0 刀**、单刀 `shimtok` 最大 **34.84 ms**；
  `shimtok > 10 ms` 只有 3–4 刀；`wait > 10 ms` 只有 1 刀（12.01 ms）。
- 最坏的 5 刀合起来 `shimtok/total` 也只有 14.5% / 27.9% / 29.8%。
- 时间四分段（1/4→4/4）`total` 中位：run1 `12.19 / 23.57 / 23.29 / 23.87`；run2 `21.43 / 22.52 / 23.54 / 23.67`；
  run3 `12.09 / 22.27 / 22.34 / 22.69` ms。

### 5.4 Phase-0（`tokenhold / holdhist / waithist / shimmax`）

| run | main hold / span | duty | main max | main segs | other hold | other max | other segs |
|---|---|---|---|---|---|---|---|
| run1 | 40,443.2 / 240,000.6 ms | 16.9% | 67.521 ms | 86,462,557 | 181.8 ms | 0.786 ms | 8,706 |
| run2 | 40,097.7 / 240,005.1 ms | 16.7% | 53.928 ms | 89,453,667 | 194.0 ms | 1.007 ms | 9,111 |
| run3 | 38,266.9 / 240,000.3 ms | 15.9% | 62.359 ms | 88,293,595 | 172.3 ms | 0.704 ms | 8,776 |

- `holdhist main`（`<1ms | 1-5 | 5-20 | 20-50 | >50`）：
  run1 `86,451,296 | 11,172 | 79 | 7 | 3`；run2 `89,442,379 | 11,220 | 60 | 7 | 1`；run3 `88,282,477 | 11,067 | 42 | 7 | 2`。
- `waithist`（同五桶）—— main：`2,371 | 0 | 0 | 0 | 0` / `2,315 | 1 | 0 | 0 | 0` / `1,951 | 1 | 0 | 0 | 0`；
  other（feeder）：`830 | 56 | 4 | 1 | 0` / `712 | 26 | 3 | 1 | 0` / `617 | 40 | 4 | 1 | 0`
  ⇒ feeder 等令牌 **>50 ms 桶 3 跑全 0**；20–50 ms 各 1 次。
- `shimmax` top（3 跑并集；`module!name` · max · calls）：
  `com!IDirect3DDevice9::DrawIndexedPrimitive` 53.9–67.5 ms（10.3k–11.8k 次）·
  `com!IDirect3DDevice9::Present` 47.0–79.7 ms（4.0k–4.2k）·
  `d3dx9_43!D3DXCreateTextureFromFileInMemory` 42.8–61.6 ms（**3 次**）·
  `com!ID3DXSprite::End` 24.2–41.6 ms（10.9万–11.4万 次）·
  `kernel32!CreateFileA` 15.0–19.6 ms（1,252 次）· `kernel32!ReadFile` 9.8–19.6 ms（4,074 次）。
- ★ 3/3 的 **main max 恰好等于 `DrawIndexedPrimitive` 的 max**（67.521 / 53.928 / 62.359）⇒ 最长的一次不间断持令牌
  就是一次 D3D9 索引绘制调用；`Present` 的 shim 墙钟更大（79.7 ms）却没成为最长持有 ⇒ 它在等设备期间**让出过令牌**
  （`YieldGuard`）—— 这是"shim 墙钟 ≠ 持有时间"的具体一例。

### 5.5 与 §4（mac）的差异：**先看口径，再谈差**

| 量 | §4（mac M3） | 本节（win32） |
|---|---|---|
| 每刀 `total` | ≈ 484 ms（= 逐刀间隔 ⇒ 利用率 ~100%） | 中位 22.4–23.4 ms（逐刀间隔 581–620 ms ⇒ 利用率 ~3.6–4.0%） |
| 最坏刀 | 1,016.04 ms | 52.66–72.63 ms |
| underruns | 2 | 0（`fill_slow` 也 0） |
| 每刀 `shimtok` 占比 | 未记（§4 没留逐刀 trace） | 1.35–2.07%（单刀最大 34.84 ms） |

⛔ **不能把上表相减当"平台差"**：场景（本配方 = 加载 + 到达 `SC0000` 后待机；§4 = 90 s 转场）、平台、构建
（本节 LTO OFF）、逐刀交付量（≈0.6 s vs ≈0.5 s）都不同。两条**待核**线索（这里不下结论）：
1. §4 那次若有 `LIFT_AUDIO_TRACE` 的逐刀行，`shimtok` 占比是**直接读数**（本节的 1.35–2.07% 是同口径的 win32 值）；
2. §4 那次构建是否已含 `732e503`（AOT feeder 走 **IR 派发**）—— 若那次 feeder 走的是解释器路径，20× 的量级差
   与令牌无关。

## 5. 复测（2026-11-18 · 当前 HEAD `00af635` · mac arm64 M3）

**之前那 1.0 s 一刀 / 2 次 underrun 在当前代码上不复现**：

| run | underruns | fill_max_ms | fill_wait_ms | insns | frames |
|---|---|---|---|---|---|
| `--audio sdl --sound --log-level info`（90 s） | **0** | **351.85** | 9.00 | 6.687e9 | 2711 |
| `--audio sdl --sound`（90 s，`--frame-log`） | **0** | **18.99** | 0.00 | 79.18e9 | 9344 |
| `--profile` 第三次（关音频，测时间轴） | — | — | — | — | 黑区间 **2.60 s**（帧 1761–2671 / 911 帧） |

★ 差异的来源：当前缺省 `PixelShaderVersion=0xFFFF0200` / `VertexShaderVersion=0xFFFE0100`（等级 2），
`required = 5 pixel + 1 vertex` 全部 `implemented` ⇒ 转场走 **FX 路径**，CPU 侧那 6 个重帧消失 ⇒ feeder 不再被饿/被挡。
⇒ 之前那条"令牌头阻塞"与"feeder 吞吐"两单按复测读数**已 dropped**（保留读数；若将来 `fill_max_ms` 逼近 500 ms
且 `fill_wait_ms` 占大头，按原 M0/M1 重新立案）。
