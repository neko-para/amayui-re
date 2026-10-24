# S2c：种类 2 抹除笔

- id: REQ-01M5PTAA93CQS0G8MPJ27ZWZP4
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-run.test.mjs#★ 种类 2：有抹除笔时产 strokes（A→B 顺序保序），不再产 composite
- tags: [emulator s2]

# 第三十批：S2c 种类 2 抹除笔

## 一句话

种类 2 的 `strokes` 从规划结构接到执行层与 Pixi 落笔：有笔时按笔序从画布 A/B 取子矩形搬到 own，不再用 composite 冒充图案。

## 代码

- `ExecOp.strokes`；种类 2 合成阶段有笔 ⇒ 产 strokes（空数组仍走 composite 兜底）。
- `CommandCanvasHost.strokes` + `runCommandOps` 派发。
- `pixi-host.drawCommandStrokes`：逐笔 `Texture` frame + `Sprite` 位置 = 矩形左上角，render 到 own；空矩形跳过。
- 守卫 +1（有笔时产 strokes、A→B 保序）；变异 +1（有笔仍 composite）。

## 未做

种类 1 遮罩（rect/flag 协议投影 + 均匀系数）；效果技法；落点 offset≠0 / 视野偏移仍是残差。

## 判据

`pnpm test` 1047/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 352/352 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §49 · `tools/test/emulator-new-scene-command-run.test.mjs`。
