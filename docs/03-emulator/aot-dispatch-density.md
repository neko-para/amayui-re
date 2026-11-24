# AOT · 派发密度：FIELD 的 `lift` 为什么是 FELLOW 的 2 倍

> 读数日期 2026-11-24 / win32 / dev（`5dd35f9` → `53d438c`）/ RelWithDebInfo / `split8-O2-lto-abi`。
> 方法：两条**独立**的只读分析 —— ① host 目标码逐块统计（`.tmp/lift-cost/`）；
> ② LLVM IR 复审（`.tmp/ir-review/`，被判 `[策略问题]` 的逐条有 IR 行号）。
> 读数来源：`.tmp/emit-edges/report.md`、`.tmp/s7-indirect/report.md`。

## 0 结论

FIELD 的 `lift` 是 **2.54 ns/guest 指令**，FELLOW 切换帧是 **0.98**（同一次构建、同一台机器）。
差异的 **≈85% 由派发密度解释**：FIELD 每 **19.95** 条 guest 指令派发一次，FELLOW 是 **42.6–46.9**。

逐项排除：**不是** x87/MMX（全语料 `lift_x87_sem` 只有 24 处）、**不是** flags 急切物化
（实际是按位惰性 + 跨静态 call 的全局 `need_in`，`select i1 false` / `and -2262` / `ctpop` 是它的形态）、
**不是**块首尾寄存器搬运（A′ 之后 0 处，`%st` 直接指 `cpu_.r[0]`）、**不是** `lift_tick`/`lift_note_write`
（`/Ob2`+LTO 已内联，`lift_note_write` 0 处）。`align 1` 是 x86 非对齐必需，也不是问题。

钱花在**每次派发的入口包装**：body 只有 **3.0–3.2 条 host 指令/guest 指令**，但每个派发要额外付
**≈18 条 host 指令**（prologue 5–11 + 块内 switch 树 2–29 + epilogue 6–9）。

## 1 四个缺陷与修法（都已入库）

| # | 缺陷 | 证据 | 修法 | 读数 |
|---|---|---|---|---|
| **D1** | 同 component 的块也被当成派发出口（`labelFor`/`emitEdge` 只查 `entries_`，不查 `comp_.blocks`） | 静态同分量 `ret i32` **1,897** 处 | 命中即 `br %bb_<EA>`；三类**必须**继续派发：hook EA（`4BAA33`/`4BA718`）、intrinsic 根、`dropped_` | `ret i32` 1,897→**0**；`bb_exit` 块 3,796→**888** |
| **D2** | 跨 component 的 `jcc` 没有 tail call（静态 `jmp` 有）⇒ 逐像素循环每圈 2 次 trampoline | `lift_cc_48B4FC` 两个出口都是别的 component | 复用 `emitTailCallTo`，判据与静态 `jmp` 逐条相同 | `tail call @lift_cc_*` 1,030→**2,201** |
| **D3** | `%r = tail call i32 @lift_dyn(…)` 紧跟 `icmp` ⇒ **不是尾调用**，`tail` 标记是错的 | 5,804 处 | 改回普通 `call`（`lift_dyn` 内部的 `return fn(...)` 不动） | 5,804→**492**（剩余全是间接 `jmp`）；三份 exe `.text` sha256 全同 ⇒ 性能持平 |
| **D4** | S7 判据 4「有间接 `call` ⇒ 整单元判死」占降级根 76% | 间接 call 首因 1,682 | 间接 call 后 `esp` 未知（不假设 callee 的 `ret imm16`）、只有可证帧恢复（`mov esp,ebp`/`leave`）才放行；配套 `espAdd()` | 可结构化 **5,343→6,411（+20%）**、宿主 `call` 站点 **1,572→2,967（+88.7%）** |

D4 的接受条件（形状级差分）见 `repo:docs/03-emulator/aot-s7-evaluation.md` §5：CTest `lift-s7-indirect`
**先红后绿**，`-DLIFT_S7_RELAX_INDIRECT=OFF` 时 8 份 `.ll` 与放宽前逐字节相同。

## 2 合并读数（SAVE11 复现；交错 A/B）

| 读数 | 起点（D1/D2 之前） | D1+D2 | +D3/D4 |
|---|---:|---:|---:|
| `iter`（8 s 窗口派发） | 64.07 M | 52.25 / 53.11 M | **51.07 M** |
| `insns/iter`（派发密度） | 16.50 | 21.42 / 21.33 | **23.64** |
| `lift` ns/guest 指令 | 1.926 | 1.761 / 1.679 | **1.442** |
| 点击大帧（≈183 M → 151 M insns） | 409.2 ms | 278.7 / 282.1 ms | **210.2 ms** |

⇒ 那条大帧从 **409 → 210 ms（−49%）**；FIELD 装载帧同期 **5.56 s → ~3.6 s**（−35%，其中大头是
`_memcpy` 内建，见 `repo:docs/03-emulator/aot-intrinsics.md`）。

## 3 还没做的（下一刀在哪）

1. **块合并 / 超大块**：密度现在 23.6，要到 FELLOW 的 43 还得靠"相邻块合成一个 component"（fallthrough /
   无条件跳转链）。这是纯形状改动，收益按密度线性。
2. **D2 只覆盖 `structuredRoot(owner)` 那一档**（1,171 / 4,651 臂），其余属 S7 判据 4 / R1。
3. **热泵 `004BBAB0` / 主循环 `00412290` 仍未结构化**（`004B60C0` / `004E8C21` 没有可跟踪的帧恢复）
   —— 要再放宽就得回答"caller 的宿主帧还在不在"，那是另一单。
4. `--no-s7` 下 `lift-ops-diff` 会在 `psubw@0044B9DE` 崩（逐块夹具假设"一次调用=一个块"）—— 夹具不覆盖
   直连边形状；D1/D2 只在 S7 开启时生效。

## 4 复现

```bash
cd apps/emulator-lift
# 生成期：S7 读数（可结构化 / 宿主 call 站点 / 降级清单）
cmake --build --preset windows-x64 2>&1 | rg '\[S7 criteria\]|\[S7 downgrade\]'
# 运行期：窗口 profile + 派发计数（SAVE11 复现插件在 .tmp/field-bench/m11.js）
out\emulator-lift-aot.exe --present window --present-hidden --game-dir ..\..\links\install ^
  --appdata ..\..\.tmp\<私有> --skip-logo --audio off --time-limit 400 --log-level warn ^
  --frame-log --profile --profile-slow-frames 150 --plugin ..\..\.tmp\field-bench\m11.js
```
