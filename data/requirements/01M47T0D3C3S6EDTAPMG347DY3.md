# 拆跨域守卫：10 个按域归位（4 个台账守卫 → M3；2 个测试组织棘轮留在本域；2 个文档域）

- id: REQ-01M47T0D3C3S6EDTAPMG347DY3
- type: req
- status: open
- parent: REQ-01M47SGXYWPQ1KWET2J49TJQST
- order: 611
- tags: [emulator ledger]

## 范围
把旧仓模拟器 `test/` 下的 **10 个跨域守卫**（旧仓用 pragma `@subsystem` 标过，正好 10 个 `ledger`）
按**它真正校验的域**拆回各域，而不是跟着模拟器一起重写。

| 守卫（旧仓 `test/*.test.ts`） | 真正校验什么 | 归哪 | 依赖无头核心？ |
|---|---|---|---|
| `journal` · `capability-ledger` · `script-ledger` · `ticket-ledger` | 台账 schema / 锚点棘轮 / 守卫用例存在 / md 与数据同步 | **台账域 → M3** | ★ **不需要**（实测零 `../src` import，只读 `analysis/`+`tickets/`+`docs-new/`） |
| `capability-gap`（能力缺口：被当 no-op 跳过却收到非平凡实参） | VM 解释器的实参显著性 | 本域（vm） | 需要 |
| `no-dead-writes`（死写棘轮：字段写了没人读） | 引擎 / 场景 / 渲染模型的字段读写 | 本域（vm+renderer） | 需要 |
| `organization`（每个 `*.test.ts` 必须声明 `@tier/@kind/@subsystem`） | 测试组织自身 | 本域（测试设施） | 不需要 |
| `harness-convergence`（自造 harness 变体棘轮） | 测试组织自身 | 本域（测试设施） | 不需要 |
| `doc-model`（`docs-new/**` 的 `kind`/`state` 状态机 + 沿革不进生成物） | **文档域约定** | 文档域（新仓**尚无该域**） | 不需要 |
| `audit-report-completeness`（审计报告自包含 + 归档清单计数一致） | 文档域约定 | 文档域 | 不需要 |

## 判据
（待写实。要点：**"拆出去"的验收 = 目标域真的有这条守卫在跑**，而不是"文件搬了个地方" ——
台账那 4 条在 M3 落地后，本节点只需断言"本域不再持有它们、且它们由台账域执行"。
4 条不需要无头核心的（台账 2 组 + 测试组织 2 条）**可以先做**，不必等核心写完。）

## 沿革
★ **本节点存在的原因**：旧仓的 `@subsystem ledger` 标签**把两类东西混在一起** ——
"校验台账的守卫"与"用台账当输入的守卫"。前者只是**放错了目录**（只读数据文件、零产品代码依赖），
后者才是"跟模拟器一起重写"。混着看会得出"拆守卫必须等整个模拟器落地"的错误结论。
