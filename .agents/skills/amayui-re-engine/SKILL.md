---
name: amayui-re-engine
description: 在本仓（《天結いキャッスルマイスター》逆向 / 重实现工程）做**引擎语义逆向**时用这条两层工作流：**用 Hex-Rays 的 C 读逻辑、用 `.lst` 定位与核验**。当用户要求：查某个 handler / 某个 `sub_XXXXXX` 在做什么 / 核实或质疑一条已登记的引擎语义结论 / 给一个 EA 定语义 / 读懂目标函数后再设计守卫 / 把逆向结论登记进知识台账时使用。★ 铁律：结论的锚**只能是二进制 EA**（或守卫用例 id）—— `.c` 的**行号与文本不许当锚**（它一处地址都没有，且是 Hex-Rays 改写过的视图）。真源：语料 `corpus/disasm/README.md`、工具 `pnpm tools disasm-at describe`、准入门 `AGENTS.md` §6。
---

# Amayui RE Engine（引擎逆向：**C 读 / lst 证**）

## 0. 三条最常踩的

1. **`.c` 里一处地址都没有**（实测：`0x00xxxxxx` 形态计数 = **0**）⇒ 从 C **回不到 EA**。
   所以：`sub_415640` 这种**符号名本身**才是回连的钥匙（名字就是 EA），而"某个 `.c` 行"什么都不是。
2. **不是每个 EA 都是函数起点**（实测：`0x40D500` 是 `loc_40D500`，属于 `sub_40CD10`，函数起点在它**前面 683 行**）
   ⇒ 问"这个 EA 属于哪个函数"**必须**用 `--pseudo --ea`（它内部走 `enclosingFunction`），
   别自己 `spanOfFunction`、也别肉眼看行区间。
3. **不是每个函数都有 C**（实测：3807 个 `proc near` 里有 **77** 个没有）⇒ 工具会**明说**"没有定义"；
   **不许**把"没读到 C"读成"这段没有逻辑"，也不许拿 C 缺失当"这条指令不存在"。

## 1. 什么时候该动用它

* 要弄清某个 handler / 某个 `sub_XXXXXX` **在做什么**；
* 要**核实**（或质疑）一条已登记的引擎语义结论；
* 要给一个 **EA 定语义**，然后过知识准入门；
* 要读懂目标函数，**然后**才设计守卫（守卫断言什么，取决于这段逻辑到底长什么样）。
* 反过来：**C 不是台账的替代**。结论照旧必须过 `AGENTS.md` §6（每条绑定可再校验观察）。

## 2. 两层工作流（★ 顺序不可颠倒）

```bash
# ① 读（C 层：提假设、看结构）—— 一次调用就把"这个 EA 属于谁 + 它的 C 体"给你
pnpm tools disasm-at pseudo --ea 0x40D500            # EA 在函数体内也照样定位到所属函数
pnpm tools disasm-at pseudo --sym sub_415640 [--lines 200]

# ② 定位（lst 层：逐字证据）
pnpm tools disasm-at at --ea 0x40D500 --lines 60     # 有界上下文（被截断会明说）
pnpm tools disasm-at at --ea 0x40D500 --fn           # 该 EA **所属**函数的 lst 区间
pnpm tools disasm-at cases --ea 0x41BF50             # switch / 跳转表（case 数 + 目标）

# ③ 核验（把假设变成结论）—— 在 ② 的窗口里逐字读指令；**锚记 EA**
# ④ 登记 —— 走 `pnpm tools ledger`（字段/锚的闭集合见 `pnpm tools ledger describe`）
```

★ **为什么不能只有 ①**：C 是 Hex-Rays 的**改写** —— 类型（`int` / `_DWORD` / `char *this`）、变量名（`v121`）、
结构全是它的**推断**；`rol/xor/ror` 被折成 `__ROL4__` / `__ROR4__`、内联块被折叠、有时整个函数体是**假象**。
本仓已有先例：AGERC 的 `_GetInstance@0` 在 `.c` 里是**空体 `{ ; }`**，真实逻辑 `mov eax, hInstance`
是 **`.lst` 定的案**（`docs/01-translation/agerc-design.md`）。
★ **为什么不能只有 ②**：53 万行 / 18 MB 里读逻辑极贵。实测同一个 handler（`sub_42CA50` = `0x60 random`）：
**`.lst` 61 行 ↔ `.c` 23 行**，而且 `帧+0x74 ← 5` 与 `Engine+0x69330 计数器 > 12` 两条结论在 C 里**各占一行**。

## 3. 该用哪个（可机械判）

| 你想知道 | 用 | 判据 / 理由 |
|---|---|---|
| 这段逻辑**大致**在干什么 · 分支/循环结构 | `pseudo`（C） | C 覆盖率 98%，守卫钉着 **>95%** |
| 这个 EA **是不是函数起点**、属于谁 | `pseudo --ea`（输出会直说） | 出现 `★ 你给的 EA 不是函数起点` |
| 某个**常量 / 移位量 / 字段偏移 / arity / 访问宽度** | `at --ea`（lst） | ★ 只有 lst/字节有：C 把 `rol eax,0Bh` 写成 `__ROL4__(v, 11)`、把 `[esi+ecx*8+5D8F4h]` 写成 `this[…+383220]`（`383220 = 0x5D8F4`） |
| 分派结构（switch / 跳转表 / 544 条 handler 赋值） | `cases --ea` **＋** `pseudo` | 互补：C 给结构，lst 给 case 值 |
| 调用关系 / 谁给谁赋值 | `pseudo`（C） | C 里 `= sub_XXXXXX` / 调用点最易读 |

## 4. 五条硬口径（登记结论 / 写守卫时必须遵守）

1. **锚只锚 EA**：`.c` 的行号、`.c` 的文本片段**不许**当锚 —— 换一次导出就全变，而且它本来就没有地址。
2. **C 只能提假设**：进台账的每一条都要有 `.lst`/字节层面的逐字证据，或一个**会红的**守卫用例 id。
3. **不许"解析友好化"落回语料**（`AGENTS.md` §1 第 5 条）：`pseudo` 的输出是**派生的内存视图**；
   `.c` / `.lst` **一个字都不许改**。
4. **C 的名字不是代码的名字**：`v1` / `this` / `a1` 是 Hex-Rays 编的，**不构成证据**；
   引用就用 `sub_XXXXXX`（= EA）或字段偏移。
5. **守卫优先锚 lst**：守卫断言的是"引擎的行为"，不是"Hex-Rays 这次怎么排版"。
   用 C 生成期望可以；**把 C 文本当断言目标不行**。

## 5. 常踩的坑（都有实测出处）

* ★ **"这个 EA 是函数起点"是要算的事实**，不是可以假设的：`0x40D500` 是 `loc_40D500`；
  而 `spanOfFunction()` 从 EA 那一行**往后**找 `proc near` ⇒ 对体内的 EA 会给你**后一个**函数。
  两者分工：`spanOfFunction` = 函数起点的区间；`enclosingFunction` = **包含**该 EA 的函数
  （守卫 `tools/test/disasm-pseudo.assets.test.mjs` 第 1 条钉着这两者**必须不同**）。
* ★ **别用 `contextAt` 定位"所属函数"**：它给的是"最近的**前一个 mark**"的行，而 marks 是**稀疏**的
  ⇒ 对函数起点的 EA 会落到**前一个函数体内**（实测把 `sub_40CB00` 当成过 `sub_40CD10`）。
* ★ **`.c` 里有原型行**：头部一大段 `int __thiscall sub_42CA50(char *this);` ⇒ 只看第一次命中会拿到**原型**
  （工具按"行尾 `;`"把原型与定义分开）。
* ★ **函数体的收尾是第 0 列的 `}`**：内层块都缩进；用"第一个 `}`"当结尾会给出**看起来对**的行区间。
* ★ **段名以 `.` 开头**（`.text`）：正则首字符限 `[A-Za-z_]` 会让 `.text` **整段漏掉** ——
  本仓踩过**两次**（`tools/lib/disasm.mjs` 顶部记着第一次，第二次是加 `pseudo` 时又踩）。
  缺事实要**报错**，不要"读不到就当空"。
* ★ **别拿 `raw N`（行号）当锚**：旧仓那套锚全废了（`docs/02-engine/README.md`）。

## 6. 真源映射（本技能**不复述** schema）

| 想知道 | 看哪 |
|---|---|
| 语料是什么 / 4 个文件 / 转写口径 / 怎么反解 | `corpus/disasm/README.md` |
| 工具全部动作 / 参数 / 自描述 | `pnpm tools disasm-at describe`（`--json` 机器可读） |
| 语料有哪些段 / EA 范围 / 行数 / 单调性 | `pnpm tools disasm-at stats` |
| **本工作流自己的判据** | `tools/test/disasm-pseudo.assets.test.mjs`（`pnpm test:assets`） |
| 结论怎么登记、锚的闭集合、`system` 域 | `pnpm tools ledger describe` + `data/ledger/README.md` + `AGENTS.md` §6 |
| 知识清理 / 校验 / 重分类的现状 | `docs/00-origin/knowledge-rebuild.md` + `pnpm tools requirements plan` |
| 为什么要改成 `.mts`、类型从哪来 | `packages/age-format/README.md` §1.1 |
