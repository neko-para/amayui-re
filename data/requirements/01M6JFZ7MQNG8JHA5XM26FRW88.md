# 块缓存无任何失效路径：guest 写已解码字节后仍执行旧 Insn

- id: REQ-01M6JFZ7MQNG8JHA5XM26FRW88
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/dispatch.cpp#smc-invalidates-block
- repro: apps/emulator-lift/src/runtime/x86.cpp#void BlockCache::clear()
- severity: S2
- tags: [emulator-lift, m1, s2]

## 一句话
块缓存**没有任何失效路径**：`BlockCache::clear()` 只在测试里被调用；写路径（`wr8/16/32/64` / `writeBytes` / `decommit` / `VirtualFree`）都不通知缓存。guest 写已解码字节后仍执行旧 `Insn`（静默错误）。

## 落地（2026-11-04）
- `BlockCache` 增加**代码页位图**（4 KiB 粒度，最多 0x100000 位 = 128 KiB）：解码一个块时把覆盖的页标上。
- `X86::writeMem`（整数 / 数据写）与 `exec_string` 的 `writeElem`（`rep movs` / `stos`）在写之前调
  `noteGuestWrite(cache, ea, size)`；命中"已解码的代码页" ⇒ 置 `stale_`。
- **不**当场清缓存（当前块还在 `machine.cpp` 的 for 里执行 ⇒ 会拿到悬垂 `Block*`）；
  `BlockCache::get()` 在 dispatcher 边界看到 `stale_` 才 `dropBlocks()`，下一次从新字节重新解码。
- 堆 / 栈写命中的是"未标记"分支（一次位图查询），不改变热路径量级。

## 判据 / 变异
- `dispatch#smc-invalidates-block`：先调 target（缓存 `mov eax,0`），guest 用 `mov byte [..+1],7`
  改立即数，再调 target ⇒ 必须 `eax==7`。旧实现是 0（缓存的 imm 没更新）。
- 变异：删掉 `writeMem` 里的 `noteGuestWrite` ⇒ 该用例红（CTest 退出码 8）。
- A4 不回归：`boot-trace` 仍是 `insns=60652 hash=D06091AF5DD19647 inputs=35CB9281F6D0881B`。

## 保留（已知边界）
- **x87 / SSE 的 store**（`exec_fpu.cpp` 里直接 `mem->wr*` 的 `fstp` / `fstenv` / `movd` 等）与
  shim 侧的 `writeBytes` / `fillBytes` / `ReadFile` **不走**这条钩子 ⇒ 若 guest 用它们写代码页，
  仍会执行旧 Insn。当前语料没有这类证据；要闭合需要把钩子下沉到 `GuestMemory`（代价是热路径多一次间接调用），
  或逐一给 FPU store 加钩子。已写进本单的边界。
- `.text` 在 PE 里是 `MEM_WRITE`（`E0000060`），所以"写代码页"在真机上本来就能发生；这次只保证
  **整数 / 串操作**这条主路径。
