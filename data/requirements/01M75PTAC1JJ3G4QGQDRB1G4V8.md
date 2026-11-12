# 音频：Stop/重定位之后仍会跑一次旧位置的位置通知 fill（BGM 切条目时跳段/错位）

- id: REQ-01M75PTAC1JJ3G4QGQDRB1G4V8
- type: bug
- status: blocked
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- blocked_by: [REQ-01M75VZXV4SDMVVM1Z72G009XJ]
- verify: apps/emulator-lift/tests/audio.cpp#audio-device/stop-then-play-without-seek
- repro: apps/emulator-lift/tests/audio.cpp#audio-device/stop-drops-stale-fill
- severity: S1
- tags: [emulator-lift audio dsound feeder]

## 现象

ADV 推进 / 切 BGM / 播完 SE 时 **错位 / 错误重复**。具体 case：GAMESTART 点"下一步"播 SE，
**SE 播完后混着 SE 的尾声被重播 2–3 次**。用户机可稳定复现；本机也能复现（`graphs/se-eof.graph`）。

## ★ 已定因（2026-11-12，插桩实测；与最初的 D1 猜测不同）

**不是**"旧位置通知跨 Stop 投递"，而是 **feeder 在 shim 边界被令牌挡了 1.3–1.7 s**：

```
fills=32 notify=49 slot_reverify_fail=0 fill_max_ms=1743.79 fill_slow=4
slot=1  q3  guest=1342.25 shimtok=1340.84 shimblk=27  total=1578.03
slot=11 q3  guest=1716.85 shimtok=1532.98 shimblk=7   total=1743.79
稳定刀（对照）guest≈180   shimtok≈2
```

⇒ 单刀几乎 **全是 `shimtok`**（等 guest 令牌），不是解码。
机制：feeder 在 fill 里不持令牌执行 guest 指令，但**每个 shim 调用**都要取一次；主线程按
"每个 dispatcher 迭代取一次"持令牌（`machine.cpp:1067`），切场景时一持几十毫秒 ⇒ 27 次边界累计 1.34 s。
后果：一刀 > 环长的一半 ⇒ 引擎本该用**静音**替换的尾巴晚到 1–2 圈 ⇒ 尾声被重混 2–3 次。

## 已落的修复（三层补偿，仍然正确、但不是本现象的解）

1. `stop()`：冻结游标（`playedCursor()`，在 `playing=false` 之前）→ `++generation` → `dropPending(obj)` → `initial_pos = frozen`；
2. `setPosition()`：同样 `++generation` + `dropPending` + 重锚；
3. `filling_obj_` + 有界等（≤3 s），等待时放开 `mutex_` 与令牌，回来后重新 `find()` + 重校验 `obj_ea`；
4. `AudioNotifyEvent` 带 `generation`：`pump()` 与 `feederEvent()`（拿槽 CS 之后）各校验一次。

**代价**：真重叠时主线程最多阻塞一刀 fill（380–460 ms）。

## 与另一张单的关系

本缺陷的**根因**是令牌调度 ⇒ 由 **`REQ-01M75VZXV4SDMVVM1Z72G009XJ`**（尖峰）修复；
令牌模型本身在复盘（含 GetMessage 时代的遗留清理）。
⇒ 本单 **blocked_by** 那一张；它修好之后用同一张 `graphs/se-eof.graph` 复跑确认：
`fill_slow=0`、`fill_max_ms≈200 ms`、WAV 里不再出现 lag=2.000 s 的重复段。

## 残留（登记，不修）

- 3 s 超时那一次：re-anchor 与 fill 真并发且不回滚 ⇒ 三层设计的**唯一破口**。
- `addBuffer` / `removeBuffer` 不等在飞 fill ⇒ `REQ-01M75RD3WNA2GZQY32S34V00S7`。
- `0x4B748E` 调 `[stream+0x247C]+0x14` 的语义**未证**（与"reset"冲突）。

## 判据 / 读数

- 守卫：`audio-device/stop-then-play-without-seek`（对外）· `stop-drops-stale-fill`（内部机制）· `replay-on-playing`。
- ⛔ 第 2/3 层与 EOF 自停路径**都没有 CTest 覆盖**（`thread_mode_ = Sdl && realTime`，CTest 全是 Virtual）；
  运行时判据只有 `dumpSummary` 的 `fill_waits` / `fill_wait_timeouts` / `fill_overlapped` / `fill_slow` / `fill_max_ms`。
