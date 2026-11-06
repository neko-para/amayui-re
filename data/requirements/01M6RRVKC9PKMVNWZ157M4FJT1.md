# 🐞S2 emulator-lift · TITLE hover 不亮：鼠标移动不更新 GetCursorPos 虚拟光标

- id: REQ-01M6RRVKC9PKMVNWZ157M4FJT1
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/present_window.cpp#present-window
- repro: apps/emulator-lift/tests/present_window.cpp#present-window
- severity: S2
- tags: [emulator-lift input hover title]

## 现象

`--skip-logo --present window --raster gpu --native-overrides all` 进 TITLE 后，把鼠标移到
“Game Start”上不出现 hover 高亮。`LIFT_TRACE_INPUT=1` 证明 SDL motion 已到达：
`[input] motion sdl=(1165.0,365.0) -> guest=(1165,365)`，并且 `WM_MOUSEMOVE` 已 post 进 guest 队列。
但 TITLE 的 `0x12e` 命中测试仍 miss。

## 根因

`present_sdl.cpp` 的 `SDL_EVENT_MOUSE_MOTION` 只 `postMessage(WM_MOUSEMOVE)`，**没有**更新
`WindowManager` 的虚拟光标。`GetCursorPos`（`0x109` 的底层）返回初值 `(0,0)` 或上一次
`SetCursorPos` 的值；TITLE 的回调按这个位置做命中查找，所以永远在屏幕左上角，按钮不亮。
⇒ 不是“没接鼠标”，也不是消息泵丢了消息；是**光标位置状态没接上**。

## 修复

- `WindowManager` 新增 `setCursorClientPos(hwnd,x,y)`：把 SDL 的**客户区**坐标加上窗口原点，
  落成 Win32 的**屏幕**坐标（`GetCursorPos` / `ScreenToClient` 的口径）。
- `present_sdl.cpp` 在鼠标移动 / 按键 / 滚轮三条路径都更新；headless 没有事件源时初值不变。
- 没有建 OS 光标：只维护 guest 可见的虚拟位置。

## 验证

- CTest `present-window` 新增：`SDL_PushEvent(SDL_EVENT_MOUSE_MOTION 1165,365)` →
  `getCursorPos == (w->x+1165, w->y+365)`，且队列里有 `WM_MOUSEMOVE`、lParam 坐标正确。
- 真实窗口：鼠标移到 Game Start 后，前后两帧差异 bbox = `(1103,294)-(1258,450)`，
  19,533 个像素变化，位置正好是 Game Start 按钮；按钮高亮。
