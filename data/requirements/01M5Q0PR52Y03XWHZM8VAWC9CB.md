# 存档读侧：信封解析（实现）

- id: REQ-01M5Q0PR52Y03XWHZM8VAWC9CB
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ parseSaveEnvelope：buildSaveFile 往返；body 与 buildSaveBody 逐字节相同
- tags: [emulator save]

# 第三十批：存档读侧信封解析

## 一句话

读侧第一段纯函数 `parseSaveEnvelope`：292 头 + 20 参数块 + 变换信封；尚未接 `0x1a1`。

## 代码

- `parseSaveEnvelope` / `SaveFormatError`：头长度、魔数、标题；参数块字数 / 密文长度 / 两个 CRC / 逆变换；前缀三字一致且 ≥12；返回未变换 body。
- 守卫 +2（`buildSaveFile` 往返 body 逐字节相同；坏魔数 / 短文件 / CRC 按 code 拒绝）；变异 +1（不校验魔数）。

## 未接

`0x1a1` 仍只做头长度 / 魔数检查 + `save-restore-unmodeled`；接入前要定“parse 失败不写 op1”的诊断口径。

## 判据

`pnpm test` 1064/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 360/360 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §55 · `tools/test/emulator-new-save-codec.test.mjs`。
