# Virtual* 三处与文档冲突：hint+MEM_COMMIT 不校验 reserve / MEM_RELEASE 非 LIFO / MEM_DECOMMIT dwSize=0

- id: REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/shims_core.cpp#virtual-release-any-order
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

## 判据（已落地）
- reserve 后再 `MEM_COMMIT` 同址 ⇒ 成功；**未 reserve 的 hint + COMMIT** ⇒ 0 + 487，且宿主内存不被清零。
- 两块 bump 分配：释放前一块 / 释放最后一块 的行为见下（★ 第 2 条在 2026-11-05 被**改正方向**）。
- `MEM_DECOMMIT(ea, 0)`（ea 是块基址）⇒ 成功，整段变未提交。

## ★ 2026-11-05 修正：第 2 条的"正确方向"不是"拒绝非 LIFO"，而是"支持任意顺序"
收口时按本单当时的判据实现成"**只认最后一块 bump 块**，非 LIFO ⇒ 0 + 87"。**那条判据本身是错的**：
- 真实 Windows 允许**按任意顺序** `MEM_RELEASE` 每一个独立预留区（失败条件只有两个：
  `dwSize != 0` ⇒ 87；`lpAddress` 不是预留区**基址** ⇒ 487）。
- 实测证据（`apps/emulator-lift` 的 boot，104.5M 条指令处）：被测引擎在窗口建立后**连续释放 5 个显式预留区**
  （`0x3E000000` / `0x4D000000` / `0x4B000000` / `0x3D000000` / `0x31000000`），五条**全被拒**成
  `ERROR_INVALID_PARAMETER(87)` ⇒ 内存永远不回收。
- 处置：`VaRangeTable` 新增 `findBegin(ea)`（释放必须认**基址**，`find()` 是"包含"）；
  `MEM_RELEASE` 现在 = "基址命中活区域 ⇒ decommit 整段 + 从账本摘掉"，否则 487。重复释放 ⇒ 487（已不是活区域基址）。
- 用例：`virtual-release-non-lifo-rejected` **改名**为 `virtual-release-any-order`
  （释放前一块 ⇒ 成功且只退它；重复释放 ⇒ 487；释放中间地址 ⇒ 487 且区域原样）。
- 旧行为的另一条断言（`virtual-alloc-commit-release` 里的"再释放一次 ⇒ 87"）同步改成 487。
- ⛔ 教训：这张单的判据是**按文档措辞推的**，而"文档允许任意顺序"这件事只在**真引擎跑起来**时才暴露 ——
  判据要尽量从"被测对象的行为"反推，而不是从"我们希望它多严格"推。

## 证据
- 代码：`apps/emulator-lift/src/shims/kernel32_mem.cpp`（`shimVirtualAlloc` / `shimVirtualFree`）、
  `src/runtime/memory.h`（`VaRange` / `VaRangeTable`）。
- 夹具：`tests/shims_core.cpp`（`virtual-commit-hint-needs-reserve` / `virtual-release-any-order` /
  `virtual-decommit-zero-size` / `virtual-alloc-commit-release`）。
- 审计报告 `.tmp/m1a-audit/D-s6core.md`（D 组 2 条 high）—— 该报告的判据 2 已按上面改正。
