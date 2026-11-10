# room-exit 的终态判据：ESC = 退出游戏（ExitProcess）不是回 TITLE ⇒ room-exit.graph 的 exit 0 要另选出口

- id: REQ-01M717SQJ2RDFRJCB41RREXQYX
- type: req
- status: blocked
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- blocked_by: [01M6ZDSPVYS3R78TK8TFKVBC8N]
- tags: [emulator-lift, interaction, seh]

## 判据（存在性 + 观察）

`REQ-01M708DF24KY0QZNEHY66S0HYW`（C++ EH 展开）收口时实测出的**口径问题**：`room-exit.graph` 要 `exit 0 + task done`，
但 ESC 这条路**根本不会回 TITLE**，所以那张图**永远等不到**它的终态。

- 真机 trace（`--interaction-graph scripts/interaction/room-exit.graph --present headless --raster cpu`，`--skip-logo --audio off`）：
  `RaiseException(0xE06D7363, flags=1, nargs=3)` → handler `0x513B90` → 嵌套 `RtlUnwind(TargetFrame=0x6FFFFB74, TargetIp=0x004E706B)`
  → 落到 catch `0x41389A` → `_WinMain@16` 返回 → **guest 自己调 `ExitProcess`**：
  `stop: stopped` / `detail: shim 主动结束运行：kernel32!ExitProcess`。
- 交互图侧：`node room done` / `node back enter frame=49` 之后再没有 Present ⇒ `back` 的 `when screen hash`（等 TITLE）没机会命中 ⇒ **exit 5**。
- ⇒ **ESC = "退出游戏"**（WM_QUIT → `Command_Exit_Exception` → catch → 进程退出），不是"回 TITLE"。这与设计一致（catch 体就是收尾 + 返回 WinMain）。
- ★ **但"这是 EU-ROOM 退出"这件事本身待核**（用户 2026-11-10 提出）：极可能是注入的 ESC/坐标点击落到了 `Quit`（abort）这条路上，
  而我们**从未真正点中过「戻る」**（`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N` 仍 open）。⇒ 见 spike 单 `REQ-01M71P87BMTZK4MHT8468BMC3G`。
  ⛔ 在该 spike 出结论前，本单的"终态判据"**不要**被当成"EU-ROOM 退出的判据"引用。

## 要裁的两条路（父级尚未裁）

1. **走「戻る」点击**：`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（房间里「戻る」按钮对注入点击无反应，仍 open）修好后，
   `room-exit.graph` 的 `back` 节点用真实点击就能拿到 TITLE hash ⇒ 那时 `exit 0 + task done` 才成立。
2. **改图的终态口径**：把 `room-exit.graph` 的终态从"等 TITLE hash"改成"等进程退出"（`stop: stopped` + `detail: ExitProcess`）
   —— 但那张图同时是 `REQ-01M6XVYY5TCKSAS80WS7PADF8P`（识别驱动状态机）"出房间同一终态"的判据，改口径会牵动那张单。

- ⛔ 不许为了凑 `exit 0` 去改 `room-exit.graph` 或业务逻辑（本轮**没有任何**这类改动）。
- ⛔ 不许把 `stopped`（`ok()` 为真）= 任务完成（这正是 `REQ-01M6WW9R6SJ7P8XA4522XCR48A` 记过的口径坑）。

## 裁决（2026-11-10）：**走第 1 条 —— 等「戻る」，⛔ 不改图口径**

- 理由：`room-exit.graph` 目前**同时**是 `REQ-01M6XVYY5TCKSAS80WS7PADF8P`（识别状态机）"出房间同一终态"的判据；
  把终态改成"等进程退出"会让那张单的判据从"回到同一屏"退化成"进程没了"，**那是把判据改弱**（`AGENTS.md` §10：不许为了变绿改判据）。
- 而第 1 条只依赖一张**已存在**的 open 单：`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（「戻る」点击无反应）。它修好后 `room-exit.graph` 用真实点击就能拿到 TITLE hash，
  `exit 0 + task done` 自然成立，且**判据没有被改弱**。
- 所以本单当前状态 = **`blocked`**，`blocked_by` 就是那张戻る单；⛔ 不要为了收口本单去动图或业务逻辑。

## 为什么单独成单

- 它**不是** SEH 的缺陷：机制级验收（raise → handler → RtlUnwind → catch → 落点）已经在 `REQ-01M708DF24KY0QZNEHY66S0HYW` 的真机 trace 里成立。
- 它**是**"终态判据选错了出口"这一类问题 —— 混在 EH 单里会让"SEH 做完了"和"图的口径不对"红在一起，下一个人分不清。
- 与 `REQ-01M6WW9R6SJ7P8XA4522XCR48A`（`Stopped` 仍 exit 0 的口径）同族：**"停住了"≠"到达目标"**。

## 与别的单的关系

- 前置/联动：`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（戻る 点击）· `REQ-01M6XVYY5TCKSAS80WS7PADF8P`（识别状态机的"出房间"终态）·
  `REQ-01M6WW9R6SJ7P8XA4522XCR48A`（停机原因与退出码映射，已收口）。
- 已收口：`REQ-01M708DF24KY0QZNEHY66S0HYW`（SEH 派发，机制级）。
