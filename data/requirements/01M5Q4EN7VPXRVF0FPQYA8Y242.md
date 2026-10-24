# S2 开闸入口：URL commandExecKinds

- id: REQ-01M5Q4EN7VPXRVF0FPQYA8Y242
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-run.test.mjs#★ parseCommandExecKinds：只收 0..3、去重、未知丢弃；空 ⇒ 零回退
- tags: [emulator s2]

# 第三十批：命令档开闸入口（URL）

## 一句话

给 S2 像素验证一个手动开闸入口：`?commandExecKinds=0,1,2`；默认仍零回退。

## 代码

- `parseCommandExecKinds`：只收 0..3、去重、未知 / 非整数丢弃；空 ⇒ []。
- `renderer.ts` 从 `window.location.search` 读取并传给 `PixiRenderHost.create`。
- 守卫 +1；变异 +1（不过滤种类）。

## 未做

真实像素截图夹具；开闸后仍只保证结构 / 次序。

## 判据

`pnpm test` 1068/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 361/361 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §61 · `tools/test/emulator-new-scene-command-run.test.mjs`。
