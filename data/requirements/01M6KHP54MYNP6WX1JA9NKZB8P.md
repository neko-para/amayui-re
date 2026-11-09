# S4-SSE：14 条 SSE 浮点形态（语料 0 条；执行到即响亮失败）

- id: REQ-01M6KHP54MYNP6WX1JA9NKZB8P
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/conformance_cases.cpp#sse-movss-load-zero-high
- tags: [emulator-lift, m1, s4]

## 一句话
`src/runtime/mnemonics.cpp` 为了让 A2（语料助记符映射）不漏拼写，登记了 14 条 **SSE 浮点**形态；
解码器（`decode.cpp` 的 `kOpMap` / `kPkMap`）里没有它们 ⇒ 落到 `Op::Other` ⇒
`execInsn` 走 `unimplemented()`：**打印 EA + 助记符 + 字节 + 寄存器 + guest 返回栈后停机**（响亮失败，不是 no-op）。
`.lst` 里这些形态 **0 条**（全文件 `xmm<数字>` 操作数 = 0）⇒ 今天撞不到。
本单是"真撞到时按什么口径做"的落点，**不是现在就要实现**。

## 范围（14 条，现查命令见 `docs/s4-fpu-audit.md` §1）

| 族 | 形态 |
|---|---|
| 搬移 | `movaps` `movups` `movss` `movdqa` `movdqu` |
| 位运算 | `xorps` |
| 转换 | `cvtsi2ss` `cvttss2si` |
| 算术 | `addss` `subss` `mulss` `divss` |
| 比较 | `comiss` `ucomiss` |

## 判据（做完怎么算完）
- 每条形态有一个实现落点（`PkOp` 子码或新 `Op`）+ 一条**改坏就红**的夹具，命名 `sse-<mnemonic>`；
- **标量形态的 128 位语义**是这族最容易写错的地方，必须逐条钉住：
  `movss xmm, m32` 把高 96 位清 0，`movss m32, xmm` 只写 4 字节；
  `addss` 等只改低 32 位、高 96 位**原样保留**；`cvtsi2ss` 同理；
- 与 x87 / MMX 的交互不变：`emms` 不碰 XMM，XMM 指令不碰 x87 的 tag word / TOP；
- `comiss` / `ucomiss` 写的是 **EFLAGS**（ZF/PF/CF），不是 x87 的 C0..C3 —— 且 QNaN 与 SNaN 的
  "quiet / signaling" 差别要按 Intel SDM 分开（`comiss` 对 QNaN 也置 invalid）。

## 边界
- ⛔ 不许为了让别名表里的名字"能跑"就先塞一个 no-op 或恒等实现 —— 未实现必须继续响亮失败。
- 优先级低于 M1 的 L2–L3 判据：只有 trace 里真出现这些助记符时才提前做。
- ★ `1,805` 个 `COLLAPSED` 区是**未解码字节**（CRT / C++ 运行时）⇒ "`.lst` 里 0 条"**不等于**
  "二进制里没有 SSE"；判据与缺口见 `docs/s4-fpu-audit.md` §2.3 / §6.2。
