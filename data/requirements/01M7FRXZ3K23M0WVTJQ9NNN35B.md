# M-lift-3 · 图形 shim 实现效率：端到端墙钟的 78% 在这里（DrawIndexedPrimitive 175 µs/次 · Clear 5 ms/次 · Present 8.4 ms/次）

- id: REQ-01M7FRXZ3K23M0WVTJQ9NNN35B
- type: req
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- tags: [emulator-lift, d3d9, efficiency]

## 为什么单独立这一节点

`M-lift-2` 把"翻译"做到 native 量级（`OBS-20261115-06`：929M 条 / 1.6 s = 1.7 ns/条），但 300 帧端到端 **8.14 s** 已由**图形 shim** 决定：

mac 基线（次 × 单次 ⇒ 折算）：`DrawIndexedPrimitive` 18,000 × 174.9 µs = 3,148 ms · `Present` 300 × 8.43 ms = 2,528 ms ·
`Clear` 301 × 5.04 ms = 1,516 ms · `DrawPrimitive` 323 × 2.28 ms = 737 ms · `SetRenderState(+TSS)` 370,850 × 1.18 µs = 438 ms
⇒ **合计 ≈8.62 s ≈ 78%**。

★ 早先（50 s 那次运行）shim 只占 13% ⇒ 绝对值没变、是别的东西变快了 ⇒ 这条线现在是端到端唯一的量级杠杆。

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
   更大的是**每 draw 3 个 `std::vector` 堆分配** + **每个索引进一次完整 world/view/proj 变换**（同顶点被多索引引用就重算）+ 批首整帧上传 3.7 MB。
2. **`Clear` 5.04 ms —— 证死**：`runtime/d3d9.cpp` `clear_area` 是**双层逐像素循环**（每像素算 `y*pitch+x*bpp` 再走 `storePixel`）。
   921,600 px × 5.7 ns ≈ 5.3 ms，与实测吻合。**不是 memcpy，不是 GPU clear。**
3. **`Present` 8.43 ms —— 部分成立**：无格式转换；= **全帧 GPU 读回**（3.7 MB）+ **全帧 FNV hash**（≈1.5–2 ms）。
4. **`SetRenderState` 1.18 µs —— 证伪**：`setRenderState` 是 O(1)；真身是 **`d3d9Log` 先 `snprintf` 再判可见性**
   ⇒ 260,675 × ~0.7 µs ≈ **180 ms 纯浪费**（默认级别根本不打印）。`pipeLog` 同形态。

## 落地（2026-11-17 / win32 / main）

| # | 改法 | 提交 | 读数 |
|---|---|---|---|
| 1 | `Clear` 逐像素 → 一行 `storePixel` 模板 + 逐行 `memcpy` | `1e28ee6` | 语义逐位同（见下） |
| 2 | `d3d9Log`/`d3d9Warn`/`pipeLog` **先判可见性再格式化** | `1e28ee6` | 同上 |
| 3 | draw 的 6 个容器改 `thread_local` 复用 | `b4cb213` | 每 draw ~74 KB 堆循环（300 帧 ≈1.3 GB）→ 稳态 **0 次** |
| 4 | 顶点变换按顶点号去重（`epoch` 判有效，不逐 Draw 清） | `b4cb213` | `DrawIndexedPrimitive` **177.8 → 105.1 µs（−41%）** |
| 5 | 全帧 hash 改抽样 | ⛔ 未做（改 `lastHash` = 判据面变更，待裁决） | — |

- **端到端**（300 帧交错 A/B，`Measure-Command`，已排除编译器进程）：1+2 **9958 → 8767 ms（−12.0%，3 有效对）**；
  3+4 **7884 → 7326 ms（−7.1%，10 对里 9 对更快）**。⚠ 本机背景负载大（chrome/steam/Discord 常驻），单对能漂 2×
  ⇒ 只信"同对方向 + 多对中位"，⛔ 不把 −7.1% 当精确值。
- **语义**：两次都逐位相同 —— `exit 0` · `frames=300` · `stop @ eip=00498AE4` · arena `in_use/peak=108595152 · surfaces=21 · heap_blocks=6674` · `misses=0`；`ctest` 99/99 + `pnpm test` 574/574。⛔ `insns`/`lastHash`/`recovery EAX` 不算指纹（§12.6，同一二进制内也会变）。
- **第 4 项的前提是实测的**（不是推理）：60 帧 3,600 次 `DrawIndexedPrimitive` ⇒ 558,180 索引 / 140,160 个不同顶点（**75% 可省**），每个 Draw `uniq == NumVertices`（4..438）⇒ 键域有界。反面守卫是既有 CTest（缓存写错 ⇒ 顶点数据错 ⇒ `d3d9-pipeline*` 当场红）。

## 仍未做 / 下一步

1. **第 0 步（前置）：`--profile` 采样**。现在对每个 shim **每 64 次采一次** ⇒ `Clear`/`Present` 只有 4 个样本，
   判据 4 的"前后单次成本"会被噪声淹没。改法：**调用数少的 shim 改成每次计时**。被 `aot_host.cpp` 占着（dev 车道）。
2. **后端合批队列的每 draw 分配**（`d3d9_gpu.cpp` 的 `draw.vertices.assign(...)`）：那是"一帧内累积所有 draw"的合批容器，
   不是临时缓冲；改它要动后端容器设计与内存峰值 ⇒ 另开单。
3. **`transformVertex` 里 `multiplyD3D(multiplyD3D(world,view),proj)` 每顶点重算**（2 次 4×4 乘）：可提到循环外、零语义风险，与去重正交 ⇒ 下一轮。
4. **第 5 项（hash 抽样）**：需裁决；⛔ 不许混在效率提交里悄悄做。
## 判据

1. **尺度**：300 帧墙钟（release，交错 A/B ≥3 对取中位）相对 `8.14 s` 下降；目标先定"图形 shim 合计 ≤ 2.5 s"（≈ −70%）。
2. **语义不变量**：`exit` / `frames` / `stop @ <EA>` / arena 三元组 / `table misses` **逐位相同**；⛔ 不改 guest 可见语义。
3. ⛔ **不得**用"跳过某个 draw / 少画一帧 / 少一次 Present"换速度 —— 那会改可见行为。
4. 每条热点 shim 的改法必须给**前后单次成本**（`--profile` 的 `shim:` 行前后对照），不只是总量。

## 边界

- 这是**图形实现效率**，不是翻译质量；⛔ 不要记进 `M-lift-2` 的账。
- 渲染后端取舍（GPU 是唯一光栅实现、CPU 兜底已禁）在 `docs/00-origin/decisions.md` §8.1 与 `apps/emulator-lift/README.md` §4.2b，⛔ 不重开。
