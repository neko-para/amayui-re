# spec 基建缺口（ISA/POOL/VM 复核反馈）：守卫误标 / 证据档 / 选择器形状 / 工具盲区

- id: REQ-01M5AM4M5A5TV4APMPN0QWEA96
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- order: 66
- done_reason: 8 条逐条处置：1/2/3/7 已改（守卫扩到五原语 + 变异实测 1/1、PC/type 注释订正、CODEC 去重、shape 位测试边界入文档）；5 已移进 gaps；4/6 的裁决写进 core/README（旧草稿不回填 covers；语料型机制靠 accepted 台账升 reviewed，assets 守卫不单独撑档）；8 原已满足
- tags: [engine spec infra]

## 结果（2026-10-21/22）

| # | 处置 |
|---|---|
| 1 守卫误标 + 覆盖不足 | `repo:tools/test/engine-operand-types.test.mjs` 扩到**五条原语**（12/14/14/10/11）并把 `0x42B4B0` 从「float 取址」改为「写整型」（数字不动）；`SPC-OPERAND-UNSUPPORTED-THROW` 的证据改挂五原语用例 + `spec-pool.assets` 的"五原语入口"用例 + accepted 台账；变异清单加一条（取址 case 7 去标签 ⇒ 守卫红，已实测 1/1）。 |
| 2 `off.operands` 注释 | 改为「`0x5D898` = PC（当前 opcode 地址）；操作数按 1 起的序号算 `地址 = PC + 8×序号`（raw 格），type 在 `[地址−4]`、opcode 在 `PC`」；`emulator-model` 的对应断言与用例名同批订正。 |
| 3 编码零的指针 | `layout.mts` 的 `EVIDENCE.codecKey` / `encZero` 改为引用 `CODEC.keyField` / `CODEC.encZeroField`（不再裸地址）。★ 原判据「`layout:CODEC.*` 能解析」**已不可能满足**：`resolveStorage` 结构性拒绝所有 `layout:`；本条的实质（去重）已落地。 |
| 4 草稿模板 `covers` | **裁决：旧草稿不回填**（`.tmp/spec-draft/*` 是过程材料、不入库；规则已合并且 `covers` 由 audit 单独维护）。口径写进 `repo:data/spec/core/README.md` §6：`covers` 只约束新草稿。 |
| 5 `pilot.outOfScope` 的句柄分支 | 已移进 `gaps`：`GAP-CONTAINERS-HANDLE-BRANCHES`（chapter `containers`，note「句柄分支不是跳转表 case」，residual `unhandled` + next）；`outOfScope` 只剩真正"域外"的两条。 |
| 6 `reviewed` 与语料型机制 | **裁决：不改校验器**。语料型机制升 `reviewed` 的路径是补一条 **accepted 台账**（锚 EA），`@env assets` 守卫只作附加证据、不单独撑档（默认门禁不跑）。口径写进 `repo:data/spec/core/README.md` §3.1。 |
| 7 `shape access` 漏寄存器位测试 | 在 `repo:docs/02-engine/shape-census.md` §3.1 写明覆盖边界：位测试只认内存位，寄存器快照上的位判据不计入；并给实测例子（4 处内存位测试 vs 约 20 处寄存器判据 / 19 位）。 |
| 8 `draft:` 占位口径 | 已满足（`core/README` §6 原文），未动。 |

## 边界

* 第 1 条的变异只证"类型表缺标签会被抓到"，不证"五张表的语义都正确"。
* 第 3 条没有新增 `layout:` 可解析性（也不该有）；`CODEC` 是唯一真源。

## 怎么查

`node --test --test-isolation=none tools/test/engine-operand-types.test.mjs` · `node tools/mutate-check.mjs --match engine-operand-types` · `pnpm tools spec report`（gaps）。
