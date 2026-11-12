# 令牌 Phase-0 读数：持有占空 / 最长持有 / 等待直方图 / 每 shim max_ns（零行为改变）

- id: REQ-01M75WWEDC20GEH7QVSHPHH4GP
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/guest_token.cpp#guest-token/phase0-counters
- tags: [emulator-lift audio token instrumentation]

## 目标

给令牌加**四个只读读数**（零行为改变），用来校准"M0 能把 1.3 s 降到 80–150 ms"这个**估计**。

## 为什么（读数缺口）

已知：feeder 在 355 次边界上共等 **13.79 s**，每次等待**中位 43.1 ms**、单刀均值最大 440.8 ms；
而 SE 那几刀的真活只有 **1.4–2.5 ms**（占 0.4%）⇒ **1.3 s = 边界数 × 交接延迟**。
但**缺**"主线程一次到底持了多久"的分布，因此无法判 43 ms 是"帧周期（47 ms）"还是"某个计算型 shim"。
两者对应完全不同的修法（M0 移出令牌 vs 直接 M1 缩作用域）。

## 读数（四个，全部只读）

1. **持有占空**：主线程持令牌的累计墙钟 / 总墙钟。
2. **最长持有** + 持有直方图（<1 / 1–5 / 5–20 / 20–50 / >50 ms）。**这个决定 M0 够不够。**
3. **每次等待的直方图**（feeder 侧，沿用已有 `wait_ms` 口径）。
4. **每个 shim 的 `max_ns`**（宿主墙钟），用来抓"哪一类 shim 是长活"（Present / `fread` / DS `mutex_` / tick）。

⚠ 记录点必须在 `GuestToken::Guard` 的构造/析构或等价处，且**只在 `LIFT_AUDIO_TRACE` / `--profile` 下记账**，
默认运行零开销（可用已有的 `lastAcquireWaitNs` 同款 TLS 手法）。

## 判据

- 四个量出现在摘要（或 trace）里，且一次真 SDL 运行能读出量级；
- **零行为改变**：`ctest` 全量 82/82 绿，断言数不变；
- `git diff --check` 干净，只动插桩相关文件。

## 不做

- 不改任何调度、不移动任何 shim、不翻默认。
- 不做 M0/M1（另单）。

## 关联

`REQ-01M75VZXV4SDMVVM1Z72G009XJ`（尖峰）· 令牌模型复盘 `.tmp/audio-perf/token-model.md`。

## 落地结果（2026-11-12，已完成）

四个读数都落了（`guest_token.h/.cpp` · `shims.cpp` · `machine.cpp` · `tests/guest_token.cpp`，+11 条断言）：

| 读数 | 摘要字段 | 口径 |
|---|---|---|
| 持有占空 | `tokenhold` | 分母 = **令牌观测窗口**（首个持有 → 最后一次释放），不是 run wall |
| 最长持有 + 五桶（1/5/20/50 ms） | `holdhist` | `Yield` 把一段切成两段 = "一次不间断持有"的正确口径 |
| 每次等待五桶 | `waithist` | 沿用 `wait_ns_` 的"真被挡才计时"口径 |
| 每 shim `max_ns` | `shimmax` | 上限 8192 项，溢出打 `overflow N`；`calls` 是**记账期内**调用数，⛔ 不是总调用数 |

**开销**：默认（无 `--profile` / `LIFT_AUDIO_TRACE`）**零额外时钟、零原子写**，摘要整块门住 ⇒ 输出逐字节不变。
打开时 ≈ **+45–50 ns/块** ⇒ 按 1.15 M blocks/s 折算 ≈ **+5–6% 墙钟**。
⚠ 因此：**`--bench --profile` 的 MIPS 不能当性能基准**（已写进 `machine.cpp` 注释）；只读比例/分布。

## 已知局限（父级接受，不再扩大边界）

1. **无法拿 run wall**：`RunResult` 无 wall 字段、`wallMs()` 在收尾已失效（`machine.h` 不在边界）⇒ 用令牌观测窗口当分母，
   与 run wall 只差 boot 前后一小段。要严格 run wall 需另开单改 `machine.h`。
2. `shimmax` 的读时钟门比字面宽：`timing_on || audio_trace`（`timing_` 还含 `LIFT_FRAME_TRACE` / window-prof / `--overlay`）；
   内部另有 `phase0_enabled_` 门保证这些臂**不产生读数**。
3. `docs/03-emulator/measurement.md` 的字段说明**未加**（不在本单边界）⇒ 需另开单或并入，⛔ 别让它悬着。
