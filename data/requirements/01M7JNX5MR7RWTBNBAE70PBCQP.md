# AOT 窗口 profile：把一段区间切成可归因的桶（闭式 + unaccounted ≤3%），首个目标 = FELLOW init 的 614M insn

- id: REQ-01M7JNX5MR7RWTBNBAE70PBCQP
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- verify: apps/emulator-lift/tests/aot_window_profile.cpp#window-profile-closed-form
- tags: [emulator-lift, efficiency, profile, aot]

## 一句话

`profile_begin/end` 之间做分桶账，按**具名行**降序打表，并给一行**自检**决定这次读数能不能引用。
零新 CLI；不开窗 = 一行不打、一次取时都不做。

## 为什么（2026-11-17 / win32 实测，零代码"第 0 刀"）

FELLOW 打开 = **0.2 s 转场 + 1.39 s「Present 完全停掉」的密集计算**（中位合计 1586 ms，`init` 占 **87%**）：

- `enter` 161 / 200 / 242 ms（min/中位/max，n=9）· `init` 1040 / 1386 / 2072 ms · 单窗自检 1577 ms（差 9 ms ⇒ 两窗没漏时间）。
- `init` 内 **0 次 Present**；之后 hash 全变 ⇒ **画面是 init 之后才出现的**（用户眼里的 1.5–2 s 就是 A+B）。
- 输入 poll **25,696 次 / 1317 ms ≈ 19.5k/s**（之后 1.7k/s）⇒ 不是空转；每 poll ≈ 23,900 条 guest insn。
- **工作量恒定、抖动全来自吞吐**：`init` 的 guest 指令数 614,169,179 / 613,809,458 / 614,503,020（**<0.12%**），
  墙钟 1317–2072 ms（**2×**）⇒ 判"这段有没有变慢"只能用 `insns`；墙钟必须带负载声明（§12.6）。

⇒ 原有 `--profile` 只有**整轮**归因，回答不了"这 614M 花在哪"；⚠ 别拿整轮的 shim 占比（1.4%）外推到 `init`。

## 落地（2026-11-17 / dev `232bb4b`+`1744a44` / RelWithDebInfo）

闭式：`wall = lift + boundary + shim + partial + instrument + unaccounted`（`present` / `wait` 是 `shim` 的**子行**）。
落点：`src/aot/window_prof.{h,cpp}`（`totals()` 在 `.cpp` 里 = 闭式唯一实现，纯头只留 O(1) 内联）·
`aot_host.{h,cpp}`（开/收/打表）· `aot_run.cpp`（块顶 `t_a`、分派前 `t_b`、shim 精确 2 取时）·
`input_plan.{h,cpp}`（`ProfileMarkHook`，不改配方语法）· `lift_abi.{h,cpp}`（`lift_dyn` 计数闸门）·
`tests/aot_window_profile.cpp`（pure，顶层 `add_test`）· `tools/mutate-check.mjs` 一条。

**读数（N=3，`.tmp/wprof/fellow.plan`）**

| 窗口 | `wall` | `insns` | `lift` | `boundary` | `shim` | 结论 |
|---|---|---:|---:|---:|---:|---|
| `fellow-init` | 2209/2302/2270 ms | 612.8/612.4/612.7 M（±0.06%） | 50.0/50.1/49.7% | **28.7/29.9/29.1%** | 18.6/17.5/18.5% | `coverage` 1.20–1.22 · `frames=0` |
| `fellow-enter` | ~200–260 ms | — | 3–4% | ~2% | **92–94%**（Present 32% / Clear 31% / Sprite::End 16%） | — |

最大单条 shim 只有 `PeekMessageA` 5.3% / `DeleteObject` 5.1% / `timeGetTime` 3.8% ⇒ **无单条 ≥10%**。
⇒ **`boundary`（块边界宿主税）是唯一 ≥10% 的具名行**，是下一刀的候选；其余按 §"优化候选口径"不动。

## 偏离设计（都留了实测理由，改判据面）

1. **采样用乘性散列**取高 6 位（平均 1/64），⛔ 不用 `iter % 64` —— 后者与 guest 循环同相，实测外推偏高 1.6×。
2. **份额来自采样、绝对量来自精确包络**（`wall − shim − partial − instrument`）：直接外推会把"区间内的取时开销 ×64"
   算进去（实测 3.1 s > 墙钟 2.0 s）。代价是 `unaccounted` **结构上恒 ~0** ⇒ **作废判据改成 `coverage`**
   （= 采样外推 / 包络，`[0.5, 2]` 之外才作废；实测 1.20–1.22）。`unaccounted` 仍照打，但不再是判据。
3. **去掉"小窗口自动全量"**：混合抽样被开头那段主导（正是 1.3× 的来源）⇒ 全窗口同一 stride，样本少只打 ⚠。
4. **`lift_dyn` 只报次数、不进闭式**：它是尾调用，区间含目标函数体 ⇒ 报时间会与 `lift` 双计。
5. **`lift` 桶含派发表查找**（`win_b` 在 lookup 之后）：design 把 lookup 归 `boundary`，先保持现状并记在 M-12。

## 判据（实际口径）

1. **闭式恒等**（误差 ≤1%），且 `unaccounted` 行**必须打印**（⛔ 不许把残差塞进最大桶）。
2. **`coverage ∉ [0.5, 2]` ⇒ 本次读数作废**（打印 `INVALID`）；`unaccounted/wall` 的 3% 闸门仍在，但结构性不触发。
3. **不开窗逐字节不变**：不带 `profile_begin/end` 的同一 run 全文与基线逐字相同；关档零取时。
4. **必须能表达"0 Present 的窗口"**：桶按**窗口**切，⛔ 不按帧切。
5. **`lift_dyn` 单独一行**（只报次数）。
6. 单条变异：拆掉窗口内 shim 计时 / 让包络漏掉 `shim` ⇒ 守卫非 0（实测红，`--match '窗口的精确包络漏掉'`）。

## 未做

按 shim **家族**聚合 · 跨平台 TSC（非 x86 退 `steady_clock`，已有）· 历史对比工具 ·
`lift_dyn` 的**分 EA** 归属（先只给一行总量）· `boundary` 内部再切六刀（另开单）·
一次 shim 里多次 Present 时子桶只累计最后一帧的拆分（代码注释与 M-12 已注明）。
