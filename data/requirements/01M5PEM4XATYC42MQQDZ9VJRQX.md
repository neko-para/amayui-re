# 残差第二十三批：纹理记录表字段 · 起始副块纠错

- id: REQ-01M5PEM4XATYC42MQQDZ9VJRQX
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ 块 1 = 浮点 raw（与输入逐位一致）；块 3 长度为 0（尾段紧接字符串段）
- tags: [spec residual]

# 残差第二十三批：纹理记录表字段 · 起始副块纠错

## 一句话

v1=3 的 20,000 B 纹理记录表字段已逐格定死（5×u32）并给出恢复门；同时纠正主体起始副块“可写 0 / 读侧不得依赖”的错误。

## 纹理记录表

- 1000 × 20 B，每项 5 个 u32：来源文件号 / 颜色键 / 恢复重建门 / 零读者标志 / 空白标记。
- 写侧从场景备份表整段复制（备份表 = 主表在每条 `0x71` 的快照，差一拍）；读侧整段拷回，逐项门「门=1 且文件号≥0」→ `sub_4A3800` 重建；成功清空白标记，失败写 −1 继续。
- 写 0 ⇒ 一条都不重建；两个配置键第 1 位都开时前置先释放全部 1000 槽。
- `0x258` / `0x259` 写的就是这张表 + 快照（`CORE-OPS-SCENE-TABLE-FLAGS`）。

## 起始副块

- `+12`（40 B）读侧原值回填；`+52`（1200 B）= 100×12 记录表、读侧逐项重建；两者语义不解释。
- 正文改为“内容本版不解释；读侧会读，⛔ 不可写 0”。

## 台账 / 实现

- 关闭 `RES-SAVE-TEXREC-FIELDS`；新增 `RES-SAVE-PREAMBLE-BLOCKS`、`RES-SAVE-RESTORE-PREFREE`。
- 20k 写侧未实现（仍留 0，注释“未建模”）。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` / `CORE-OPS-SCENE-TABLE-FLAGS` · `docs/02-engine/spec-audit-2026-10.md` §40。
