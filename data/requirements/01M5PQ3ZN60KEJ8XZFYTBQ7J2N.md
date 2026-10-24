# 0x3/0x6 装载失败与重试语义落地

- id: REQ-01M5PQ3ZN60KEJ8XZFYTBQ7J2N
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-frame.test.mjs#★ 0x6 打开失败：当前帧停在目标槽、抛类别 7（可重试）；深度失败是类别 1
- tags: [emulator frame]

# 第三十批：0x3 / 0x6 装载失败状态

## 一句话

打开失败时当前帧停在目标槽 / 子槽、抛类别 7；深度失败类别 1；成功路径恢复原帧。运行器不自动重试。

## 代码

- `FrameLoadError`（category 7 / 1）；分派器包装保留 category；修掉失败后取 `this.rec` / `this.ip` 的二次故障。
- `0x6`：先 `cur=slot` 再 load；失败停在目标槽、类别 7；成功恢复 caller 再 `loadFrameAt`；深度类别 1。
- `0x3`：先 `cur=caller+1` 再 load；失败停在子槽、类别 7；成功恢复再 `enterScript`。
- 守卫 +2；变异 +2（不切帧、类别 7→1）。

## 未做

运行器无对话框 / 重试循环，只保留 category；头坏与打开失败仍共用 `loadScript→null`，core 无法区分（头坏应恢复并退出）。

## 判据

`pnpm test` 1062/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 360/360 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §54 · `tools/test/emulator-new-frame.test.mjs`。
