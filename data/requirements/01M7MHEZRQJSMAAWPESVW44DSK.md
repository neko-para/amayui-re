# 🐞S1 AOT 臂 audioThreadMode 恒 false（has_machine 门）⇒ 恒主线程 D 泵、预读 600 ms ⇒ SE/音效延迟（用户裁决：错误实现）

- id: REQ-01M7MHEZRQJSMAAWPESVW44DSK
- type: bug
- status: done
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- verify: apps/emulator-lift/tests/aot_audio_flags.cpp#s0-thread-mode-truth-table
- repro: apps/emulator-lift/src/runtime/audio.cpp#kLeadBytes
- severity: S1
- tags: [emulator-lift, audio, feeder]

## 一句话（已修，2026-11-17）

AOT 臂把「要不要起真 feeder 线程」判成 `has_machine`，而 AOT 有意没有 Machine（`cpu_.machine == nullptr`）
⇒ 从第一刻起恒为主线程 **D 泵**；D 泵的预读 lead = **600 ms** ⇒ 所有音效从「触发」到「听见」都有 ~0.6 s 间隔
（用户实测：hover TITLE 按钮的 SE 与真正出声有明显可见间隔）。

★ **用户裁决**：D 泵从来不是被决策过的方案，AOT 侧按 `has_machine` 判成恒 D 是**绝对的错误** ⇒ 修法不是"调小 lead"，
而是**给 AOT 实现真 feeder 线程**。

## 修法与判据（唯一一份）

- `audio_policy.h`：`audioThreadMode(mode, real_time_clock)` = **两个事实**（`mode == Sdl && real_time`）；
  "能起 feeder"由 `attach(..., const GuestLayout&)` 的**引用入参**在编译期钉死（⛔ 不是运行时能力位）。
  `--audio virtual` 仍 D 泵（dump 的确定性不被第二条线程破坏）。
- `leadBytesFor(thread_mode)`：600 ms / 100 ms 的唯一来源（`mixStep()` 与日志行都调它）。
- AOT 新增 **`CriticalSectionTable`**（`cpu_.cs_table`）+ `init()` 判空 —— feeder 的硬前提（否则 guest CS 走退化路径，
  争用返回非空 err ⇒ `feederEvent` 当致命 ⇒ 第一次并发就打死 feeder）。
- feeder 的 guest 执行器 = **AOT IR 派发**（新 `src/aot/aot_feeder.{h,cpp}`；`AGENTS.md` §12.8 执行来源只有 IR + shim）。
  TEB 来自 `GuestLayout::feeder_teb`（**自有**，⛔ 不共主线程 TEB ⇒ BUG-22 不复发）。
- 读数面：`--log-level`（缺省仍 `warn` ⇒ 默认输出不变）+ `audio-pump:` 行尾
  `feeder=host-thread|main-pump lead=<B> dev_frames=<n>`。

## 读数（mac arm64 / RelWithDebInfo / 同一命令、未暂停；父级复跑同值）

| 量 | 改前（D 泵） | 改后 |
|---|---|---|
| `pushed - played` | 115200 B = **600.0 ms** | **19200 B = 100.0 ms**（4/4 次精确同值） |
| `feeder=` / `lead=` | 主线程泵（方案 D）/ 115200 | host-thread / 19200 |
| `calls=` | 122632 / 123727 | **0** |
| underruns / `fill_slow` | 0 / 0 | 0 / 0（4/4） |
| `dev_frames` | 未取 | 1024 帧 = 21.3 ms（另一笔账） |

## 守卫与变异

`aot_audio_flags.cpp#s0-thread-mode-truth-table`（用户裁决的机械表达）· `#s0-lead-bytes-by-thread-mode` ·
`#cli-log-level-default-and-closed-set` · `audio.cpp#audio-device/feeder-teb-is-own`（BUG-22 回归口）·
`#audio-device/cs-table-blocks-across-threads`。4 条 mutation 实测红（`audio-feeder-shape` / `lead` / `teb` / `attach`）。

## 未做

真人听感（本机只有设备读数）；win32 的 `audio-sdl-device` / `aot-audio-wiring` 未跑。
