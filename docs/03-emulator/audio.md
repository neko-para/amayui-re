# 音频（AOT 臂）：泵 / 真 feeder 线程 / 延迟 / 暂停口径

> 只写**口径与实测**；缺陷史在 `defects.md`（BUG-22 共用 TEB、BUG-29 AOT 恒 D 泵），
> 需求判据在 `REQ-01M7MHEZRQJSMAAWPESVW44DSK`。

## 1. 两条喂料路线（唯一判据：`runtime/audio_policy.h` 的 `audioThreadMode()`）

| | host-thread（方案 C） | 主线程泵（方案 D） |
|---|---|---|
| 谁填 guest 环 | feeder 线程（guest 的 `0x004B5AA0` 线程体） | 主线程在块边界调 `AudioEngine::pump()` |
| 预读 lead | `kThreadLeadBytes` = **100 ms**（19200 B） | `kLeadBytes` = **600 ms**（115200 B） |
| 延迟量级 | ~100 ms | **~600 ms**（D 在一次 fill 期间暂停混音 ⇒ lead 必须撑过整刀 fill；`audio.cpp:42-47` 注释自述） |
| AOT 现状（2026-11-17 起） | **可达**（`--audio sdl`；`feeder=host-thread`） | 仅 `--audio virtual` / 设备回退 |
| AOT 现状（2026-11-17 之前） | 不可达（`has_machine` 门；BUG-29） | 恒 |

★ `lead` 是**预读字节数**，不是设备缓冲：它决定"这一刀 fill 之前先垫多少声音"，也就直接决定音效从 `Play` 到听见的间隔。

## 2. 暂停与音频（★ 用户裁决 2026-11-17）

**暂停（控制层 `pause` / 配方驱动的停住）不暂停音频的宿主线程**：允许音频继续按旧状态 loop、允许它继续出声/错拍。
⇒ ⛔ 不要给 mixer / feeder 加"跟随 guest 暂停"的钩子，⛔ 不要把"暂停期间还在响"记成缺陷。
判据只在"不暂停时"取：延迟、underrun、`fills` / `fill_bytes` 的读数。

## 2.1 现状（2026-11-17 起）

AOT 臂 `--audio sdl` = **真 feeder 线程**（`feeder=host-thread`，lead 19200 B）；`--audio virtual` 仍 D 泵（lead 115200 B）。
判据的唯一一份在 `runtime/audio_policy.h`（`audioThreadMode()` 两个事实 + `leadBytesFor()`），
"能起 feeder"由 `attach(..., const GuestLayout&)` 的引用入参在编译期钉死（⛔ 不是运行时能力位）。

| 量 | 改前（AOT 恒 D 泵） | 改后（feeder 线程） |
|---|---|---|
| `pushed - played` | 115200 B = **600.0 ms** | **19200 B = 100.0 ms**（4/4 次精确同值） |
| `calls=`（主线程泵次数） | 12 万级 | **0**（thread 档主线程一次都不泵） |
| underruns / `fill_slow` | 0 / 0 | 0 / 0（4/4） |
| `dev_frames`（设备缓冲，另一笔账） | 未取 | 1024 帧 = 21.3 ms @48k |
| 一刀 fill 墙钟 | 13.87 / 13.95 ms | 11.86 / 12.32 ms（启动段抖动时 96–109 ms，仍 < 500 的 quarter） |

## 3. 怎么量（读数口径）

- `--audio sdl` 会在启动 readout 打 `mode=… feeder=host-thread|主线程泵（方案 D） … lead=… B` 一行（`audio.cpp:214-222`）。
- `LIFT_AUDIO_TRACE=1`：`play state=… bytes=… loop=…`（起播）+ 每个 buffer 的状态行（`audio.cpp:381-390`）。
- `--audio-dump <path>`：这一跑**真交付**的 PCM（44 B 头 + 交付字节；`data=0` 就是没交付）。
- 设备侧队列：`SDL_OpenAudioDeviceStream` 的 `spec.samples` 未设置 ⇒ 设备缓冲由 SDL 缺省决定；改它属于**设备延迟**，与 lead 是两笔账，别混。

### 3.1 一次可复制的 run（mac arm64 / RelWithDebInfo；父级 2026-11-17 复跑同值）

```bash
cd apps/emulator-lift
LIFT_AOT_AUDIO_TRACE=1 ./out/emulator-lift-aot --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
  --game-dir ../../links/install --appdata ../../.tmp/audio-feeder/appdata \
  --skip-logo --max-frames 12 --audio sdl --sound --time-limit 120 --log-level info
```

要看的三行（`--log-level info` 才有前两行；⛔ 缺省 `warn` 档一行都不打）：

```
[audio] mode=sdl 48000 Hz / 16 bit / 2 ch（lead=19200 B）；游标=SDL 设备已消费字节；feeder=host-thread（方案 C）；muted=0（--sound）；PCM 由 guest 解码器产生，宿主只做混音与输出
[audio] 统计：played=409600 mixed=428800 pushed=428800 notify=3 fills=3 …
audio-pump: calls=0 fills=3 fill_bytes=288000 pushed=428800 feeder=host-thread lead=19200 dev_frames=1024
```

★ 判据 = `pushed - played`（稳态等于预读 lead）⇒ 上例 `428800 - 409600 = 19200 B = 100.0 ms`。
★ `calls=0` 是 **thread 档的正常读数**（主线程一次都不泵），⛔ 不要拿它当"没跑音频"。
★ `dev_frames` 是 SDL 设备缓冲，与 lead **两笔账**（提前垫多少 / 设备一次取走多少），⛔ 不要相加成"总延迟"。
◆ 设备打不开 ⇒ 回退 `virtual` ⇒ 自然是 D 泵（`lead=115200 B`）：那时只记"真设备档未测"，⛔ 不要放宽谓词。
