# 🐞S3 emulator-lift · EU-ROOM 的「戻る」按钮「注入点击无反应」⇒ **不是缺陷**（2026-11-09 是量测窗 artifact）

- id: REQ-01M6ZDSPVYS3R78TK8TFKVBC8N
- type: bug
- status: dropped
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: docs/03-emulator/defects.md#BUG-16
- dropped_reason: 不是缺陷 / 不修：点击一直有效 —— room-exit.graph 换回 click center 即 exit 0 + task done + TITLE hash（三次读数逐位相同）。2026-11-09 读到的'点不动'是量测窗 artifact：出房间要跨 103 帧退场 + 单帧 730,765,167 条的重载帧，那三次 run 的 quit 全落在重载帧里面。判据保留在两份任务图的真机 run + interaction-graph 的 pure 守卫。
- tags: [emulator-lift, input]

## 现象（原记录）

`EU-ROOM` 右下角「戻る」按钮（约 `1200..1272 × 652..711`，框内均值 `0xB6957E`）对**注入点击**没有反应：
hover 会换高亮、点 CG 鑑賞 有效、ESC 有效，而同一位置的 click / 长按 4 s 再抬 / 双击画面都不再变、也不产生停因。

## 改判（2026-11-10）：**不是缺陷** —— 2026-11-09 的读数是量测窗 artifact

- `room-exit.graph` 的 room 节点换回 `action click center` ⇒ **exit 0 + `task done`**，终态 TITLE hash `0x94CB492BF7F1AEEB`；
  三次 run 读数逐位相同（`insns=2,120,728,564`、`transitions=11 actions=3 samples=249 changes=44`）。
- 点击**当时就生效**：抬起那一下的 hover 重绘（第 115 帧 `0xF13F…`）就是它的效果；之后是 **103 帧退场**
  + **单帧 730,765,167 条**的 TITLE 重载帧（≈73 s guest / 24 s 墙钟）。
- 2026-11-09 那三次 run 的 `quit` 落在抬起后 +98M / +188M / +350M 条 —— **全在那个重载帧里面**，
  而且三次都恰好"抬起后 103 帧"（与今天的退场帧数相同）⇒ 退出已经在进行，只是取样窗提前关掉了。
- **四路对照（同机同构建）**：空白点 `click 100 100` / 不点击 / 只 `mouse_move` ⇒ 全部 exit 5
  （`expect-change` 或 `timeout(node back)`）；点 CG 鑑賞 `1100,150` ⇒ 画面变了但不是 TITLE。

## 处置

- 图：`repo:apps/emulator-lift/scripts/interaction/room-exit.graph` 的 room 节点回到 `action click center`
  （删掉"退回 ESC"的临时行与错误注记）；新增 `repo:apps/emulator-lift/scripts/interaction/room-exit-right.graph`
  （右键 = 引擎取消位 bit5，同一终态）。
- 判据：两份图真机 `exit 0 + task done + TITLE hash`；pure 守卫
  `repo:apps/emulator-lift/tests/interaction_graph.cpp` 的 `graph-scripts-parse`（两份图必须能解析）
  与 `parse-button-right` / `emit-button-carried`。
- 文档：`repo:docs/03-emulator/defects.md` 的 BUG-16 已改写为"不是缺陷"；读数进 `OBS-20261110-01..04`。

## 重开条件

若再出现"点了没反应"：先确认观测窗盖住了一次 TITLE 重载帧（今天 7.3e8 条），
⛔ 别再用 `--input-script` 里 `at insn` 的硬锚点当终点。
