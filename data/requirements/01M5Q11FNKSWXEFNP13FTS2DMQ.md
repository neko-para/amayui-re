# 存档读侧：负载结构解析（实现）

- id: REQ-01M5Q11FNKSWXEFNP13FTS2DMQ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ parseSaveBody：帧记录 / 纹理记录 / 三池往返；v1≠3 时没有纹理表
- tags: [emulator save]

# 第三十批：存档读侧负载结构解析

## 一句话

`parseSaveBody` 解析六池计数、帧记录、纹理记录表；只解析不应用，`0x1a1` 尚未接入。

## 代码

- `parseSaveBody(body, version1, intKey)`：frame 0..39；v1=3 的 20000 B 纹理表；帧记录 parent/scriptIndex/ret/tableA/tableB；整型池 decodeInt、浮点 raw、字符串按 c2；引用三池跳过。
- 守卫 +1（往返 + v1≠3 无纹理表）；变异 +1（不认 20000 B）。

## 未接

`0x1a1` 尚未调用 `parseSaveEnvelope` / `parseSaveBody`；还原 / 整池重编码未做。

## 判据

`pnpm test` 1065/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 357/357 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §56 · `tools/test/emulator-new-save-codec.test.mjs`。
