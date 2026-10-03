# data/requirements/ — 需求台账（**高层次的进度视图**，新仓自己的基建）

> ★ 这里回答的是「**还要做什么、到哪一步了**」，而且是给**人**看的：整棵树有节点数与单节点行数的**预算**（守卫会红）。
> 它**不回答**「知道什么」—— 逆向结论 / 观察 / 长调查属于**知识台账** `data/ledger/`（或 `docs/`）。
> 一个文件一个节点，文件名（ULID）**就是身份**；字段语义 / 不变量 / 预算 / 命令**只有一份真源**：
> **`pnpm tools requirements describe`**（机器可读加 `--json`）。本文件不复述 schema，只写口径与指向。

## 1. 为什么需要它（用户口径）

* 旧仓那套 `tickets/` 把**纯逆向分析的知识、子任务、行为核对**全塞进同一批单据里 ⇒ 208 张之后，
  **所有者已经无法阅读**（"无法了解具体单据情况了"）。根因不是票太多，是**层次不分**：
  "知道什么"与"还要做什么"混在一处、且没有父子关系可供逐层展开。
* 所以本模型刻意做两件事：**① 只放高层次、可收敛的工作**；**② 靠父子关系归类**
  （project / 常态化工作 / 有限任务都是节点），于是"某一支到哪一步了"可以由子树**聚合**出来，不用人肉汇总。
* 与业务**不耦合**：仓库迁移 / 模拟器开发 / 引擎逆向 / 翻译更新 共用同一套字段与守卫；
  **迁移本身只是其中一个节点**。

## 2. 两类节点的流程**不一样**（最容易踩的一条）

| | 需求（`req` / `spike`） | 缺陷（`bug`） |
|---|---|---|
| 锚的是什么 | **要交付的能力** | **被观测到的分歧**（现状 vs 预期 / 基线） |
| 收口（`done`）凭据 | `verify`：能力存在的守卫（或 `done_reason`） | `verify` **或** `repro`：分歧消失的证据；`repro` **同时就是重开条件** |
| 开着就必须写 | 判据 | `repro`（+ 建议 `severity`） |
| `dropped` 语义 | "不做了" | **"不是缺陷 / 不修"**（都必须写理由，禁静默关单） |
| `severity` | 不写（可能没有） | 写（`S0..S3`） |
| `superseded` | 不用 | **不许用**（"重复"请用 `dropped` + 理由指向那条） |

字段层面的落实：`repro` / `severity` 是**缺陷专属字段**（出现在别的 type 上会被守卫判红），
且 `repro` 与 `verify` 一样过**锚点棘轮**（指向的文件 / 测试名必须真实存在，否则红）。

## 3. 一条记录只能进一个地方（防止重演旧仓的不可读）

* **可收敛的工作**（有判据、会收口）⇒ **本目录**。
* **可再校验的知识 / 观察**（没有"完成"的概念）⇒ 知识台账 `data/ledger/`。
* **长调查 / 设计 / 沿革** ⇒ `docs/` 或 `git log`（本目录**不记** `history[]`：git 就是沿革）。
* 写不进预算的内容 ⇒ 要么**再开一个更低的节点**，要么**滚去知识台账** —— 这正是预算存在的意义。

## 4. 怎么查

```bash
pnpm tools requirements plan            # ★ 进度视图（按父子树打印 + 聚合状态）
pnpm tools requirements list            # 一览（type / status / 严重度 / 父 / 子树活节点数）
pnpm tools requirements show <引用>      # 一个节点：字段 + 子树 + 正文
pnpm tools requirements validate        # 5 条不变量（红 = 退出码 1）
pnpm tools requirements describe        # ★ 字段 / 不变量 / 预算 / 命令（本文件不含这些）
```

★ **`<引用>` 认三种写法**：完整 id / 裸 ULID、**唯一前缀**、**唯一后缀**（歧义就拒绝）。
后缀最实用 —— ULID 以时间开头，同一批节点的**前缀全都一样**，短别名只能靠尾部（例：`show 1CTKKQFC4`）。

## 5. 怎么改

**写路径只有一条**（规则住在领域模型 `tools/lib/requirements.mjs` 里），两个调用方共用它：

* **CLI**：`pnpm tools requirements`（缺省 **dry-run**，`--write` 才落盘）；
* **工作台网页**：`apps/workbench/` 里的"新建需求单"（`POST /api/nodes`）—— 只有**新建**，
  而且只在服务监听回环时开；它调的也是同一个 `planAdd()`，所以网页建的与命令行建的是同一种文件。

直接手改参数区、重命名文件、新建 `.md` 都会被 `validate` 判红。写入一律用规范形态（固定键序、同输入同字节）
并**写后回读复验，不绿回滚**。

```bash
# 开一个需求（父是哪个节点就写哪个 id；根是唯一的 parent: null）
pnpm tools requirements add --title "<标题>" --parent <父 id> --write

# 开一个缺陷（repro 是"存在证明 + 重开条件"，必须给）
pnpm tools requirements add --title "<现象>" --type bug --parent <父 id> \
     --severity S2 --repro "tools/test/xxx.test.mjs#用例名" --write

# 收口 / 改状态 / 补齐凭据
pnpm tools requirements set <前缀> --status doing --write
pnpm tools requirements set <前缀> --status done --verify "tools/test/xxx.test.mjs#用例名" --write

# 不可自动化的收口（文档 / 裁决类）用 done_reason；不做了用 dropped + dropped_reason
pnpm tools requirements set <前缀> --status done --done-reason "文档类：无守卫可挂" --write
```

★ **`--id` 只在"已有外部编号表 / 种子脚本"这类场景用**（缺省由工具生成 ULID；给了就以调用方为准，但格式与唯一性仍会被校验）。
★ 被守卫拒绝时**写后回滚**：`add` 会把刚写的节点整颗删掉，`set` 会把原节点原样写回（都不留残file）。
★ 字段 / 不变量 / 写入口的**真源**是 `pnpm tools requirements describe`；本文件只写口径与指向，不复述 schema。

## 6. 预算（"人看的"靠它）

单节点 **≤ 80 行**、节点总数 **≤ 40**。两个数都在 `pnpm tools requirements describe` 里，
改它们 = 改 `tools/lib/requirements.mjs` 的 `BUDGET`（改完守卫立刻按新预算判）。
超了**不要放宽**，先问："这一层是不是塞了本该更低层或本该进知识台账的东西？"
