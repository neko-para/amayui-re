# 位/格类残差的写者闭包生产者（语料写点 → opcode 集合）

- id: REQ-01M696ZMKDJQKJWRSYS97P8P9G
- type: spike
- status: dropped
- parent: REQ-01M6961WQTDWVMY0X4V6ZN9JR7
- dropped_reason: 本轮口径：需求树只留能推进当前交付物（lift / 翻译 / 基建）的活节点。spec 内容批次 / 残差收口 / 证据重锚这一支服务的是「按 spec 重写」的实现线；2026-11 起实现线改为 emulator-lift（执行原机器码），spec 对 lift 只作调试对照、不是真源（apps/emulator-lift/README.md §1）。本支自 2026-10-19 起无推进。需要时从 git 恢复。
- tags: [spec residual, tooling]

## 背景（为什么还要一条）

`REQ-01M6961WQTDWVMY0X4V6ZN9JR7` 已落 `routes footprint`（动态 op/gate/cell + 静态 op）与
`spec residuals --footprint` 三桶 join。opcode 类判据是**机械**的（`covers.ops` / 标题里的 `0x…` ∩ 足迹）。

**位 / 格类**目前只做了一半：join 已有 `writers` 入口（位号 / 地址 → 写它的 opcode 集合），
但没有**生产者** ⇒ 不命中动态 `gate` / `cell` 的位 / 格残差一律记「判不了」，无法落到「路径外」。
这正是需求单判据里「查不到则做写者闭包」那一条。

## 目标

做一个**只读派生工具**，从语料 / 绑定机械产出"写者闭包"：

1. 输入：位号（0..31）或引擎格地址 / `shape:` 成员指针。
2. 输出：写它的 **opcode 集合**（不是函数名）。
3. 数据来源只许两类：`data/spec/bindings.json` 的 `sites` / `functions` / `storage`，以及
   `corpus/disasm` 的写点（`disasm-fn` 的调用者 / 反汇编写指令）。
4. 闭包为空或有数据时，`spec residuals --footprint` 的位 / 格类才可能从「判不了」变成「路径外 / 路径内」。

## 判据

- 闭包必须**可重算**：不手写位→opcode 表；每次从当前数据 / 语料派生。
- 生产不出闭包时**显式**输出"没有数据"（join 仍记「判不了」），⛔ 不许用空集冒充"没有写者"。
- 守卫（`@env pure` 或 assets）：合成写点 ⇒ 得到预期 opcode 集合；删掉写点 ⇒ 集合变空 / 变"无数据"。
- 不得改 `data/spec` 的 schema；闭包是派生视图，落 `.cache/`。

## 非目标

- 不要求覆盖引擎对象字段的**运行期**读写（那需要逐字段插桩；`footprint.h` 已写明边界）。
- 不把闭包当可达性证明：它只回答"谁写"，不回答"这条路径一定会写到"。
