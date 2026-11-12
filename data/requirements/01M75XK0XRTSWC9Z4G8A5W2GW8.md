# rep movs/stos 解释器快路径（每元素 45–75 条宿主指令 → 近 memcpy；覆盖 299 站点）

- id: REQ-01M75XK0XRTSWC9Z4G8A5W2GW8
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/rep_bulk.cpp#gate/direction-overlap
- tags: [emulator-lift interpreter performance]

## 目标

把解释器里 `rep movs*` / `rep stos*` 的**每元素 45–75 条宿主指令**压到接近 `memcpy` 的水平（**不是 override**）。

## 评估要点（2026-11-12）

- `rep` **不是** trivial：`exec_string.cpp:110` 是"一条指令内的宿主 `while`"，整条 `rep movsd` = **1 次 dispatcher 迭代 + 1 次解码**。
- 贵在**每元素固定税**：45–75 条/dword（11–19 条/字节），其中 **15–22 条是 SMC 记账**（`writeElem` → `noteGuestWrite` 每元素一次、跨 TU 无 LTO）。
- 覆盖 **299 个静态站点 / 159 个函数**（267 `rep movsd` + 31 `rep stosd` + 1 `rep stosw`；AGERC 另有 87 `rep stosd`）；
  大头在 Live2D / GDI 文本 / UI / loader（236/299），编解码段只 6%。`rep cmps/scas` 语料 **0 处** ⇒ 不做。
- override 只覆盖 `_memcpy` / `_memcpy_0` 两个 CRT 体，这 299 处**一处碰不到** ⇒ 本单是**增量**收益。

## 落地

- `exec_string.cpp:149` `repBulkPlan`（**闸门唯一真源**）· `:99` `repBulkExecute`；调用点在 `ecx==0` 早退之后、`while` 之前，
  非 Ok 即退回同一个逐元素臂。
- `src/runtime/rep_bulk.h`（新）：`RepBulkWhy/RepBulkPlan/repBulkPlan`；★ 头注说明为何把"判定"暴露给夹具
  （跨 4 GiB / committed 这两条在 guest 可见状态上**观察不到**——放过去就是 fault，夹具自己先崩）。
- `src/runtime/guest_copy.h`（新）：两条方向循环 + `fillPatternBytes`，**从 `native_crt.cpp` 纯搬运**；`nativeMemcpy` 本体一字未改。
- `X86::rep_bulk`（默认 true、**同进程可切**）+ `rep_bulk_hits/declines`；摘要新增 `rep-bulk : hits=… declines=… (bulk=on|off)`。

## 八道闸门（判据全在 `repBulkPlan`）

① `rep!=0 && op∈{Movs,Stos} && size∈{1,2,4}`（F3/F2 等价，有 `repne movsd` 用例）·
② `n=(u64)ECX*size`；**`n>=4GiB` 单列退回**（否则 `start=0` 时 `[0,4GiB)` "合法"却被截成 0 ⇒ 什么都不搬、ECX 却清零）·
③ 访问区间：DF=0 `[start,start+n)`、**DF=1 `[start-n+size,start+size)`**（首元素在起点）·
④ 写前 safe-mode committed · ⑤ `ECX==0` 不碰内存 · ⑥ EFLAGS 一位不写 ·
⑦ **方向×重叠**：只有"写落后于读"才按块搬，其余退回逐元素臂 ·
⑧ SMC：写前一次 `noteGuestWrite(整段)` —— **逐页等价**（`noteWrite` 内部本就按页循环，早退条件在闸门下不触发），不是变粗，且少 `n/size-1` 次跨 TU 调用。

## 守卫

`tests/rep_bulk.cpp`（pure，顶层注册）：8 `n` × 7 op（含 `repne movsd`）× 2 DF × 4 重叠关系 = **448 场景 / 896 run / 9009 checks**；
比目标区逐字节 + 8 个寄存器 + EFLAGS + `insns` + `g_access`；三档均非空（hits=338 / 闸门退回=54 / 不适用=56，防"从没走到快路径也全绿"）。

变异（每条实测红后还原）：①方向闸门互换 ⇒ **255 红，其中 34 条是真内存"涂抹"**（已作 `mutate-check` 代表）·
②去 committed 预判 ⇒ 3 红 · ③`ECX*step` 改 32 位 ⇒ 3 红 · ④`n>=4GiB` 闸门失效 ⇒ 1 红。

## 收益（op-count 估，未反汇编核对）

每字节 **11–19 → 0.2–0.5**；1 MiB ≈ **25–80×**、4 KiB ≈ 25–60×、32 B ≈ 6–10×；
**单个 dword ≈ 1×**（固定税主导，⛔ 别算进"提速"叙事）。

## 没落地

- **没有 `--no-rep-bulk` 进程级开关**（只做了 `X86::rep_bulk` 供夹具切两臂）。
- **feeder 的计数不进主摘要**：feeder 用 `audio_cpu_ = *cpu_` 副本 ⇒ 音频路径上的 `rep-bulk` 读数**偏低**。
- ⚠ 副作用：agent 为跑 `mutate-check` **重新 configure 了 `apps/emulator-lift/out`**（只 configure、未 build）。
