# 批 M3 · 台账层（append-only 文本真源 + 派生查询层 + 可删可重建守卫）

- id: REQ-01M3TCQTY0X8T8B0FEY3JPJ761
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 50

## 范围
**只建框架，不装数据**：台账的**记录 schema** 与在本仓落地所需的**基建** —— append-only 文本真源（`data/ledger/`）
一行一条、自带 ULID、按 key 前缀分片；派生 SQLite 查询层落 `.cache/`（gitignore，永不入库）。
`data/ledger/README.md` §5 写的"只落格式与守卫、不含任何旧条目"就是这一条。

**本节点交付的框架对 K1/K2/K3 是前置**：K1 的清理判据、K2 的"绑定可再校验观察"、K3 的落位，都按这里的 schema 做。
K1 的**归一化口径**（统一 6 种字段记法 / 剔出 14 条字符串化 JSON）**不得自行发明** —— 它按本节点的判据做。

## 判据
（缺 `verify` 的原因：本节点**同时交付判据落点本身**，而它落地后才作为 G1–G3 的证据存在。反之"先挂空守卫再补"正是本节点要防的那种假绿 ⇒ 收口时再挂。）

| | 交付什么 | 怎么判 |
|---|---|---|
| G1 | **记录 schema 冻死**：`id`(ULID) / `at` / `kind` / `subject` / `anchor[]` / `status` / `conflicts[]`；一行一条、顺序不依赖文件位置；分片规则（按 kind + 月）写进 `--describe` | `pnpm tools ledger describe` 与 `data/ledger/README.md` 一致（`json-docs` 那条约定：JSON 的 schema 只由控制脚本自述） |
| G2 | **锚点两种形态，且都不锚行号**：① 二进制 EA（★ 可加语料行区间，但**只当派生索引**：换一次反汇编只重建 `EA → 行号` 映射）；② **可执行守卫用例** `file#用例名` 双锚 | `--validate` 逐条重校验绑定；失效 ⇒ 机械降级 `stale`（不删除）；冲突显式化成产物。★ 旧仓 `scripts/lib/guard-spec.cjs`（`guardAnchor.ts` 只是它的再导出）证明**文件存在 ≠ 用例存在**，这条必须照搬 |
| G3 | **四个静态台账守卫移植**成 `packages/ledger/*.mjs`：旧仓 `test/{journal,capability-ledger,script-ledger,ticket-ledger}.test.ts` —— 它们**只读台账、零 `../src` import**（实测），规则分别是：沿革结构自检 · schema 合法 + `guard` 指向**用例存在** + 人读 md 与数据同步 · **锚点棘轮**（锚必须真出现在声明的行区间内）+ links 不悬空 · `done` 必须带真实存在的守卫 + 证据锚点棘轮 | 移植后能红：喂一条坏记录（锚指向不存在的用例 / 锚不在声明的行区间内）必须报错 |
| G4 | **DB 可删可重建**：删掉本地 DB 后一条命令确定性重建（同输入同字节） | 守卫断言：同一份文本真源连建两次 ⇒ 逐字节相同 |
| G5 | **★ 跨仓有效性校验的口径**：知识记录的锚**可能指向只读参考仓**（旧仓）而不是本仓 —— 在 K2 完成锚点重挂之前这是常态 | 口径写进 `data/ledger/README.md`：`--validate` 必须**显式**回答"该绑定落在本仓还是只读参考仓"；**不许**因为"本仓找不到"就判红。少了这条，79 条 B 类会集体假红 |

★ **写入者只有 K3**：本节点落完格式与守卫之后，`data/ledger/` 里**仍然没有任何条目**；
旧仓知识（`analysis/**`(7) / `tickets/**`(208) / `docs-new/**`(114)）**一律不迁移** ⇒ 见 `knowledge-rebuild.md`。

## 与其它节点的接口（★ 只有这一处，别扩）
* **M4-1** 拆出的 4 个台账守卫**回流到本节点**（G3）：其中一个（`ticket-ledger`）的判据是"没有判据的单不算单"，
  正好就是本节点 `--validate` 原语；本节点落地它，M4-1 不必再管这 4 个。
* **K1** 可按本节点的判据开工；**K2** 不需要本节点（但它要用 G2 的锚格式）；**K3** 是本节点的消费者。
