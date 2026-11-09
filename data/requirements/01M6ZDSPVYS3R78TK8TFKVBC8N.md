# 🐞S3 emulator-lift · EU-ROOM 的「戻る」按钮对注入点击无反应（hover 有效、ESC 有效）

- id: REQ-01M6ZDSPVYS3R78TK8TFKVBC8N
- type: bug
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: docs/03-emulator/defects.md#BUG-16
- tags: [emulator-lift, input]

# 🐞S3 emulator-lift · EU-ROOM 的「戻る」按钮对注入点击无反应（hover 有效、ESC 有效）

- id: REQ-01M6ZDSPVYS3R78TK8TFKVBC8N
- type: bug
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: docs/03-emulator/defects.md#BUG-16
- tags: [emulator-lift, input]

## 现象

`EU-ROOM` 右下角「戻る」按钮（约 `1200..1272 × 652..711`，框内均值 `0xB6957E`）对**注入点击没有反应**：

- 把光标移上去 ⇒ 画面**会**变（按钮高亮：房间稳定 hash `0x96E51755F00B2E9E` → hover 态 `0xF13F23F24EEEE636`）；
- 在同一位置 `click` / `mouse_down` 4 s 后 `mouse_up` / 双击 ⇒ 画面**一次都不再变**，也不产生任何停因；
- 同一次运行里点 **CG 鑑賞**（`1100,150`）正常切画面，TITLE 上点 OPTION（`812,622`）也正常，按 **ESC** 有效。

⇒ 房间的"退出"这件事今天只能靠 ESC 触发（而 ESC 那条路本身还缺后缓冲 LockRect，见同父的另一张单）。

## 已排除（别重复查）

- 光标与命中测试在工作：hover 会换高亮。
- 注入的按钮电平在工作：`click-release … auto=1 waited_samples=3`（guest 采到过按下）。
- 不是"房间不吃输入"：CG 鑑賞 / ESC 都有效。

## 待查方向（按代价排序）

1. 该按钮的命中区与绘制区不同（把 `screen rect` 往按钮**外框**挪几像素再试；hover 高亮可能来自整块 UI 的"光标进入"而非按钮本身）。
2. 它走另一条读点（只在某个定时器回调 / 另一条消息路径里轮询按钮电平）⇒ `--trace-shims` 看点击前后 `GetAsyncKeyState` / `GetCursorPos` 的调用是否发生、参数是什么。
3. 它要求 `WM_LBUTTONUP` 落在**同一次光标采样**内（我们的自动 hold 是"等一个采样周期再抬"，可能刚好跨过它要的窗口）。

## 判据（修好后）

把 `repo:apps/emulator-lift/scripts/interaction/room-exit.graph` 的 `room` 节点从 `action key 0x1B` 换回
`action click center`，真机跑 **exit 0** + trace 末尾 `task done`（终态 = TITLE 稳定帧 hash）。

## 读数（2026-11-09，macOS arm64 / RelWithDebInfo，`--skip-logo`）

三次独立运行（纯 click / move 后 click / down 4 s 后 up）：房间帧 hash 稳定在 hover 态 `0xF13F23F24EEEE636` 不变。
缺陷史 `repo:docs/03-emulator/defects.md` 的 BUG-16。

## 只读取证（2026-11-09，**未实施**；来自 `.lst` 的 EA 级阅读）

★ 先纠正本单"已排除②（注入按钮电平在工作）"：`waited_samples` 的计数点只有 `GetCursorPos` 与 `GetAsyncKeyState`，
而这条 UI 的 20 Hz 心跳来自 `GetCursorPos` ⇒ 它只证明"读了 3 次位置"；`GetAsyncKeyState` **一行日志都没有**，
今天无法用 trace 判死"电平被读到"。

- WndProc = `sub_4B92F0`（`0x4BA973`；类 "ARCGameEngine"）。`WM_MOUSEMOVE`(0x4B9B27) 只做 `_TrackMouseEvent` + 光标自动隐藏 SetTimer，
  **不做命中测试、不派发点击**。
- `WM_LBUTTONDOWN`→`0x4B9D66`：`if (dword_55E1D4 != 0) return;` → `sub_4B8DC0(0)`；`WM_LBUTTONUP`→`0x4B9D18`：`if (dword_55E1D4 == 0) return;`
  → `sub_4B8DC0(0)`。**两个 case 都不读 lParam**（坐标另取）。
- `dword_55E1D4` = 配置 **`set:ClickOnUp`**（`0x4BB428/0x4BB432`），本机配置 = **1** ⇒ **DOWN 被有意丢弃，只有 UP 派发一次事件**
  ⇒ 之前"长按 4 s"与"纯 click"是同一路，不能拿来排除时序。
- `sub_4B8DC0`：`[engine+0x1E70] != 0` ⇒ **吞掉这一次点击**（清位直接 return）；否则 `[engine+0x1620] |= 1<<btn`。
  该锁存由 `sub_477280` 消费成输入字第 4 位；另有**电平路** `sub_477150`（GetAsyncKeyState 当前按下也置同一位）
  ⇒ "吞一次"只解释一次点击，不解释"永远点不动"。
- 已判死：坐标映射（`setCursorClientPos`/`cursorClientPos` 与 `ScreenToClient`/`ClientToScreen` 同一原点）、
  "DOWN/UP 同一次采样"（ClickOnUp=1 + 电平每帧置位）、消息没送到（其它按钮同路径有效）。
- 剩余候选（按代价）：C1 命中区 ≠ 绘制区（一次 run 点 5 个点）；C2 该按钮在房间顶层不是点击目标（先点 CG 鑑賞 1100,150 再点它）；
  C3 被 `+0x1E70` 吞一次（同一 run 连点两次）；C4 另一读点 / 定时器（`--trace-shims --log-cat shim --log-level info` 抓 SetTimer/KillTimer/SetCursor）。
- **探针判据要改**：原先"停在 `0x4A4F8D`（LockRect）"已不成立（后缓冲 LockRect 已收口）。今天点中「戻る」会走退出路径，
  在 C++ EH 修复前表现为停在 `0x4123C4`（`RaiseException`，见 `REQ-01M708DF24KY0QZNEHY66S0HYW`）；EH 修好后表现为 `task done`。
  修复形状：C1 ⇒ 只校正 room 节点的 `screen rect`（`click center` 不动）；C2/C3 需裁决。
- 分析全文 `.tmp/reveq/01M6XVYY5TCKSAS80WS7PADF8P/analysis.md`（**不入库**；卡点 2 部分）。
