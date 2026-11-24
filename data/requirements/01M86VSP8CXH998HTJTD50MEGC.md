# 库函数调用的 per-function 动态计数（内建普查的测量前置，M1–M4）

- id: REQ-01M86VSP8CXH998HTJTD50MEGC
- type: req
- status: open
- parent: REQ-01M86NP9S7BEGSP1QR4VX51G9N
- tags: [emulator-lift, aot, diagnostics, intrinsics]

## 为什么需要

内建普查（`REQ-01M86NP9S7BEGSP1QR4VX51G9N`）的结论：现有三份 `window-dispatch-top` 只能给**派发份额**
（`ms` 列不是该 EA 的实测成本），给不出 per-function 调用数 / 单次成本；装载帧**没有** EA 表、
FELLOW 只有旧二进制的**整跑**表 ⇒ 无法按 `calls × ns/次` 排序，"第 2 个内建收谁"无法判定。

## 要做什么（M1–M4）

1. **M1**：把 `src/aot/memcpy_diag.*` 的硬编码区间 `[0x4E74B0,0x4E7811)` 改成一张小表
   （候选 EA / 名字），仍按"派发到该 EA"计数；保留 `_memcpy` 的字节画像。
2. **M2**：`src/aot/aot_run.cpp` 派发循环里已有 `memcpy_diag::on()` 一次判定；扩成"命中表里任一 EA ⇒
   计数"，缺省零行为（⛔ 不改变默认输出 / 不改变语义指纹）。
3. **M3**：三份窗口 —— 点击（`m11-seg.js` 的 seg2）、装载（要先给装载段 `profile.begin/end` 或慢帧差分）、
   FELLOW 切换（`save09-fellow-switch.plan` 的 6 个窗口）。三份都要能重跑。
4. **M4**：每候选给：窗口内调用数、平均 n（如适用）、`_memcpy` 同口径对照、insn/次（若 diag 能算区间 insns）。

## 判据

- 同一窗口、同一二进制，**开↔关诊断**的五项指纹逐位同；诊断缺省时 stdout/stderr 与今天逐字节同。
- 表中每个候选在点击窗口给出 calls；装载 / FELLOW 至少一份**窗口级**读数（若装载段仍无窗口表，
  写明并只交两份，⛔ 不拿整跑表冒充窗口表）。
- 读数进 `docs/03-emulator/aot-intrinsics.md`（新节）或 `.tmp` 报告；⛔ 不进 test、⛔ 不断言当前数字。

## 边界

⛔ 不改 `aot/intrinsics.txt`（第 2 个内建由普查单在测量后决定）；⛔ 不按 EA 特判产品路径
（诊断只在 opt-in 时计数）；⛔ 不把"派发份额"换算成调用次数。
