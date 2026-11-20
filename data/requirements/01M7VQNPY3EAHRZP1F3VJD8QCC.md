# AOT 蹦床/块边界瘦身：P（泵判定内联）+ lookup-TLS + A′（%st 别名 cpu_）

- id: REQ-01M7VQNPY3EAHRZP1F3VJD8QCC
- type: req
- status: doing
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, perf]

## 一句话

把 SAVE05 移动窗口的 `boundary`（块边界宿主税，23.6%；compute-bound 窗口 27.7–29.9%）压下去：
每派发 ≈149 条 host 指令里，**P**（合成 EA 泵判定内联）与 **A′**（`%st` 别名 `cpu_`，删掉每派发
22 条寄存器拷贝）合计目标 **−24% boundary**。判据 = **每派发 ns**（wait-closed 窗口下绝对 ms 不可判）
+ 语义指纹。

## 读数（2026-11-20 / mac arm64 / `out/emulator-lift-aot` 反汇编 + SAVE05）

- `AotHost::run` 循环头 → `bl lookup` 的 fast path **93 条**；`bl liftRegsToCpu` + 本体 19 条 = **22 条/派发**；
  `bl pumpSyntheticEas` + fast body ≈ **29 条/派发**；boundary ≈ **149 条/派发**（拷贝 14.8% / 泵 19.5%）。
- SAVE05 移动窗口：boundary **1878.8 ms** / 195 帧（干净档 N=3）；prof60 **1825.5 ms**。
- **P 已落地**（commit `793826b`）：泵三首判内联 + lookup-TLS 提升 ⇒ boundary **11.075 → 10.091 ns/派发（−8.9%）**、
  lift **11.677 → 10.598 ns/派发（−9.2%）**；绝对 ms 只 −2.9%（wait-closed：多转了 6.4% 派发）。
  设计全文 `.tmp/perf-reassess/p2-design.md`（临时区，⛔ 不入库）。

## 待做（A′）

- `X86` 的 `eip`/`eflags` 互换 ⇒ `r[8]@0 / eflags@32 / eip@36`，与 `LiftRegs`（36 B）逐字节同布局；
  `x87_layout.h` 零改动（fpu 仍 @64），同步 `lift_abi.cpp` 3 条断言 + `lift_abi_probe.cpp` 3 条。
- 5 处 `LiftRegs*` 改 `void*` + `liftRegsView()`（只比较地址，不产生指向 `X86` 的 `LiftRegs*` ⇒ 规避严格别名 UB）；
  `liftRegsToCpu`/`liftCpuToRegs` 别名时跳过；AOT 删 `regs_` 与两个 sync、fault site 改 provenance。
- 变异 1 条：删别名判据 ⇒ 非别名调用方在过期寄存器上执行 ⇒ `ctest:lift-ir-shim` 红。
- 后续可立项：`lookupUncounted`（`lift_dyn` 45–48 M 次/轮）仍走 TLS；缺省档 `probe`/`table` 的
  comparisons 变 0（要 `n/a` 需加 armed 位）。

## 判据

1. 反汇编：`AotHost::run` 内 `bl liftRegsToCpu` = **0**；`bl pumpSyntheticEas` fast path 不可达（P 已达成：
   静态 1 条但只在命中分支）。
2. A/B：**boundary ns/派发** ≥5%（wait-closed 下用同派发数折算；绝对 ms 只报不判）；≥3 对交错。
3. 指纹（`exit` / `stop @` / arena 三元组 / `table misses`）逐位同；ctest 全绿。
4. 变异一条能红（`--match 'lift-ir-shim'`）。

## 边界

⛔ 不改 guest 可见语义；⛔ 不做 B（惰性同步：漏一个消费点 = 静默错值，且 RelWithDebInfo 档没有可红的守卫）；
⛔ 不做直连 / chaining / 超级块 / 跨块 SSA（另单）。
