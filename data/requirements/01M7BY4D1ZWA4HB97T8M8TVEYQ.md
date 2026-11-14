# emulator-lift · CRC-32 两变体（sub_436D00 / sub_436D50）函数级 native override

- id: REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#crc/decline-no-write
- done_reason: 两条 CRC-32 变体做成函数级 override（native_crc32.{h,cpp} 的纯核 crc32Bytes + 两个 fn，thiscall/8B、只写 EAX、表从 guest 的 [this,this+0x800) 读 —— 文件里 0 个多项式常量）。闸门顺序 G2（this+0x804 committed）→ G1（[this+0x800]==1）→ G3（循环次数==0 ⇒ 接管返回 0）→ G4（[buf,buf+n) 不跨 4 GiB 且 committed）；kMax 18→20 并更新'表已满'注释。★ 两处按机器更正了单子（正文已重基）：① n=0x80000000 **不是**'循环 0 次'（0x436D29 dec edx ⇒ 0x7FFFFFFF ⇒ 0x436D2A js 不跳 ⇒ 真跑 2^31 次）⇒ 实现把负 n 折成循环次数，INT_MIN 落 G4 **回退**，守卫拆成 crc/nonpositive（5 个负值接管+0）与 crc/int-min-declines（回退）；② 两个变体**不是同一个 CRC**（d00=CRC-32/ISO-HDLC '123456789'⇒0xCBF43926、d50=CRC-32/BZIP2⇒0xFC891918）⇒ 删掉'两变体应一致'那条假红，改成两条标准 check value（同时钉住'两个 fn 查同一张表'）。守卫：pure 288407 checks（decline-no-write 8 个回退类别 + 对象/缓冲逐字节不变、nonpositive、take-over）、assets 真字节两臂 2705 checks（n=0/1/3/31/4096 × 两变体、两条标准值、两条 flag==0 回退重放；比 EAX + 512 dword 表 + 缓冲 + cpu.insns 闭式 8n+18 / 9n+18）；修前红（三处破坏各自打红自己那组，移位写反下 d50 保持绿）。变异 crc>>8→<<8 ⇒ crc/d00 组红（父级复跑 1/1）。A/B 读数：d00 全吃（块 00436D31 整行消失）、d50 只吃一半（每个 CRC 对象第一刀 flag==0 必须回退 = G1 的结构性代价），单刀 native ≈3.8 ms vs 解释器 624-1049 ms；两臂窗口/调用数不对齐 ⇒ guest 7223.6→6457.6 不作收益结论。已知遗留：d50 剩下那 2 刀要靠段级 override @0x436D31/0x436D81（另开单）；=== 异常 === 每次存档都会出现（declines 被 machine.cpp 算成异常，属既有口径，未改）。
- tags: [emulator-lift, native-override, save-load]

## 2026-11-14 判据更正（实现者按机器复核，父级采纳）

原文两处与指令流不符，**以机器为准**：

1. **"`n <= 0` ⇒ 循环 0 次"只对 `n == 0` 与"负数（除 INT_MIN）"成立**。
   `0x436D29 dec edx` 把 `0x80000000` 变成 `0x7FFFFFFF`（SF=0）⇒ `0x436D2A js` **不跳** ⇒ guest 真的跑 **2^31 次**
   （循环尾 `0x436D44 dec edx` / `0x436D45 jns` 是同一个计数器）。
   ⇒ 实现按机器走：负数先折成**循环次数**（`n == 0` 或「负数取绝对值后为 0」⇒ 接管并返回 0）；
   **`INT32_MIN ⇒ 2^31 次**，会落到 G4（`[buf, buf+2^31)` 跨 4 GiB / 未提交 ⇒ **回退**，不是返回 0）。
   守卫拆成两条：`crc/nonpositive`（`0 / -1 / -2 / 0x80000001 / 0xFFFF0000` ⇒ 接管 + EAX=0）与
   `crc/int-min-declines`（`0x80000000` ⇒ **回退**）。
2. **两个变体不是同一个 CRC**：实测（解释器两臂，真字节）`"123456789"` ⇒ `0x436D00`（反射）= **0xCBF43926**
   （CRC-32/ISO-HDLC）、`0x436D50`（非反射）= **0xFC891918**（CRC-32/BZIP2）。
   ⇒ ⛔ 不许写"两变体应给同一值"这类断言（假红）；改成两条**标准 check value**，它同时能钉住
   "两个 fn 查了同一张表"这类错。
3. 函数级表**没有** `feeder_ok` 字段（`audio.cpp` 把整张函数表拷给 feeder）⇒ 原文"`feeder_ok = false`"在函数级无落点；
   本项在 feeder 里也安全（纯读 / 零写 / 零分配）。
## 身份（锚 EA，取自 `.lst`）

| EA | 原文 | 结论 |
|---|---|---|
| `0x436D00` | `sub_436D00 proc near` / `retn 8` | `__thiscall`，2 个栈参 = `(n, buf)`，ECX = 表对象；**反射变体**（表在 `+0x400`） |
| `0x436D50` | `sub_436D50 proc near` / `retn 8` | 同上；**非反射变体**（表在 `+0`） |
| `0x436D06` / `0x436D56` | `cmp dword ptr [esi+800h],0` / `jnz` | 惰性初始化标志在 `[this+0x800]` |
| `0x436D0F` / `0x436D14` | `call sub_436BD0` / `call sub_436C60` | 两个建表器（多项式 `0x4C11DB7` / `0xEDB88320`） |
| `0x436D31..0x436D45` | 每字节 8 条（`xor edi,ebx` / `shr eax,8` / `xor eax,[esi+edi*4+400h]`） | 反射查表 |
| `0x436D81..0x436D93` | 每字节 9 条（`shr edi,18h` / `shl eax,8` / `xor eax,[esi+edi*4]`） | 非反射查表 |
| `0x43758A` / `0x4375A6` / `0x437842` / `0x437858` | 写一条存档记录时 4 次 `call sub_436D50/00` | ⇒ 每次存档 ≈ 6× payload 的逐字节 CRC |

## 判据

1. **进表**：`NativeOverrideTable` 两条，`arg_bytes = 8`、`GuestConv::Thiscall`、只写 EAX。
2. **⛔ 不硬编码多项式**：表必须从 guest 的 `[this, this+0x800)` 读（512 个 dword）。硬编码 `0x4C11DB7` /
   `0xEDB88320` 在"表被别的路径重建"时会**静默分叉**，而 CRC 分叉的表现是"存档写出去、读回来对不上"，没有报错。
3. **闸门可判定**（不满足 ⇒ Declined，交解释器）：
   `[this+0x800] == 1`（表已建，否则回退由解释器建）· `[this, this+0x800)` committed ·
   `n > 0` 时 `[buf, buf+n)` 不跨 4 GiB 且 committed；`n <= 0` ⇒ **接管并返回 0**（guest 的循环 0 次，
   `~0xFFFFFFFF == 0`，不是回退）。
4. **逐字节等价**：同一 `--appdata` 副本上开/关这两项各存一次档 ⇒ `SAVE00.DAT` / `SAVE.DAT` 的 sha256 **相同**。
5. **零写**：不写 guest 内存（除不满足 G1 时回退）；差分只比 EAX/ESP/EBX/ESI/EDI/EBP/EIP 与内存。
6. **读数**：`--native-overrides all` vs `all,-sub_436D00,-sub_436D50` 两臂，保存段 / 读档段的
   `present-tail tail_wall_ms` 下降（读数进 `observations.md`，⛔ 不进断言）。

## 未取证 / 风险

- `sub_436D00` 与 `sub_436D50` **是否只差三处**（查哪张表 / idx 高低字节 / 移位方向）已逐字核过；但
  "还有没有第三个变体（第三种表）"**未取证** ⇒ 闸门里不要假设只有两张表，按 `this` 传表最安全。
- 两条函数的 `feeder_ok`：本轮**不需要** feeder（save/load 走主线程），照既有口径填 `false`。
