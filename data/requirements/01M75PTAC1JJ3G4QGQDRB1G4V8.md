# 音频：Stop/重定位之后仍会跑一次旧位置的位置通知 fill（BGM 切条目时跳段/错位）

- id: REQ-01M75PTAC1JJ3G4QGQDRB1G4V8
- type: bug
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/audio.cpp#audio-device/stop-then-play-without-seek
- repro: apps/emulator-lift/tests/audio.cpp#audio-device/stop-drops-stale-fill
- severity: S1
- tags: [emulator-lift audio dsound feeder]

## 现象（用户实测）

ADV 推进 / 切 BGM 时 **错位 / 错误重复**，尤其"播了新音效、音效结束"这类**条目变更**的时刻。

★ **2026-11-12 复测：仍未修好**（用户自建 `out-user`，真 SDL 设备）。原话：
"在音频边界依然会错误混合/重播大概 2 s 左右的片段 3~4 次"。
`dumpSummary`：`mode=sdl device=ok muted=0 fills=97 fill_bytes=7669200 mixed=8943360 played=8924160
notify=103 deferred=0 dropped=0 underruns=0 stale=0 slot_miss=0 play_on_playing=0
fill_waits=1 fill_wait_timeouts=0 fill_overlapped=5`

读数含义（判据在下面"判据/读数"一节）：
- `stale=0` ⇒ 代次校验（第 2 层）**一次都没触发**；`dropped=0` ⇒ 每次 `dropPending` 时队列都是空的
  （喂料跟得上，与 `underruns=0` 一致）。
- `fill_overlapped=5` 但 `fill_waits=1` ⇒ 有 5 刀 fill 结束时代次已变，其中**只有 1 次真的等过**。
  剩下 4 次只可能是"stop 时 `filling_obj_ != obj`"或"**fill 内的 EOF 自停**（`0x4B6C58`：feeder 自己调 `stop()`，
  按 `isFeederThread()` 跳过等待）"。
- **4 次 EOF 自停 ↔ 听感 3–4 次重播** ⇒ 首要嫌疑是 **EOF 自停路径**，不是 stop/重定位那条（那条这次只命中 1 次）。
  ⚠ 这是相关性，**未证**；见"待查"。

## 机制（已定因的那一半）

引擎的喂料线程（`sub_4B5AA0` → `sub_4B6C30`）**从不排空位置通知**，它依赖一条隐含不变量：
"越过通知点之后 µs/ms 级就把那一刀 fill 做完"。真机 fill 是 µs–ms 级；本实现一刀 fill 是 **380–460 ms**
（占空 76–92%）⇒ 在 Stop / 重定位 / 预填 / Play 那段（**不在槽 CS 里**）里，那条已越过的通知必然还在队列里，
于是预填之后又跑一刀**旧位置**的 fill ⇒ 跳段 / 错位。

`jns` 的语义（`0x4B73F5`/`0x4B73FD`、`0x4B6B6D`/`0x4B6B75`）：**这次 `--` 之后 ≥0 就跳过 EnterCriticalSection**。
⇒ 经 `sub_4B73E0` 起播被串行化；**直接**调 `sub_4B6B60` 的那几处（`0x418532` P2 · `0x4B6118` dsStop ·
`0x4B7185` Release · `0x4B6C58` fill 的 EOF 自停）会与 fill 并发。

## 已落的修复（三层补偿）

1. `stop()`：先冻结游标（`playedCursor()`，**必须**在 `playing=false` 之前）→ `++generation` → `dropPending(obj)`
   → `initial_pos = frozen`。
2. `setPosition()`：同样 `++generation` + `dropPending` + 重锚。
3. `filling_obj_` + **有界等**（≤3 s），等待时放开 `mutex_` 与令牌，回来后重新 `find()` + 重校验 `obj_ea`。
4. `AudioNotifyEvent` 带 `generation`：`pump()` 与 `feederEvent()`（拿到槽 CS 之后）各校验一次。

**代价**：真重叠时主线程最多阻塞一刀 fill（380–460 ms）。

## 待查（本轮重开的原因）

- **EOF 自停**（`0x4B6C58`）在 `stop()` 里跳过了等待 —— 那是对的（自等会死锁），但它同时把
  `initial_pos` 改成了"EOF 时刻的游标"。循环重启若依赖 `initial_pos` / `[+0x245C]` 的既有状态，
  就会从**错误位置**继续 ⇒ 重播一段。**待证**。
- 是否与 `playedCursor()` 在 feeder 线程上取值（`played_` 由主线程推进）有关。
- 需要用户侧的一份 `LIFT_AUDIO_TRACE=1` 日志（含 `stop ... cursor= gen=` 与每刀 fill 的 `obj/quarter`），
  对齐听感时刻，才能把"2 s 片段"钉到具体一刀。

## 残留（登记，不修）

- 3 s 超时那一次：re-anchor 与 fill 真并发且不回滚 ⇒ 三层设计的**唯一破口**。
- `addBuffer` / `removeBuffer` 不等在飞 fill ⇒ `REQ-01M75RD3WNA2GZQY32S34V00S7`。
- `0x4B748E` 调 `[stream+0x247C]+0x14` 的语义**未证**（与"reset"冲突）⇒ 与 `0x418520` 的残留量级一起标未证。

## 判据 / 读数

- `apps/emulator-lift/tests/audio.cpp` 的 `audio-device/stop-then-play-without-seek`（对外）·
  `audio-device/stop-drops-stale-fill`（内部机制）· `audio-device/replay-on-playing`。
- ⛔ **诚实说明**：第 2/3 层与 EOF 自停路径**都没有 CTest 覆盖**（`thread_mode_ = Sdl && realTime`，CTest 全是 Virtual）。
  运行时判据只有 `dumpSummary` 的那几个计数 —— 而这次的 `mode=sdl` 读数正好把嫌疑指向 EOF 自停。
