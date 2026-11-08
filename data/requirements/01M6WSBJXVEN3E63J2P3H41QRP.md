# emulator-lift · 输入注入基建：--input-script 指令式时间线（唯一输入状态源；不需焦点 / 不读 OS 键鼠）+ --present-screenshot-frame 定帧截图

- id: REQ-01M6WSBJXVEN3E63J2P3H41QRP
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/input_injection.cpp#input-injection
- done_reason: 注入与 SDL 真实事件走同一段应用代码（runtime/input.h），一起改 WindowManager 的影子键电平 / 按钮位图 / 虚拟光标再 post 同一组 WM_*，所以 guest 的三类读点（GetAsyncKeyState 采样 / GetCursorPos 光标 / PeekMessageA+DispatchMessageA 消息）读到同一份状态；只发一条消息会留下第二份状态。优先级：每个 VK 有所有者，脚本按下即归脚本（期间 OS 按键与焦点丢失清键都不生效），脚本抬起即交还；光标首次 mouse_move 起归脚本，期间 OS 鼠标不生效；收口时补发 WM_* 抬起再交还，不留卡住的键。不需要焦点的判据：守卫进程从不 SDL_Init(VIDEO)、没有窗口与真实光标，guest 仍看到注入；同一条通道的另一端仍是真实 SDL 事件（窗口模式覆盖），不是被替换掉的旧路。
- tags: [emulator-lift input present guard]

## 一句话

`--input-script <file>` 把“在某个时刻点一下 OPTION / 按一下键”从**手工 OS 动作**（`SetForegroundWindow` → 挪真实光标 → `SendInput` → 等几百毫秒）变成**一次可复现的运行**；`--present-screenshot-frame N` 让那次运行只在第 N 个 Present 落一张图。判据一句话：**注入不依赖任何只有 OS 能提供的条件**。

语法与用法（唯一真源）：`repo:apps/emulator-lift/README.md` §4.2c。

## 交付

- 指令式时间线：`at frame N` / `at guest_ms T`（含 `after` / `wait`，动作前必须先给时间）、`mouse_move` / `mouse_down` / `mouse_up` / `click` / `key_down` / `key_up` / `quit`；解析失败响亮（带行号，exit 2）。
- 定帧截图：`--present-screenshot-frame N` 只回读第 N 个 Present（0/缺省 = 每帧都写），配合 `--max-frames N` 就是“定帧截图”。
- 守卫 `input-injection`：顶层 CTest（pure，不建窗口、不要游戏），逐条断言下面三节的判据。

## 为什么是「状态源」而不是「发一条消息」

- guest 的输入读点有三类：`GetAsyncKeyState`（**采样**）、`GetCursorPos`（**光标**）、`PeekMessageA` / `GetMessageA` → `DispatchMessageA` → WndProc（**消息**）。
- 只 post 一条 `WM_LBUTTONDOWN` 只覆盖第三类：前两类仍会去读 OS ⇒ “脚本点了、引擎采样到没按”的两份状态。所以 SDL 真实事件与注入**汇进同一段应用代码**（`repo:apps/emulator-lift/src/runtime/input.h`），一起改 `WindowManager` 的同一份影子键电平 / 按钮位图 / 虚拟光标，再 post 同一组 `WM_*`。
- 反证写进了守卫：把注入改走 SDL 事件泵、或把 `GetAsyncKeyState` 改回读 OS，它当场红。

## 两条来源的优先级（显式规则）

- 每个 VK 有**所有者**（`Os` / `Script`）。脚本按下某键 ⇒ 该键归脚本：期间 OS 的按下 / 抬起 / **焦点丢失清键**一律不生效（注入不因失焦而丢）。
- 脚本 `key_up` / `mouse_up` ⇒ 该键**交还 OS**（状态置抬起、所有者清空）。
- 光标：第一次 `mouse_move` 起归脚本；期间 OS 的鼠标移动与按键都不生效（它们的坐标来自 OS 光标，半截输入比拒绝更糟）。
- 收口（动作跑完 / `quit`）：仍按着的键 / 按钮按**同一组 `WM_*` 抬起**并交还所有权 ⇒ 真人接着玩不会撞上“卡住的键”。
- 规则只有一份：`repo:apps/emulator-lift/src/runtime/host_window.h` 的 `InputSource` 注释。

## 「不需要焦点」的判据

- 守卫进程**从不调用 SDL 初始化**（自己断言 `SDL_WasInit(VIDEO)==0`）：没有窗口、没有焦点、没人挪过真实光标，guest 仍通过它**自己的读点**看到注入的键电平 / 光标坐标 / 消息，且 `ScreenToClient` 能回到注入坐标（复用同一套 client↔guest 映射，注入侧没有第二套）。
- **真实 SDL 路径仍工作**：这是同一条通道的另一端，不是被替换掉的旧路 —— 窗口模式的 SDL 事件解释器仍把键鼠送进同一段应用代码。
- 时间线在**块边界**推进；guest 阻塞在 `GetMessageA` 时由窗口模式的宿主事件泵推进，`quit` 同时投 `WM_QUIT` 把阻塞中的 guest 唤醒。

## 落点

`repo:apps/emulator-lift/src/runtime/input.h` · `repo:apps/emulator-lift/src/runtime/input.cpp` · `repo:apps/emulator-lift/src/runtime/input_script.h` · `repo:apps/emulator-lift/src/runtime/input_script.cpp` · `repo:apps/emulator-lift/src/runtime/host_window.cpp` · `repo:apps/emulator-lift/tests/input_injection.cpp` · `repo:apps/emulator-lift/CMakeLists.txt`（顶层 `add_test(NAME input-injection)`）

## 边界 / 后续（都是**兄弟**单，因为本节点已收口）

- 格式与 `emulator-cxx` / `emulator-new` 的两套语法**暂不统一**（冻结件）→ `REQ-01M6WS9975PXRSXHEHCMZPWXB9`
- `at guest_ms` 在窗口模式不可复现；`--anchor-script` 要先有“引擎脚本可观察事件”接口 → `REQ-01M6WS9G1EQRZ0EYM21V0DPZVY`
- `tools/mutate-check.mjs` 还缺 `input-injection` 条目（本轮只登记）→ `REQ-01M6WSAGESXYYZ6QR919J2NFWC`
- 两张 BMP 的像素比对没有落点 → `REQ-01M6WSANRPND80XW52QY4Y5WA8`
