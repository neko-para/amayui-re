# `data/spec/bindings.json` —— 说明书

> 同名说明书（约定见 `../../docs/00-origin/decisions.md` §6，由 `tools/test/json-docs.test.mjs` 守）。
> 字段 / 枚举 / 不变量 / 操作的唯一真源是 `pnpm tools spec describe` —— 本文件不复述 schema。

## 这个文件是什么

**机器绑定**（规范的出处）：每条规则一行，`rule` 关联章节 Markdown 里的 `<!-- rule: ID -->`。
包含 `sites[]`（EA 是唯一合法的机器锚）、`functions[]`、`storage[]`（`layout:` / `shape:` 指针）、
`evidence[]`（`KN:` 台账记录 / `guard:` 可执行守卫用例）。

它回答"这条规范凭什么"，⛔ 不是规范本身；布局只经 `storage` 指针引用。

## 怎么查

```bash
pnpm tools spec describe                 # 字段 / 不变量 / 操作（schema 真源）
pnpm tools spec show <id|前缀>            # 打印规则 + 每个绑定的证据有效状态
pnpm tools spec at <EA|sub_xxxxxx>        # 反向：站点/函数 → 规则
pnpm tools spec validate                 # storage 指针可解析、证据可解析、三份文件一一对应
```

## 怎么改

1. 加/改绑定：只动 `bindings.json` 对应 `rule` 的那一行；`rule` 必须存在于某个章节的规则标记里。
2. `sites[].ea` 必须是语料里的指令行首；`functions` 必须是 `sub_XXXXXX` 形态；
   `storage` 只认 `layout:<token>` 与 `shape:<type>`（会真的解析到 `layout.mts` / `types.json`）。
3. 证据按 id / 用例名引用；换一条证据就重跑 `validate`（`reviewed` 会因证据变弱而红）。
4. 改完跑 `pnpm tools spec validate`。
