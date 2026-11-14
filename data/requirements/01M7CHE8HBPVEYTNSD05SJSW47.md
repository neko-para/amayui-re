# ADR · 最终形态 = Shape A：guest 代码 lift 成 IR 并与 shim 联编成单一二进制（解释器只作测试 oracle）

- id: REQ-01M7CHE8HBPVEYTNSD05SJSW47
- type: decision
- status: done
- done_reason: 已裁决（用户 2026-11-14）：最终形态 = Shape A（整代码段 lift 成 IR 与 shim 联编成单一二进制）。四条口径 + 三条落地附加口径（发货产物不含解释器 / .ll 不入库、构建期从 PE 字节生成 / 起跑线 = PE entry 而非 _WinMain）见正文。首个落地件 = REQ-01M7CHEA5HQ1905ZAJQYHX97CN（已收口，读数 OBS-20261114-15）。decision 不可变，推翻需新增并 supersedes。
- tags: [lift, arch]

## 决策

**最终形态 = Shape A**（用户 2026-11-14 口径：「预期最终需要按 shape A 实现」）：

guest 代码段（`.text` 等）→ LLVM IR → 与 runtime + Win32/D3D shim **一起编链成一个二进制**。
**解释器不在执行路径上**：发货二进制里没有 dispatcher、没有 decode、没有块缓存。
解释器只留在**测试构建**里当差分 oracle。

## 为什么不是 Shape B（"更强的 override 表"）

| | **Shape A** | **Shape B** |
|---|---|---|
| 结构 | lifted 代码与 shim 编链成同一个二进制 | lifted 块被 dispatcher 叫进去、跑完**回解释器** |
| 每次跨块成本 | 一次间接跳（`EA → 代码` 表，~2–5 ns） | 一次 dispatcher 往返（≈ 解释器的 `b` ≈ 107 ns/块，外加函数出口链） |
| 能不能拿到 spike 的 57–118× | 能（`OBS-20261114-12`） | 基本不能（只有"函数内自环"能） |
| 与手写 override 的关系 | 换后端；手写 override 退成"局部更优件" | 就是"更强"的 override 表 |

Shape B 只是验证 lift 机制与差分的**脚手架**，不是目标：它在热路径上每块都付解释器那一份钱。

## 四条口径（硬）

1. **间接控制转移不是"不可 lift"**：`call reg`(3028) / `call [mem]`(15) / `jmp reg`(26) / `jmp [mem]`(6) / `ret`
   一律 = `EA → 宿主函数指针` 表查 + 间接跳。普查里那 32 个"静态解析不了"的站点全是"跳函数指针"的
   3–7 条指令 stub（目标即函数入口）⇒ 可 lift。**唯一 residue = SMC**。
2. **SMC（guest 写自身代码）⇒ 响亮停**（硬失败），不做运行时补译：arm64 macOS 的 JIT 要
   `com.apple.security.cs.allow-jit`，与"构建期 AOT、两份平台同一份产物"冲突。
3. **guest 指针仍是 32 位平坦地址**：lifted 代码只用 `base + zext(EA)`；EA **必须零扩展到 i64** 再接 GEP
   （`i32` 下标会符号扩展 ⇒ EA ≥ 0x8000_0000 全错，`OBS-20261114-12`）。
4. **guest 栈是数据**（`push`/`pop`/`ret` 是对 guest 内存的 load/store），宿主栈只承载我们自己的帧。

## 落地的三条附加口径（2026-11-14 用户裁决）

1. **发货产物不含解释器**：最终二进制里没有 dispatcher / decode / 块缓存；解释器只以**测试构建**存在
   （差分 oracle），⛔ 不保留"解释版"作为第二个可交付产物。过渡期允许"declined → 解释器"的混合模式，
   但它是**会失败的负控**（`--lifted-only`，原型已实现），最终判据是 `handoffs == 0`。
2. **`.ll` 不入库**：构建期由 generator 从 PE 字节生成（手写的 44 条留作 golden 对照）。
   理由：整 `.text` 的 IR 文本是百 MB 级，而真源是 PE 字节 —— 生成物入库会让真源分叉。
3. **起跑线 = PE `entry`**（连 CRT 启动一起 lift），⛔ 不从 `_WinMain` 起：否则要为 CRT 启动造一堆
   只在这一处存在的 shim（`_cinit` / `__fpmath` / SEH 注册 / 模块句柄…）；而且 PC=53 的那条 `fldcw` 链
   就在 `_cinit` 里（`OBS-20261114-14`）—— 从 `_WinMain` 起会把那条决策依据切到 lifted 世界之外。

## 与既有实现的关系

- **前置读数**：`OBS-20261114-11`（按执行指令数加权的可 lift 覆盖率 A+B+C = 96.124% / 93.409%）、
  `OBS-20261114-12`（57–118×、块边界 1.92 ns/块、IR 零新 runtime 原语）、`OBS-20261114-15`（原型两阶段）。
- **`native_overrides`（函数级 / 段级）不是替代品**：它是"语义上比 lift 更优"的手写件（`L3-crc-01`）。
  Shape A 之后它可以继续作为"被 lifted 代码调用的一等函数"存在，但**提速职责由 lift 承担**。
- **跨平台**：`.ll` 是同一份源，x86-64 与 arm64 都编；浮点模型见浮点 ADR（`REQ-01M7CHE9T4NKHGKY2TQGVX6WK3`）——
  那是**一致性**决策（arm64 只能软实现 fp80），不是效率决策。

## 落地

首个落地件 = 原型（`REQ-01M7CHEA5HQ1905ZAJQYHX97CN`，阶段 A/B 均已收口）：真实函数链 lift + `EA → 代码` 派发 +
shim trampoline，解释器不在环里；读数进 `OBS-20261114-15`。整 `.text` 的落地按
`REQ-01M7CD1NRK9P5WKR5PJ6GW3067` 的立项判据推进。
