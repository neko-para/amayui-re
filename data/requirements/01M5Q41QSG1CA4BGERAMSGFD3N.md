# 0x1a1：整池重编码

- id: REQ-01M5Q41QSG1CA4BGERAMSGFD3N
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-1a1.test.mjs#★ 0x1a1：完整文件 ⇒ 解析成功 + 仍记 restore-unmodeled；坏 CRC ⇒ 解析诊断且不写 op1、不记 restore
- tags: [emulator save]

# 第三十批：整池重编码落地

## 一句话

`applyGlobalPools` 把文件解出的整型值按**当前 key** 重编码；`0x1a1` 不再记 `save-recode-unmodeled`。

## 代码

- `0x1a1` 去掉 `save-recode-unmodeled`。
- 守卫：完整存档后 `decodeInt(globalInt.get(0), KEY) === 7`；不再要求 recode 诊断。
- 变异 +1（不重编码）。

## 未做

引用池内容、尾段 / 图像对象重建、`0x1a0` 复用。

## 判据

`pnpm test` 1067/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 360/360 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §60 · `tools/test/emulator-new-save-1a1.test.mjs`。
