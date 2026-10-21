# spec：补启动前段指令族（数值/位/薄标量/按索引写表/转发/事件门）

- id: REQ-01M5FATYZ32YAFRNER8VCJTRYA
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [spec, ops, boot]

## 结果（2026-10-22）

按族补了 **12 条规则 / 35 个 opcode**（`60-ops-*` 现共 60 条 / 89 opcode）：

| 规则 | opcode |
|---|---|
| `CORE-OPS-NUMERIC-ARITH` / `-COMPARE` | `0x52`–`0x5f`（13） |
| `CORE-OPS-BIT-SET-RESET` / `-TEST` | `0x135` / `0x136` / `0x13f` |
| `CORE-OPS-SCALAR-THIN-PROLOGUE` / `-PAIR-PROLOGUE` / `-VARIANT-PROLOGUE` / `-TABLE-10C` | `0xfe` `0x10f` `0x248` `0x261` `0x8b` / `0x1a4` `0x260` / `0x76` `0x77` `0x2bd` / `0x10c` |
| `CORE-OPS-FORWARD-PROLOGUE` | `0x308` `0x75` `0x197` `0x1a5` `0x2fe` |
| `CORE-OPS-EVENT-143` / `-SAVE-VERSION-AE` / `-SHOW-LOGO-130` | `0x143` / `0xae` / `0x130` |

`0x107` / `0x10b` / `0x30a` **不重复写**：合同在《引擎级状态》三条标量表，章首已注明、不计入 ops 计数。

## 边界

* 全部 `draft`：新证据多数是 pure 守卫；`FORWARD-PROLOGUE` 只有一条 proposed 台账（弱证据），没有升 reviewed。
* 以下 opcode 的**部分效果**只能写"本版不规定"并进残差（逐条附 handler）：`0x308`（平台受理不可观察）、`0x1a5`/`0x2fe`/`0x75`/`0x197`（入口内部写点）、`0x10c`（目标表身份）、`0x143`（255 格表/事件容器、队列非空分支）、`0xae`（版本判定/修复分支）、`0x130`（标志消费者）、`0x2bd`（常量 700 的含义）、`0xfe`/`0x10f`/`0x248`/`0x261`/`0x8b`（各格领域身份）、`0x76`/`0x77`（字体重建入口内部）。

## 怎么查

`pnpm tools spec chapters` · `pnpm tools spec show <规则 id>` · `pnpm tools spec validate`。
