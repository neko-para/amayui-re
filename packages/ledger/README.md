# @amayui/ledger — 台账：文本日志 + 派生 SQLite 查询层

## 1. 职责

* 读 / 写 `data/ledger/` 的 **append-only 文本真源**（校验、追加、compact 出 canonical snapshot）；
* 从文本真源**确定性重建**派生 SQLite 查询层（放 `.cache/`，gitignore，**永不入库**）；
* 提供 `--validate`：格式 / 主键（`scope+offset`，**绝不能是 name**）/ 观察绑定是否存在。

## 2. 本轮状态

只有最小 `package.json`（`type: module`）+ 本 README，**没有代码、没有任何条目**。

## 3. 三条不可谈判的点

1. **真源是文本**，DB 是**派生只读查询层**：删掉本地 DB 后一条命令必须能重建（同输入同字节）。
2. **DB 永不入库**（`.gitignore` 里已有 `*.sqlite`/`*.sqlite3`/`*.db`；LFS 也救不了它 —— LFS 不解决合并）。
3. 知识条目过准入门才准进台账（绑定可再校验的观察；锚点锚 EA 不锚行号；
   观察失效 ⇒ 降级 `stale` 但**不删除**；冲突**显式化**，不许静默改写）。

## 4. 来源（旧仓，只登记不迁移）

旧仓 `scripts/build-*.mjs`(8) 与 `scripts/journal.js` —— 登记为 `tooling/ledger`（`role: rebuild`）。
**台账形态要换**（旧仓把台账生成器放在名义上的翻译包里、且真源与派生不分区），旧生成器**只作参考**。

## 5. 语言口径

本包**用 `.mjs`**（只有 `apps/emulator` 用 TypeScript）。

## 6. 迁移批次

**M3**：文本日志格式落地 + 派生 SQLite 构建 + "DB 可删可重建"守卫。**只落格式与守卫，不含旧条目。**
