# M-lift-3 · 图形 shim 实现效率：端到端墙钟的 78% 在这里（DrawIndexedPrimitive 175 µs/次 · Clear 5 ms/次 · Present 8.4 ms/次）

- id: REQ-01M7FRXZ3K23M0WVTJQ9NNN35B
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, efficiency]

## 为什么单独立这一节点

`M-lift-2` 把"翻译"做到 native 量级（`OBS-20261115-06`：929M 条 / 1.6 s = 1.7 ns/条），但 300 帧端到端 **8.14 s** 已由**图形 shim** 决定：

| 项（mac 基线） | 次 × 单次 | 折算 |
|---|---:|---:|
| `DrawIndexedPrimitive` | 18,000 × 174.9 µs | 3,148 ms |
| `Present` | 300 × 8.43 ms | 2,528 ms |
| `Clear` | 301 × 5.04 ms | 1,516 ms |
| `DrawPrimitive` | 323 × 2.28 ms | 737 ms |
| `SetRenderState` + `SetTextureStageState` | 370,850 × 1.18 µs | 438 ms |
| **合计** | | **≈8.62 s ≈ 78%** |

★ 早先（50 s 那次运行）shim 只占 13% ⇒ **shim 的绝对值没变，是别的东西变快了** ⇒ 这条线现在是端到端唯一的量级杠杆。

## 2026-11-17 win32 复核（只读诊断，300 帧，`exit 0`）

命令：`emulator-lift-aot --skip-logo --present headless --game-dir links/install --appdata <.tmp> --max-frames 300 --profile --time-limit 120`
读数：墙钟 **8.99 s**（本轮有并行编译 ⇒ 偏大）· 帧时 6.36 s（前 2.53 s 是 boot）· `insns=930,463,332`（≈104 M/s，比 FELLOW init 的 450 M/s 低 ⇒ 时间被 shim 吃掉）· **`freezeNs=0`**（不带 `--control-root` 时冻结副本零成本）· `liftone: 没有样本`。

| # | shim | 样本 | 调用 | 平均 | 折算 ms |
|---|---|---:|---:|---:|---:|
| 1 | DrawIndexedPrimitive | 281 | 18,000 | 138.7 µs | 2,497 |
| 2 | Present | **4** | 300 | 6.16 ms | 1,849 |
| 3 | Clear | **4** | 301 | 5.28 ms | 1,588 |
| 4 | DrawPrimitive | 5 | 338 | 2.02 ms | 682 |
| 5 | SetRenderState | 4,073 | 260,675 | 0.847 µs | 221 |

top-6 = **6.93 s / 8.99 s = 77%** ⇒ 78% 在 win32 上同量级。⚠ **但折算总和 > 帧时 6.36 s ⇒ 过估约 9%**（`Clear`/`Present` 各只有 4 个样本）⇒ 78% 只能当"量级"，⛔ 不能当判据。

## 四条假设的判定

1. **`DrawIndexedPrimitive` 175 µs —— 部分成立**：管线/根签名**每 draw 一条**（`gpu_raster.h:12`：合批只合并 Submit，不合并 draw）；
   更大的是**每 draw 3 个 `std::vector` 堆分配** + **每个索引进一次完整 world/view/proj 变换**（同顶点被多索引引用就重算）+ 批首整帧上传 3.7 MB（`d3d9_pipeline.cpp:4479-4481`）。
2. **`Clear` 5.04 ms —— 证死**：`runtime/d3d9.cpp:683-697` 是**双层逐像素循环**（每像素算 `y*pitch+x*bpp` 再走 `ddraw::storePixel`→`pixel_format::storePixel`）。
   921,600 px × 5.7 ns ≈ 5.3 ms，与实测吻合。**不是 memcpy，不是 GPU clear。**
3. **`Present` 8.43 ms —— 部分成立**：无格式转换；= **全帧 GPU 读回**（3.7 MB）+ **全帧 FNV hash**（`surface.cpp` `hashPixels`，461k 轮依赖乘法 ≈1.5–2 ms）。
4. **`SetRenderState` 1.18 µs —— 证伪**：`setRenderState` 是 O(1)；真身是 **`d3d9Log` 先 `snprintf` 再判可见性**（`runtime/d3d9.cpp:88-95`，`logV` 到 `log.h:141` 才 gate）
   ⇒ 260,675 × ~0.7 µs ≈ **180 ms 纯浪费**（默认级别根本不打印）。`pipeLog`（`d3d9_pipeline.cpp:384`）同形态。

## 修法排序（每条都要"前后单次成本"）

| # | 改法 | 落点 | 预期 | 风险 | 动语义 |
|---|---|---|---|---|---|
| 1 | `Clear` 逐像素 → 按 bpp 提分支 + 行级 `fill_n<uint32>` | `runtime/d3d9.cpp` `clear_area` | −1.2~1.5 s | 低 | 否 |
| 2 | `d3d9Log`/`pipeLog` **先判可见性再格式化** | `d3d9.cpp:88` · `d3d9_pipeline.cpp:384` | −250~290 ms | 极低 | 否 |
| 3 | draw 的 3 个 `vector` 改 thread_local 复用 | `d3d9_pipeline.cpp:5154` / `:4162` | −0.3~0.6 s | 低 | 否 |
| 4 | 顶点变换去重（按顶点号缓存） | 同上 | −0.3~1 s | 中 | 否 |
| 5 | 全帧 hash 改抽样（+`--hash-full`） | `surface.cpp` / `present.cpp` | −0.3~0.45 s | 中 | **是**（改 `lastHash`）⇒ 需裁决 |

★ **第 0 步（前置）**：`--profile` 现在对每个 shim **每 64 次采一次** ⇒ `Clear`/`Present` 只有 4 个样本，判据 4 的"前后单次成本"会被噪声淹没。
改法：**调用数少的 shim 改成每次计时**（成本可忽略）。不做这一步，第 1/2 项的收益**测不出来**。

## 判据

1. **尺度**：300 帧墙钟（release，交错 A/B ≥3 对取中位）相对 `8.14 s` 下降；目标先定"图形 shim 合计 ≤ 2.5 s"（≈ −70%）。
2. **语义不变量**：`exit` / `frames` / `stop @ <EA>` / arena 三元组 / `table misses` **逐位相同**；⛔ 不改 guest 可见语义。
3. ⛔ **不得**用"跳过某个 draw / 少画一帧 / 少一次 Present"换速度 —— 那会改可见行为。
4. 每条热点 shim 的改法必须给**前后单次成本**（`--profile` 的 `shim:` 行前后对照），不只是总量。

## 边界

- 这是**图形实现效率**，不是翻译质量；⛔ 不要记进 `M-lift-2` 的账。
- 渲染后端取舍（GPU 是唯一光栅实现、CPU 兜底已禁）在 `docs/00-origin/decisions.md` §8.1 与 `apps/emulator-lift/README.md` §4.2b，⛔ 不重开。
- 第 5 项（hash 抽样）**改了 `lastHash` 语义** ⇒ 属"判据面变更"，⛔ 不许混在效率提交里悄悄做。
