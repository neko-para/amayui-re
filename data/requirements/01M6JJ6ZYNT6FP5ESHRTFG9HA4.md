# Virtual* 三处与文档冲突：hint+MEM_COMMIT 不校验 reserve / MEM_RELEASE 非 LIFO / MEM_DECOMMIT dwSize=0

- id: REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4
- type: bug
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- repro: apps/emulator-lift/src/shims/kernel32_mem.cpp#shimVirtualAlloc
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`kernel32_mem.cpp` 的 `Virtual*` 有三处与 MSDN 明确冲突（审计 D 组已核；2026-11-04 从 S6 三族单里拆出，避免把"夹具补齐"和"语义修正"混在一张单里）。

## 三处
1. `shimVirtualAlloc` 的 `hint + MEM_COMMIT` 不校验该地址是否先 `MEM_RESERVE`，直接 `commitZeroed`
   ⇒ 会清零任意地址（文档：未 reserve 的显式地址 COMMIT ⇒ `ERROR_INVALID_ADDRESS(487)`）。
2. `shimVirtualFree(MEM_RELEASE)` 只校验 `regionOf(ea)->begin == ea`，不校验它是**最后一块** ⇒
   非 LIFO 释放会把后面的块一起 decommit（与本文件偏差 4 的"只支持 LIFO"自相矛盾）。
3. `shimVirtualFree(MEM_DECOMMIT)` 拒绝 `dwSize == 0`；文档允许 `dwSize==0 + 块基址` 退整段。

## 判据（待写实）
- reserve 后再 `MEM_COMMIT` 同址 ⇒ 成功；**未 reserve 的 hint + COMMIT** ⇒ 0 + 487，且宿主内存不被清零。
- 两块 bump 分配：释放**前一块** ⇒ 0 + 87，后一块仍可读写；释放最后一块 ⇒ 成功。
- `MEM_DECOMMIT(ea, 0)`（ea 是块基址）⇒ 成功，整段变未提交。

## 证据
- 代码：`apps/emulator-lift/src/shims/kernel32_mem.cpp`（`shimVirtualAlloc` / `shimVirtualFree`），
  文件头偏差 3/4 已点名"只支持 LIFO"。
- 审计报告 `.tmp/m1a-audit/D-s6core.md`（D 组 2 条 high），夹具在 `tests/shims_core.cpp` 只覆盖了
  bump 分配 / RELEASE / DECOMMIT 的正常路径，没有覆盖这三条冲突形态。
