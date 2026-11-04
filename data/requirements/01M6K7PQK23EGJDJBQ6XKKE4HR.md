# S4 逐项复核：x87 / MMX / SSE 的实际缺口（已实现 / 失败 / 偏差）

- id: REQ-01M6K7PQK23EGJDJBQ6XKKE4HR
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/conformance_cases.cpp#x87-fucomp
- tags: [emulator-lift, m1, s4]

## 一句话
M1 能跑到 `_WinMain` 时，**S4（x87 / MMX / SSE）只做了"执行到就响亮失败"的骨架**：C++ 里已有 `Op::Other` 的显式失败路径，还没有按 plan §6 S4 的形态清单逐项核对"哪些真的实现了、哪些只是失败、哪些数值有偏差"。本单是那次逐项复核 —— 结论与守卫都落在 `apps/emulator-lift/docs/s4-fpu-audit.md`。

## 复核产出（事实表在 `apps/emulator-lift/docs/s4-fpu-audit.md`）
- **x87**：**47 个形态 / 18,668 条**（现查 `pnpm tools lift corpus-report`；另有 `wait` 7 条不在该集合里）——**全部已实现**；
- **MMX**：**19 个形态 / 950 条**（`corpus-report` 报 778，因 `tools/lift.mjs` 的 `MMX_SSE` 集合漏了 5 个助记符 —— 见 doc §1）——**全部已实现**；
- **SSE**：`.lst` 里 **0 个形态 / 0 条**（判据：全文件 `xmm<数字>` 操作数 = 0）；别名表里登记了 14 条但没有实现 ⇒ 执行到即响亮失败（**不是 no-op**）；
- **状态计数（66 个语料形态）**：已实现（逐位夹具）**61** · 已实现（有登记偏差，SCOPE §1 的超越函数族）**5** · 未实现（响亮失败）**0** · 未取证 **0**；
- **修掉 4 处静默偏差**（未实现会停在诊断上；算错会把错误数据带进后续帧）：
  `compare()` 把 `fucom`/`fucomp` 永远比成相等（语料 12 条）· MMX 移位计数被 `& 0xFF` 截断 ·
  MMX 寄存器被 x87 的 TOP 旋转（语料 950 条全受影响）· 5 个超越常数只有 double 精度；
- **新增 29 条守卫**（`lift-conformance` 183 → 212 条）：23 条给此前没有夹具的 x87 形态 + 2 条常数 +
  2 条高频寄存器形态 + 2 条静默算错回归；4 处修复各做过一次"改坏 ⇒ 指定用例当场红"的变异验证。

## 缺口去了哪
- **SSE 14 条** → 子单 **`REQ-01M6KHP54MYNP6WX1JA9NKZB8P`「S4-SSE：14 条 SSE 浮点形态（语料 0 条；执行到即响亮失败）」**。
  ★ 它挂在**同一个父**（M1）下、不挂在本单下：本单收口后子树里不能再有活节点（不变量 #4）。
- `fbld` / `fbstp` / `ficom` / `ficomp` / `feni` / `fdisi` / `fsetpm` / `ffreep`（语料 0 条、解码器没登记）：
  **不排单** —— 真撞到会响亮失败并停下，按"实现指令"流程走。
- `tools/lift.mjs` 的 `X87` / `MMX_SSE` 集合漏了 6 个助记符（只在 `corpus-report` 的计数上体现）：
  doc §1 已点名并给出修正后的数；工具本身不在本单边界内。

## 判据（本单的收口口径 = 核对与排单）
- 一张 **形态 → 状态** 表（`apps/emulator-lift/docs/s4-fpu-audit.md` §2），状态只有四种，语料条数**现查**；
- 每一条"已实现"至少有一条会在改坏时变红的守卫 —— 补齐后 66/66 成立（变异记录见 doc §4.2）；
- 未实现项要么排单、要么写进 `SCOPE.md`（⛔ 不许静默 no-op）—— 已排单，SCOPE 未改（本单没有新增已批准偏差）。

## 边界
- 本单只做**核对与排单**；具体指令实现落到各自的子单。
- 不改变 M1 的 L2 判据（S4 不阻塞 A1–A5 的现有通过）。
- 没核完的**非形态级**证据缺口（`wait` 的 trap 语义 / `fprem` 的 C2=1 分支 / pseudo-denormal /
  COLLAPSED 区里的 SSE）逐条写在 doc §6.2 —— 那里明说了"缺什么证据"，不是"未取证"的形态。
