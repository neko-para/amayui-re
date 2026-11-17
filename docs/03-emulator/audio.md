# 音频（AOT 臂）：泵 / 真 feeder 线程 / 延迟 / 暂停口径

> 只写**口径与实测**；缺陷史在 `defects.md`（BUG-22 共用 TEB、BUG-29 AOT 恒 D 泵），
> 需求判据在 `REQ-01M7MHEZRQJSMAAWPESVW44DSK`。

## 1. 两条喂料路线（唯一判据：`runtime/audio_policy.h` 的 `audioThreadMode()`）

| | host-thread（方案 C） | 主线程泵（方案 D） |
|---|---|---|
| 谁填 guest 环 | feeder 线程（guest 的 `0x004B5AA0` 线程体） | 主线程在块边界调 `AudioEngine::pump()` |
| 预读 lead | `kThreadLeadBytes` = **100 ms**（19200 B） | `kLeadBytes` = **600 ms**（115200 B） |
| 延迟量级 | ~100 ms | **~600 ms**（D 在一次 fill 期间暂停混音 ⇒ lead 必须撑过整刀 fill；`audio.cpp:42-47` 注释自述） |
| AOT 现状（2026-11-17 之前） | 不可达 | **恒**（`has_machine` 门；见 BUG-29） |

★ `lead` 是**预读字节数**，不是设备缓冲：它决定"这一刀 fill 之前先垫多少声音"，也就直接决定音效从 `Play` 到听见的间隔。

## 2. 暂停与音频（★ 用户裁决 2026-11-17）

**暂停（控制层 `pause` / 配方驱动的停住）不暂停音频的宿主线程**：允许音频继续按旧状态 loop、允许它继续出声/错拍。
⇒ ⛔ 不要给 mixer / feeder 加"跟随 guest 暂停"的钩子，⛔ 不要把"暂停期间还在响"记成缺陷。
判据只在"不暂停时"取：延迟、underrun、`fills` / `fill_bytes` 的读数。

## 3. 怎么量（读数口径）

- `--audio sdl` 会在启动 readout 打 `mode=… feeder=host-thread|主线程泵（方案 D） … lead=… B` 一行（`audio.cpp:214-222`）。
- `LIFT_AUDIO_TRACE=1`：`play state=… bytes=… loop=…`（起播）+ 每个 buffer 的状态行（`audio.cpp:381-390`）。
- `--audio-dump <path>`：这一跑**真交付**的 PCM（44 B 头 + 交付字节；`data=0` 就是没交付）。
- 设备侧队列：`SDL_OpenAudioDeviceStream` 的 `spec.samples` 未设置 ⇒ 设备缓冲由 SDL 缺省决定；改它属于**设备延迟**，与 lead 是两笔账，别混。
