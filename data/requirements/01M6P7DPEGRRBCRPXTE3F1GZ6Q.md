# emulator-lift · 窗口交互：SDL 事件 → guest 消息泵（GetMessageA / 键鼠）

- id: REQ-01M6P7DPEGRRBCRPXTE3F1GZ6Q
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/window_events.cpp#window-events
- tags: [emulator-lift present sdl input]

## 背景
`REQ-01M6P6YSTMC3MCDTHS7RSQYRRE` 交付了 headless/CPU 与 SDL/GPU 呈现，但只把像素送进窗口；
`GetMessageA` 在空队列时仍 `shimAbort`，键鼠/关窗没有进 guest 消息队列。本单把 SDL 事件接进
`WindowManager` 的同一个消息队列。

## 实现
- `WindowManager` 新增可选 `HostEventPump`（`std::function<void(X86&)>`）：
  - 未安装 ⇒ `get()` 保持旧语义（pump 计时器后空队列返回 false，`GetMessageA` 仍 abort）。
  - 安装 ⇒ `get()` 在队列空时反复调用事件源，阻塞到有消息或窗口关闭。
  - 新增 `primaryWindow()`（关窗消息的 hwnd）与 256 项影子键状态（`setKeyDown/keyDown/clearKeys`）。
- `SdlWindow` 新增 `attachWindowManager()` / `pumpHostEvents(wait_ms)`：
  - `SDL_PollEvent` → `SDL_WaitEventTimeout(5ms)` → 再 poll；`PresentSink` 的 0ms 轮询语义不变。
  - 键盘：`WM_KEYDOWN/UP`（ESC 同时触发关窗）；`GetAsyncKeyState` 读同一份影子状态。
  - 鼠标：`WM_MOUSEMOVE` / 左中右键 / `WM_MOUSEWHEEL`；坐标按虚拟 1280x720 + letterbox 反算。
  - 关窗：`SDL_EVENT_WINDOW_CLOSE_REQUESTED` 先发 `WM_CLOSE`；宽限 **3 个 Present 帧 / 500ms**，
    到点仍不退出 ⇒ `stopRequested()` + `postQuit(0)`；`Machine::run` 以
    `StopReason::Stopped` / `detail="present: 窗口关闭"` 收口（exit 0）。
  - `SDL_EVENT_QUIT` 走立即 `WM_QUIT`；`WINDOW_FOCUS_LOST` 清键鼠状态。
- `main.cpp`：窗口 sink 装配后把 `SdlWindow*` 挂到 `machine.cpu().windows` 并安装事件源；
  运行结束清掉事件源（避免悬空）。

## 验收
- `window-events`（pure，28 checks）：假事件源注入 `WM_KEYDOWN`/`WM_MOUSEMOVE`/`WM_CLOSE`，
  guest 的 `GetMessageA` 依次收到；第 4 次 `postQuit` 返回 0；`GetAsyncKeyState('A')=0x8000`；
  未装事件源的 `WindowManager::get` 仍返回 false。
- `present-window`（74 checks）：dummy SDL 窗口上注入键/鼠标/滚轮，断言消息与坐标；
  注入 `WINDOW_CLOSE_REQUESTED` ⇒ 先 `WM_CLOSE`，3 帧后 `stopRequested`；
  另有一条 `Machine::run` + SDL sink 的关窗用例，断言 `StopReason::Stopped` / detail / `r.ok()`。
- headless：没有事件源时事件泵路径不执行，`GetMessageA` 行为与旧版一致。

## 已知限制
- `PeekMessageA` 仍只 pump 计时器；阻塞等待由 `GetMessageA` 承担（引擎主循环在 Peek 空后调用它）。
- `TranslateMessage` 仍未生成 `WM_CHAR`（本单只要求键消息；文本输入另开）。
- 不做 IME / 摇杆 / 多窗口；关窗宽限的 500ms 是墙钟兜底，窗口模式本来就是 `--real-time`。

## 复现 / 验证
`ctest --test-dir apps/emulator-lift/out -R 'window-events|present-window' --output-on-failure`
