# AOT · 发射形状：同分量边直连 + 跨分量 jcc 走 tail call

- id: REQ-01M85TQW3Z2QR2KWGCQB34KWAP
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- verify: apps/emulator-lift/tests/emit_edges.cpp#lift-emit-edges
- tags: [emulator-lift, aot, perf, dispatch]

# AOT · 发射形状：同分量边直连 + 跨分量 `jcc` 走 tail call（派发 −17~18%）

## 一句话

FIELD 的 `lift` 是 2.54 ns/guest 指令、FELLOW 是 0.98，**85% 由派发密度解释**（19.95 vs 42.6–46.9 条
guest 指令/派发）。本单修两处发射形状：① `labelFor`/`emitEdge` 原来只查 `entries_`、不查 `comp_.blocks`
⇒ 目标块**就在当前 component 里**的边仍发 `ret i32 <EA>`（回 trampoline 再派发回来），共 **1,897** 处；
② 跨 component 的条件分支没有 tail call（静态 `jmp` 有）⇒ 逐像素循环每圈吃 2 次 trampoline。

## 判据

1. 同分量边命中即 `br %bb_<EA>`；`jcc` 的"跳成功"一支复用**静态 `jmp` 已有的** tail-call 形态
   （`emitTailCallTo`，⛔ 不发明第二种约定）。
2. **三类必须继续派发**（已加守卫、合成分量图各自钉住）：hook EA（`0x4BAA33`/`0x4BA718`）、
   intrinsic 根（`0x4E74B0`，闸门只从 switch 根 case 进）、`dropped_`（无块 ⇒ 非法标签）。
3. CTest `lift-emit-edges`（默认档，31 条 IR 文本断言）：静态同分量 `ret i32` 计数为 0、三类仍派发、
   `jcc` 的 tail call 形态存在；变异（删 `labelFor` 直连分支）必须红并点名 `bb_exit_401010`。
4. 五项指纹（`exit` / `stop @` / arena / `table misses`）逐位同。

## 读数（2026-11-24 / dev `71649df`；move11，同 8 s 墙钟）

| 读数 | 基线 | 改后（两趟） |
|---|---:|---:|
| `iter`（派发次数） | 64.07 M | **52.25 / 53.11 M（−18.4% / −17.1%）** |
| `insns/iter`（密度） | 16.50 | **21.42 / 21.33（+29%）** |
| `lift` ns/guest 指令 | 1.926 | 1.761 / 1.679 |
| 点击大帧 | 294.13 ms | **278.70 / 282.14 ms（−5.2% / −4.1%）** |
| boot / FIELD 装载帧 | — | −26~28% / −23~33% |

计数：静态同分量 `ret i32` **1,897 → 0**、`bb_exit` 块 3,796 → 888、`tail call @lift_cc_*` **1,030 → 2,201**。
门禁：`ctest` 139/139 · `pnpm test` 618 · `pnpm verify` 466。

## 未做 / 已知

- **只在 S7 开启时生效**；`--no-s7` 下 `lift-ops-diff` 会在 `psubw@0044B9DE` 崩 —— 那个逐块夹具假设
  "一次调用=一个块"，不覆盖直连边形状。
- `jcc` 的 tail call 只覆盖 `structuredRoot(owner)` 那一档（1,171 / 4,651 臂），其余属 S7 判据 4 / R1。
- 下一刀是**块合并 / 超大块**（密度 23.6 → FELLOW 的 43）：见
  `repo:docs/03-emulator/aot-dispatch-density.md` §3。
