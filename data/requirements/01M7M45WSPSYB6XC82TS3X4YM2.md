# SC0000 前置：加载确认框（SBUNKI）不接受注入点击 ⇒ SC0000 到不了

- id: REQ-01M7M45WSPSYB6XC82TS3X4YM2
- type: req
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, sc0000]

## 现象（2026-11-17 / mac arm64 / `--game-dir links/raw`）

加载存档屏 → 选槽 001（缩略图台词逐字 = SC0000 第一句）→ 弹 `SBUNKI.BIN`「ロードしますか？」。
**`はい` 点不动**：确认动作块 `[0x30AC,0x32E8)` 从未执行。4 次独立 run 可复现（`.tmp/sc0000/run-raw{8,9,10,15}.log`）。
⇒ SC0000 的渲染路径**根本到不了**。

## 已排除（都有读数，⛔ 不要再试）

- **注入链路**：`WM_LBUTTONDOWN/UP` 全部 `DispatchMessageA` 进 guest WndProc `0x004B92F0`；
  `GetAsyncKeyState(VK_LBUTTON)` 在 `read-mouse-button` 那条指令处返回 `0x8000`（SBUNKI `pos=0x0B0C`）。
- **按住时长**（`kForceReleaseReads 16→4000`）、**`GetForegroundWindow` 返主窗口**：都无效。
- **`i2fc`（0x2FC）不是断点**：它是触摸队列读取（台账 `KN-01M7M6B3477V7H2Z1Z0S04475G`）。

## 已取到的静态几何（台账 `KN-01M7M7BZZB2R1F512K3M0V5437`）

- 命中测试 `i12e`（0x12E → `sub_42F230`）：8 个操作数，命中式 `x_i+i0 <= mx <= x_i+i1` 且
  `y_i+i2 <= my <= y_i+i3`；扫描起点 = `op1+1`（第一次移到某项会跳过第 0 项）。
- **`はい` = 项 0：`x∈[517,764] × y∈[300,347]`（中心 640,323）**；いいえ = 项 1：`y∈[370,417]`。
- 鼠标兜底路径 `0x0F60` 的 gate = `0 <= (local 9) < f8dfc` 且 `f8dff[f8dfc-1] == f8dfe`。

## 最新读数（2026-11-17 / 三次点击计划）

计划（`.tmp/sc0000/load20.plan`）：`SBUNKI [0x09F8,0x0B18)` 门后 **两次不同坐标**点击
`600 323` + `690 323`（都在 はい 矩形内，用来跨过"扫描起点跳过第 0 项"）。
读数 `rc=5` · `fired=4 released=4` · **`await position SBUNKI 0x0F60 0x1040` 8 s 到期**
⇒ **确认路径 `0x0F60` 一次都没到达**（不是 gate 不成立，是那条路根本没走）。

## 结论与下一步

- 剩下两种可能，**都要先读值**才判得动：① 左键抬起没走到 `0x0F60`（按钮/边沿那半）；
  ② 走到了但 gate 的 `(local 9)` / `f8dfe` / `f8dfc` 不满足。
- 需要的工具：**按槽读脚本局部/全局变量（解码后）** ⇒ 已立 `REQ-01M7M7C7JFYHQ587TP75HVQZQN`
  （控制层，只读）。拿到 `(local 7/9)`、`f8dfc`、`f8dfe`、`f8fa5`、`a9cd` 的值后按 §5 的判定表收口。
- 触摸语义（M1：`RegisterTouchWindow`/`GetTouchInputInfo` + `WM_TOUCH`）仍然可选，但**先别做**：
  它会把整条 UI 语义带进来，属 §6 裁决面；当前证据还不支持"这是触摸缺口"。

## 判据

- 过确认框：`[0x0F60,0x1040)`（鼠标支路）或 `[0xDA8,0x0F60)`（触摸支路）命中后，`[0x30AC,0x32E8)` 命中，
  且 `await script SC0000` 命中。
- 之后按那次 run 的 `stop:` 名字选 D3DX 的实现单元（⛔ 不猜）。
