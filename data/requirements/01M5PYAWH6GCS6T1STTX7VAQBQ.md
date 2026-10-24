# S2c-3：影片结束补遮罩

- id: REQ-01M5PYAWH6GCS6T1STTX7VAQBQ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-commands.test.mjs#★ 种类 1 影片结束：对象槽空且 duration>0 ⇒ 遮罩补反值、置动画、清 duration（只一次）
- tags: [emulator s2]

# 第三十批：S2c-3 影片结束补遮罩

## 一句话

种类 1 影片结束时补遮罩反值、置动画、清 duration；用“对象槽空”作为最小结束判据。

## 代码

- `Machine.advanceCommands()` 对种类 1：`duration > 0` 且对象槽没有该 textureId ⇒ `mask = 255 − mask`、`duration = 0`、`animating = 1`；对象还在不动；第二次不翻。
- 守卫 +1；变异 +1（影片结束不补遮罩）。

## 诚实项

“对象槽空”是当前最小结束判据；真实影片结束事件若与槽释放不同步，需再收窄。像素等价未验证。

## 判据

`pnpm test` 1049/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 355/355 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §51 · `tools/test/emulator-new-scene-commands.test.mjs`。
