# M2 · boot 推进：user32 窗口/输入/光标族（headless 语义）

- id: REQ-01M6KKG9WNSEEF658XQWJX25Q6
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/user32_window.cpp#cursor
- tags: [emulator-lift, m2]

## 一句话
把 boot 从 `user32!SetCursor` 的缺口推过去（**headless 真语义**），并把 user32 **窗口 / 输入 / 光标 / 监控器 / 菜单族**在导入表里剩下的缺口一次补齐；boot 现在停在 `shlwapi!PathRemoveFileSpecA`（**不属本切片**，条件 B）。未进 DirectDraw、未到第一帧。

## 卡点表（每次 boot 撞到的**第一条**缺口）
| # | 缺口（module!name） | 返回地址 | 处理 | 语义依据 |
|---|---|---|---|---|
| 1 | `user32!SetCursor`（派生 stub 0x7FFE03CC，`argc=0`） | `0x004B8670` | **实现**：记录并返回**旧** HCURSOR；登记 `argc=1`（stdcall 弹参） | Win32 `SetCursor`；`.lst` `0x004B8692 call SetCursor`（`sub_4B8680` 应用 `[esi+408h]`，**不读**返回值——但仍按真机语义返回旧值） |
| 2 | `shlwapi!PathRemoveFileSpecA`（stub 0x7FFE04D0） | `0x00490382` | **停**（条件 B：不是 user32 窗口/输入族） | `.lst` `0x0049037D`：`CreateFileA` 失败后"去掉文件名 → 补 `\` → 建目录"的文件/路径工具 ⇒ S6 文件路径族邻域（shlwapi 归属待裁决） |

★ 第 1 条之前 boot 还调过 `GetSystemMetrics(23)`（旧日志是"未建模 ⇒ 0"）⇒ 补成 `SM_SWAPBUTTON = 0`（Win32 真值：没交换左右键）。
★ 派生缺口的 `argc=0` 是 `addUnimplemented` 的占位；**实现必须按真实签名登记 argc**，否则 stdcall 少弹 4 字节。

## 本切片做了什么（每条都有夹具）
**headless 真语义**（状态真记在 `WindowManager`，返回值是 Win32 真值）：
- `SetCursor` / `GetCursorPos` / `SetCursorPos` / `ShowCursor`：虚拟光标位置（初值 **(0,0)**）、返回旧句柄、**显示计数**（`>=0` 才显示，返回**新**计数）
- `MonitorFromWindow` / `GetMonitorInfoA`：单显示器句柄 `0x00030001`；`rcMonitor = rcWork = (0,0,1280,720)`、`MONITORINFOF_PRIMARY`；`MONITOR_DEFAULTTONULL` + 无效窗口 ⇒ NULL
- `SystemParametersInfoA`：只认引擎 `.lst` 里真出现的 **3 条** —— `0x1014 SPI_GETSELECTIONFADE`（写 FALSE）、`0x1015 SPI_SETSELECTIONFADE`（TRUE + 日志）、`0x005C SPI_GETWINDOWSEXTENSION`（NT 口径 FALSE）；其余 FALSE + 日志
- `MsgWaitForMultipleObjects`：**消息队列语义**（按 QS 类匹配、**不取走**；`WM_QUIT` 也算一条）；`0ms` ⇒ `WAIT_TIMEOUT`；有限超时 ⇒ 推进虚拟时钟后 `WAIT_TIMEOUT`；`INFINITE` / `nCount>0` 且需要等对象 ⇒ **硬错误**（SCOPE §1「等待（未 signaled）」）

**确定性伪句柄 + 日志**：`LoadCursorFromFileA`（⛔ 不读 `.cur`，照 P0 `LoadCursorA` 的口径）
**SCOPE §1 永久 no-op（`addStub` 登记）**：`GetMenu`（窗口状态真值，没有菜单 = 0）/ `GetSubMenu`（NULL）/ `GetSystemMenu`（非空假句柄）/ `RemoveMenu` / `DestroyMenu` / `DrawMenuBar`（TRUE）
**其它 no-op**：`UnhookWindowsHookEx`（本线没有 `SetWindowsHookEx` 导入 ⇒ 没有可解的钩子）

落点：`repo:apps/emulator-lift/src/shims/user32_input.cpp`（新文件）· `repo:apps/emulator-lift/src/shims/user32_menu.cpp` · `repo:apps/emulator-lift/src/runtime/host_window.h` / `repo:apps/emulator-lift/src/runtime/host_window.cpp`（**追加式**：光标 / 捕获 / `peekAt`）· `repo:apps/emulator-lift/src/shims/register.cpp` · `repo:apps/emulator-lift/CMakeLists.txt`。
派生文件 `repo:apps/emulator-lift/src/shims/register_import_gaps.cpp` 与 `repo:data/lift/overrides.json` 只经 `node tools/lift.mjs sync --write` 重生成（12 条 user32 缺口随之消失）。

## 判据（守卫 + 变异）
夹具：`repo:apps/emulator-lift/tests/user32_window.cpp`（CTest `user32-window`，**没有新建测试二进制**）。
新增第二阶段 `families-setup … families-run`，逐条断言：`cursor` / `cursor-pos` / `show-cursor` / `load-cursor-from-file` / `monitor` / `system-parameters` / `msg-wait` / `unhook` / `menu` / `system-metrics`（**79 checks / 0 failures**）。
变异（实测"改坏就红"，均已撤销）：
1. `SetCursor` 返回**新**句柄而不返回旧 ⇒ `[cursor]` 2 条红（`R_SETCUR_OLD1/2`）。
2. `ShowCursor` 隐藏时不减计数（`+= show ? 1 : 1`）⇒ `[show-cursor]` 红（期望 1，得 3）。

## boot 最终停放
```
stop  : unimplemented @ eip=7FFE04D0
        shlwapi!PathRemoveFileSpecA（返回地址 0x00490382）
insns : 101805860（101.8M）
shims : imported 310 / called-unimplemented 1
```
⇒ **条件 B**（卡点换成非 user32 窗口/输入族）⇒ 本切片停手。完整日志：`.tmp/w3-boot-final.log`。

## 剩余（下一位）
1. **`shlwapi!PathRemoveFileSpecA`**（返回地址 `0x00490382`）：去掉路径最后一段（Win32 语义，含尾部反斜杠口径）；属 S6 文件/路径族邻域 ⇒ **先裁决归属**（`repo:apps/emulator-lift/SCOPE.md` 没有点名它）。
2. 过了它才会走到的本族**真实调用**（已有夹具、但 boot 还没实测）：`MonitorFromWindow`/`GetMonitorInfoA`（0x414080/0x4140B2）、`SystemParametersInfoA`（0x408773/0x494DED）、`LoadCursorFromFileA`（0x4B8BDE）、菜单链（0x4B98C2 / 0x4B9E6F / 0x4BB0ED）、`MsgWaitForMultipleObjects`（0x4CF949，线程等待 helper）、`UnhookWindowsHookEx`（0x4760DE）。
3. `GetCursorPos` / `SetCursorPos` 的调用点（0x404040 / 0x406852 / 0x42202E / 0x4B93EC）在 DDraw 与输入路径上，同样待 boot 走到。

## 没做（以及为什么）
- **不碰** region（gdi32）/ ole32 / oleaut32 / version / imagehlp / imm32 / winmm：条件 B，属别的切片。
- **不做真实 SDL 输入 / 光标形状**：P3 的落点；本线只做 headless 状态（SCOPE §1 口径）。
- `GetCapture` / `SetCapture` / `AppendMenuA`：**不在导入表**（`.lst` 里也没有调用点）⇒ 不注册（不凭空造 API）。
- `user32!RegisterClassA` / `GetDC` / `ReleaseDC` / `SetFocus`：登记成 `addUnimplemented` 但**从未被导入引用**（`lift check` 的 never-imported 清单），静态不可达 ⇒ 本切片不动（`GetDC`/`ReleaseDC` 是 P1/P4 表面 DC 的落点）。
- `SystemParametersInfoA` 只认 3 条 action：引擎 `.lst` 里的全部调用点就这 3 条，其余按失败值（"别过度实现"）。
- 没有实现 `shlwapi!PathRemoveFileSpecA`：文件/路径切片，⛔ 不在本线假装成功。
