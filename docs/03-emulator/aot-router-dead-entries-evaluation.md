# AOT router “死项清理”评估：no-go（5,312 不是死项）

> 读数日期 2026-11-20 / win32 / main `ede8671`（note OFF；alloca 实验已回退）。
> 证据与命令见 `.tmp/aot-router/analysis.md`（本轮只读分析，未改 tracked / 未构建）。
> 生成物口径：`apps/emulator-lift/out/aot` 的 8 份 `.ll` + `lift_table.inc` + 300 帧基线 run。

## 0. 一句话

**不做。** 之前的“5,312 个 `lift_dyn` resume 是死项”是**把调用点数当成了死项数**：
`out/aot` 有 35,222 个 `switch` case（10,980 roots + 24,237 call_ret + 5 D21），
`lift_table.inc` 也正好 35,222 行；300 帧基线里 `lift_dyn lookups=16,450,343 misses=1,432,759`
（`dispatch shim=1,432,759`、`table misses=0`）——这些 resume **正在被走**，删表项会把 143 万次
路径变成 `lookup` miss。真正能证明“没有任何静态引用”的 host-call resume 只有 **353 个（1.0%）**，
删掉只省 353 行表（约 15 KB）/ 353 个 case，**不减少任何块代码**，`.text` 收益≈0。

⇒ 不实现；把精力放在真正的 router 形状（入口 switch / 派发 / 块边界），而不是删表项。

## 1. 计数（可复核）

| 量 | 读数 |
|---|---:|
| `define i32 @lift_cc_*` / `switch i32 %ea` | 10,980 / 10,980 |
| `switch` case 数 | **35,222** = 10,980 roots + 24,237 call_ret + 5 D21 |
| `lift_table.inc` 行数 | **35,222**（cases = table = pin 计数） |
| `emitCallReturnCheck` 站点 | **6,884** = 1,572 host + 5,312 `lift_dyn` |
| 不同 resume EA | 6,312 |
| 300 帧基线 `entries` | 35,222 |
| 300 帧基线 `lift_dyn` | lookups 16,450,343 / **misses 1,432,759** / shim 1,432,759 |
| 300 帧基线 table | `lookups=52,024,536 hits=52,024,536 misses=0` |

`lift_dyn` miss 的语义（`src/runtime/lift_abi.cpp:457-467`）：把目标 EA 原样返回；shim 执行完
`cpu.eip = resume`；trampoline 再 `lookup`（`src/aot/aot_run.cpp:895-941`）。⇒ 这些 resume EA
**必须**在 `lift_table.inc` 里；删了不是“省一条 case”，是让这条 1.43M 次的路径变成 miss。

## 2. 353 个“干净”候选也不值得

1,129 个 host-call resume 走本地 `br`，其中：

- 17 个仍有静态引用（`bb_exit` / host target / `ret i32 E`）；
- 759 个 callee 闭包可达 dispatch 出口 / 间接 jmp；
- 剩 **353** 个：pinned PE 全 8 个 section、所有字节偏移扫 dword = **0 命中**（也不是 D17/D21/D26 指针）。

但 353 个块仍要发射（`emitCallReturnCheck` 的本地 `br` 就是进 `%bb_resume`），删掉只少 353 个
`switch` case 和 353 行表；`kSlotCount` 仍 131,072、`avg_cmp` 不变，`.text` 约 0。对比 note OFF
（39,644 个调用站点、`__text` −15.6%），量级差两个数量级。

## 3. 若强行做，失效方式

`switch` / `lift_table.inc` / pin 计数三处必须同步：

- 表项删了但 `switch` 还在 / 反之 ⇒ `lookup` miss（`decode-error`，响亮但可能很晚）或
  `bad:` 的 `llvm.trap`；
- pin 计数忘改 ⇒ readout 静默说谎（今天 `--verify-split` 只比函数名，不比 EA）。

⇒ 即使做，也只能是 `LIFT_AOT_DROP_INTERNAL_RESUMES` 的 opt-in 实验 + tombstone 计数，只对 353 做；
⛔ 不作为产品改动。

## 4. 裁决

- **不实现死项清理**；不改 `lift_gen.cpp` / `lift_gen_emit.cpp` / `lift_table.inc` / pin。
- 下一步（router 形状）另开评估：入口 switch / 派发 / 块边界到底值多少，是否该拆；
  不要再用“删死表项”这个前提。
