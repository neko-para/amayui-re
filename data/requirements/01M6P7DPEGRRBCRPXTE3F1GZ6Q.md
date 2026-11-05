# emulator-lift · 窗口交互：SDL 事件 → guest 消息泵（GetMessageA / 键鼠）

- id: REQ-01M6P7DPEGRRBCRPXTE3F1GZ6Q
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift present sdl input]

## 背景

`REQ-01M6P6YSTMC3MCDTHS7RSQYRRE` 已交付可切换的 **headless/CPU（缺省）** 与 **SDL/GPU（显式）** 呈现；
但本阶段只做“像素送进窗口”，没有把 SDL 事件接进 guest 消息队列。

现有事实：`shimGetMessageA` 在队列为空时 `shimAbort`（`apps/emulator-lift/src/shims/user32_window.cpp`），
而引擎主循环是 `PeekMessageA(PM_NOREMOVE) → GetMessageA → TranslateMessage → DispatchMessageA`。
⇒ 窗口能看到第一帧，但 guest 一旦在空队列上等消息就会 abort；交互（键鼠 / 关窗确认）不可用。

## 目标

- `WindowManager` 增加**可选宿主事件源**（`std::function`）；未安装 ⇒ 保持今天的语义（含 abort）。
- 安装后 `GetMessageA`：先 pump SDL（`SDL_PollEvent`）→ 仍空则 `SDL_WaitEventTimeout(1..10ms)` +
  `pumpTimers`，直到有消息或窗口关闭。
- `SdlWindow` 把 SDL 事件翻译成 Win32 消息：`WM_CLOSE` / `WM_QUIT` / `WM_KEYDOWN/UP` /
  `WM_MOUSEMOVE` / 鼠标键 / 滚轮；坐标按虚拟 1280x720 换算（不把宿主 DPI/尺寸泄漏给 guest）。
- 关窗：先 `WM_CLOSE`（走 guest 自己的确认）；宽限 K 帧后仍不退出 ⇒ `presenter().stopRequested()`，
  `Machine::run` 以 `StopReason::Stopped` 收口（`detail="present: 窗口关闭"`）。

## 非目标

- 不做多窗口 / 全屏 / IME / 摇杆。
- 不改 guest 的固定虚拟屏（1280x720）与确定性 trace。

## 验收

- `user32-window` 或新增 pure 夹具：假事件源注入 `WM_KEYDOWN`/`WM_CLOSE`，断言 guest 消息队列顺序、
  `GetMessageA` 不再 abort、关窗走 `WM_CLOSE`。
- `present-window` 场景：真实窗口按 ESC / 关闭 ⇒ 进程 exit 0（`StopReason::Stopped`），不是 abort/2。
- headless（无事件源）行为逐字节不变；现有 CTest 全绿。

## 风险

- 输入翻译必须与 `user32_input` 的影子键盘状态同源，否则 `GetAsyncKeyState` 与 `WM_KEYDOWN` 打架。
- `GetMessageA` 的等待会让虚拟时钟/真实时钟切换更敏感；窗口模式已自动 real_time，需单独核对节奏。
