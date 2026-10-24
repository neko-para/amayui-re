# S2c-2：种类 1 遮罩

- id: REQ-01M5PTNR4XNEVC9ZYVN8W8NN20
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-run.test.mjs#★ 种类 1：首帧 copy 后接 mask；非首帧只有 mask（不再 copy）
- tags: [emulator s2]

# 第三十批：S2c-2 种类 1 遮罩

## 一句话

种类 1 的 `rect` / `flag` 投影与 `mask` op 接上；均匀系数最小模型，逐像素遮罩未建模。

## 代码

- `protocol.SceneCommandSnapshot` 加 `rect?` / `flag?`；session 投影。
- `commandExecCommandsOf` 投影两格；`CommandExecCommand` 加两格。
- `ExecOp.mask`：首帧 copy 后 mask、非首帧只 mask；记录 `mask`（写表 flag=1⇒0、否则 255），`coeff = mask===255 ? 254/255 : 0`；诊断 `scene-command-mask-uniform`。
- `pixi-host.drawCommandMask`：画布 A 的 rect 子矩形、`sprite.alpha=coeff` 画到 own。
- 守卫 +1；变异 +1（不产 mask）。

## 未定

`flag` 两时刻互补的时序未建模（只 0 / 254/255 两档）；逐像素遮罩、效果技法未做。

## 判据

`pnpm test` 1048/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 354/354 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §50 · `tools/test/emulator-new-scene-command-run.test.mjs`。
