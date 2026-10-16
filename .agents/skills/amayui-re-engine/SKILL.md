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
# ① 读（C 层：提假设、看结构）—— 一次调用就把"这个函数是谁 + 两侧区间 + 参数/全局/调用关系"给你
pnpm tools disasm-fn show sub_415640 [--lines 200]   # ★ 主入口（给 EA 也行：show 0x40D500）
pnpm tools disasm-fn show sub_415640 --both          # 两侧正文一起给
pnpm tools disasm-fn args sub_415640                 # 参数 ↔ 栈偏移 / 寄存器（含**配对依据**）

# ② 定位（lst 层：逐字证据）
pnpm tools disasm-fn show sub_415640 --lst            # 直接给该函数的 .lst 窗口（逐字核验用）
pnpm tools disasm-at at --ea 0x40D500 --lines 60     # 有界上下文（被截断会明说）
pnpm tools disasm-at cases --ea 0x41BF50             # switch / 跳转表（case 数 + 目标）

# ③ 影响面（"改这里会牵扯谁"）—— 结论**必带下界说明**
pnpm tools disasm-fn callers sub_415640              # 谁调用它（权威边）+ 取址证据（动态分派，另册）
pnpm tools disasm-fn impact sub_415640 --up --depth 3   # 反向闭包（谁会到它）
pnpm tools disasm-fn globals sub_415640              # 它碰了哪些全局；`globals --who <名|EA>` 反过来查
pnpm tools disasm-fn data --match off_               # 数据 item（函数指针表 / 跳转表）

# ④ 核验（把假设变成结论）—— 在 ② 的窗口里逐字读指令；**锚记 EA**
# ⑤ 登记 —— 走 `pnpm tools ledger`（字段/锚的闭集合见 `pnpm tools ledger describe`）
```

★ **为什么不能只有 ①**：C 是 Hex-Rays 的**改写** —— 类型（`int` / `_DWORD` / `char *this`）、变量名（`v121`）、
结构全是它的**推断**；`rol/xor/ror` 被折成 `__ROL4__` / `__ROR4__`、内联块被折叠、有时整个函数体是**假象**。
本仓已有先例：AGERC 的 `_GetInstance@0` 在 `.c` 里是**空体 `{ ; }`**，真实逻辑 `mov eax, hInstance`
是 **`.lst` 定的案**（`docs/01-translation/agerc-design.md`）。
★ **为什么不能只有 ②**：53 万行 / 18 MB 里读逻辑极贵。实测同一个 handler（`sub_42CA50` = `0x60 random`）：
**`.lst` 61 行 ↔ `.c` 23 行**，而且 `帧+0x74 ← 5` 与 `Engine+0x69330 计数器 > 12` 两条结论在 C 里**各占一行**。
★ **为什么影响面要单独一步**：`callers` 为空**不等于**"没人用它"。实测 `sub_42CA50` 一个直接调用方都没有 ——
它是**函数指针 / 跳转表**分派到的（`disasm-fn callers` 会把"取址证据"另册列出来）。任何闭包都只是**下界**。

## 3. 该用哪个（可机械判）

| 你想知道 | 用 | 判据 / 理由 |
|---|---|---|
| 这段逻辑**大致**在干什么 · 分支/循环结构 | `disasm-fn show`（C 体） | C 覆盖率 98%，守卫钉着 **>95%** |
| 这个 EA **是不是函数起点**、属于谁 | `show <EA>`（输出会直说） | 出现 `★ 你给的 EA 不是函数起点` |
| 某个**常量 / 移位量 / 字段偏移 / arity / 访问宽度** | `show --lst` 或 `at --ea` | ★ 只有 lst/字节有：C 把 `rol eax,0Bh` 写成 `__ROL4__(v, 11)`、把 `[esi+ecx*8+5D8F4h]` 写成 `this[…+383220]`（`383220 = 0x5D8F4`） |
| **参数在哪个偏移 / 哪个寄存器** | `disasm-fn args` | 名字优先、顺序兜底的配对，并标出**配对依据**；`.c` 侧的结构偏移是**十进制** |
| **调用链**（谁调它 / 它调谁） | `disasm-fn callers` / `callees` | 权威边 = `.lst` 的 `call`/`jmp sub_XXXXXX`；间接调用**只计数**（下界） |
| **影响面**（改它牵扯谁） | `disasm-fn impact --up/--down` | 传递闭包 + **必带的诚实项**（间接调用点 / 取址证据） |
| **全局变量**（它碰了什么 / 谁碰这个） | `disasm-fn globals` / `globals --who` | 权威侧是 `.lst`（C 会把 load 提到局部变量 ⇒ 漏报，实测 954 vs 9） |
| 分派结构（switch / 跳转表 / 544 条 handler 赋值） | `cases --ea` **＋** `show` ＋ `data` | 互补：C 给结构，lst 给 case 值与表项 |

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
  （守卫 `tools/test/disasm-enclosing.assets.test.mjs` 第 1 条钉着这两者**必须不同**）。
* ★ **别用 `contextAt` 定位"所属函数"**：它给的是"最近的**前一个 mark**"的行，而 marks 是**稀疏**的
  ⇒ 对函数起点的 EA 会落到**前一个函数体内**（实测把 `sub_40CB00` 当成过 `sub_40CD10`）。
* ★ **`.c` 里有一大片原型行**：`int __thiscall sub_42CA50(char *this);` —— 判据是"列 0 + 以 `)` 结尾 +
  **下一个非空行恰为 `{`**"。⛔ 别用"行尾 `;`"：**带尾注释的原型**（`…); // idb`）结尾是注释 ⇒ 会被当成定义，
  于是该符号的"C 体"是**文件顶部的一行原型**（实测 40 个；`sub_40B6C0` 的真定义在 `.c:15186`，
  错的那个给 `.c:275`），而调用方**照样打印、不报错**。判据与守卫：`tools/lib/catalog.mjs` 的 `readSignature`
  + `tools/test/disasm-fn-format.test.mjs`。
* ★ **签名可能跨行**（`int __thiscall sub_X(int a1,` / `int a2)` / `{`）：只认单行的判据会**整条漏掉**它，
  而漏掉的表现与"Hex-Rays 没反编译"**一模一样**（实测 `sub_4D13F0` 就是这么被漏掉的）。
* ★ **`__usercall` / `__userpurge` 把寄存器写在名字和参数上**：`int __userpurge sub_404FF0@<eax>(_BYTE *a1@<edx>, …)`。
  不剥掉名字里的 `@<eax>` ⇒ 定义对不上 `.lst` 符号；不认参数上的 `@<reg>` ⇒ 偏移全错。这类函数的**栈偏移**
  只对那些没有 `@<reg>` 的参数可判 —— 工具会标 `convNote`，**不许**自己按位置猜。
* ★ **参数配对是"名字优先、顺序兜底"**，不是纯位置：实测 `sub_401100`（`__thiscall`）`.lst` 声明
  `lpFileName= dword ptr 8` + `arg_4= dword ptr 0Ch`，而 `.c` 是 `(int this, LPCSTR lpFileName, const char *a3)` ——
  位置法会把 `a3` 配到 `[ebp+10h]`（那里**没有声明**）。`disasm-fn args` 会打印**配对依据**（`name`/`order`/`reg`/`ref`）。
* ★ **被引用但没声明的栈槽也是事实**：IDA 会漏声明（实测 `sub_401100` 用了 `arg_8` 却没那行）——
  对 `arg_XX` / `var_XX` 这类名字，偏移**从名字就能算**（`arg_8` ⇒ `[ebp+10h]`），工具会标 `declared:false`。
* ★ **间接调用解不出目标**：`call eax` / `call [esi+4]` / `call ds:dword_X`（全语料 3100+ 处）
  ⇒ 任何"调用闭包"都只是**下界**。其中 `call ds:dword_X` 是**旧实现整类漏掉**的形态（实测 69 处被静默丢出调用图）
  —— 漏掉的表现是**数字变小**，不是报错。反过来说：`.c` 侧常把目标**点名**（`v2 = GlobalFree; … v2()`），
  所以 `disasm-fn callees` 同时给 `.lst` 权威边与 `.c` 侧点名，**不一致就报出来、不合并**。
* ★ **跨侧比偏移要归一化**：`.c` 的结构偏移是**十进制**（`this + 280`），`.lst` 是**十六进制**（`[esi+118h]`）——
  同一个偏移（`280 = 0x118`）。
* ★ **函数体的收尾是第 0 列的 `}`**：内层块都缩进；用"第一个 `}`"当结尾会给出**看起来对**的行区间。
* ★ **段名以 `.` 开头**（`.text`）：正则首字符限 `[A-Za-z_]` 会让 `.text` **整段漏掉** ——
  本仓踩过**两次**（教训记在 `tools/lib/lst-format.mjs`：格式词汇表只有一份）。
  缺事实要**报错**，不要"读不到就当空"。
* ★ **别拿 `raw N`（行号）当锚**：旧仓那套锚全废了（`docs/02-engine/README.md`）。

## 6. 真源映射（本技能**不复述** schema）

| 想知道 | 看哪 |
|---|---|
| 语料是什么 / 4 个文件 / 转写口径 / 怎么反解 | `corpus/disasm/README.md` |
| **item 目录 / 影响面**全部动作与字段 | `pnpm tools disasm-fn describe`（`--json` 机器可读） |
| **定位与切片**全部动作与参数 | `pnpm tools disasm-at describe`（`--json` 机器可读） |
| 语料有哪些段 / EA 范围 / 行数 / 单调性 | `pnpm tools disasm-at stats` |
| 目录一览 + 诚实项 + 不变量自检 | `pnpm tools disasm-fn summary` |
| **本工作流自己的判据** | `tools/test/disasm-fn-format.test.mjs`（`pure`）· `tools/test/disasm-fn.assets.test.mjs`（`assets`）· `tools/test/disasm-enclosing.assets.test.mjs` |
| 结论怎么登记、锚的闭集合、`system` 域 | `pnpm tools ledger describe` + `data/ledger/README.md` + `AGENTS.md` §6 |
| 知识清理 / 校验 / 重分类的现状 | `docs/00-origin/knowledge-rebuild.md` + `pnpm tools requirements plan` |
| 为什么要改成 `.mts`、类型从哪来 | `packages/age-format/README.md` §1.1 |
