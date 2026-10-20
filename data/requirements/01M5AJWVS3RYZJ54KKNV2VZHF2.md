# CORE-ISA 批次：指令协议取证、条目与 EA 映射

- id: REQ-01M5AJWVS3RYZJ54KKNV2VZHF2
- type: req
- status: done
- parent: REQ-01M5AJWPM4DXWG3XR6W7JR6XHD
- order: 10
- done_reason: 9 条 CORE-ISA 规则已合入 10-format.md，77 个站点进 bindings，证据挂 spec-isa.assets.test.mjs（564 脚本 0 失败）；TABLE-RECON/OPERANDS 作为 audit 残差保留（前者缺独立守卫、后者归 CORE-POOL）
- tags: [engine spec core isa]

## 目标

把"脚本字节 → 指令"做成可独立实现的规范：脚本头、`argc` 边界、`type`/`raw`、指令表。
迁移自 `data/spec/chapters/10-format.md`（内容只搬，不重写；补映射与 marks）。

## 判据

1. 闭集 = 指令表条目；每条在条目里"被回答 / 未规定 / 残差"三选一，不许沉默。
2. `argc` 由**数据**验证：解码真实脚本时指令流正好落在头部声明的边界（区域算术）；
   失步 = 0，或每一处失步都有解释（表缺口 / 坏数据 / 失步原因）。
3. 每条条目有 `sites`（解码 / 调度 EA）+ `functions` + `evidence`。
4. 正文不含 EA / 偏移 / 台账 id（自包含不变量绿）。

## 合并状态（每次更新这一段）

* 已合入 9 条：`HEADER-MAGIC` / `HEADER-FIXED32` / `HEADER-SUBHEADER` / `OPERAND-COUNT` / `INSTRUCTION-ENCODING` /
  `OPCODE-DOMAIN` / `BODY` / `TABLE-POINTERS` / `EXEC-END`（全部 `draft`；站点 77 个）。
* 未合：`TABLE-RECON`（argc 对账，缺独立守卫）、`OPERANDS`（归 `CORE-POOL`）。
* 证据缺口：9 条共用一条 assets 守卫（签名 / 头长 / 表偏移 / 域 / 表区不越界）；逐条的更细证据待补（`KN:` 或分条 guard）。

## 非目标

* 不写逐 opcode 语义（那是 `CORE-OPS`）。
* 不把指令表的**名字**当事实：助记符是编造的，只许用编号 / `argc` / 字节。
