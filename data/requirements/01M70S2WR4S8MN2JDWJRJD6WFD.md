# emulator-lift · 吞吐的派发层拆分：每块 1 次真 mutex + 每 4096 条 1 次 steady_clock（块长扫描 + 令牌诊断 A/B）

- id: REQ-01M70S2WR4S8MN2JDWJRJD6WFD
- type: req
- status: done
- parent: REQ-01M6XVZC086PFXJRWE5AASRJ8G
- verify: apps/emulator-lift/tests/dispatch_tax.cpp#dispatch-tax
- done_reason: 派发层已量清：块长扫描（真测量）b=67.6-71.4 ns/块、c=20.0-22.2 ns/条；真工况 8.65 ms/M=8.0% 墙钟（下界）。A/B（N=5 交错、audio off）只关 mutex 得 2.08 ms/M=1.9%（每块 13.8-17.1ns，与微基准 8.9-10.1ns、合成体 Δb 8.1-12.2ns 吻合）；令牌原子记账另 ~1.6%，裁决不做（代价是 --assert-guest-token 只在打开时可信）。整个派发层拿掉只到 10.02 M insn/s（99.81 ms/M），离目标 52.63 差 6.5 倍 ⇒ 必须压逐条成本（~99.8ns → ≤44ns）。顺带修了一条会挂死 harness 的既有变异并另开单要给 CTest 守卫加超时。
- tags: [emulator-lift, performance, throughput, dispatch]

## 要回答的问题

`REQ-01M6XVZC086PFXJRWE5AASRJ8G` 的基线已经能一条命令复跑（`pnpm tools lift bench idle --n 5` ⇒ TITLE 空转中位
**105.87 ms/M = 9.446 M insn/s**，10 遍合并 105.65；目标 19.0 M insn/s ⇔ 52.6 ms/M ⇒ 差 2.01×）。本单只回答**钱花在哪**：

1. **派发层每块交了多少固定税**：现状（只读复核）`Machine::run` 每块 **1 次真 `std::mutex`**（`GuestToken::Guard`
   → `guest_token.cpp` 的 `mutex_.lock()`）+ **每 4096 条 1 次 `steady_clock`**；块命中 99.981%、平均块长 8.20–8.23
   （运行摘要 `blocks :` 免费给的）。⚠ 母单正文写的"每块两次无条件 `steady_clock`"要**按现状复核**，别照搬。
2. **块长与单条成本**：块长扫描（每块 1 / 3 / 8 条合成体 ⇒ `t = b + k·c`，**不用 profiler**）；单条指令成本用
   "同一条重复 N 次"微基准。⛔ 不能用 profiler 的 `guest fns` self（那是块粒度）。
3. **令牌的代价边界**：只关令牌的**诊断开关** A/B（⛔ 不改 guest 语义；先例是 OBS-15 的 shim 边界令牌）。

## 判据

- 同 `idle` 工况、同一 build、**N=5** 的 `ms/M` + `blocks/s`（`pnpm tools lift bench` 已经能给，⛔ 不新造口径）。
- 每条结论都要能回答"哪条命令 + 哪个读数 + 哪份 JSON"；算术估算必须标成**算术**（不是测量）。
- ⛔ 不以 profiler 开着时的读数当基线；⛔ 不用 Debug 构建；⛔ 不为提速改 guest 可见语义（要改就另开语义单，走准入门 `AGENTS.md` §6）。

## 已知线索（算术，不是测量）

窗口 ≈1.15M 块/s；若每块能省 50–60 ns ⇒ 约 **5–7% 墙钟**。这不足以到 19.0 M insn/s ⇒ 结论里要明确
"派发层最多值多少"，好让母单决定是继续压解释器还是转去打 shim/host（母单正文里的地板：`shim+host 34.3%`）。

## 不做

- ⛔ 不做装载帧的段表 / 热点 override（那是 `REQ-01M6XVZ8AXJNG492WA2QAMTA35`）。
- ⛔ 不动音频线程模型、不动令牌的**并发契约**（只做诊断开关 A/B）。
