# @amayui/ledger — 台账：文本日志 + 派生 SQLite 查询层

## 1. 职责

* 读 / 写 `data/ledger/` 的 **append-only 文本真源**（校验、追加、分片归位）；
* 从文本真源**确定性重建**派生 SQLite 查询层（放 `.cache/`，gitignore，**永不入库**）；
* 提供 `--validate`：schema / 追加序 / 锚点（**EA** 与 **可执行守卫用例**双形态）/ 观察重校验 / 冲突显式化。

**实现落在工具层，不在这里**：模型 `tools/lib/ledger.mjs`、CLI `tools/ledger.mjs`（`pnpm tools ledger`）。
本包是**落点声明**（域归属 + 口径），所以只有 `package.json` + 本 README。
★ 为什么模型不放本包：本仓的写路径纪律是"工具层三层 + 一个入口"（`docs/00-origin/decisions.md` §7），
台账的 schema 必须与 `pnpm tools ledger describe` 是同一份，不能有第二个家。

## 2. 现状

`package.json`（`type: module`）+ 本 README；**实现与守卫已落地**（工具层 + `tools/test/ledger.test.mjs`），
`data/ledger/` 里**没有任何条目** —— 这是有意为之（K3 才是唯一写入者）。

## 3. 三条不可谈判的点

1. **真源是文本**，DB 是**派生只读查询层**：删掉本地 DB 后一条命令必须能重建。
   ★ 判据不是"文件字节相同"，而是**逻辑内容相同**（同输入 ⇒ 同建表语句 + 同排序行）；
   口径与"为什么"写在 `data/ledger/README.md` §3.1。
2. **DB 永不入库**（`.gitignore` 里已有 `*.sqlite`/`*.sqlite3`/`*.db`；LFS 也救不了它 —— LFS 不解决合并）。
3. 知识条目过准入门才准进台账（绑定可再校验的观察；**锚点锚 EA 不锚行号**；
   观察失效 ⇒ 降级 `stale` 但**不删除**；冲突**显式化**，不许静默改写）。

### 3.1 `--validate` 的两条口径（★ 最容易被实现漏掉的两条）

* **锚点两种形态**：① 二进制 EA（可带语料行区间，但行号只是**派生索引**，EA → 文件偏移由 PE 节表现算）；
  ② 可执行守卫用例 `路径#用例名` —— **必须落到真的用例名上**，不是"文件里随便一个子串"。
* **锚可以指向只读参考仓（旧仓）**：K2 完成锚点重挂之前这是常态 ⇒ 校验必须**显式回答"落在哪个仓"**，
  且参考仓不在场时是 **warning 不是 error** —— 否则 79 条 B 类会集体假红。

## 4. 来源（旧仓，只登记不迁移）

旧仓 `scripts/build-*.mjs`(8) 与 `scripts/journal.js` —— 登记为 `tooling/ledger`（`role: rebuild`）。
**台账形态要换**（旧仓把台账生成器放在名义上的翻译包里、且真源与派生不分区），旧生成器**只作参考**。
★ 格式的**真源**是 `pnpm tools ledger describe`；本文件只写口径与指向，**不复述 schema**。

## 5. 语言口径

本包**用 `.mjs`**（语言口径的真源是 `AGENTS.md` §3，这里不复述）。

## 6. 迁移批次

**M3**（`REQ-01M3TCQTY0X8T8B0FEY3JPJ761`）：文本日志格式落地 + 派生 SQLite 构建 + "DB 可删可重建"守卫
+ 从旧仓**移植四个静态台账守卫**（`test/{journal,capability-ledger,script-ledger,ticket-ledger}.test.ts`
—— 实测它们**只读台账、零 `../src` import**，所以归本域而不是模拟器）。
**只落格式与守卫，不含旧条目。**
