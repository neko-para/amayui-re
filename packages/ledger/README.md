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

### 3.1 `--validate` 的两条口径（★ 都由 M3 定，别在实现时才发现）

* **锚点两种形态**：① 二进制 EA（可带语料行区间，但行号只是**派生索引**）；
  ② 可执行守卫用例 `file#用例名` —— **文件存在 ≠ 用例存在**（旧仓 `scripts/lib/guard-spec.cjs` 的教训）。
* **锚可以指向只读参考仓（旧仓）**：K2 完成锚点重挂之前这是常态 ⇒ 校验必须
  **显式回答"该绑定落在哪个仓"**，不许因为"本仓找不到"就判红（否则 79 条 B 类集体假红）。
  口径与"为什么"写在 `data/ledger/README.md` §3.1。

## 4. 来源（旧仓，只登记不迁移）

旧仓 `scripts/build-*.mjs`(8) 与 `scripts/journal.js` —— 登记为 `tooling/ledger`（`role: rebuild`）。
**台账形态要换**（旧仓把台账生成器放在名义上的翻译包里、且真源与派生不分区），旧生成器**只作参考**。

## 5. 语言口径

本包**用 `.mjs`**（只有 `apps/emulator` 用 TypeScript）。

## 6. 迁移批次

**M3**（`REQ-01M3TCQTY0X8T8B0FEY3JPJ761`）：文本日志格式落地 + 派生 SQLite 构建 + "DB 可删可重建"守卫
+ 从旧仓**移植四个静态台账守卫**（`test/{journal,capability-ledger,script-ledger,ticket-ledger}.test.ts`
—— 实测它们**只读台账、零 `../src` import**，所以归本域而不是模拟器）。
**只落格式与守卫，不含旧条目。**
