# S2b-2：命令推进与强制完成

- id: REQ-01M5PSZ7MGCT0GRGB6D5G8S3CQ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-commands.test.mjs#★ advanceCommands：种类 0 未开始/进行中置动画，到期清 duration；种类 3 未开始也算进行中
- tags: [emulator s2]

# 第三十批：S2b-2 命令推进与强制完成

## 一句话

核心 `advanceCommands` 接上：种类 0/3 的动画标记与 duration 清理、种类 2 `mode<0` 的 `forcedComplete`；并把 forcedComplete 投影进进度。

## 代码

- `Machine.advanceCommands()`：种类 0 未开始置 animating；到期清 duration；进行中置 animating。种类 3 不夹取（未开始也算进行中）。种类 2 `mode<0` 置 forcedComplete。
- `session.snapshot` 的 forcedComplete 取 `machine.scene.forcedComplete`；`syncScene` 盖章后调 advanceCommands。
- `commandExecCommandsOf` 的 progress：forcedComplete=1 ⇒ 1。
- 守卫 +2；变异 +2（到期不清 duration、不置强制完成）。

## 未做

种类 1 尾段 duration 推进（影片结束信号）；像素等价。

## 判据

`pnpm test` 1046/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 351/351 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §48 · `tools/test/emulator-new-scene-commands.test.mjs`。
