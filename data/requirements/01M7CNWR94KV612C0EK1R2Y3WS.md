# 阶段 B · shim 蹦床：IAT 桩 EA → lift_shim → invokeById（含探针 / 实参 / 整块回读三条独立观测）

- id: REQ-01M7CNWR94KV612C0EK1R2Y3WS
- type: req
- status: done
- parent: REQ-01M7CHEA5HQ1905ZAJQYHX97CN
- verify: apps/emulator-lift/CMakeLists.txt#lift-ir-shim
- tags: [emulator-lift, lift, shim]

## 判据（都进守卫）

- **③ 与 ④ 走两条不同的路**：控制流序列两臂都是
  `0x4D4B40 → 0x436D00 → 0x4D4B5D → <桩 EA> → 0x4D4B63 → 哨兵`，且两臂**各自独立观测**
  （lifted = trampoline 的 dispatch 记录；解释器 = `cpu.last_eip` 真轨迹，断言窗口完整 + 每个 EA **恰好 1 次** + 顺序）。
- **shim 真的被调到**：探针（换掉 `timeGetTime` 的 `fn`，返回 `0x5EED1234`）每臂 **1 次**、`[E+0xFC]` 等于它；
  负控（探针不写 `call.result`）⇒ `[E+0xFC]` 变 0。
- **③ 的实参**：`[esp]` / `[esp+4]` / `[esp+8]` 两臂都断言（`buf` 用 `0xA5A51234` 这种非零标记）——
  把 `push ecx` 改压 `0xDEADBEEF` 只有这一条红。
- **整块回读**：探针写 `EDX = 0xABCD0001`；回读只留 EAX+ESP 就红（shim 会自己写别的寄存器）。
- 差分 0 不一致（80 组：8 GPR + ESP + EIP + 三片内存）· 写集合 ⊆ 声明区域 · `--lifted-only` 零移交。

## 两条接口事实（会咬人）

1. 桩 EA → shim **不能**用 `(ea - kStubBase) >> 2`：`shim_id` 是**注册序**、桩槽号是**发号序**，
   两者只在"注册顺序恰好等于发号顺序"时一致 ⇒ 走 `BlockCache::peek(ea)->shim_id`（与 dispatcher 同源）。
2. 桩页范围是 **32 KiB**（`GuestLayout`），不是 256 B ⇒ `isShimEa` 用 `[shim_page, shim_page + slots*4)`。

## 落点

`repo:apps/emulator-lift/tests/ir/lift_4D4B40.ll` · `repo:apps/emulator-lift/tests/lift_ir_shim.cpp` ·
`repo:apps/emulator-lift/src/runtime/lift_abi.h`（`lift_shim` / 停机哨兵）· `repo:apps/emulator-lift/src/runtime/lift_abi.cpp` ·
`repo:apps/emulator-lift/CMakeLists.txt`（新增 `lift-ir-shim`，pure）。

## 未做

`take_control`（shim 里调 guest 回调）的口径 · `.ll` 的 `bad:` 只在**链接期**保证不可达（没有进程内守卫）·
真 PE 的 `bind` 走法（本测试是 hermetic 的桩页）· 生成器。
