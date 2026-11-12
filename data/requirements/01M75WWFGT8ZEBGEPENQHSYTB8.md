# 令牌 = 容器锁：M0 把阻塞等待/宿主专属工作移出令牌 → M1 作用域缩到容器区域

- id: REQ-01M75WWFGT8ZEBGEPENQHSYTB8
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio token design]

## 模型裁决（已定）

**令牌 = 容器锁，不是执行锁。** 复盘证据：

- **guest CPU 状态不是共享的**：每个线程一份 `X86`（`audio.cpp:1407`），feeder 跑 guest 指令**根本不持令牌**（`audio.cpp:1744-1746`）；
- **guest 的 CS 是另一套**：`CriticalSectionTable` 的 per-EA 宿主递归锁（`critical_sections.h:33-35` / `kernel32_sync.cpp:1325-1355`），
  令牌只是"拿它之前必须让开"的外层（`kernel32_sync.cpp:1336`）；
- ⇒ 令牌唯一的内容是 **(i) 宿主容器**；**guest 内存数据面从来不由令牌保护**。

★ 因此"三件事被同一把锁串行化"这个说法**不成立**（父级原判断需撤回）。真正的问题是**持锁时长**，不是覆盖面。

## 成本结构（实测）

- 固定税很小：mutex 1.9% + 原子记账 1.6% ≈ **3.5% 墙钟**（~25–30 ns/块）；
- 真等：`tokenwait waits 745 / 14449.5 ms`，其中 feeder **13.79 s / 355 次边界**，占 32 刀 guest 墙钟的 **80.2%**；
  每次等待中位 **43.1 ms**；
- 关键：SE 那几刀真活只有 **1.4–2.5 ms**，却跨 27–44 次边界 ⇒ **1.3 s = 边界数 × 交接延迟**；
- **"插队/反复饿"不是主因**（纯公平性预测每次 µs 级）；主因是主线程**一次不间断持有**多久。
  43 ms ≈ 帧周期 47 ms（算术，**未取证**）⇒ 长活很可能在 shim 里。

## 阶段（按可独立验证排，⛔ 不要一次大重构）

**Phase-0（前置，另单）**：四个读数（持有占空 / 最长持有 / 等待直方图 / 每 shim `max_ns`），零行为改变。

**M0 —— 把"阻塞型宿主等待 + 宿主专属工作"移出令牌**（四处，彼此独立）：
1. **GPU / Present**（`d3d9_gpu.cpp:421,444,482,505,1170,1561` 等 + `present_sdl.cpp:854`）；
2. **`fread`**（`kernel32_file.cpp:785`）；
3. **DS shim 里的 `mutex_`**（`audio.cpp:516,546,603`，注意 mixer 跨整个 `mixStep` 持锁 `:1047-1050`）；
4. **harness 每块 tick**（占墙钟 9–16%，`01M6XVZG4T9WY8SQ1WS7P8RNW9.md:70`）。

预期：每次交接 43 ms → 1–2 ms ⇒ 单刀 1.34–1.74 s → **80–150 ms**，`fill_slow` **4→0**，`fill_max_ms` ≤0.2 s。
⚠ **这是估计**，要 Phase-0 校准；若 43 ms 是**计算型** shim（纹理上传/解码），M0 打不中 ⇒ 直接上 M1。

**M1 —— 作用域从"迭代"缩到"容器区域"**（默认翻过来的那一步，风险最高）
前置三条，缺一不可：
- `VirtualClock::now_100ns_` **原子化**（`clock.h:46`，feeder 的时间 shim 会读它）；
- `check()` 覆盖补全：**Vfs / ComRegistry / GuestMemory 簿记今天一个 check 都没有**；
- **负控实验**（故意漏一个非 shim 容器访问 ⇒ 必须能被 `--assert-guest-token` / TSAN 抓到）。

**M2/M4** 后手；**M3（公平/时间片）不做** —— 长 hold 不可抢占，解决不了这个现象。

## 不做（已否）

- **父级候选 (a)「块边界发现 feeder 在等就让出」打不中**：一次 hold 里**没有块边界**（都在一个 shim 内部 / `GetMessageA` 循环里）。
- **M3**：同上。
- **退役 D 模式**不是 M0/M1 的前置（泵自己取令牌 `audio.cpp:1189`，M1 照旧工作）⇒ **另单**；
  真实代价只有"无音频设备环境下的确定性回放/降级"（`--audio virtual` + dump 的 CTest 路径）⇒ 应改写成**纯宿主夹具**而不是删掉。

## 判据

- M0：`fill_slow=0`、`fill_max_ms ≤0.2 s`、`tokenwait` 总量下降一个量级、`ctest` 82/82；
- M1 翻默认前：`check()` 覆盖率补齐 + 负控通过 + `ctest` 全绿 + 一次真 SDL 运行无 `violations`；
- ⛔ 不许用"听感变好"当判据 —— 用 `fill_slow` / `fill_max_ms` / `tokenwait`。

## 风险

**M1 漏一个非 shim 容器访问 = 静默数据竞争**（今天靠"整块持锁"兜着；`--assert-guest-token` 只能证明
7+6 个**已知**点在锁里，不能证明没有第 8 个）。⇒ M1 必须在 Phase-0 + M0 之后、且带负控。

## 基线与验收线（2026-11-12，Phase-0 读数已就位）

- **现状**：feeder 在 **355 次边界**共等 **13.79 s**，单次中位 **43.1 ms**、最大均值 **440.8 ms**；
  而那几刀的真活只有 **1.4–2.5 ms** ⇒ 43 ms 就是"主线程一次不间断持有"的长度。
- **合并构建 e2e**（`graphs/se-eof.graph`）：`fill_max_ms` = 1687.7 / 1302.8 / 1045.9，`fill_slow` = 3 / 1 / 1。
- **M0 的验收线**：`fill_slow = 0` 且 `fill_max_ms ≤ 0.2 s`；`holdhist` 的 `>50 ms` 桶显著下降；
  `[audio] feeder fill … shimtok` 与 `shimblk` 同步下降。
- ★ **读数口径**：`--profile` 会开令牌记账（+5–6% 墙钟）⇒ 验收读数**别在 `--profile` 下取**，
  用 `LIFT_AUDIO_TRACE=1` 的 `[audio] feeder fill … guest=… shimtok=… total=…`。
