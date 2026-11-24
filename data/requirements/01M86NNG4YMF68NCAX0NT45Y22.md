# AOT · 派发密度：按函数扩组 + 入口直入（23.6 → FELLOW 的 ~43 条 guest/派发）

- id: REQ-01M86NNG4YMF68NCAX0NT45Y22
- type: req
- status: dropped
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- dropped_reason: 定价：第 2 笔跨函数 call/ret ≥70%（S7 的域），S7 已 done 但端到端 A/B 不可判定、无实测收益（默认 OFF）；第 1 笔同函数跨分量仅 10.6–13.3%（上界，含 .lst 噪声），本机 <5% 分辨力；第 3 类 shim 调用对 25–31% 是另一条链（shim 直呼）。⇒ 本单不做；若 shim 直呼落地，再重跑 pricing 决定扩组。
- tags: [emulator-lift, aot, perf, dispatch]

## 结论（2026-11-25：dropped）

第 0 步定价 + S7 收口后重判：**第 2 笔（跨函数 `call`/`ret`）≥70%，是 S7 的域；S7 已 done 但端到端
A/B 不可判定、无实测收益（默认 OFF）**。第 1 笔（同函数跨分量）只有 **10.6–13.3%**（上界，含 `.lst`
同函数判定噪声），本机分辨不了 <5%；第 3 类（shim 调用对 **25–31%**）是另一条链（shim 直呼），
不属于本单。⇒ **本单不做了**。若将来 shim 直呼落地，再重跑 `pricing.mjs` 决定扩组值不值。

## 一句话

FIELD 的 `lift` 曾是 2.54 ns/guest 指令、FELLOW 0.98 —— 归因证明**85% 由派发密度解释**（19.95 vs 42.6–46.9）。
D1/D2/D3/D4 之后密度到 **23.64**、`lift` **1.442 ns**、点击大帧 **409→210 ms**。本单继续：
**把更多块放进同一个 component**，让组内转移从 `ret i32 → trampoline → cmp+switch` 变成一条 `br`；
并把「必须付的那次派发」做薄。

## 第 0 步：定价结论（2026-11-24 已做，只读）

6 趟 `move11`（窗口 48.1–52.7 M 派发）：

| 笔 | 口径 | 读数 | 判读 |
|---|---|---:|---|
| 第 1 笔 · 同函数不同 component | 静态入边 `same-func-cross-comp`，残差按站点分摊 | **10.6–13.3%** | 小 |
| 第 2 笔 · 跨函数 `call`/`ret` | 调用侧 `cross-func` 41.0–45.6% + 返回侧 `ret-of-call` 29.2–34.8% | **≥70.2%（点估计 75%）** | **大头** |
| 第 3 类（本单未列）· shim 调用对 | `shim EA` + `ret-of-call:{reg,iat}` | **25–31%** | 独立来源，不属两笔 |

⇒ 按本单自己的判据：**不动发射器**，收益在 S7。S7 已于 2026-11-24 收口（`-DLIFT_S7_IAT`
缺省 OFF、端到端 A/B 不可判定、无实测收益；见 `REQ-01M86NMT0YJ9V78XS2DZZV364Q`）⇒ 本单不再被阻塞，
但第 2 笔的收益没兑现；**本单仍不先动发射器**。下一步：重跑 `pricing.mjs`（S7 的 `host-call` 已变），
或评估「shim 直呼」（第 3 类 25–31%，与本单不同面），再决定扩组值不值。
- 第 1 笔的 2,668 个静态站点里只有 **1,425** 能靠扩组消掉（762 edge + 663 返回续延）；
  剩 1,243 是静态 `call` 站点（同函数里的入口被当成分量根），扩组后仍要一次返回 ⇒ 要 S7/R1。
- 原始数据与脚本：`.tmp/reveq/pricing/`（`pricing.mjs` / `pricing-report.txt` / `blockchain.txt`）。

## 方案（按顺序；S7 之后）

1. **按函数扩组**：同一 `entry_owner` 的块放进一个 component（`comp_.blocks`），组内边全部 `br`
   （D1 已经做了「同组边直连」，本单是把组**变大**）。
2. **收缩入口集合**：只有**外部可达**的 EA 才在入口 switch 里留 case（`lift_table.inc` 的行 + hook EA +
   intrinsic 根 + `dropped_` 四类；前三类旧规则照旧「必须继续派发」）；只被组内跳到的块**不留 case**。
3. **入口直入（R2）**：每 entry 发薄 thunk，或把稀疏的 `cmp` 链降成跳表 / `blockaddress`，
   把每次派发现付的 **≈18 条 host 指令**压下来。
4. **热簇（可选、后置）**：把频繁互跳的 2–3 个函数合成一个簇（只对热簇做）。
   ⛔ **不做**「所有块塞进一个巨型 component」：入口映射已经是 35,222 行、全部块 74,502 个，
   LLVM 优化/RA 会超线性变慢、失去热/冷分离，而**跨函数 `call`/`ret` 那部分它一分钱都拿不到**
   （见 `repo:docs/03-emulator/aot-dispatch-density.md` §3）。

## 判据

- **主判据**：同一 8 s 窗口、交错 A/B ⇒ `iter`（派发次数）↓、`insns/iter` ↑、`lift` ns/guest 指令 ↓；
- 五项指纹（`exit` / `stop @` / arena / `table misses`）逐位同；
- `ctest` 全绿（至少含 `lift-emit-edges` / `lift-s7-indirect` / `lift-s7-structured` / `lift-intrinsics`）；
- 变异：**hook 直连**（删 `lift_gen_emit.cpp` 的 `mustDispatchAt` 分支）⇒ `lift-emit-edges` 红。
  ⚠ 直接「少留一个外部入口 case」会先撞 `emitBodies` 的生成期硬闸（构建红，不是用例红）⇒ 变异选 hook 直连。

## 起点读数（2026-11-24 / `53d438c` / SAVE11 点击复现）

| 读数 | 起点（优化前） | 现在 |
|---|---:|---:|
| `iter`（8 s 窗口派发） | 64.07 M | **51.07 M** |
| `insns/iter`（密度） | 16.50 | **23.64** |
| `lift` ns/guest 指令 | 1.926 | **1.442** |
| 点击大帧 | 409.2 ms | **210.2 ms** |

## 风险 / 未做

- 代码膨胀（i-cache）、编译时间、寄存器分配质量：**先按函数粒度**，不要一次全并；每步都取 A/B。
  ⚠ RA 质量 / i-cache 今天没有读数，只有静态体积 ⇒ 不能声称「扩组不会伤 i-cache」。
- `--no-s7` 下 `lift-ops-diff` 有既存崩溃（逐块夹具假设「一次调用=一个块」）—— 本单不要扩大那个面。
- 入口 switch 的「四类必须派发」是语义面，改动必须带守卫 + 变异。
