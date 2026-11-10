# emulator-lift · 指令吞吐：15.5 M insn/s 的基线与瓶颈定位（装载帧延迟的根因量级）

- id: REQ-01M6XVZC086PFXJRWE5AASRJ8G
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 4
- done_reason: 基线可复跑（pnpm tools lift bench idle --n 5 ⇒ 中位 105.87 ms/M = 9.446 M insn/s，10 遍合并 105.65，块命中 99.981%、平均块长 8.20-8.23），目标已裁（装载帧 ≤4.0 s ⇔ 19.0 M insn/s），瓶颈定位由子单完成：b=67.6-71.4 ns/块、c=20.0-22.2 ns/条，派发税 8.0% 墙钟（下界），可安全拿回的只有 mutex 1.9%。把整个派发层拿掉只到 10.02 M insn/s，离目标 52.63 ms/M 差 6.5 倍 ⇒ 光靠派发层永远不够；下一步必须压逐条成本（~99.8 ns → ≤44 ns，2.3×）。仍未做：不 Present 的空转工况（与 OBS-05 同表）与逐条成本分解。
- tags: [emulator-lift, performance, throughput]

## 为什么单独成单

装载帧慢的**量级**由解释器吞吐决定：空转 64.7 ms / 百万条指令（≈15.5 M insn/s），装载帧还更慢（55–58 ms/M）。即使 C 把两个帧拆干净，只要吞吐不变，"一帧 75.9M / 151M 条"就注定是秒级。

但这里改的是**核心执行循环** ⇒ 影响所有路径与所有守卫，必须与 C 分开裁决、单独 A/B。

## 要做的

1. **基线可复现**：一条命令给出"每百万条指令的宿主毫秒"（空转 + 装载两种工况），重复 N 遍取中位数，落文件。
2. **瓶颈定位**：区分 ① 取指 / 译码（含 block cache 命中率）② 派发 ③ 单条指令的宿主开销（shim 调用、FPU、字符串）④ 采样 / 计时本身的开销。
3. **定目标**：先定"装载帧 ≤ X 秒"倒推需要的 M insn/s，再决定投入到哪一层；⛔ 不先改再声称变快。

## 判据

- 吞吐基线有**一条命令 + 落文件**的形态（重复 N 遍、中位数、带 build 配置与工况标注）。
- 任何提速改动都要给出"改前 / 改后 × 同一工况 × N 遍"的对照，并给出**正确性证据**：`repo:apps/emulator-lift/tests` 全绿 + 至少一条端到端路径的像素 / 终态等价（工具见子单 C / E）。
- ⛔ 不改 guest 可见语义换速度（真要改就另开语义单，走准入门）。
- ⛔ 不以 profiler 开着时的读数当基线（profiler 本身有膨胀，口径见子单 E）。

## 不做

- ⛔ 不在本单顺手做"两个装载帧的热点 override"（那是 C 的地盘）。
- ⛔ 不用 Debug 构建的读数（性能判据固定 RelWithDebInfo）。

## 设计已就绪（2026-11-09，只读分析，**未实施**）

- **口径纠正**：需求单把"空转 64.7 / 装载 55–58 ms/M"拼成一张表不成立 —— 档位不同（前者无仪器长程，后者逐帧墙钟只能来自 `LIFT_FRAME_TRACE=1`）；
  且 55–58 < 64.7 ⇒ "装载每百万条更慢"是读反，装载慢在**条数**。enter 帧按 OBS-01 自己的数应是 68–74 ms/M。
- **基线不需要 profiler**：`--input-script` 的 `at insn N` + `wait 0 insn` 会打 `[input-script] … guest_ms= insns=`；窗口模式自动 real-time
  ⇒ guest_ms 差 = 宿主毫秒。落点 `tools/lift.mjs` 新 op `bench`（与 `report --bench` 分开），脚本 `apps/emulator-lift/scripts/bench/`，读数 `.cache/lift-bench/`（gitignored）。
  `pnpm tools lift report --bench N` **不能**当基线（合成循环 + 单次样本 + 不落文件，只能当 core 探针）。
- **最值得先量的是派发层**：每块**两次无条件 `steady_clock`** + 每块一次**真 mutex**（令牌）——不是 profiler 膨胀，但会冒充解释器成本。
- **现成计数器**够三件事：块命中率 / 平均块长（运行摘要 `blocks :` 行的 `insns/(hits+misses)` 免费）、shim 调用次数。
  缺的只有 guest/shim 命中拆分、译码时间、shim Top-N、frames、逐 op 分布（零热路径或只在 miss 路径）。
- **目标已裁决：装载帧 ≤ 4.0 s（enter / exit 各自）** ⇒ 需 ≈19.0 M insn/s（现状 15.5）。★ 地板：`T_new = T(1−f) + Tf/s`，
  `f=(guest+interp)/wall`，OBS-10 上界 65.4% ⇒ **shim+host 地板 34.3%**；exit 帧 T=8.7 s 时地板 ≈3.0 s ⇒ **X<3.0 s 换谁写解释器都到不了**（要先打 shim/host）。
- 分层法：块长扫描（每块 1/3/8 条合成体 ⇒ `t=b+k·c`，不用 profiler）；单条指令成本用"同一条重复 N 次"微基准，
  ⛔ 不能用 profiler 的 `guest fns` self（块粒度）。
- 风险：本机 mach 时基 41.67 ns **分辨率**（不是调用成本）；`--present window` 必须 `--present-vsync 0`；
  基线必须带 `CMAKE_BUILD_TYPE` 回读 + 二进制新鲜度 + **工作树指纹**。
- 分析全文 `.tmp/reveq/01M6XVZC086PFXJRWE5AASRJ8G/analysis.md`（**不入库**；入库的是本节要点）。

## 本轮进展（2026-11-10）：基线已成形，**瓶颈定位未做**

- **`pnpm tools lift bench idle --n 5`**（一条命令 + 落文件）已落地：`tools/lift.mjs` 新 op `bench`（口径 A 写死
  `--present window --present-vsync 0 --present-hidden --raster gpu --native-overrides all --skip-logo`；每遍**独立子进程**；
  JSON 落 `.cache/lift-bench/`，含 exe sha256+mtime、`CMAKE_BUILD_TYPE` 回读、脚本 sha256、工作树指纹、逐遍原始值 + 中位数；
  `Debug` 直接拒绝；解析失败**不估算**，落 `*-FAILED.json`）。守卫 `repo:tools/test/lift-bench.test.mjs`（10 用例，纯解析器）。
- **N=5 基线（TITLE 空转）**：`106.19 / 105.42 / 105.05 / 106.37 / 105.87` ms/M ⇒ **中位 105.87 ms/M = 9.446 M insn/s**（极差 1.2%）；
  另一轮独立 N=5 = 104.97（极差 1.4%）；10 遍合并中位 105.65 ⇒ **可复跑**。块命中 99.981% · 平均块长 8.20–8.23。
  ⚠ 工况是 **TITLE（每帧都 Present，≈34 fps）**，⛔ **不能**与 OBS-05 的 64.7 ms/M（EU-ROOM 静置、Present=0）相减比快慢。
- 与已裁决目标（约 19.0 M insn/s ⇔ 52.6 ms/M）差 **2.01×**；换算：75.9 M 条 ⇒ 8.03 s，151 M ⇒ 15.98 s。
- **派发层已拆（子单 `REQ-01M70S2WR4S8MN2JDWJRJD6WFD` 收口）**：块长扫描**真测量** ⇒ `b = 67.6–71.4 ns/块`、`c = 20.0–22.2 ns/条`；
  真工况派发税 **8.65 ms/M = 8.0% 墙钟**（下界）。可安全拿回的只有 mutex：A/B（N=5 交错、`audio off`）**2.08 ms/M = 1.9%**；
  令牌原子记账另 ~1.6%，**裁决不做**（不值得让 `--assert-guest-token` 变成只在打开时可信）。
  ★ **把整个派发层拿掉只到 10.02 M insn/s（99.81 ms/M）**，离目标 52.63 ms/M **差 6.5 倍** ⇒ **光靠派发层永远不够**。
- **结论与下一步**：要达标必须把**每条成本从 ~99.8 ns 压到 ≤44 ns（2.3×）** ⇒ 本单之后该压的是**解释器逐条开销**（译码/dispatch 表/热 op 的内联），
  不是派发层。⛔ 且不许用"改 guest 可见语义"换速度（要改另开语义单，走 `AGENTS.md` §6）。
- **仍未做**：① "不 Present 的空转"工况（与 OBS-05 的 64.7 ms/M 同表 —— 现在只有 TITLE 工况，两数不可相减）；
  ② 逐条成本的分解（译码 / dispatch / 热 op / FPU / 字符串各自占比）；③ 合成体 `pnpm tools lift report --bench` 的块长**不可调**（`tests/bench.cpp` 写死 3 条/块），
  已在 `dispatch-tax` 里另造可调合成体 ⇒ 若要把"块长扫描"做成常设能力，应把它挪进 `tools/lift.mjs`。
