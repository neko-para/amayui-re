# 🐞S2 控制层 resume 的陈旧事件闸门从未 arm：AotHost::controlOnResume() 无调用者（文档已写成可用）

- id: REQ-01M7JFDZK1SZW72SBAW6BC3DRT
- type: bug
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- repro: apps/emulator-lift/src/aot/aot_host.cpp#controlOnResume
- severity: S2
- tags: [emulator-lift, control-plane, window]

## 现象

控制层 `resume` 的**陈旧事件闸门从未被 arm**：`AotHost::controlOnResume()`（`src/aot/aot_host.cpp:1052`）
是唯一该把"恢复时刻"传给窗口 sink 的地方，但它**没有任何调用者**（全仓 3 处：声明 `control_snapshot.h:96`、
覆写声明 `aot_host.h:293`、实现 `aot_host.cpp:1052`）。

⇒ `SdlWindow::noteControlResume()`（`present_sdl.cpp:1110`，唯一写 `stale_input_before_ns` 的点）永不执行
⇒ `stale_input_before_ns` 恒 0 ⇒ `classifyStaleEvent()`（`present.h:148-162`）因 `resume_ns == 0` 一律 `Keep`。

**后果（用户可见）**：`pause` 是"完全停"（主线程睡在汇点、`pumpEvents` 一次不跑）⇒ 暂停期间点/拖窗口、
按 ESC、关窗的事件都堆在 SDL 队列里；`resume` 首轮把它们当"刚发生"处理 —— 正好是 `present.h:118-133` 那段
注释声称已解决的三件事（关窗把这一轮结束掉 / ESC 同 / 输入洪峰压进 guest 队列）。

**文档已经把它写成可用**：`docs/03-emulator/control-plane.md:84`「陈旧事件 | resume 时丢弃
`event.timestamp` 早于恢复时刻的 OS 输入；暂停态的关窗/ESC 当宿主 stop」⇒ 这是**尚未落地的口径**。

## 为什么现有用例是绿的（关键）

`tests/present_window.cpp:125` 的 `control-resume-stale-events`（`present-window`，`CMakeLists.txt:1075` 顶层 ⇒ pure 档）
在 `:137` / `:152` **直接**调 `win.noteControlResume()` ⇒ 绕过 `Control::serve()` 的 Resume 分支
⇒ 它测的是"闸门逻辑对不对"，**不是"接线存不存在"**。

## 修法（已定，⛔ 不要重新裁决）

1. `Control::serve()` 的 Resume 分支（`control.h` 的 `Context::host` 已有 `controlOnResume()` 虚函数）
   在放行后调 `ctx.host->controlOnResume()`（`host != nullptr` 时）。落点 `src/runtime/control_ws.cpp`。
2. 判据改成**走真接线**：新用例（或改造现有那条）必须经 `Control::serve()` 完成一次 pause→resume，
   断言 `stale_input_before_ns != 0` 且"早于恢复时刻的 OS 输入被丢弃"。⛔ 不许再直接调 `noteControlResume()`。

## 判据（每条能红）

1. 经 `Control::serve()` 的 pause→resume ⇒ 窗口 sink 的"恢复时刻"非 0，且一条**时间戳早于恢复时刻**的
   `SDL_EVENT_MOUSE_BUTTON_DOWN` 被丢弃（不进 guest 队列）；同一用例在修前必红（今天的接线不存在）。
2. 关窗/ESC 的陈旧事件仍按 `HostStop` 处理（`bye.reason:"window-closed"`），⛔ 不倒退成 `Drop`。
3. `resume --for-ms` 的**重停**路径不重复 arm（arm 一次/一次 resume），⛔ 不许把"N 次 resume"变成"N 次覆盖"。

## 边界

- 只改接线与判据；⛔ 不动 `classifyStaleEvent()` 的三类动作（那是已收口的合同）。
- headless（无窗口 sink）⇒ `noteControlResume()` 是 no-op，不产生读数；判据只在 window 档成立。
