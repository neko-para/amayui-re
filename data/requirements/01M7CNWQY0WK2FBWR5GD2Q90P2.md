# 阶段 A · 形态：sub_436D00 的 .ll + resume-EA 表 + 与解释器混合模式 + 逐位差分 + lifted-only 负控

- id: REQ-01M7CNWQY0WK2FBWR5GD2Q90P2
- type: req
- status: done
- parent: REQ-01M7CHEA5HQ1905ZAJQYHX97CN
- verify: apps/emulator-lift/CMakeLists.txt#lift-ir-proto
- tags: [emulator-lift, lift, prototype]

## 判据（都进守卫）

- **差分 0 不一致**：240 组（冷 120 / 热 120；`n ∈ {0,1,3,31,4096}` × 随机 buffer，含非对齐与 `0x00`/`0xFF` 等边界），
  比 8 GPR + `EFLAGS & 0x8D5` + 三片内存哈希（表区 / buffer / 栈窗口）+ 出口 EIP + ESP。
  外部锚（不依赖解释器）：`n=9`、`"123456789"` ⇒ `0xCBF43926`。
- **冷路径真的走了**（堵"早退也当跑完"）：进入时 `[T+0x800] == 0`、两个建表器各移交解释器 **1 次**、见过 `0x436D19`、
  dispatch 冷 **5** / 热 **1**、解释器条数 = 闭式 `8n+19`（`n==0` ⇒ 14）+ 冷路径额外 3 + 建表器 **19,333**（实测）。
- **`--lifted-only` 负控**：一旦需要回解释器 ⇒ 子进程 **exit 9** 并打出 EA；热路径照常绿。

## 形态（本阶段钉住的）

- 入口 `i32 fn(LiftRegs*, u8* mem, u32 resume_ea, void* cpu)`，`resume_ea` = 函数入口 ∪ 每条 `call` 的下一条
  （`0x436D14` / `0x436D19` 都在函数中段，表必须按 resume EA 建）。
- trampoline 只做 `EA → 宿主函数` 表查 + 间接调用；分量内分支直接跳；`call` = 压 guest 返回地址 + 返回被调方 EA；
  `retn imm16` = 弹返回 EA（`esp += 4 + imm16`）+ 返回它。
- 混合模式：两个建表器**故意留在解释器**，用来证明"lifted ↔ 解释器"的往返真的通。

## 落点

`repo:apps/emulator-lift/src/runtime/lift_abi.h` · `repo:apps/emulator-lift/src/runtime/lift_abi.cpp` ·
`repo:apps/emulator-lift/tests/ir/lift_436D00.ll` · `repo:apps/emulator-lift/tests/lift_abi_probe.cpp` ·
`repo:apps/emulator-lift/tests/lift_ir_proto.cpp` · `repo:apps/emulator-lift/CMakeLists.txt`（新增两条 pure CTest）。

## 未做

`.ll` 生成器（这 30 条是手写的）· 块边界的 22 类钩子 · x87 / SSE · SMC 写自身代码 · arm64 · 整 `.text`。
