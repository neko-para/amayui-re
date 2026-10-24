# S2b：进度与退路合成

- id: REQ-01M5PSE5A5J48SWXRW151J1WBR
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-run.test.mjs#★ 种类 0 的进度透传到 composite；种类 3 单源也用同一进度
- tags: [emulator s2]

# 第三十批：S2b 进度与退路合成

## 一句话

种类 0 / 3 的进度从命令时间戳算出来并透传到退路合成；第二块画布按进度 alpha。核心 `advanceCommands` / `forcedComplete` 仍未接。

## 代码

- `commandExecCommandsOf` 新增 `progress`：种类 0 未开始 0、到期 1；种类 3 不夹取（未开始可为负）；进行中线性。
- `CommandExecCommand.progress`；`composite` op 加 `progress` + `path:'fallback'`；`runCommandOps` 透传。
- `pixi-host.compositeCommandSurface`：两源先 A 不透明、后 B `alpha=clamp(progress,0,1)`；单源不透明。
- 守卫 +2（进度透传 / 三段边界）；变异 +2（恒 1、进行中也算 1）。

## 未做

核心 `advanceCommands`（到期清 duration / animating）、`forcedComplete` 写者；种类 1 copy 已接但影片 RT 争用未验；遮罩 / strokes / 效果技法。

## 判据

`pnpm test` 1044/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 349/349 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §47 · `tools/test/emulator-new-scene-command-run.test.mjs`。
