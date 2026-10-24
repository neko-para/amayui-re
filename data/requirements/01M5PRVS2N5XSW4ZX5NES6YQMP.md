# S2a-3：核心写回（盖章 / 收尾清表）

- id: REQ-01M5PRVS2N5XSW4ZX5NES6YQMP
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-commands.test.mjs#★ 命令时间戳盖章：只盖 timestamp === 0 的记录，重写后再次为 0 才重新盖
- tags: [emulator s2]

# 第二十九批：S2a-3 核心写回（盖章 / 收尾清表）

## 一句话

补上命令档的两项核心写回：`stampCommands`（只盖 timestamp=0）与 `finishRenderStep`（无动画清表 + 清强制完成），并接进 session。

## 代码

- `Machine.stampCommands()`：只写 `timestamp === 0`；返回条数。
- `Machine.finishRenderStep()`：`animating === 0` ⇒ 清命令表；`forcedComplete = 0`；返回是否清表。
- `SceneState.forcedComplete` 初值 0。
- `session.ts`：`syncScene` 快照前盖章；`onIteration` `await present` 后 `finishRenderStep`。
- 守卫 +2；变异 +2（每帧都盖、不管动画都清）。

## 未做

consumed 位 16 仍是键级近似；`advanceCommands`（种类 0/2 进度、forcedComplete 写者）未接；像素等价未验证。

## 判据

`pnpm test` 1041/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 346/346 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §46 · `tools/test/emulator-new-scene-commands.test.mjs`。
