# 批 S1g · T0（obj0 根类型）覆盖率欠账：读数三档 + 逐批补齐（每个成员要 witness EA）

- id: REQ-01M548MHRWEFW3SA82QQDD3F81
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 47
- verify: tools/test/shape-census-debt.test.mjs#★debt-01
- tags: [engine shape]

## 目标

把 **obj0 根类型**（现名 `T_Command`；类型命名棘轮之后 `T0` 这个名字已不存在）的"观察了但没声明"变成一份
**可复现的清单**，并逐批补齐：每个补上的成员都必须有 witness EA、且补完判定器仍全绿。

## 结论（裁决：本单清零；最后一批**不是欠账**）

* 成员表由 **S2 的 33 个批次**逐批补齐（内嵌子对象 / Engine 根锚定 / BaseObject / Command / Font / Scene /
  FileDB / Input / Sound …）。本单此后只负责**读数与裁决**，不再自己补成员。
* 最后剩下的 5 个偏移（`0x4` `0xc` `0x10` `0x14` `0x18`，32 处站点，只来自 `sub_4224E0` / `sub_424970`）
  **不是"没声明"，是助手把基址算错了**：`0x422527 lea esi,[edi+edx*4]` 的 `edx` 来自跳表
  （`ds:0[eax*8] − eax`），解不出来时旧代码把目标寄存器当成 `this+0` ⇒ 下游 `[esi+4..+0x18]` 被记成
  `obj0+4..+0x18` 的**假观察**。基址其实是 `Engine + (跳表值−序号)*4`（每条描述记录一段），
  **不属于 `T_Command` 的成员表** ⇒ ⛔ 不许为清零给它编成员。
* 修法：`unusable`（"`this` + 未知项"）随 `lea` / 指针算术传下去 ⇒ 下游进「**位移不可用（索引未解析）**」档
  （`obj0` 相对 7644 → 7520、位移不可用 317 → 441，守恒③ 仍等式成立）。
* 守卫：`tools/test/shape-access.test.mjs` 的 `★access-07` · 变异清单第 46 条（把 `unusable` 改回 `false` ⇒ 当场红）。

## 判据（可复核，判据本体在命令里）

1. **`pnpm tools shape census --debt`** —— 三档读数：已声明 / 落在 `count:null` 窗口内（**判不了**，
   ⛔ 不算已声明；口径同 S1f）/ **未声明**（清单本体，按站点数降序、带 witness EA）。
   本单收口判据 = **未声明 = 0**；`--debt-root` 可换类型，类型名写错**当场报错**（不许静默返回空表）。
2. 判定器全绿：`pnpm tools shape claims data/shape/carriers.json`（G1..G9 · J3 · 守恒①~④）。
3. 欠账分类的算术：`tools/test/shape-census-debt.test.mjs`（三档不许混 · 无界窗口 · 闭区间会吞掉一格）。
4. 变异自检：`pnpm tools mutate check --match shape`（含本次两条）。

## 已完成的第一批（10 个偏移 / 17 处残差清零；witness 保留）

判据：17 处 `⚪` 逐条 `--verify` 过（全是 `lea <reg>,[esi+imm]`，`access=a`、`stride=0`、`indexSym=null`），
成员只写 `{off, width:4}`（`evidence` 不进产物）。

| off | witness EA |
|---|---|
| `0x8` | `0x41c95a` `0x429588` `0x41abe3` |
| `0x408` | `0x428c00` |
| `0x1ee8` | `0x42956c` |
| `0x511c` | `0x428c4a` |
| `0x55d8` | `0x428c3f` |
| `0x4ed10` | `0x428d61` |
| `0x5d808` | `0x428ab3` |
| `0xa609c` | `0x41abdd` `0x41ac01` `0x429533` `0x429541` `0x429578` `0x429595` |
| `0xaab48` | `0x428bf9` |
| `0xcac0` | `0x428c2e` |

★ 旁证（**不当判据**）：`0x8` 的 3 处访问后面紧跟 `push 400h`（当 Buffer 传），而 `0x408` 恰为 `0x8+0x400`
⇒ 像是"一块 0x400 字节缓冲"。但成员表**只按观察写 `width:4`**；"它是 char[0x400]"是语义推断，走台账准入。

## 三类"不是欠账"（别当覆盖率）

1. **数据段符号**（`ds:` 前缀，无基址寄存器）⇒ 形状层表达不了（单列桶，⛔ 不是通过）；
2. **分支合并后的未解析基址**（实测 `sub_41C6A0`：`add esi,8` 只在抛异常分支上，汇合点两条路不同
   ⇒ 助手判 UNK 是**正确**的；我曾以为是 bug，打补丁实测 **0 变化**，补丁作废）；
3. **跨两级以上的指针链** ⇒ 不判，用**局部类型载体**表达；
   其中"层级 3 的指针只在 caller-saved 寄存器、活窗口被 `call` 打断"的那一小撮**结构性判不了**
   （实测 `0x42165f` / `0x42189f`），必须留在这档。
4. ★ 新增第四类（本单最后清零的正是它）：**助手丢掉"索引那一项"** ⇒ 位移是假的，不是"没声明"
   （判据：`lea` 的目标寄存器带 `unusable` 之后，下游必须进「位移不可用」）。
