# 🐞S3 陈旧关窗不可判：WM_CLOSE 的 SDL 事件时间戳在 push 时才填 ⇒ 暂停期间的关窗永远不被当 HostStop

- id: REQ-01M7JNVD59RD49RSKD9N2S9TXW
- type: bug
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- repro: apps/emulator-lift/src/runtime/present.h#classifyStaleEvent
- severity: S3
- tags: [emulator-lift, control-plane, window]

## 现象

暂停期间的**关窗**事件在 `resume` 之后仍按"投 `WM_CLOSE` 进 guest"处理，**不会**变成 `bye.reason:"window-closed"`。
文档（`docs/03-emulator/control-plane.md` §4「陈旧事件」）与 `present.h` 的陈旧事件注释把"关窗 / ESC / 输入"三者
写成同一件事（都靠陈旧戳挡），实际只有后两者成立。

## 根因（SDL 3.4.18 源码核对，2026-11-17 / win32）

- **输入 / ESC 成立**：Win32 后端用 `WIN_GetEventTimestamp()` 把时间戳**回溯到消息投递时刻**
  （`GetMessageTime()` 的 `message_tick` + 偏移校正）；`SDL_PushEvent` 只在戳为 0 时填 `SDL_GetTicksNS()`
  ⇒ 暂停期间排队的点击 / 拖动 / ESC 的戳 < 恢复时刻 ⇒ 被 `classifyStaleEvent` 判陈旧。
- **关窗不成立**：`WM_CLOSE` → `SDL_SendWindowEvent(..., SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0)`，
  该函数把 `event.common.timestamp = 0`，随后 `SDL_PushEvent` 填**当下**（= pump 时刻，必 > 恢复时刻）
  ⇒ 恒 `Keep` ⇒ 生产路径上**永远拿不到陈旧戳**。
- 为什么现有用例是绿的：`tests/present_window.cpp` 的 `control-resume-stale-events` 用 `SDL_PushEvent`
  **自己造了陈旧戳** ⇒ 它测"闸门逻辑对不对"，不测"SDL 会不会给出陈旧戳"。

## 判据（要能红；⛔ 不许为绿改弱）

1. 真窗口 + `--control-root`：暂停期间**用真鼠标关窗**（点 × 或 `Alt+F4`）⇒ `resume` 后必须是**宿主 stop**
   （`bye.reason:"window-closed"`），⛔ 不是"guest 收到 `WM_CLOSE`"。★ 今天这条**必然红**（戳恒新）—— 它就是本单的存在理由。
2. ⛔ 不许靠"用例自己 push 一个陈旧戳"过判据 —— 那层已被 `present-window` 覆盖，重复它等于没测。

## 可能的修法（未定，⛔ 不是本单判据）

- pause 时记 Win32 队列水位（`GetQueueStatus` 的变化），resume 后据此判"pump 之前就排着"；
- 或对 `SDL_EVENT_WINDOW_CLOSE_REQUESTED` 单独打标：`noteControlResume()` 之后、首个 pump 之前的关窗一律当陈旧；
- 或自建时间戳（绕开 SDL 的"0 戳填当前值"）。

## 出处

`REQ-01M7JFDZK1SZW72SBAW6BC3DRT` 判据 2 的"关窗"那一半已因本单移入 `## 未做`；
合并前复审（2026-11-17）逐条核对了 SDL 3.4.18 的 `SDL_windowevents.c` / `SDL_events.c`。
