# AOT · 派发密度：按函数扩组 + 入口直入（23.6 → FELLOW 的 ~43 条 guest/派发）

- id: REQ-01M86NNG4YMF68NCAX0NT45Y22
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, perf, dispatch]

# AOT · 派发密度：按函数扩组 + 入口直入（把 FIELD 的 23.6 条 guest/派发推向 FELLOW 的 ~43）

## 一句话

FIELD 的 `lift` 曾是 2.54 ns/guest 指令、FELLOW 0.98 —— 归因证明**85% 由派发密度解释**（19.95 vs 42.6–46.9）。
D1/D2/D3/D4 之后密度到 **23.64**、`lift` **1.442 ns**、点击大帧 **409→210 ms**。本单继续：
**把更多块放进同一个 component**，让组内转移从 `ret i32 → trampoline → cmp+switch` 变成一条 `br`；
并把"必须付的那次派发"做薄。

## 第 0 步：先定价（不构建、不改码，半小时量级）

静态统计两笔，决定本单值不值：

1. **同函数、不同 component** 的边：多少条、覆盖多少派发（用 `window-dispatch-top` 的派发占比加权）；
2. **跨函数 `call`/`ret`**（S7 覆盖不到的）：覆盖多少派发。

⛔ 第 1 笔占大头之前不动发射器。若第 2 笔占大头 ⇒ 收益在 S7（见 `REQ-01M86NMT0YJ9V78XS2DZZV364Q`），
本单应降级/转成"S7 做完再看"。

## 方案（按顺序）

1. **按函数扩组**：同一 `entry_owner` 的块放进一个 component（`comp_.blocks`），组内边全部 `br`
   （D1 已经做了"同组边直连"，本单是把组**变大**）。
2. **收缩入口集合**：只有**外部可达**的 EA 才在入口 switch 里留 case（`lift_table.inc` 的行 + hook EA +
   intrinsic 根 + `dropped_` 四类；前三类旧规则照旧"必须继续派发"）；只被组内跳到的块**不留 case**。
3. **入口直入（R2）**：每 entry 发薄 thunk，或把稀疏的 `cmp` 链降成跳表 / `blockaddress`，
   把每次派发现付的 **≈18 条 host 指令**（prologue 5–11 + switch 2–29 + epilogue 6–9）压下来。
4. **热簇（可选、后置）**：把频繁互跳的 2–3 个函数合成一个簇（只对热簇做，靠 dispatch 占比挑）。
   ⛔ **不做**"所有块塞进一个巨型 component"：入口映射会涨到 35,222 行、单函数 74,502 块让 LLVM 优化/RA
   超线性变慢、失去热/冷分离，而**跨函数 `call`/`ret` 那部分它一分钱都拿不到**（见
   `repo:docs/03-emulator/aot-dispatch-density.md` §3）。

## 判据

- **主判据**：同一 8 s 窗口、交错 A/B ⇒ `iter`（派发次数）↓、`insns/iter` ↑、`lift` ns/guest 指令 ↓；
- 五项指纹（`exit` / `stop @` / arena / `table misses`）逐位同；
- `ctest` 全绿（至少含 `lift-emit-edges` / `lift-s7-indirect` / `lift-s7-structured` / `lift-intrinsics`）；
- 变异：**少留一个必须的外部入口 case**（或把 hook/intrinsic 根直连）⇒ 对应用例当场红。

## 起点读数（2026-11-24 / `53d438c` / SAVE11 点击复现）

| 读数 | 起点（优化前） | 现在 |
|---|---:|---:|
| `iter`（8 s 窗口派发） | 64.07 M | **51.07 M** |
| `insns/iter`（密度） | 16.50 | **23.64** |
| `lift` ns/guest 指令 | 1.926 | **1.442** |
| 点击大帧 | 409.2 ms | **210.2 ms** |

## 风险 / 未做

- 代码膨胀（i-cache）、编译时间、寄存器分配质量：**先按函数粒度**，不要一次全并；每步都取 A/B。
- `--no-s7` 下 `lift-ops-diff` 有既存崩溃（逐块夹具假设"一次调用=一个块"）—— 本单不要扩大那个面。
- 入口 switch 的"四类必须派发"是语义面，改动必须带守卫 + 变异（不是"看起来还能跑"）。
