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
