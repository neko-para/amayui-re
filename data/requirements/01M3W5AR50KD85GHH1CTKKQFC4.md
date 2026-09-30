# 批 M3a · 需求台账（高层次进度视图：需求/缺陷 + 父子树 + 守卫）

- id: REQ-01M3W5AR50KD85GHH1CTKKQFC4
- type: req
- status: done
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 55
- verify: tools/test/requirements.test.mjs#写后回滚
- tags: [migration, requirements]

## 范围
`data/requirements/`（文本真源）+ `tools/lib/requirements.mjs`（领域模型）+ `tools/requirements.mjs`（CLI）：
`type` / `status` / `parent` / `blocked_by` / `verify` / `repro` / `severity` 等字段，
父子树与 5 条不变量（闭合性 / 单根 / 状态自洽 / 父不先于子收口 / 一屏预算）。

## 判据
1. 5 条不变量都能**红**（`tools/test/requirements.test.mjs` 逐条失红）。
2. 需求与缺陷的**收口凭据不同**：需求看 `verify`（能力在不在）、缺陷看 `verify` 或 `repro`（分歧还在不在）。
3. 被守卫拒绝时**写后回滚**（新增删净、修改逐字节还原）。

## 与 M3 的分工
两者**共用**派生查询层（`packages/ledger`，派生 SQLite 落 `.cache/`、可删可重建），
但**各自一份文本真源**：本目录是"就地编辑的当前文档"，知识台账是"append-only 事件流"。
