# spec：补启动前段指令族（数值/位/薄标量/按索引写表/转发/事件门）

- id: REQ-01M5FATYZ32YAFRNER8VCJTRYA
- type: req
- status: open
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- tags: [spec, ops, boot]

## 一句话

通往 LOGO.BIN 入口的启动前段里，有一批指令在 spec 里**没有条目**（`60-ops` 的 48 个 opcode 只覆盖了少数薄标量）。本单把它们按族补成规则，避免"实现里有、规范里没有"（那是把 emulator 当规格）。

## 要补的指令（按族）

| 族 | opcode | 语义（证据位置） |
|---|---|---|
| 数值双目/比较 | `0x52 mul` · `0x53 div` · `0x54 mod` · `0x57 or` · `0x58 sar` · `0x59 shl` · `0x5a eq` · `0x5b ne` · `0x5c lt` · `0x5d lte` · `0x5e gr` · `0x5f gre` · `0x56 and` | 与 0x50/0x51 同族（旧实现 `binInt`/`cmpInt`；纯数值族台账 `01M48FCQ5D692W28473A3C4967`） |
| 位操作 | `0x135` / `0x136` / `0x13f` | 位置/位号与越界口径（位号 > 0x1f 的三种处置不同） |
| 薄标量写 | `0xfe`（≤0x1f）· `0x10f` · `0x248` · `0x261` · `0x8b` · `0x260`（四格）· `0x1a4`（成对）· `0x76`/`0x77`（`bswap24`）· `0x2bd`（二选一常量） | 知识层 `ENGINE_SCALAR_WRITES` 的条目 + 旧实现 |
| 按索引写的表 | `0x107` · `0x10b` · `0x30a` · `0x10c` | 索引操作数位置不同、越界口径 skip/throw、`0x10c` 的目标表未取证（只记录） |
| 转发型 | `0x308` · `0x75` · `0x1a5` · `0x2fe` · `0x197` | 只写"交给谁 + 实参形态 + 不改脚本可观察数据"三句 |
| 事件/门 | `0x143`（事件刷新，队列空分支）· `0xae`（存档版本检查的早退分支）· `0x130`（show-logo 标志，初始化 1） | 各自的分支条件与未建模处要写成残差 |

## 要做什么

1. `60-ops` 按族补条目（每族一到多条，别一个 opcode 一条流水账）：数值/比较族可合并；薄标量族按"落点 + 形态"列表化。
2. `bindings`：站点 = 各 handler 的读取/写入行（EA 从 `.lst` 取）；`evidence` = 现成台账 + `tools/test/emulator-new-prologue.test.mjs` 的用例。
3. `audit`：`handled` + 残差（`0x143` 的事件队列与通道表、`0xae` 的修复分支、`0x107/0x10b` 的 skip 口径）。
4. 章首计数同步（48 → 按实际条数）。

## 判据

- `pnpm tools spec validate` 13/13；每条有 `binding.evidence`；
- 守卫会红（`emulator-new-prologue.test.mjs` 的 14 条 + `mutate-check` 的 8 条变异）。
