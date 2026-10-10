# 转发的**完整实参**没有记录（只在台账/注释里）—— 常量与引擎槽实参、尾跳 vs 调用

- id: REQ-01M4E603JJTVVWBJHKKWT362SR
- type: req
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- verify: tools/test/emulator-engine-scalars.test.mjs#★ `0x78` 的欠账看得出是**尾跳**（`transfer: tail`）、`0x76` 是 `call`；实参是子对象槽不是 op1
- done_reason: 落点：① 知识层 `packages/age-format/src/engine/layout.mts` 新增 `CallArgForm`（`operand` / `const` / `slot` 三形态）与 `ScalarCallAfter`（`callee` + `transfer: call|tail` + `args`），`ENGINE_SCALAR_WRITES` 标上 `ScalarWrite` 接口（原先**无注解** ⇒ `callsAfter` 被推断成 `string[]`，形态与尾跳**根本没处表达**）；四行 `callsAfter` 按事实写实（`0x78`/`0x2db` = `tail`、`0x76`/`0x77` = `call`，实参 = 子对象槽 `Engine.d21324`）；`PROLOGUE` 的 `0x70`/`0x71` 行新增 `callArgs`（`0x70` 末位 = **写死的 0**；`0x71` = 接收者槽 + op1 + **引擎槽 `Engine.d97055`**）。② 模型 `apps/emulator/src/vm/ops.ts` 的欠账载荷新增 `transfer` 与 `argForms`，`args` 记实参真值；`layout.mts:217` 的口径段与 `ops.ts` 的 `SCALARS_BY_OPCODE`/`scalarHandler` 消费者同步。③ 收口凭据 = 两条守用例（`0x70` 的常量实参 0 与 `0x71` 的引擎槽实参被记下 / `0x78` 的欠账看得出是尾跳）。台账：`KN-01M4GZ0G441A4A0P0C166B1G26`（accepted，`Emulator.vm`，锚 = 7 个 EA + 上述两条守卫）；它 replaces 了 `opcode/0x71`（撤回记录 `KN-01M4GZ037S7D186F5W6Y7E7F4H`，只更正"不在记录里 / 没有守卫"两句）。★ 未声称：其余转发 handler 的非操作数实参未逐个取证（欠账仍按"只有操作数"记，少记 ≠ 只收这些）。

## 缺口（`PROLOGUE` 的转发记录**只有符号，没有实参**）

本层对"未建模的子系统调用"只记 `callee` + 操作数（`system.engine.forward`）。但 handler 传给 callee 的实参
**不止操作数** —— 有一批是 handler 里**写死的常量**或**引擎槽**，它们不在任何记录里。

### 两个实测例子（锚 = EA，见台账条目）

* `0x70`（handler `sub_41ED20` → callee `sub_45D660`）：第 6 个实参是**写死的 0**，
  而 callee 里 `if (a7 >= 0)` 因此**恒真** ⇒ 这条分支在本层完全看不见。
* `0x71`（handler `sub_41ED80` → callee `sub_45EC60`）：同一位置传的是 `Engine[97055]`（当 bit31 门用），
  而**这个槽谁写的没取证**。

### 另一处同类（口径要细分）

`0x78` / `0x2db` 的 `callsAfter`（我在知识层登记成"写完标量之后还有一次子系统调用"）**其实是尾跳**
（`jmp sub_459F40`），不是 `call`。在 `[保真欠账]` 里两者**长得一样** ⇒ 读日志的人分不出"会回来"与"不回来"。

## 要做什么

1. 知识层的 `callsAfter` 记录**实参形态**（至少区分"操作数" / "常量 N" / "引擎槽 `Engine.dNNN`"）；
2. `callsAfter` 增加"**尾跳** vs **调用**"的区分（两者在欠账里必须能分开）；
3. 判据：一条守卫能证明"callee 的实参里那个常量/槽被记下来了"，
   且一条 `0x78` 的欠账记录能看出它是尾跳。

## 为什么值得进树（而不是只写在注释里）

这是一个**系统性**盲点：只要 handler 给 callee 传了操作数以外的东西（常量、引擎槽、`op1+12` 这类算术），
本层就**看不见**它 —— 而"看不见"会让"这条分支永远不会走"与"走了但没记录"变成同一种表现。
