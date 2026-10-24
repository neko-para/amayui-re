# 残差第二十二批：零读者关闭 · v1=3 纹理记录表

- id: REQ-01M5PDQGJPWRMPHH3MP23G6KZJ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ 块 1 = 浮点 raw（与输入逐位一致）；块 3 长度为 0（尾段紧接字符串段）
- tags: [spec residual]

# 残差第二十二批：零读者关闭 · v1=3 纹理记录表

## 一句话

关闭三条静态残差（表语义、空白标记、0x30a 小表均为零读者/已定），并纠正 v1=3 20,000 B 副块的身份：它是**纹理记录表**，读侧依赖，不能写 0。

## 关闭

- `RES-ISA-TABLES-SEMANTICS`：表 1 → `0x71`、表 2 → `0x03`、表 3 → `0x8f`；写点/读点逐个复核，已由 `65-save` 吸收。
- `RES-TEX-BLANK-READER`：纹理记录 `+0x10` 空白标记 8 个写点、0 个读点。
- `RES-POOL-SCALAR-30A-MEANING`：`0x30a` 表 1 个写点、0 个读点（区间扫描 `1EC4h` 唯一）。

## 正文纠错

`+1252` 的 20,000 B（v1=3） = **纹理记录表（1000×20 B）**：写侧整段复制，读侧整段拷回并据此重建纹理对象（字段为 1 才重建）。⛔ 写 0 ⇒ 一条纹理都不重建。原“可写 0、读侧不得依赖”删除。

## 台账 / 实现

- 新增 `RES-SAVE-TEXREC-FIELDS`（记录 `+0x08/+0x0C` 语义未命名）。
- `FMT-SAVE-CODEC` 补三个内容层锚（`0x40D6B2` / `0x40D730` / `0x40F9E0`）。
- `save-codec.ts` 注释：v1=3 20k 暂留 0 是未建模，接读侧前必须补。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-ISA-TABLE-POINTERS` / `FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §39。
