# AOT 蹦床/块边界瘦身：P（泵判定内联）+ lookup-TLS + A′（%st 别名 cpu_）

- id: REQ-01M7VQNPY3EAHRZP1F3VJD8QCC
- type: req
- status: doing
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, perf]

## 一句话

把 SAVE05 移动窗口的 `boundary`（块边界宿主税；compute-bound 窗口 27.7–29.9%）压下去：
**P**（合成 EA 泵判定内联）+ **lookup-TLS**（已落地）与 **A′**（`%st` 别名 `cpu_`，删掉每派发 22 条
寄存器拷贝，已落地）合计 boundary **−13.4% ns/派发**。判据 = **每派发 ns**（wait-closed 下绝对 ms 不可判）
+ 语义指纹。

## 读数（2026-11-20 / mac arm64 / SAVE05 s05move / A/B 交错）

| 项 | before | after | 变化 |
|---|---|---|---|
| P + lookup-TLS（`793826b`）boundary | 11.075 ns/派发 | 10.091 | **−8.9%**（3/3） |
| 同上 lift | 11.677 | 10.598 | −9.2% |
| A′（`fbe4760`）boundary（干净 6 对） | 10.780 | 10.239 | **−5.02%**（6/6 同向；中位 −4.47%） |
| A′ boundary（prof60 2 对） | 11.387 | 10.738 | −5.70%（2/2） |

- 反汇编：`AotHost::run` 内 `bl liftRegsToCpu` **2 → 0**、`bl liftCpuToRegs` **4 → 0**；
  `bl pumpSyntheticEas` 静态 1 条但 fast path 执行 **0**（13 条 guard + `cbz` 跳过）。
- 指纹 17 次 run 逐位同：exit 0 · `stop @ eip=00412383` · arena `89668632,119683880,182` · `table misses=0`。
- 布局：`X86` = `r@0 / eflags@32 / eip@36 / seg@40`；`x87_layout.h` 零改动（fpu @64）。
- 变异：`--match 'lift-ir-shim'` 1/1 红（CTest 8），还原后绿。
- 报告：`.tmp/perf-reassess/p2-design.md` / `p-implement-report.md` / `a-prime-report.md`（临时区，⛔ 不入库）。

★ **A′ 的判据裁决**（2026-11-20，父级）：记为**方向确认**（6/6 + 2/2 同向；臂中位之比 −5.02%，
逐对中位 −4.47% —— 落在本机 5% 分辨力的边缘）⇒ 不写成"稳健 ≥5%"；hidden 档复核留待后续。

## 未做 / 待核

- **A′ 的 A/B 是 visible 档**（hidden 新口径 14:27 才到，按"不混臂"保留）；后续评测用
  `--present window --present-hidden`（`.tmp/save05-bench/protocol.md`）。若要 hidden 读数，两臂都重跑。
- `lookupUncounted`（`lift_dyn` 45–48 M 次/轮）仍走 TLS；缺省档 `probe`/`table` 的 comparisons 变 0
  （要 `n/a` 需加 armed 位）。
- win32 的 `/Ob1` + LTO、`offsetof` 条件支持合法性 `<待核>`（本轮只验 macOS）。
- ⛔ 不做 B（惰性同步：漏一个消费点 = 静默错值，无可红守卫）；⛔ 不做直连 / chaining / 超级块 / 跨块 SSA（另单）。

## 判据（每次改动）

反汇编 `bl liftRegsToCpu` = 0；boundary **ns/派发** ≥5%（≥3 对交错）；指纹逐位同；ctest 全绿；
变异一条能红（`--match 'lift-ir-shim'`）。
