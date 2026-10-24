# 残差第十八批：平键序独立复核（合同级纠错）

- id: REQ-01M5P9P5GJ39E4CMDT9SV8QCB5
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-plan.test.mjs#★ 三表合并序：同键时辅助表 A 在网格之前（typed 表秩 0/1/2）—— 引擎比较链是 A→网格→绘制项
- tags: [spec residual]

# 残差第十八批：平键序独立复核（合同级纠错）

## 一句话

独立复核 `CORE-SCENE-DRAW-ORDER`：四张同键表的身份与平键序原文错位；已按三链互证改正，并修规划器的合并序。

## 纠错

- `+0x448` = 辅助表 A、`+0x438` = 辅助表 B、`+0x428` = 网格、`+0x408` = 绘制项（步长 572 / 572 / 60 / 740 B）。
- **平键序 = 辅助表 A → 辅助表 B → 网格 → 绘制项**；原文“网格 → 辅助 B → 辅助 A → 绘制项”前三个名字整体错位一格。
- 命令档覆盖三表 = 辅助表 A + 网格 + 绘制项（辅助表 B 不参与）；同键序 A → 网格 → 绘制项。
- `bindings.json` 10 个 `what` 改名；`RES-SCENE-DRAW-AUX-SEMANTICS` 的“排在网格与绘制项之间”改为“都在网格之前”。

## 实现

`scene-command-plan.ts::mergeTableKeys` 原来网格先于辅助表 A（复用 `KIND_TEXTURE_OVERLAY`）；改为 `MERGE_AUX_A=0 / MERGE_MESH=1 / MERGE_ITEMS=2` 并导出；typed 守卫同键 `[A, 网格, 绘制项]`；变异 +1。

## 仍开着

辅助表 A/B 的领域语义（unknown）· 同键叠放真机采样 · 真实绘制 S2+ · 保存读侧。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` · `docs/02-engine/spec-audit-2026-10.md` §35 · `tools/test/emulator-new-scene-command-plan.test.mjs`。
