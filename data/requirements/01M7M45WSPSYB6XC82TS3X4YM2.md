# SC0000 前置：加载确认框（SBUNKI）不接受注入点击 ⇒ SC0000 到不了

- id: REQ-01M7M45WSPSYB6XC82TS3X4YM2
- type: req
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, sc0000]

## 现象（2026-11-17 / mac arm64 / `--game-dir links/raw`）

加载存档屏 → 选槽 001（缩略图台词逐字 = SC0000 第一句）→ 弹 `SBUNKI.BIN`「ロードしますか？」。
**`はい` 点不动**：注入点击的 `fired/released` 递增，但确认动作块 `[0x30AC,0x32E8)` 从未执行。
可复现：4 次独立 run（`.tmp/sc0000/run-raw{8,9,10,15}.log`）同一步停住；读数见
`.tmp/sc0000/read2.md`。⇒ SC0000 的渲染路径**根本到不了**。

## 已排除（都有读数，⛔ 不要再试）

- **注入链路本身**：3 次 click 的 `WM_LBUTTONDOWN/UP` 全部 `DispatchMessageA` 进 guest WndProc `0x004B92F0`
  （lParam 逐位对）；`GetAsyncKeyState(VK_LBUTTON)` 在 `read-mouse-button` 那条指令处返回 `0x8000`
  （SBUNKI `pos=0x0B0C`；`GetCursorPos@0x0AF8` 返回 (650,320)）。
- **按住时长**：`kForceReleaseReads 16→4000`（跨 2 拍、两拍都读到 mask=0x1）仍然 FAIL。
- **`GetForegroundWindow` 恒 0**：改成返回主窗口 hwnd 后仍然 FAIL（`.tmp/sc0000/exp-a.md`）。
- **`i2fc`（0x2FC）不是断点**：它是**触摸输入队列**的读取（handler `sub_431BA0`，台账
  `KN-01M7M6B3477V7H2Z1Z0S04475G`）；`e == 1` 只是触摸支路 `0xDA8` 的前置，鼠标会话必走
  **`jcc a3 → 0x0F60`** 的鼠标兜底。

## 现在最可能的断点：`0x0F60` 的 gate

`0x0F60` 是"鼠标行选中确认"，gate = `1 <= (local 9) < f8dfc`，再过 `f8dff[f8dfc-1]` / `f8dfe`。
`local 9` 由 `i12e`（0x12E，handler **`sub_42F230`**，未读）算出来——它是"鼠标位置命中第几个项"的判据。
⇒ 怀疑：点在 `650 320` 时 `i12e` 没命中"はい"（视觉 hover 高亮可能来自另一条路径）。

## 要裁决 / 下一步（二选一）

1. **M0 继续取证（只读 RE）**：读 `sub_42F230`（`i12e` 的命中矩形）、`f8dff`/`f8dfc` 的装载、
   以及加载框入口 `f8dfc` 的值 ⇒ 算出"はい"的命中矩形，用一次位置门 run 验证。
   ⚠ 要在仿真里直接读 `(local 9)`/`f8dfc` 得给控制层加"按帧槽读脚本局部变量"（改 schema ⇒ 先立单）。
2. **M1 实现触摸语义（产品裁决）**：`RegisterTouchWindow`/`GetTouchInputInfo`/`CloseTouchInputHandle`
   现在被 `src/shims/dynamic_exports.cpp:27/30/33` 显式判成"XP 没有这个导出"，`WM_TOUCH(0x240)` 也不在注入面。
   实现三件套 + 注入 `WM_TOUCH` ⇒ 触摸支路 `0xDA8` 可达。**风险**：给 guest 造设备 = 语义面新增（过 §6）、
   整条 UI 语义漂移（`0xA1C` 把 local4 强置 1 等）、与"本机报 XP 5.1"的口径冲突。

## 判据

- 过确认框：`await position SBUNKI 0x0F60 0x1040`（鼠标支路）或 `[0xDA8,0x0F60)`（触摸支路）命中，
  且 `await script SC0000` 命中。
- 之后按那次 run 的 `stop:` 名字选 D3DX 的实现单元（⛔ 不猜）。
