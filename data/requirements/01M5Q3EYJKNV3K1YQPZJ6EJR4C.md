# 0x1a1：帧记录重建

- id: REQ-01M5Q3EYJKNV3K1YQPZJ6EJR4C
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-1a1.test.mjs#★ 0x1a1：有 loadScript ⇒ 按帧记录重建（cur / ip / parent / ret / 表）
- tags: [emulator save]

# 第三十批：0x1a1 帧记录重建

## 一句话

按存档帧记录重建 0..current 帧槽：重新装载脚本、写 parent/ret/tables、用 tableB/tableA 下标算执行位置。

## 代码

- `Machine.restoreFrames`：需要 `loadScript`；清 40 槽；逐帧 freshFrame + body + parent/ret/tables；`tableB` 优先 `×4+12`，否则 `tableA ×4`；设 cur。
- `0x1a1` 三池后调用；成功记 `save-1a1-frames-restored`，不再记 `save-restore-unmodeled`。
- 守卫 +1（cur/parent/ret/tableB 优先/诊断）；变异 +1（忽略 tableB）。

## 未做

引用池内容、尾段 / 图像对象重建、整池重编码；局部池按 spec 由 freshFrame 重建。

## 判据

`pnpm test` 1067/0 · `pnpm test:assets` 204/0（1 skip）· `pnpm test:mutation` 360/360 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`docs/02-engine/spec-audit-2026-10.md` §59 · `tools/test/emulator-new-save-1a1.test.mjs`。
