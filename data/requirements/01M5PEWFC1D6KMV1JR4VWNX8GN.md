# 残差第二十四批：纹理记录表写侧落地

- id: REQ-01M5PEWFC1D6KMV1JR4VWNX8GN
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-textures.test.mjs#★ v1=3：+1252 起 1000×20 B，五格逐项按序落盘（file / color / 门 / 零读者标志 / blank）
- tags: [spec residual]

# 残差第二十四批：纹理记录表写侧落地

## 一句话

把 v1=3 的 20,000 B 纹理记录表写侧落进 `save-codec`：1000×20 B、五格按序，`v1=2` 不写；接读侧前不再留 0。

## 代码

- `SaveSnapshot.textureRecords?`：`file / color / flag0=恢复重建门 / flag1=零读者标志 / blank`。
- `buildSaveBody`：仅 `v1===3` 在 `+1252` 写 1000×20 B；缺项 0；`v1!==3` 不写、`recBase` 仍 1252。
- `buildSlotFile`：id 0..999，`file/color/blank` 从 `scene.textures`，`flag0/flag1` 从 `flagTableA/B`；无记录三格 0（不写 −1）。
- 新增 `emulator-new-save-textures` 4 条 pure（五格逐项 / 只改 20000 B / v1=2 无块 / `0x19e` 真路径）。
- 变异 +2：项步长 20→5、恢复门与零读者标志互换。

## 未做

读侧整段拷回 + 按「门=1 且文件号≥0」重建纹理对象 / 清空白标记；`0x258` 两表同值位对与现模型 A=bit0/B=bit1 的偏差。

## 判据

`pnpm test` 1024/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 338/338 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §41 · `tools/test/emulator-new-save-textures.test.mjs`。
