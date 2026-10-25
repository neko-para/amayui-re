---
name: amayui-emulator
description: 在本仓推进《天結いキャッスルマイスター》引擎**模拟器**（重实现）时用这条**观测驱动的闭环**。★ 用户的请求通常只有两种形态 —— 「**实现指令 NN**」与「**模拟器里 X 的表现不符合预期**」；不要去等"sub_xxx 看起来是 yy 系统的一部分"这类信息（**那正是要发现的东西**，用户给不出）。当用户说：实现/补一条指令、某个 opcode 的语义要落到实现与守卫、修一个行为不符、给模拟器加一块能力、判断"模拟器下一步该做什么"、为 K2/K3 复核找"能真跑的守卫"时使用。★ 逆向取证是**下一层**：交 `amayui-re-engine`（**可当 subagent 用**）；进度与收口走 `amayui-requirements`。
---

# Amayui Emulator（模拟器开发：**观测驱动**的闭环）

## 0. 三条最常踩的

1. **⛔ 不要把"逆向"当起点**。起点**永远是可观测的东西**：一条 opcode、一次不符预期的表现、一个跑不起来的场景。
   `sub_xxxxxx` / 偏移 / 字段是**手段**。反过来做（先漫游反汇编找"有意思的函数"）在本仓是**反模式**：
   既不可复现，也没有判据说"做到哪算完"。
2. **⛔ 不要问用户"这是不是 yy 系统 / 该不该看 sub_xxx"** —— 他**给不出**这个信息（他知道的话就不需要这条技能了）。
   要"这是哪块"就**自己算**：分派表 / 调用图 / 语料检索（工具见 §6）。
3. **⛔ 不许凭记忆写引擎语义**（`AGENTS.md` §0.3 / §6）。每条结论绑可再校验观察；`.c` **只能提假设**，
   锚**只锚 EA 或守卫用例 id**。★ 「K3 通过前一律不许写台账」**已经作废**（2026-10 变更）：
   今天的闸门是"**每条都绑可再校验观察**"（见 `AGENTS.md` §6.6）。

## 1. 两种请求形态 → 两条流水线

### A. 「**实现指令 NN**」（今天的主线）

```bash
# ① 这条指令在表里吗？argc 多少？（argc 是**唯一边界**：长度 = 4 + argc*8）
pnpm tools opcodes report                    # 旧表 ↔ 本仓派生表对账
pnpm tools disasm-fn summary                  # 语料目录一览：函数 / 数据 item / 诚实项
pnpm tools disasm-at stats                   # 语料段/EA 范围（要先有语料）
pnpm tools opcodes handlers                  # opcode → handler EA（表基址 + 赋值行 + 走缺省的 opcode）

# ② 取证（**下一层**，可 subagent 扇出）—— 拿到"取证包"（§2.1）
#    交 `amayui-re-engine`：`pnpm tools disasm-fn show sub_XXXXXX` → C 体 + .lst 窗口 + 参数/全局/调用关系

# ③ 实现：**三条线各有一个落点**（★ 先确认你要改哪条，别在废弃的那条上干活）
#    · apps/emulator-cxx/src/core/   原生 C++（SDL_GPU 呈现）；`vm.cpp` 只做分派（一个 opcode 一行），
#      handler 按族分在 `vm_ops_*.cpp`（影片 0x20F 在 `vm_movie.cpp`，值/池原语在 `vm_value.cpp`）
#    · apps/emulator-new/src/core/   只读 data/spec 的参考实现（handlers.ts / machine.ts）
#    · apps/emulator/src/model/      最早的 TS 模型（**已废弃**，只剩 `@env assets` 守卫还 import 它）
#    ★ 硬口径：**语义层不写偏移/EA**（那是 packages/age-format/src/engine/layout.mts）。
#      C++ 侧允许在注释里写 EA 当锚，但⛔ 不许把 EA 当数据（不要建"按 EA 分派"的表）
#    ★ 池值：int 族读必过 DEC、写必过 ENC（value-codec）；越界不 clamp、要留痕

# ④ 守卫 + 变异条目（两条线各有各的门）：
#    · TS：tools/test/emulator-*.test.mjs（@env pure|assets）+ tools/mutate-check.mjs 加一条
#    · C++：apps/emulator-cxx/tests/core_selftest.cpp（CTest `emulator-core-selftest`），
#      以及 CMakeLists 里"要真安装"的 CTest（title / logo 影片 …）
#      ★ mutate 清单**不覆盖 C++** ⇒ 自己施加一处破坏、确认 CTest 会红、再撤销（判据一样）
pnpm test:assets && pnpm test:mutation       # ★ 判据是"它会红"，不是"它绿"
ctest --test-dir apps/emulator-cxx/out/build/windows-x64 --output-on-failure

# ⑤ 登记：data/ledger/（一条 observation；锚 = EA / 守卫用例 id）
```

**判据（缺一条就没做完）**：③ 里**没有偏移**；④ 的守卫**当场会红**（TS 侧看变异条目，C++ 侧手动施加破坏验过）；
⑤ **锚可解析**。

### B. 「**X 的表现不符合预期**」（观测驱动的修 bug）

```
① 先**复现**：写一个**能红**的用例（repro）—— ⛔ 不许先改代码
② 定位：二分（哪条指令 / 哪个池 / 哪个 handler / 哪一帧 / 哪个门外服务）
③ 与静态观察对账：`amayui-re-engine` 看这条 handler 的静态形态
④ 修 + 把 ① 的用例**钉成守卫**；★ 若原结论错了 ⇒ 台账**追加更正记录**（`replaces`），
   ⛔ 不许就地改写已有结论（`AGENTS.md` §6.4）
⑤ 开单 / 收口：`amayui-requirements`
```

★ 这一类**今天能真跑**，观测面有两个（各有各的边界）：

| 观测面 | 能看见什么 | 怎么跑 |
|---|---|---|
| `apps/emulator-new`（headless） | **执行轨迹 / 引擎数据 / 副作用记录**；没有像素、没有窗口 | `node apps/emulator-new/src/main.ts --install <dir> …`（`--trace` / `--effects` / `--json`）+ `@env assets` 守卫 |
| `apps/emulator-cxx`（原生） | **像素**（GPU 场景批次 / 离屏 BMP 导出）+ 真窗口节奏 + CTest | `--play-boot`（开窗）· `--render-title` / `--render-title-gpu` / `--render-logo-gpu`（离屏 BMP）· `ctest` |

⛔ 两条边界：**"能跑"≠"有 oracle"**（"看起来对"不是证据）；**没有真机 trace** ⇒ 引擎内部时序仍只能靠静态读 + 残差。
所以 ① 那个"能红的 repro"必须是**机械判据**（像素统计 / 请求次数 / 字段值 / 步数），不是"我看了眼截图"。

## 2. 分层：谁调用谁（★ 这就是"逆向技能当 subagent 工具"的落地）

| 层 | 技能 | 输入 → 输出 | 谁调用 |
|---|---|---|---|
| **上：观测驱动闭环** | **本技能** | 用户的两种请求 → **落地的模型 + 会红的守卫 + 台账条目** | 主 agent |
| **下：逆向取证** | `amayui-re-engine` | **一个 opcode / EA / 符号** → **取证包**（§2.1） | 本技能；**可 subagent 扇出** |
| 横：台账与进度 | `amayui-requirements` | 发现 → 需求单 / 缺口的收口 | 两者 |

### 2.1 ★ **取证包**：下层技能的**输出契约**（当 subagent 用时按这个发任务）

发任务时**只给这三样**（自包含；subagent 看不到你的上下文）：

```
输入：① 一个 opcode（例 `0x60`）或一个 EA/符号（例 `0x40D500` / `sub_42CA50`）
      ② 一句问题（例「它做了什么」/「这条分支的判据是什么」/「它碰了哪些引擎字段」）
      ③ 目标（要进模型？要写守卫？只登记？）
```

要求它**按这个结构回**（缺项要显式写"没拿到"）：

| # | 内容 |
|---|---|
| ① | **函数**：符号 + 起点 EA + `.lst` 行区间 |
| ② | **逐字证据**：`.lst` 行窗口里的指令序列 —— ★ **这才是锚** |
| ③ | **C 摘要**：`.c` 行区间 + 它读起来像什么 —— ★ 只是**阅读路径**，不是证据 |
| ④ | **结论**：`subject` / `claim` / `anchor[]`（锚只能是 EA 或守卫用例 id） |
| ⑤ | **守卫草案**：能机械判的 2–3 条（要"会红"的那种，不是"复述代码"） |
| ⑥ | **不确定项**：哪些没解出来、**缺哪个观察**才能解 —— ⛔ 不许糊过去 |

⛔ 契约里的禁止项：拿 `.c` 行号/文本当锚 · 把"没观测到"当成"不存在" · 把 `v1`/`this` 这类
Hex-Rays 编的名字当证据 · 顺手去改别的文件。

## 3. ★ 今天的现实：闭环**哪一段是硬的**

| 环节 | 今天能不能做 | 依据 / 正确动作 |
|---|---|---|
| 「实现指令 NN」（A 类） | ✅ 能 | 批 R1 就是这么干的（纯数值族 39 条：模型 + 守卫 + 台账）；两条实现线各有落点（§1 A ③） |
| `opcode → handler EA` 的**机械查询** | ✅ 能 | `pnpm tools opcodes handlers`（表基址 + 赋值行 + 走缺省的 opcode；`--opcode` 查单条）。★ 缺口在**实现侧**：`packages/age-format/src/engine/handlers.mts` 只登记了一部分（现查）——别把"实现里没有"当成"引擎里没有" |
| 「行为不符」的复现（B 类） | ✅ 能（观测面已落地） | 无头：`apps/emulator-new` 的 CLI + `@env assets` 守卫；有像素：`apps/emulator-cxx` 的 `--play-*` / `--render-*` + CTest（§1 B 那张表） |
| **像素级的 oracle** | ⛔ 没有 | 只有"离屏 BMP + 像素统计"这种**自建**判据；⛔ 不能拿它宣称"与引擎等价"（`AGENTS.md` §11 措辞纪律） |
| **真机 trace / 逐帧对照** | ⛔ 没有 | 真机探针（`apps/inspector`）给的是"某时刻看起来怎样"，不是合同；引擎内部时序仍只能静态读 + 登记残差 |

★ **"缺一段"时的正确动作是开单或登记残差，不是绕过去作假**（`amayui-requirements`）。
观测面落地**不等于**判据落地：先问"这条结论的**可证伪观察**是什么"，再问"用哪条命令能看见它"。

### 3.1 ★ 能力（capability）：**枚举 opcode 看不出来的那一半** —— 怎么发现（**可推进的工作线**）

**它是什么**（旧仓口径，`analysis/engine-capabilities.json`）：引擎的**常态行为**，分 **4 类**：
**逐帧流程 / 门控标志 / 惰性创建 / 资源生命周期**
（`docs-new/99-records/2026-09-audit/audit-2026-09-capabilities.md` 原文）。
一条 capability 记的是"引擎**持续**在做 / 持续在管"的事，**不是**"某条指令执行了一次的效果"
⇒ 它没有 opcode 那样的观测点，它的存在只能在**帧**这个尺度上看出来。

**★★ 发现的主体是读源码**（静态、有限、可枚举）；四类各有入口：

| 类 | 从哪读出来 | 候选 = |
|---|---|---|
| **逐帧流程** | **主循环体** | 主循环里的 callee（每帧都跑到） |
| **门控标志** | **主循环体** | 主循环里**读**的引擎字段（`if (*(…)(this + X))`） |
| **惰性创建** | **分配点** | "首次使用时创建"的写端（≠ 装载期） |
| **资源生命周期** | **分配点 + 释放点** | 谁建 / 谁清 / 何时清 |

**★★ 采样（真机探针 M7）是 debug 手段，不是开发手段**：它只在**两条读法给出互斥结论**时当**裁判**。
⛔ 不用来发现清单：采样的**窗口 / 时机 / 噪声**都不可控，它给的是"某个时刻看起来怎样"，
不是"引擎持续在做什么"。（★ 主循环是**静态可读**的 —— 把发现判给采样是错的。）

**★ 为什么它能"推进"（而不是触发式发现）**：主循环的 callee 与读字段是**有限集** ⇒ 天然是一张清清单：

```bash
# ① 定位主循环（一次性；判据 + 守卫钉住）—— 见需求单 REQ-01M4AF3G7RR2DG33CJD0GSDJY7
pnpm tools disasm-fn show <主循环符号>
# ② 机械抽候选：主循环的 callee ∪ 它读的字段（＋ 另两类的分配/释放点）
#    ★ 从 C 体抽 callee 就是"列出里面的 sub_XXXXXX("；抽读字段就是"列出 this + N"
# ③ 逐条判定：是不是"常态"（在帧尺度上重复）+ **帧两端的可观测差异**是什么
# ④ 每条收口：本仓锚（EA）+ 逐字证据 + 帧尺度判据 + **会红的守卫**（+ 变异条目）
# ⑤ 覆盖度：候选 N / 已建判据 M —— ★ 由工具输出给出，⛔ 不手写数字
```

★ 进度真源仍是 `pnpm tools requirements plan`；**候选清单是派生物**（可重建、不入库）。
⛔ 不为它新造一个"能力台账"数据文件。

**对照来源（不是主体）**：旧仓那份按 4 类**机械分组**，用来找"我们漏了哪类"。
★ 它真正有用的东西是 **`engine.fns`**（`sub_XXXXXX` = EA，可直接 `--sym` 重挂）；
⛔ 旧仓 `engine.raw` 的**行号不可用**（另一份导出 + 被 sanitize 过）；⛔ `name`/`note` 不许搬（`AGENTS.md` §9）。

**三条硬口径**：

1. **capability 的名字是旧仓的记账单位，本仓不继承名字** —— 要继承的是**可观测判据**。
2. **守卫必须真断言行为**。旧仓的能力台账守卫**只校验 `guard` 指向的文件存在**
   （`test/capability-ledger.test.ts`），于是"填了守卫但没在验"一路绿灯 ——
   旧仓自己的审计因此产出 **98** 条 finding。★ 本仓不许重复（正面例子：`tools/test/emulator-*.test.mjs` + 变异条目）。
3. **"每帧"必须能被观测到**，否则只能记 `partial` / 待定 ——
   ⛔ 不许用"我读了代码、觉得它每帧都会跑"来收口。

## 4. 并行化（544 条 handler 这种量级）

* **取证可扇出**：一个 subagent **只做一条**（输入独立、输出结构化 ⇒ 天然可并行）。
  发任务照 §2.1 的三样 + 契约。
* **建模 / 守卫 / 台账不并行**：同一批文件、同一套口径（偏移不许进模型、锚只锚 EA）⇒ 串行，主 agent 自己做。
* ★ 判据：**取证**（读语料、出结论）与**落盘**（改模型、加守卫）分开；前者扇出，后者收口。

## 5. 常踩的坑（都有实测出处）

* **拿 `.c` 行号或文本当锚** —— `.c` **一处地址都没有**（实测 `0x00xxxxxx` 计数 = 0），换一次导出全变。
* **在 TS 模型里写偏移/EA** —— 守卫 `tools/test/emulator-model.test.mjs`（`@env assets`）会红
  （它断言 `apps/emulator/src/model/pools.ts` 里没有 `base`/`count`、也不导出 `frameBaseOf`）。
  偏移属于 `packages/age-format/src/engine/layout.mts`。
* ★ **spec 没写的地方 ⛔ 不许发明**（实测两次：把"脏就渲染"当成主循环闸门、把影片收尾当成"headless 立即结束"）。
  正确动作是**先读那一则**：写清了就照做；**没写就补 spec（带证据）或登记残差**，再把实现对齐。
  ⛔ "实现里先这么写、以后再说"会让宿主长出 spec 里没有的规则，而所有测试仍然绿。
* **把"能跑起来"当成"判据成立"** —— 开窗看得见画面、BMP 里不是黑屏，都不是证据；
  证据是**会红的机械判据**（请求次数 / 字段值 / 像素统计 / 步数），而且要在破坏时当场红。
* **把"语料里 0 次出现"当成"这条指令没语义"** —— 那是**判据**（`UNUSED_IN_CORPUS`），不是"没见过"。
* **恒真守卫**："语料里 0 命中"这类断言可能是恒真的（台账里有一条就是因此被自己撤回的）
  ⇒ 每条守卫都要问"它到底在防什么坏"。
* **把 A/B 两类的产物混在一起**：A 类产出**能力**（模型+守卫），B 类产出**分歧消除**（repro 消失）。
  收口凭据不一样（`verify` vs `verify`/`repro`）。
* **"未初始化 = 0"** —— int 族初值是 `enc_zero`（非 0 的位模式）。
* **只信注释/命名**：C++ 里 `case 0x1F7: // 拆离绘制键` 这种注释可能已经**窄于** spec
  （实测它只删了绘制项，spec 要求四张同键表）⇒ 落实现时**以 spec 那一则的字面为准**，注释跟着改。

## 6. 真源映射（本技能**不复述** schema）

| 想知道 | 看哪 |
|---|---|
| **三条实现线各自做什么、边界在哪** | `apps/emulator-cxx/README.md`（原生 C++，含 CLI 与 CTest 清单）· `apps/emulator-new/README.md`（只读 spec 的参考实现）· `apps/emulator/README.md`（已废弃，仅架构参考） |
| 模拟器域的来源（旧仓对应物 / 落点 / 边界） | `docs/03-emulator/README.md`（★ 其中 M4-1/M4-2 的分层是**当时的计划**，M4-1 已 drop 并被 `apps/emulator-new` 取代） |
| **逆向取证怎么读**（C 读 / lst 证、硬口径、六条坑） | 技能 `amayui-re-engine` |
| 语料是什么、怎么定位与切片 | `corpus/disasm/README.md` + `pnpm tools disasm-at describe` |
| 逐函数/逐定义（item 目录）与影响面 | `pnpm tools disasm-fn describe`（不变量与诚实项的唯一真源） |
| 指令表（argc / 助记符）身份与改法 | `tools/opcodes.md` + `packages/age-format/src/asm/instruction-set.md` |
| 引擎**布局观察**（槽位/偏移/opcode→handler） | `packages/age-format/src/engine/{layout,handlers}.mts` |
| 结论怎么登记、锚的闭集合 | `pnpm tools ledger describe` + `AGENTS.md` §6 |
| 还要做什么、到哪一步 | `pnpm tools requirements plan`（★ 某条实现线若在需求树里没有单，它的前沿只能从**它自己的 README + `ctest` 清单**读出来；⛔ 不要替它编一张进度表） |
