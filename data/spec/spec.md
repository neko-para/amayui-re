# `data/spec/spec.json` —— 说明书

> 同名说明书（约定见 `../../docs/00-origin/decisions.md` §6，由 `tools/test/json-docs.test.mjs` 守）。
> 字段 / 枚举 / 不变量 / 操作的唯一真源是 `pnpm tools spec describe` —— 本文件不复述 schema。

## 这个文件是什么

**manifest**：规范标题 + 章节顺序（`chapters[]`：`id` / `file` / `title`）。
它**不装规范正文** —— 正文在 `chapters/*.md`；本文件只规定阅读 / 分发时的章节顺序。

## 怎么查

```bash
pnpm tools spec describe                 # 字段 / 不变量 / 操作（schema 真源）
pnpm tools spec validate                 # 章节文件存在 / 规则标记 / 跨文件一一对应
```

## 怎么改

1. 新增章节：把 Markdown 放进 `chapters/`，在 `chapters[]` 加 `{id, file, title}`，
   并在 `audit.json` 的 `chapters[]` 加同一条 `id`。
2. 调整顺序：只改 `chapters[]` 的顺序（正文与审计都按这个顺序展示）。
3. 规则：在章节里加 `### 标题` + `<!-- rule: ID -->` + 正文，并在 `bindings.json` 的 `bindings[]`
   （字段 `rule`）与 `audit.json` 的 `rules[]`（字段 `id`）各登记同一条规则。
4. 未写的章节：不要放进 manifest；放 `audit.json` 的 `planned[]`。
5. 改完跑 `pnpm tools spec validate`。
