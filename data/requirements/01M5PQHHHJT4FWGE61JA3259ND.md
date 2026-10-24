# S2a-1：命令档纯执行层

- id: REQ-01M5PQHHHJT4FWGE61JA3259ND
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-exec.test.mjs#★ 默认关闸：enabled 缺省 / false ⇒ 零 op、零诊断（产品零回退）；空命令表 ⇒ 零 op
- tags: [emulator s2]

# 第二十七批：S2a-1 命令档纯执行层

## 一句话

命令档从“只规划”推进到“有可执行的纯 op 序列”：阶段带表身份、按种类闸门产出 target/clear/emit/copy/composite，默认关闭（产品零回退），仍不画像素。

## 代码

- `scene-command-plan.ts`：`covered` / `below-range` 加 `entries {key,table}`；`tableKeys` 兼容；below 只含键 < 段起点；同键 A→网格→绘制项。
- 新增 `scene-command-exec.ts`：`ExecOp` 闭集合；种类 1 首帧 copy A→own；种类 0/2 双画布 composite；种类 3 单画布；`enabled` 缺省 false ⇒ 0 op。
- 守卫 9 条；变异 +3（总闸 / copy 方向 / clear 目标）。

## 未做

pixi-host 真实执行、画布 36/37、遮罩 / strokes / 进度、核心盖章写回。

## 判据

`pnpm test` 1033/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 341/341 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` / `CORE-OPS-SCENE-COMMAND-RECORDS` · `docs/02-engine/spec-audit-2026-10.md` §44 · `tools/test/emulator-new-scene-command-exec.test.mjs`。
