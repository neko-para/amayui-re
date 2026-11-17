# 🐞S1 AOT 臂 audioThreadMode 恒 false（has_machine 门）⇒ 恒主线程 D 泵、预读 600 ms ⇒ SE/音效延迟（用户裁决：错误实现）

- id: REQ-01M7MHEZRQJSMAAWPESVW44DSK
- type: bug
- status: open
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- repro: apps/emulator-lift/src/runtime/audio.cpp#kLeadBytes
- severity: S1
- tags: [emulator-lift, audio, feeder]

## 一句话

AOT 臂把「要不要起真 feeder 线程」判成 `has_machine`，而 AOT 有意没有 Machine（`cpu_.machine == nullptr`）
⇒ 从第一刻起恒为主线程 **D 泵**；D 泵的预读 lead = **600 ms** ⇒ 所有音效从「触发」到「听见」都有 ~0.6 s 间隔
（用户实测：hover TITLE 按钮的 SE 与真正出声有明显可见间隔）。

★ **用户 2026-11-17 裁决：D 泵从来不是被决策过的方案，AOT 侧按 `has_machine` 判成恒 D 是绝对的错误。**
⇒ 本单不是"调小 lead"，而是**给 AOT 实现真 feeder 线程**。

## 证据（父级已核）

- `src/runtime/audio_policy.h:5` / `src/runtime/audio.cpp:203-208`：`has_machine = cpu_ && cpu_->machine`；
  `audioThreadMode(mode, real_time_clock, has_machine)` 三者全真才 thread 模式 ⇒ AOT 恒 false。
- `src/runtime/audio.cpp:47` `kLeadBytes = 600 ms`（注释自述「代价是 SE 也有 ~600ms 延迟」）；
  `src/runtime/audio.cpp:50` `kThreadLeadBytes = 100 ms`（thread 模式）。
- `src/runtime/audio.cpp:1325-1352` 的 `pump()` 锁序论证**建立在**「绝不存在 feeder 在跑而 pump 走 D」之上
  ⇒ 那段的注释与前提要跟着改，⛔ 不能只改谓词。
- 结构前提在 `AudioEngine::setupFeederContext()`（`audio.cpp:1677`）：`Machine::layout().feeder_teb`（`src/runtime/image.h:108`）
  + `audio_stack_top_` ⇒ AOT 要另找这两样的来源。
- 回归口：`docs/03-emulator/defects.md` **BUG-22**（feeder 与主线程共用 TEB ⇒ `fs:[0]` 被改写 ⇒ Quit 的 C++ throw 派发到已复用 record）。

## 判据（收口时）

- AOT + `--audio sdl`（墙钟）⇒ 真 feeder：mode 行 `feeder=host-thread`、lead = 19200 B；`--audio off` 缺省输出**逐字节不变**。
- feeder 有**自己的 TEB**（≠ 主线程 TEB），BUG-22 那条路径不复发（一条能红的用例）。
- 读数：hover SE 的 `play` → 首个 PCM 交付/出设备的间隔（改前 ~600 ms 量级、改后 ~100 ms 量级；同机、同 plan、交错 ≥3 对）。
- 守卫：谓词（AOT 形状 ⇒ thread 模式）+ lead 取值 + TEB 不同；各配 `tools/mutate-check.mjs` 一条并实测红。

## 未做

- 真人听感确认（需要一台有音箱的机器）；本轮只做设备队列与时间戳读数。
- 与之相伴的"黑屏期间一部分音效提前出现"（SN0000→SC0000 过渡）只登记，等本单收口后再查。

## 追加裁决（用户 2026-11-17）

**暂停（控制层 `pause` / 配方驱动停住）不暂停音频宿主线程** —— 允许音频继续按旧状态 loop / 出声 / 错拍。
⛔ 不要给 mixer / feeder 加"跟随 guest 暂停"的钩子；⛔ 不要把"暂停期间还在响"记成缺陷。判据只在"不暂停时"取。
口径已落 `repo:docs/03-emulator/audio.md` §2。
