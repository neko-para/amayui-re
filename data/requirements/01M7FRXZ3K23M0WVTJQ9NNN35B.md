# M-lift-3 · 图形 shim 实现效率：端到端墙钟的 78% 在这里（DrawIndexedPrimitive 175 µs/次 · Clear 5 ms/次 · Present 8.4 ms/次）

- id: REQ-01M7FRXZ3K23M0WVTJQ9NNN35B
- type: req
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- done_reason: 第 1-4 项落地（指纹逐位同：exit 0/frames 300/stop @00498AE4/arena 108595152·21·6674/misses 0；ctest 99/99、pnpm test 574）。判据 1 的'图形 shim 合计 ≤2.5 s'按用户裁决撤回（目前没撞到渲染瓶颈）；第 5 项 drop（hash 抽样会让 last_hash 名不副实，且它不驱动 input plan）。详见正文裁决。
- tags: [emulator-lift, d3d9, efficiency]

## 为什么单独立这一节点

`M-lift-2` 把"翻译"做到 native 量级（`OBS-20261115-06`：929M 条 / 1.6 s = 1.7 ns/条），但 300 帧端到端 **8.14 s** 已由**图形 shim** 决定。
mac 基线（次 × 单次 ⇒ 折算）：`DrawIndexedPrimitive` 18,000 × 174.9 µs = 3,148 ms · `Present` 300 × 8.43 ms = 2,528 ms ·
`Clear` 301 × 5.04 ms = 1,516 ms · `DrawPrimitive` 323 × 2.28 ms = 737 ms · `SetRenderState(+TSS)` 370,850 × 1.18 µs = 438 ms
⇒ **合计 ≈8.62 s ≈ 78%**。★ 早先（50 s 那次）shim 只占 13% ⇒ 绝对值没变、是别的东西变快了 ⇒ 这条线曾是端到端唯一的量级杠杆。

## 2026-11-17 win32 复核（只读诊断，300 帧）

墙钟 8.99 s（当轮有并行编译 ⇒ 偏大）· `insns=930,463,332` · top-6 shim = 6.93 s（77%）·
⚠ 折算总和 > 帧时 6.36 s ⇒ **过估约 9%**（`Clear`/`Present` 各只有 4 个样本）⇒ 78% 只当量级，⛔ 不能当判据。
逐行读数与命令见 `.tmp/perf-gfx/analysis.md`。

## 四条假设的判定

1. `DrawIndexedPrimitive` 175 µs —— **部分成立**：管线/根签名每 draw 一条；更大的是每 draw 的容器堆分配 + 每索引一次完整变换 + 批首整帧上传。
2. `Clear` 5.04 ms —— **证死**：`clear_area` 是双层逐像素循环（每像素算 `y*pitch+x*bpp` 再走 `storePixel`）；921,600 px × 5.7 ns ≈ 5.3 ms。
3. `Present` 8.43 ms —— **部分成立**：无格式转换；= 全帧 GPU 读回（3.7 MB）+ 全帧 FNV hash（≈1.5–2 ms）。
4. `SetRenderState` 1.18 µs —— **证伪**：`setRenderState` 是 O(1)；真身是 `d3d9Log` 先 `snprintf` 再判可见性 ⇒ 260,675 × ~0.7 µs ≈ 180 ms 纯浪费。

## 落地（2026-11-17 / win32 / main）

| # | 改法 | 提交 | 读数 |
|---|---|---|---|
| 1 | `Clear` 逐像素 → 一行 `storePixel` 模板 + 逐行 `memcpy` | `1e28ee6` | 语义逐位同 |
| 2 | `d3d9Log`/`d3d9Warn`/`pipeLog` **先判可见性再格式化** | `1e28ee6` | 同上 |
| 3 | draw 的 6 个容器改 `thread_local` 复用 | `b4cb213` | 每 draw ~74 KB 堆循环（300 帧 ≈1.3 GB）→ 稳态 **0 次** |
| 4 | 顶点变换按顶点号去重（`epoch` 判有效） | `b4cb213` | `DrawIndexedPrimitive` **177.8 → 105.1 µs（−41%）** |

- **端到端**（300 帧交错 A/B，已排除编译器进程）：1+2 **9958 → 8767 ms（−12.0%，3 有效对）**；
  3+4 **7884 → 7326 ms（−7.1%，10 对里 9 对更快）**。⚠ 本机背景负载大，单对能漂 2×，只信同对方向 + 多对中位。
- **语义**：两次都逐位相同（`exit 0` / `frames=300` / `stop @ eip=00498AE4` / arena `108595152·21·6674` / `misses=0`）；
  `ctest` 99/99 + `pnpm test` 574/574。⛔ `insns`/`lastHash`/`recovery EAX` 不算指纹（§12.6）。
- **第 4 项前提是实测的**：60 帧 3,600 次 `DrawIndexedPrimitive` ⇒ 558,180 索引 / 140,160 个不同顶点（**75% 可省**），
  每个 Draw `uniq == NumVertices`（4..438）⇒ 键域有界。反面守卫是既有 CTest（缓存写错 ⇒ 顶点数据错 ⇒ `d3d9-pipeline*` 红）。

## 裁决（2026-11-17 · 用户）

1. **判据 1 的"图形 shim 合计 ≤ 2.5 s（≈−70%）"撤回**：目前**没有撞到渲染导致的瓶颈**
   （`DrawIndexedPrimitive` 占墙钟已从 44% 降到 26%），暂不额外处理；将来**真撞到**再重开，
   且那时要拿**新读数**，⛔ 不拿这份 78% 的旧账当依据。
2. **第 5 项（全帧 hash 改抽样）drop** —— 理由不是"没收益"，而是**收益换掉的东西不值**：
   `lastHash` 今天是「最后一次 Present 的后缓冲整帧哈希」（`present.cpp` 的 `arena.hash(surface)`），被四处消费：
   摘要行 `present : … lastHash=`、`--frame-log` 逐帧、控制层 `sample`/`screen` 的 `last_hash`、
   以及两条 CTest 的确定性断言（`tests/ddraw.cpp` ⑦、`tests/d3d9.cpp` ⑤，判"同输入 ⇒ 同哈希"）。
   ⇒ 抽样**不会**破坏确定性（采样方式确定，断言照过），但会让 `last_hash` **名不副实**（叫整帧哈希、其实是采样哈希）；
   而它**不驱动 input plan**（用户口径）⇒ 为 0.3–0.45 s 把字段语义弄脏，不值。
   真要提速只能：另开语义单 + **新增**字段（⛔ 不许复用 `last_hash` 这个名字）。
3. 后续再压 `Present` / `Clear` 属**新单**，⛔ 不在这张单里继续。
4. **第 0 步（`--profile` 采样：调用数少的 shim 改成每次计时）仍没做** —— 它是判据 1 的前置；
   它属"渲染侧"，按第 1 条一并暂缓（两棵树现在都不占它）。

## 判据（修正后）

1. **单次成本**：每条热点 shim 的改法必须给**前后单次成本**（`--profile` 的 `shim:` 行前后对照；
   第 0 步没做时，用"同 argv 下**没改的 shim** 保持同区间"作对照 —— 本轮就是这么判的）。
2. **语义不变量**：`exit` / `frames` / `stop @ <EA>` / arena 三元组 / `table misses` **逐位相同**；⛔ 不改 guest 可见语义。
3. ⛔ **不得**用"跳过某个 draw / 少画一帧 / 少一次 Present"换速度 —— 那会改可见行为。
