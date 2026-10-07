# opcode → handler EA 的机械查询（模拟器 A 类流水线的第一步）

- id: REQ-01M4AER5K0S0ZATTPE00ES6QJS
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/opcodes-handlers.assets.test.mjs#★ 逐条钉死
- tags: [emulator]

## 范围

把 **`opcode → handler EA`** 变成一条**机械查询**（今天没有这个能力）。

★ **为什么需要它**：模拟器开发的主线是「实现指令 NN」（技能 `amayui-emulator` 的 A 类流水线），
而它的**第一步**就是"这条 opcode 的 handler 在哪"。今天只有两条歪路：
① `packages/age-format/src/engine/handlers.mts` —— **只有 39 条**（纯数值族那批）；
② `tools/test/disasm.test.mjs` 里那段**现算分派表**的提取逻辑（544 条齐全，但它活在**守卫**里，不是可调用的动作）。
⇒ 每实现一条新指令都要把"现算"重走一遍，而这套提取逻辑**已经有守卫在验**（见判据）⇒ 不该有第二份实现。

## 判据

一条工具动作（域/动作名待定，例 `pnpm tools <域> handler --opcode 0xNN`）能给出：

* **handler 符号 + 起点 EA**：表基址 `Engine+0xA509C`、`opcode = (偏移 − 0xA509C) / 4`；
* ★ **没被覆写的 opcode 必须明说"走默认 handler `sub_418E30`"** ——
  表长 **1024 格、初值全是默认**（`rep stosd`），之后才 544 条覆写 ⇒ "未赋值"不等于"没有 handler"
  （台账 `Engine+0xA509C/dispatch-init-REWORDED` 纠正过一次这个说法）；
* 提取结果与旧仓 `analysis/opcodes.json` **逐条一致**（544 条）—— 这件事守卫 `tools/test/disasm.test.mjs`
  已经在验 ⇒ **复用它的提取逻辑**，不要第二份实现；
* 语料不在场时**报错**（不是"读不到就当空"）。

## 备注

★ 缺它**不阻塞** A 类流水线（能临时读守卫里的提取逻辑），代价是**每条指令**都要重来一次 ——
所以它是"做起来很快、省下来很多"的那一类，而不是前置。
