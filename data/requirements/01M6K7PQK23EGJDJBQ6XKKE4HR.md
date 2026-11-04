# S4 逐项复核：x87 / MMX / SSE 的实际缺口（已实现 / 失败 / 偏差）

- id: REQ-01M6K7PQK23EGJDJBQ6XKKE4HR
- type: req
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- tags: [emulator-lift, m1, s4]

## 一句话
M1 目前能跑到 `_WinMain`，但 **S4（x87 / MMX / SSE）只做了"执行到就响亮失败"的骨架**：C++ 里已有 `Op::Other` 的显式失败路径，还没有按 plan §6 S4 的形态清单逐项核对"哪些真的实现了、哪些只是失败、哪些数值有偏差"。本单是那次逐项复核。

## 范围（真源 `repo:apps/emulator-lift/docs/plan-m1-cpu-base.md` §6 S4）
- x87：8 寄存器栈 / tag / 控制字 / 条件码 / 47 个 x87 形态（语料里约 18,668 条）；
- MMX / SSE：20 个形态（约 0.94k 条）；
- 超越函数按 SCOPE 走宿主 libm（1 ulp），其余走 Berkeley SoftFloat（逐位）。

## 判据（复核产出，不是代码）
- 一张 **形态 → 状态** 表：`已实现（逐位夹具）` / `已实现（有登记偏差）` / `未实现（响亮失败）` / `未取证`；
- 每一条"已实现"至少有一个会在改坏时变红的守卫（CTest 用例 id）；没有的补；
- 未实现项要么排进 M1b 的实现单，要么写进 `SCOPE.md`（⛔ 不许静默 no-op）。

## 边界
- 本单只做**核对与排单**；具体指令实现落到各自的子单。
- 不改变 M1 的 L2 判据（S4 不阻塞 A1–A5 的现有通过）。
