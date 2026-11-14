# emulator-lift · SetCursorPos 的真实光标 opt-in（--cursor-warp）

- id: REQ-01M7BXM46NVYY3QHYW2ZKJZKZN
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/cursor_warp.cpp#cursor-warp/on-calls-once-with-client-coords
- done_reason: 落地并由两轮独立复审收口：--cursor-warp（缺省关）—— 关 / 无 guest 窗口 / 窗口隐藏 ⇒ 只改 guest 虚拟光标并返回 TRUE；开着且可见窗口 ⇒ 经 runtime/host_cursor 的系统接口真实挪宿主光标（Win32 ::SetCursorPos / macOS CGWarpMouseCursorPosition + CGAssociate，Linux 启动即 exit 2）、挪不动 ⇒ FALSE + warn。几何只有一份（runtime/cursor_map.h 的 letterboxFor(w,h,iw,ih)，present_sdl 的 mapMouse 与 warp 共用）；回灌抑制按**窗口像素**在 SDL motion 入口做（runtime/warp_echo.h，FIFO 8、±2px、每条一次；⛔ 不在按键/滚轮上 clear —— 那会让'warp 前排队的按键'抹掉期望、回灌被当真人移动）；第一次成功 warp 后一次 hostCursorQueryScreen 读回自检。守卫：cursor-warp（7 组 case + warp-echo 5 组 + 回灌两个反例）/ cli-cursor-warp（headless + flag ⇒ exit 2）/ 变异 1 条红。★ 真机验证（OBS-20261114-08）：加载存档界面那次调用 = SetCursorPos(640,325)（正是二次确认'是'的位置），开臂日志'虚拟光标 + 宿主光标（客户区 640,325）'且自检无 warn（系统光标真的到位），关臂同坐标只报虚拟，两臂实参逐字相同 ⇒ 开关不改变 guest 可见调用。已知边界：--present window --present-hidden 只 warn 不 exit 2（auto 回退在 init 之后）；macOS 分支与未聚焦/多显示器未实测。
- tags: [emulator-lift, user32, input, opt-in]

# emulator-lift · SetCursorPos 的真实光标 opt-in（`--cursor-warp`）

`SetCursorPos` 今天只有 headless 语义：改 guest 虚拟光标（`shims/user32_input.cpp` → `WindowManager::setCursorPos`），**从不碰宿主光标**。用户要求加一个**缺省关**的开关，开启时经**系统接口**真实移动宿主光标；验收现场就是引擎在「加载存档界面」的那一次调用（点槽位后自动把光标移到二次确认按钮）。

## 判据

1. **缺省关，且关着时 guest 可见行为不变**：`SetCursorPos(320,240)` ⇒ 返回 TRUE，`GetCursorPos` 读到 (320,240)，**不**产生额外 `WM_MOUSEMOVE`。
2. **开启后真的挪系统光标**：`--cursor-warp` + `--present window`（可见窗口）+ 支持平台 ⇒ 调用 `::SetCursorPos`（Win32）/ `CGWarpMouseCursorPosition`（macOS），入参 = guest 屏幕坐标 − guest 窗口原点 → letterbox → + 宿主窗口原点（**不乘** pixel density）。
3. **开着但做不到 ⇒ 响亮**：平台不支持 / 窗口隐藏 / 没有 guest 窗口 / 系统拒绝 ⇒ 不挪 + 一行 warn，且启动时打一行「本轮不生效」（⛔ 静默）。
4. **开关不改变 guest 状态**：同一脚本开/关两臂，`GetCursorPos` 与消息序列逐条相同（`--cursor-warp` 只加宿主副作用）。warp 引发的 OS 回灌必须在 `WindowManager` 里被**吞掉一次**，⛔ 不许让它反写虚拟光标（`mapMouse` 会 clamp 到 0..1279，越界值会被改写）。
5. **守卫能红**：`cursor-warp`（顶层纯档，假 sink，不建真窗口、不碰真光标）覆盖：关着不动 / 开着恰好一次且坐标正确 / 挪不动 ⇒ FALSE / 没有窗口不挪 / 不 clamp；CLI 闸门：`--cursor-warp` + `--present headless` ⇒ exit 2。
6. **变异**：`tools/mutate-check.mjs` 至少一条（「忘了减 guest 窗口原点」⇒ 守卫红）。

## 落点

- 纯函数：`src/runtime/cursor_map.h/.cpp`（letterbox 正/反两份，接 `window_w,window_h,image_w,image_h`；`mapMouse` 与它共用同一份）。
- 平台原语：`src/runtime/host_cursor.h/.cpp`（`hostCursorPlatform()` / `hostCursorWarpToScreen()` / `hostCursorQueryScreen()`）。
- 接缝：`PresentSink::hostCursorWarpEnabled/hostCursorWarp`（缺省 false），实现只在 `present_sdl`；⛔ 不把 SDL 类型带进 runtime。
- 开关：`main.cpp` 的 `--cursor-warp`（与 `--overlay` 同一套：窗口参数名单 + headless 拒绝 + `describe()` 追加 `cursor-warp=on|off`）。

## 契约

`docs/03-emulator/contracts-win32.md` 追加 L1-MSG-05：TRUE/FALSE 的三种组合（关着 = TRUE，因为它真的改了虚拟状态；开着挪不动 = FALSE）。
