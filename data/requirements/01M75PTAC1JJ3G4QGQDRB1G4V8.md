# 音频：Stop/重定位之后仍会跑一次旧位置的位置通知 fill（BGM 切条目时跳段/错位）

- id: REQ-01M75PTAC1JJ3G4QGQDRB1G4V8
- type: bug
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/tests/audio.cpp#audio-device/stop-drops-stale-fill
- severity: S1
- tags: [emulator-lift audio dsound feeder]

# 音频：Stop / 重定位之后仍会跑一次"旧位置"的位置通知 fill（BGM 切条目时跳段/错位）

- 现象（用户实测口径）：实际游玩时音频**错位 / 错误重复**，尤其在一条 BGM 流"条目变更"的过程中
  （开始播放新音效、音效结束）。
- 结论（裁决）：**(b) 引擎自身的潜伏竞态，被"正确但慢"的实现放大**。不是 Win32 接口实现错。

## 机制（每条都有 `.lst` 锚）

引擎的喂料线程 `sub_4B5AA0` 等 61 个 auto-reset 位置通知事件；每越过一个通知点就
`sub_4B6C30(stream, quarter)` 解码 0.5 s 写进环的"刚播完那 1/4 圈"。于是：

- **少一刀 fill** ⇒ 那个 region 保留上一圈内容 ⇒ 听到 0.5 s **重复**；
- **多一刀 fill** ⇒ 解码器白跳 0.5 s 且写错 region ⇒ **跳段/错位**。

两条重播路径都以 `sub_4B6B60`（Stop）开头：

- **P1** `sub_4B73E0`：`Stop`(`0x4B742F`) → `SetCurrentPosition(0)`(`0x4B7441`) → 解码器 reset(`0x4B748E`)
  → 预填 q=1..3(`0x4B74B7`) → `Play`(`0x4B7575`)；
- **P2** `sub_418520`：`dsbStop`(`0x418532`) → `n × sub_4B6C30(stream,1)`(`0x418540`) →
  `[+0x245C]=1`(`0x41855F`) → `dsbPlay`（`0x4B7429` 因此**跳过** restart 块 ⇒ 不 seek、**不 reset**）。

引擎**从不排空**通知：`sub_4B73E0` / `sub_4B6B60` 各只有一次 `InterlockedDecrement`（无自旋），
整段音频唯一的等待是关机路径的 `0x4B6522`。Win32 也不 flush（`SetNotificationPositions` 只在游标
**reaches** offset 时 signal；与 Stop 挂钩的只有 `DSBPN_OFFSETSTOP` 哨兵，引擎的偏移是 `0/L/4/L/2/3L/4`，
**没用哨兵**）。

⇒ 引擎依赖一条隐含不变量：**喂料线程永远闲着，越过通知点后 µs/ms 级就把那一刀 fill 做完**。
真机的 fill 是 µs–ms 级；emulator 的一刀 fill 是 **380–460 ms**（RelWithDebInfo、48k stereo Ogg），
占空比 76–92% ⇒ 在 Stop/重定位/预填/Play 这段（**不在槽 CS 里**）里，那条已越过的通知必然还在队列里，
于是预填之后又跑一刀旧位置的 fill ⇒ 就是用户听到的跳段/错位。

## 修复（emulator 侧补偿，恢复真机因果次序）

- `stop()`：先冻结游标（`playedCursor()`，必须在 `playing=false` 之前）→ `++Buffer::generation` →
  `dropPending(obj)` → `initial_pos = frozen`。**无等待**（⛔ 不能等 feeder：`0x4B73FD` 的 `jns`
  让空闲路径整个 `sub_4B73E0` 持着槽 CS，主线程等 feeder 必死锁）。
- `setPosition()`：同样 `++generation` + `dropPending`。
- `AudioNotifyEvent` 带 `generation`；`pump()`（D 模式）与 `feederEvent()`（C 模式，**拿到槽 CS 之后**）
  各校验一次，不符 ⇒ 不跑 fill + `stats_.stale++`。这条关掉"已 pop、还没拿到 CS"的窗口。
- 同批顺手修的两处（同一文件、无独立守卫）：
  - `pending_` 以前在 `mutex_` 下被裸 `erase`（feeder 在 `pending_mutex_` 下 pop）⇒ 宿主 UB，统一走 `dropPending()`；
  - `Play` 对**已在播**缓冲以前会重锚 + 重发 offset 0（真 DS：DS_OK、继续播、不重置游标）⇒ 改 no-op，
    单独由 `audio-device/replay-on-playing` 钉住。

## 残留偏差（登记，不修）

P2（`sub_418520` 的 seek 路径）**不 reset 解码器**：被作废的那一刀会让解码器少推进一个 quarter
⇒ BGM 起始内容比请求位置**早 ≤0.5 s**。只出现在 seek 路径；不污染环（P2 随后自己预填 q=1..3）；
P1 有 reset + 预填，整环被重写 ⇒ 无残留。

## 判据

`apps/emulator-lift/tests/audio.cpp` 的 `audio-device/stop-drops-stale-fill`（修前红：Stop 之后
pending 里还留着 q1）+ `audio-device/replay-on-playing`（修前红：第二次 Play 会入队 q0 并把下一次越点
从 q2 变成 q1）。删掉 `stop()` 的 `dropPending` 或删掉 `play()` 的 no-op ⇒ 对应用例当场红。
