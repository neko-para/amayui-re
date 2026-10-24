# 0x1a1：接入读侧解析

- id: REQ-01M5Q1CJGPA6JD3N1B9JRD3A3Q
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-1a1.test.mjs#★ 0x1a1：完整文件 ⇒ 解析成功 + 仍记 restore-unmodeled；坏 CRC ⇒ 解析诊断且不写 op1、不记 restore
- tags: [emulator save]

# 第三十批：0x1a1 接入读侧解析

## 一句话

`0x1a1` 读完整文件时调用 `parseSaveEnvelope` + `parseSaveBody`；解析失败不写 op1、不记还原。

## 行为

- `length >= 312` ⇒ 解析；成功记 `save-1a1-parsed`，仍记 `save-restore-unmodeled` / `save-recode-unmodeled`。
- 解析失败 ⇒ `save-1a1-parse-<code>`、不写 op1、不记还原。
- 292 字节旧夹具保持原路径。

## 守卫 / 变异

守卫 +1（完整文件解析成功 / 坏 CRC 点名且不写 op1、不记还原）；变异 +1（失败也继续还原）。

## 未做

帧 / 池应用、整池重编码；`0x1a0` 尚未复用解析。

## 判据

`pnpm test` 1066/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 358/358 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §57 · `tools/test/emulator-new-save-1a1.test.mjs`。
