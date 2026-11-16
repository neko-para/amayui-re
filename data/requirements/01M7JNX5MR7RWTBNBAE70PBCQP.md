# AOT 窗口 profile：把一段区间切成可归因的桶（闭式 + unaccounted ≤3%），首个目标 = FELLOW init 的 614M insn

- id: REQ-01M7JNX5MR7RWTBNBAE70PBCQP
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, efficiency, profile, aot]

## 一句话

给 AOT 臂加**窗口 profile**：`profile_begin/end` 之间做闭式 `wall = lift + shim + boundary + present + wait + lift_dyn + unaccounted`，
按**具名行**降序打表（`shim:<module!name>` / `lift:<EA>` / `present` / `wait` / `lift_dyn` / `boundary`），
**`unaccounted` 必打**，**> 3% ⇒ 本次读数作废**。零新 CLI；不开窗 = 一行不打、一次取时都不做。

## 为什么（2026-11-17 / win32 实测，零代码"第 0 刀"）

FELLOW 打开 = **0.2 s 转场 + 1.39 s「Present 完全停掉」的密集计算**（中位合计 1586 ms，`init` 占 **87%**）：

- `enter` 161 / 200 / 242 ms（min/中位/max，n=9）· `init` 1040 / 1386 / 2072 ms · 单窗自检 1577 ms（差 9 ms ⇒ 两窗没漏时间）。
- `init` 内 **0 次 Present**（`n=1508 t_ms=20813.857` → `n=1509 t_ms=22183.256`，中间 1317–1389 ms 无帧）；
  之后 `n=1509` 的 hash 全变 ⇒ **画面是 init 之后才出现的**（用户眼里的 1.5–2 s 就是 A+B，不是"早画出来只是点不动"）。
- 输入 poll **25,696 次 / 1317 ms ≈ 19.5k/s**（之后降到 1.7k/s，差 11×）⇒ 不是空转；每 poll ≈ 23,900 条 guest insn。
- **工作量恒定、抖动全来自吞吐**：`init` 的 guest 指令数 614,169,179 / 613,809,458 / 614,503,020（**<0.12%**），
  同段墙钟 1317–1386 ms（纯墙钟档 1040–2072 ms，**2×**）⇒ 吞吐 443–466 M insn/s。
  ⇒ **判"这段有没有变慢"只能用 `insns`；墙钟必须带负载声明**（§12.6）。

⇒ 现有 `--profile` 只有**整轮**归因（派发 EA / shim 名 / 指令 op / Present 子项），回答不了"这 614M 花在哪"；
能按窗归因的今天只有输入 poll 计数。⚠ 别拿整轮的 shim 占比（1.4% of lift）外推到 `init` —— 那是整轮口径，会假绿。

## 判据（每条要能红）

1. **闭式恒等**：`wall == lift + shim + boundary + present + wait + lift_dyn + unaccounted`（误差 ≤1%），
   且 **`unaccounted` 行必须存在并打印**（⛔ 不许把残差塞进最大桶）。
2. **`unaccounted` ≤ 3%**，否则本次读数**作废**：打印 `INVALID: unaccounted=X%`，读数不得被引用。
3. **不开窗逐字节不变**：不带 `profile_begin/end` 的同一 run ⇒ 全文（含摘要）与基线逐字相同；
   `--profile` 两臂 `insns`/`frames`/`lastHash` 逐位相同（关档零取时）。
4. **必须能表达"0 Present 的窗口"**：桶按**窗口**切，⛔ 不按帧切（那会得到空表）。
5. **`lift_dyn` 单独一行**：S7 间接跳转 trampoline 今天**无任何读数**（不计 dispatch、不进 liftone）⇒ 本单给它一行。
6. 单条变异：拆掉窗口内的 shim 计时或把 `unaccounted` 恒 0 ⇒ 指定守卫非 0。

## 优化候选的判定口径（写死，单口径）

任何具名行占窗口 wall **≥10%** ⇒ 候选；**3–10%** ⇒ 观察名单（需两次读数复现）；**<3%** ⇒ 噪声。
**最高具名行 <10% ⇒ 结论写"纯 CPU 均匀分布，暂不优化"**（合格交付，⛔ 不为凑产出去挑 6% 的行）。
真机 0.5–1 s 只当**量级参照**，⛔ 不进判据（§12.6：跨机器墙钟不可比）。

## 落点（dev 工作树）

`src/aot/aot_host.{h,cpp}`（`AotWindowProf` + 打表）· `src/aot/aot_run.cpp`（窗口内取时 + shim 精确 + `lift_dyn` 计数 + Present 子项累计）·
`src/runtime/input_plan.{h,cpp}`（`ProfileMarkHook` 回调，**不改配方语法**）· `src/aot/aot_main.cpp`（接线）·
新增 `tests/window_prof.cpp`（顶层无条件 `add_test` = pure 档）· `tools/mutate-check.mjs` 一条 ·
文档三处：`docs/03-emulator/measurement.md`（新增一节，含上面的第 0 刀读数）、`docs/03-emulator/input-plan.md`（"本臂没有区间 profiler"改字）、
`apps/emulator-lift/README.md`（补 AOT `--profile` 入口 —— 现在整节标着"解释器侧已退役"）。

## 未做

按 shim **家族**聚合（先按名字；真出现 >10% 的单条再说）· 跨平台 TSC（非 x86 退 `steady_clock`，已有）· 历史对比工具 ·
`lift_dyn` 的**分 EA** 归属（先只给一行总量）。
