# emulator-lift · 退出写存档 LZSS Encode（sub_4367B0）的 native override 落地

- id: REQ-01M7375DARF1QFZEFNWX3CAK6T
- type: req
- status: done
- parent: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- verify: apps/emulator-lift/tests/native_override_e2e.cpp#native-override-e2e
- tags: [emulator-lift profile native-override lzss save]

## 一句话

`sub_4367B0`（**`0x4367B0`**，`retn 8`、thiscall，`ecx` = `Compress` 对象 `0xD430`）的**整函数** override
**已进 `all`（2026-11-11）**：1 次拦截覆盖退出写存档尾段 92.85% 的墙钟。

## 已核（锚 = EA）

- 尾段 = 最后一帧后 3.58 s / 131.1 M 条指令、0 次 Present；`sub_4367B0` 子树占 93.46% 指令 / 92.85% 时间。
- 形状：`+4 text_buf[4114)` / `+0x1018 dad` / `+0x501C lson` / `+0x9020 rson` / `+0xD424 match_pos` /
  `+0xD428 match_len` / `+0xD42C out`；整函数 `delete[]` 旧 `out` + `new[](*a3+4096)`。
- 两个调用点：一个是 `new Compress`，另一个是**栈局部对象**（`0x457D17 lea ecx,[ebp+var_D44C]` /
  `0x457D25 call sub_4364E0`）；两者 `out` 都为 0。
- `0x436811` 的 `if (!p) return 0` 是**死代码**：`??_U`(`0x4E6DB8`) → `??2`(`0x4E7D0D`) 有 `__callnewh` 重试，
  失败 `_CxxThrowException`(`0x4E8A6E`)，不返回 0。
- `_malloc`(`0x4EA519`) = `HeapAlloc(hHeap,0,max(size,1))`（无 CRT 块头）⇒ 与
  `cpu.heap->alloc(in_size+0x1000, zero=false)` 同源；`_free`(`0x4EA462`) 能释放我们的块。
- 出参：`a3[1]` = 实际消费字节数（按计数写）、`a3[2]` = 压缩后大小；`EAX = [this+0xD42C]`；
  `comp >= in` ⇒ 原样存储。

## 实现与准入

- 纯核 `lzssEncodeObject`（原地改 `this` 的宿主视图；`in_ea` 只用来复现 32 位地址比较）+ 薄包装
  `nativeLzss4367B0`（准入 / guest 堆分配 / 出参 / 失败回退）。
- 准入 7 条（全部在**任何 guest 写之前**）：`a3` 12 B 已提交 · `[this, this+0xD430)` 已提交 · 输入跨度已提交
  （`a2+a3[0]` 不回绕）· `a3[0] <= 0xFFFFEFFF` · `[this+0xD42C] == 0`（`ReusedOut`）· 有可用 guest 堆 ·
  分配失败回退。
- 已知边界：**不补虚拟时钟**（省掉的指令数随数据变，没有定值 k）⇒ 存档时间戳会比不 override 时早 ~12 s。
  相位偏移是登记的已知偏差，不是缺陷。

## 守卫

- pure：`lzss/tree-invariants` · `tie-strict-greater` · `independent-transcription` · `roundtrip-decode` ·
  `wrap-address` · `admission-no-write`；`naiveLzssEncode` 的 `lson` 补到 `N+1+256` 并加左支上界自检。
- assets：9 档真字节两臂（4 KB / 150,068 B 的可压与不可压、`tie-8`、空、1 字节、17/18）；比
  `EAX/ESP/EIP/EBX/ESI/EDI/EBP` + `info[0..2]` + 整个 `0xD430` 对象 + `out`。
- assets 新增 `lzss/decline-replay`：预设旧出缓冲（`out != 0`）⇒ `hits=0` / `fallbacks=1`，两臂全同、
  `allocCount` 相同（证明 override 没在旁路分配）。夹具同时补绑了 `HeapFree` 的 IAT（原来只绑 `HeapAlloc`，
  回退后 guest 的 `delete[]` 会跳进数据段）。
- e2e：三份 run（a1 空 / b 开 / a2 空）；时钟判据改成**字段语义**（`SYSTEMTIME@264` 的 16 B ∪ `derived@280` 的 4 B），
  不再用固定 5 字节掩码（旧掩码是数据快照：秒数跨 256 就无 bug 红）。

## 实测

- e2e：尾段 131,104,827 → 8,495,927 条（−93.53%）；墙钟 7681.5 → 649.9 ms；`hits=1 fallbacks=0`。
- 存档差异 4 B 全落 `SYSTEMTIME`；派生秒 a1=93 / b=81、`Δtail_insns = 122,608,900` ⇒ 12.261 s（相位差的直接读数）。
- a1 vs a2（同一臂两次）逐字节相同。

## 未做

- 不覆盖 `sub_436550`（InsertNode）：它的逻辑已在 `lzoInsertNode` 里，单独加项被整函数支配（150,086 次拦截只换
  35%，整函数 1 次换 92.85%），且 `sub_4367B0` 命中后它一次都不会执行。
- 不覆盖 `sub_4366A0`（DeleteNode）/ `sub_437480`（写文件）/ `sub_438320`（序列化）/ `sub_40AAE0`（事务层）。
- 不证明"存档能被游戏读回"。
