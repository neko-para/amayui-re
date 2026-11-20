# 🐞S3 requirements：位置参数形式 show|set <id> 静默退回 plan（退出码 0、什么都不改）

- id: REQ-01M7W2WKFA21NJ0YRYRP950V5X
- type: bug
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: tools/requirements.mjs#out.rest.push(a)
- severity: S3
- tags: [toolchain, requirements, ledger]

## 现象

`node tools/requirements.mjs show <id>`（或 `set <id> --status done --write`）**不报错、不改动**，
而是把位置参数丢进 `rest`、动作退回缺省 `plan` ⇒ **打印一遍进度树并以 0 退出**。

实测（2026-11-21 / mac arm64）：

- `node tools/requirements.mjs show REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ | head -4`
  ⇒ 输出 `进度（活节点 …）` 与根节点，**不是**该节点正文；
- `node tools/requirements.mjs --show REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ | head -4`
  ⇒ 正常输出 `# A · 整 .text 静态 lift…`。

## 为什么会静默

`parseArgs()`（`tools/requirements.mjs:331`）把非 `--` 开头的参数一律 `out.rest.push(a)`；
只有 `--show` / `--set` 才会把 `rest[0]` 当 id。未识别的位置参数没有任何检查
（"不认识的选项"只对 `--xxx` 生效），因此 `show <id>` ⇒ `action=null` ⇒ `'plan'`。

## 影响

按自然写法准备 `set <id> --status done --write` 的人 / agent 会得到"绿"的假象：
退出码 0、屏幕上有一棵进度树（看起来像正常输出）、需求单**一个字都没改**。
2026-11-21 的 open 单审计里，审计 agent 差点据此把 6 条收口命令当成"已执行"（后发现并改用 `--set`）。

## 修法（建议）

1. `main()` 在动作分派前：`args.action === 'plan' && args.rest.length > 0` ⇒ 打印用法并 `exit 2`。
   （`plan` 本身不接受位置参数。）
2. `--show` / `--set` 多余的 `rest`（>1 项）同样报错，避免 `--set a b` 静默丢 `b`。
3. 守卫：`tools/test/` 加一条进程级用例 —— `node tools/requirements.mjs show <id>` ⇒ 退出码 2、
   stdout 不含 `进度（`。⛔ 不要断言具体 usage 文案。

## 边界

- ⛔ 不改 `--show` / `--set` 的正常行为；⛔ 不改 `plan` 的输出格式。
- 与 `REQ-01M7ARWN4SQ92FF6RZHK7EMZSY`（批量修复悬空）无关：那条是"写后全树校验 + 回滚"，这条是"参数被吞"。
