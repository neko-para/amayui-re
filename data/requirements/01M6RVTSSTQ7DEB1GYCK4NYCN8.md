# emulator-lift · 段级 native override：sub_414AC0 填池循环

- id: REQ-01M6RVTSSTQ7DEB1GYCK4NYCN8
- type: req
- status: done
- parent: REQ-01M6PDSMKDBNW46Q8SEMAFF14S
- verify: apps/emulator-lift/tests/native_override.cpp#native-override

## 目标

`sub_414AC0`（SYS4INI 装载器）的 1.18 s 几乎全在**函数中段**的填池循环（`.lst` 0x415384，
约 7.37M 次迭代）。函数级 override 只在 `call` 目标拦 ⇒ 够不着；克隆整个装载器
（文件 I/O + guest 容器 / 堆 / `VirtualAlloc`）不值得。用户给定前提“**guest 代码只读** ⇒
特定 EA 处的代码是固定的” ⇒ 做**段级** override：在循环入口 block 按固定 EA 拦一次，
native 填完，按 guest 语义设寄存器/EFLAGS 并跳到循环出口。

## 取证（锚 = EA；`disasm-at at --ea 0x415340 --lines 45`）

```text
0x41537E  xor eax,eax
0x415380  test edx,edx
0x415382  jle 0x41539F                 ; count<=0 ⇒ 整段跳过
0x415384  mov ecx,[esi-10h]            ; 目标数组（= 0x415203 的 VirtualAlloc 返回值）
0x415387  mov edx,[ebx+5EC90h]         ; 填充值（dword）
0x41538D  mov [ecx+eax*4],edx
0x415390  mov ecx,[ebx+5D7E8h]
0x415396  inc eax
0x415397  lea edx,[ecx+edi+1]          ; count = [ebx+5D7E8] + edi + 1（32 位回绕）
0x41539B  cmp eax,edx
0x41539D  jl 0x415384                  ; **唯一**回边
0x41539F  mov eax,[ebx+5D7ECh]         ; 循环后第一件事；不读 EFLAGS
```

- 循环后：EAX = EDX = count、ECX = `[ebx+5D7E8]`；ESI/EDI/EBX/ESP 不变；
  EFLAGS = `cmp eax,edx` 相等 ⇒ ZF=1、**PF=1（差 = 0，低字节 0x00 奇偶为 even，与 count 无关）**、
  CF=AF=SF=OF=0（0x41539F 之后直到 0x4153A8 的 `add` 才再改标志，中间不读）。
- 全语料只有这一条 `jl` 引用 0x415384；`sub_414AC0` 只有一个调用方 `_WinMain@16` ⇒ 按 EIP 拦安全。
- 本段只填**一个**池（其余 4 个由紧随其后的 `_memset` 填，0x4153B5）；循环体无 call / 无异常。

## 实现

- 新增 `NativeBlockTable`（`entry_ea` / `resume_ea` / `name` / `fn` / `hits` / `declines` /
  `fallbacks` / `disabled`），与函数级表**分开**；dispatcher 在 `cache_.get(eip)` 之前按 EIP 查。
- `BlockOverrideOutcome = Replaced / Declined / Failed`：
  - `Replaced` ⇒ fn 已写好 EIP（= 段出口）/ EAX / EDX / ECX / EFLAGS；
  - `Declined` ⇒ 回退解释器，且**本次运行永久停用**该项（段级前提是静态的，继续每迭代重试只会
    把成本加回热路径）；`Failed` ⇒ 回退 + 计 fallback + 一行诊断。
- `nativeFillPool414AC0`：入口判据 `EAX==0`（`xor eax,eax` 之后；回边进来时 EAX≥1 ⇒ 一次调用只拦一次）；
  前置条件（`[esi-10]` / `[ebx+5D7E8]` / `[ebx+5EC90]` 已提交、count>0、目标区间在 4 GiB 内且已提交）
  全部满足才替换。
- ★ `replaced_insns = 8 * count`（每迭代 8 条），dispatcher 用 `cpu_.insns += n; clock_.tick(n);`
  补账 —— 否则 `timeGetTime` 会少走 ≈737 ms 虚拟时间，首帧可能与解释器路径不同。
- 启用：`KnownBlockNative { name="sub_414AC0", enclosing_fn="sub_414AC0", entry=0x415384,
  resume=0x41539F }`，`debug=false` ⇒ 进 `all`；`all,-sub_414AC0` 可排除。
  启用前用 `.lst` 校验锚（entry 属于 `sub_414AC0` 且首指令逐字是 `mov ecx, [esi-10h]`、
  出口是 `mov eax, [ebx+5D7ECh]`）⇒ 换镜像/改 EA 当场拒绝（⛔ 不静默换 EA）。
- 设计记录（本地、不入库）：`.tmp/native-override-414ac0-design.md`。

## 守卫（会红）

- `tests/native_override.cpp` ⑭：合成最小循环夹具（手写编码，逐条与 `.lst` 对齐），解释器 vs override 比
  目标数组 / 结果 dword / EAX·EBX·ECX·EDX·ESI·EDI·EBP·ESP·EIP / **EFLAGS** / **insns**；
  count ∈ {1,5,17,4097}；另有一组在 **0x41539F 就停**的 flags 子测（count ∈ {1,2,3,5,17,255,256,257,4097}）
  —— 续延里的 `add` 会覆盖标志，不停在出口就等于没比。Declined 路径（EAX≠0）也比一次。
- 变异验证（已做）：`replaced_insns` 从 `8*count` 改成 `7*count` ⇒ 差分当场红；
  把 EFLAGS 的 PF 去掉 ⇒ `eflags=0x246/0x242` 红。
- 真实 boot A/B（`--max-frames 1 --skip-logo`，fresh appdata）：段级 hits=1 / declines=0 / fallbacks=0；
  首帧 hash `0xD64F5548CAFE92E9` / BMP SHA256 与关 override **完全相同**；
  guest-block 墙钟 24,149 ms → 3,416 ms（其中 `sub_414AC0` 那 1.2 s 基本消失）。

## 剩余风险

- 只替换了**第一个池**的填充；其余 4 个池仍走 `_memset`（本来不热）。
- 段级项依赖“guest 代码只读”：本线不 patch 字节，锚校验只在启用时做一次；若将来 SMC 改到
  0x415384，需要重新评估（当前引擎不改这一块）。
- 段级项已经补偿虚拟时钟；函数级项仍是“一次 override = 1 条”（用户裁决：时间补偿不做）。
