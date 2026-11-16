# 🐞S2 控制层 resume 的陈旧事件闸门从未 arm：AotHost::controlOnResume() 无调用者（文档已写成可用）

- id: REQ-01M7JFDZK1SZW72SBAW6BC3DRT
- type: bug
- status: done
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- verify: apps/emulator-lift/tests/control_endpoint.cpp#control-resume-arms-host-hook
- repro: apps/emulator-lift/src/aot/aot_host.cpp#controlOnResume
- severity: S2
- done_reason: 接线已接上并走真 serve() 判据（506 checks；拆掉接线 5 failures）；未做=陈旧关窗（SDL 在 push 时填戳），已拆残余单 REQ-01M7JNVD59RD49RSKD9N2S9TXW
- tags: [emulator-lift, control-plane, window]

## 现象

控制层 `resume` 的**陈旧事件闸门从未被 arm**：`AotHost::controlOnResume()`（`src/aot/aot_host.cpp:1052`）
是唯一该把"恢复时刻"传给窗口 sink 的地方，但它**没有任何调用者**（全仓 3 处：声明 `control_snapshot.h:96`、
覆写声明 `aot_host.h:293`、实现 `aot_host.cpp:1052`）。

⇒ `SdlWindow::noteControlResume()`（`present_sdl.cpp:1110`，唯一写 `stale_input_before_ns` 的点）永不执行
⇒ `stale_input_before_ns` 恒 0 ⇒ `classifyStaleEvent()`（`present.h:148-162`）因 `resume_ns == 0` 一律 `Keep`。
文档 `control-plane.md` 曾把这一半写成"已可用"。

## 根因

整条链只有中间一段是死的：`ControlHostSide::controlOnResume()` 默认 no-op、`AotHost` 实现了它
（→ `presenter_.noteControlResume()` → `sink_->noteControlResume()` → 记时刻），
但 `Control::serve()` 的 Resume 分支一次都没调它。

## 收口（2026-11-17 / win32）

- **一行接线**：`control_ws.cpp:1107-1114` —— Resume 分支在 `paused=false` / `++state_seq` 之后、
  `if (for_frames || for_ms)` **早退之前**调 `ctx.host->controlOnResume()`（`host != nullptr`）
  ⇒ 普通 `resume` 与 `resume --for-ms/-frames` 两条出口都 arm。
- **arm 语义**：只在 paused→running 转移点 arm。`pause`/`sample`/`screenshot`/`click` 不离开暂停态 ⇒ 不 arm；
  `requestQuit`（idle/orphan）虽清 `paused` 但同时置 `stop_requested` ⇒ 无"首轮"⇒ 不 arm。
  读数（**累计**计数）：`pause`→0 · `resume`→1 · 运行态 `resume`（`not_paused`）→1 · `sample`→1 ·
  `resume --for-ms 30`→2 · 再 `resume`→3。
- **判据走真 `serve()`**：`control_endpoint.cpp#control-resume-arms-host-hook`，`506 checks`；
  拆掉接线 ⇒ `5 failures`（全在该用例内，`ctest -R control` ⇒ `1 failed out of 5`）。

## 判据（收口后的实际面）

1. 经真 `Control::serve()` 的 pause→resume ⇒ `controlOnResume()` 被调 **1 次**；其余 op 不 arm（见上表）。**证的是"接线存在"**。
2. `resume --for-ms` 的重停路径不重复 arm（累计计数，不是"N 次 resume = N 次覆盖"）。
3. ★ **"早于恢复时刻的 OS 输入被丢弃"不在本单判据里**：那一条由 `present-window` 的
   `control-resume-stale-events`（**自己造**陈旧戳）证明。两层缺一不可 —— 这正是本缺陷的病史：
   机制层绿、接线层死。

## 未做（⛔ 别当成已修）

- **陈旧关窗不可判**：`WM_CLOSE` → `SDL_SendWindowEvent(..., CLOSE_REQUESTED, 0, 0)` 把
  `event.common.timestamp` 置 0，随后 `SDL_PushEvent` 填**当下**（pump 时刻，必晚于恢复时刻）⇒ 恒 `Keep`。
  ⇒ 暂停期间的关窗**仍是"投 `WM_CLOSE` 进 guest"**，不会变成 `bye.reason:"window-closed"`。
  ESC / 点击 / 拖动**成立**（Win32 后端用 `GetMessageTime()` 回溯投递时刻）。
  残余单：`REQ-01M7JNVD59RD49RSKD9N2S9TXW`（🐞S3）。
- `AotHost → presenter_ → sink_` 这层 1 行转发未单独加判据（`present-window` 已直接测 sink 层）。

## 边界（复核过，未变）

- 配方不受影响：`scripts/recipes/*.plan` 里 `resume` 命中 **0 条**；`--input-plan` 无 `--control-root`
  ⇒ `Control` 不存在 ⇒ 该 hook 从不被调；headless 档 `noteControlResume()` 是 no-op。
- 行为只在 `--control-root` + window 档变：resume 后，暂停期间已进 SDL 队列且戳 < 恢复时刻的输入从
  "当刚发生灌进来"变成丢弃；暂停期间的 ESC 变成宿主 stop。⛔ 不动 `classifyStaleEvent()` 的三类动作。
