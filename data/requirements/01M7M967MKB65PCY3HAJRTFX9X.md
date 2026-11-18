# 输入计划：单次 click 不保证按下被一拍看到（需要 press/release 形状或采样感知的兜底抬起）

- id: REQ-01M7M967MKB65PCY3HAJRTFX9X
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: apps/emulator-lift/tests/input_inject_visible.cpp#input-inject-visible
- tags: [emulator-lift, input]

## 现象（2026-11-17 / mac / `load20` vs `load24`）

同一屏（SBUNKI 确认框）同一坐标，只因为**相邻两条 `click`**：

- `load20`（`click 600 323` 紧跟 `click 690 323`）：两条在**同一读点**被消费 ⇒ 按下只活约 17 读点
  （`kForceReleaseReads=16` 兜底）⇒ 在脚本自己的 `read-mouse-button`（`0x0B0C`）之前就抬起 ⇒
  `(local b)` 只到 4（bit2）、从未到 5（bit0+bit2）⇒ 确认路径 `0x0F60` 从未进入。**FAIL**。
- `load24`（`click 600 323` → `wait 300` → 门 → `click 690 323`）：**PASS**（`rc=0`，进 SC0000）。

根因是**施加层**：`InputPlan::atPoint` 的 while 里一次消费多条 step；`releaseDue()` 的兜底只看
"读过多少个读点"，不看"有没有被 KeyLevel/CursorPos 采样到"。

## 要做什么（二选一或都做）

1. **DSL 形状**：给配方 `press x y` / `release`（或 `click x y hold <读点数>`），让"按下 → 跨一拍 →
   抬起"可以被写成**语义**而不是"两个不同坐标 + wait"的巧合。
2. **兜底策略**：`releaseDue()` 的强制抬起条件从"读过 N 个读点"改成"**至少被一次 KeyLevel 或
   CursorPos 采样看到过**（或超时上限）"；⛔ 不许无限期保持按下。

## 判据

- 顶层 pure CTest / `tools/test/` 契约：一次 `press` + 跨一拍 + `release` 后，`GetAsyncKeyState` 至少
  一次返回 0x8000、一次返回 0（自造 guest + 假读点，与 `input-plan-*` 同形）。
- 变异：把兜底改回"只看读点数" ⇒ 该用例红。
- ⛔ 不改 guest 可见语义；⛔ 不改 `click` 的坐标映射。

## 已知边界

`exp-a` 的 B4 已证明"只拉长按住"不够：SBUNKI 需要**按下前的一拍无按键**（置 bit2）。
⇒ 用例要覆盖"三拍形状"，不能只覆盖"按住更久"。

## 裁决（2026-11-19 / 落地时）：不做 `press` / `release` DSL，改「跨位置读点 + N 个 KeyLevel 读点」

正文给了两条路（DSL 形状 / 采样感知的兜底）。取证后换成**第三条**并落地（`.tmp/input-inject/analysis.md`，全锚 EA）：

- 引擎的按键**只从电平读** `GetAsyncKeyState(VK_LBUTTON)`（EA `0x477241`）；`WM_LBUTTONDOWN/UP` 只写一个
  **全语料无读者**的 dword `+0x1620`。⇒ "按下被看见"的**唯一**判据是**按键电平读点**，不是消息。
- 位置采样被锁存（`+0x74A4`），所以"按下前的一拍无按键"（SBUNKI 要的 bit2）由**跨一个已完成的位置读点**提供。
- ⇒ `click` 的唯一形状 = `move → 跨位置读点 → down → N(=2) 个 KeyLevel 读点 → up`；两条墙钟兜底（500 ms / 250 ms）
  保证不会永久按住。⛔ 不开 `press`/`release`：那会让"配方写错就静默不生效"回来。

守卫：`apps/emulator-lift/tests/input_inject_visible.cpp#input-inject-visible`（96 checks）+
`apps/emulator-lift/tests/input_plan.cpp#input-plan-read-points`（521 checks）。变异（闸门换回"总样本数"）⇒ 当场红（CTest 8）。

## 读数

- 2026-11-19 / win32 / RelWithDebInfo / dev 车道 `1c4e648`：全量 ctest **121/121**（0 failed，155.22 s）；
  定向 `input-inject-visible` 96/0、`input-plan-read-points` 521/0、`lift-control-endpoint` 524/0。
- 2026-11-19 / win32 / main `234d887`（合并后）：全量 ctest **123/123**（141.66 s）。
- **真游戏端到端**（SAVE05 复现，`.tmp/save05-black/report.md` §5）：`pause → move 640 322 → resume → click 640 322 → resume`
  每次都生效 —— `pos 0x30A4 → SYSTEM4.BIN 0x990`、`0x41A5C → 0x238EC`，加载 SAVE05 / 点宝藏 / 推进对话框全通。
  ⚠ 注意：**按下与按键电平读点各要跨一次** ⇒ 一次 `resume --for-ms 60` 不够走完 `up`，要两次。
- 副作用修掉：注入会话收口交还光标所有权（"注入一次之后真鼠标全死"）。
