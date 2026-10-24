# 残差第十二批：STH 尾段归属修正 · 抹除笔接入规划器

- id: REQ-01M5N7VJ85MBNYM8RYMHRH47FZ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-plan.test.mjs#★ 种类 2：同键条目在表里但没有矩形 ⇒ scene-command-wipe-rect-unmodeled 且不产笔（⛔ 不编矩形）
- tags: [spec residual]

# 残差第十二批：STH 尾段归属修正 · 抹除笔接入规划器

## 一句话

合并上一批留下的 STH `VERSION2` / `TRAILER` 收窄与正文修正；把 `wipePattern` 接进命令档规划器（S2 第一步，仍不发绘制）。

## 合同修正

- 版本字段 2 不只是相等比较：块 1 / 4 内容写入与回填的门（≥ 20），配置 v1==1 时用 `== 20` 选另一条恢复分支。
- "尾段"拆成：负载末尾的 740/2964 **只写表**（无读取路径，只影响布局）与负载之后的**文本缓冲尾段**（12B 压缩前缀 + LZSS；完整变体、v1≥2 才读；写侧只有"存到槽"追加；顺序定位）。
- `RES-SAVE-VERSION2-MEANING` 收窄；`RES-SAVE-TRAILER` 改为"归属已定、字段含义未定"。

## emulator-new 落地（S2 第一步）

- 规划器 `tables.itemRects` + `CommandPlanPhase.strokes`；种类 2 有矩形 ⇒ `wipePattern`（elapsed 减延迟）挂 composite；无矩形 ⇒ `scene-command-wipe-rect-unmodeled` 不产笔；`wipe-*` 诊断转 `scene-command-wipe-*`。
- **仍不真的画**：pixi 不消费 strokes。
- 守卫 `emulator-new-scene-command-plan` +5；变异 +2（328 条）。

## 仍开着

真实绘制 S2+（画布 36/37 / 表面拷贝 / 遮罩 / 效果技法 / 真实清表）· 笔的落点 / 视野偏移 · `RES-SAVE-STH-SHORTREAD` · `RES-SAVE-VERSION2-MEANING` · 其余真机项。

## 判据

`pnpm test` 994/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 328/328 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §29 · `tools/test/emulator-new-scene-command-plan.test.mjs`。
