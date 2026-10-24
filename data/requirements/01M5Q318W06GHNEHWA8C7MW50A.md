# 0x1a1：应用三池

- id: REQ-01M5Q318W06GHNEHWA8C7MW50A
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-1a1.test.mjs#★ 0x1a1：完整文件 ⇒ 解析成功 + 仍记 restore-unmodeled；坏 CRC ⇒ 解析诊断且不写 op1、不记 restore
- tags: [emulator save]

# 第三十批：0x1a1 应用三池

## 一句话

读档应用第一段：六个全局池清零 + 整型/浮点/字符串三池写入 + 整型重编码；帧仍未还原。

## 代码

- `Machine.applyGlobalPools`：清三值池与三引用池；写 `encodeInt(ints[i])`、raw floats、strings；返回条数。
- `0x1a1` 解析成功后调用；诊断 `save-1a1-pools-applied` + 仍 `save-restore-unmodeled`。
- 守卫：完整存档后未存档 op1 格清 0；坏 CRC 不写 op1、不记还原。
- 变异 +1（不清整型池）。

## 未做

帧重建 / 脚本重载 / 执行位置 / 引用池内容 / 整池重编码。

## 判据

`pnpm test` 1066/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 359/359 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §58 · `tools/test/emulator-new-save-1a1.test.mjs`。
