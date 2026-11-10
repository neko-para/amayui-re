# emulator-lift · 令牌等待：过渡帧 HeapAlloc 的 shim 墙钟 ~85% 在等 guest 令牌（单令牌是上一轮修堆损坏引入的）

- id: REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 7
- verify: apps/emulator-lift/tests/guest_token.cpp#guest-token/wait-split
- done_reason: M1 计量落地（tokenwait 栏、ns 口径变真、SurfaceArena 检查点、3 条守卫）+ 31 遍实测给出新基线：默认 --audio off 下三工况 waits=0（结构性：音频关掉就没有 feeder 竞争）⇒ 单里「过渡帧 shim 墙钟 ~85% 在等令牌」不成立；过渡帧分桶 guest 50.4% + interp 31.1% vs shim 1.06%，HeapAlloc 整轮 6.1ms/1586 次 ⇒ M2 在默认口径收益为 0，裁决不做；开音频时 wait/wall = 5.1%（空转）/16.6–19.8%（进房间）/11.5–12.1%（出房间）⇒ 另开单跟踪。
- tags: [emulator-lift, performance, token]

## 观测

过渡帧里 `HeapAlloc` 的 shim 墙钟 **~85%** 是**等 guest 令牌**（进房间窗 605.8 ms、过渡帧 849 ms）。令牌来自单令牌契约（`REQ-01M6X64J338E6YDQXJB9RKAFSG`，done）：它把"guest 可见容器无互斥"的堆损坏修掉了，代价是另一条线程在 guest 里时必须排队。

## 问题的形状

单令牌不是性能设计，是**正确性**设计。所以这里不是"删令牌"，而是：**减少需要令牌的时长**，或**把不必要的令牌区收窄**。要分清三种时间：

1. 真的在 guest 里工作（该持有）；
2. 只是在等（不该记进自己的墙钟）；
3. 拿令牌做了顺带的活（可以挪出临界区）。

## 判据

- 基线：同一脚本、同一窗口，`token_wait_ms` 与 `HeapAlloc` 的墙钟 / 次数（该 shim 自己会打印，见 `.tmp/roomopt/heapmeas-*.log`）；N 遍中位数。
- 改后：过渡帧墙钟下降，且 ①`--assert-guest-token` 仍 `violations=0` ②`repo:apps/emulator-lift/tests/guest_token.cpp` 仍绿 ③进出房间的终态像素不变。
- ⛔ **不许用放宽令牌换速度**（那等于把堆损坏放回来）；并发契约真要变，先改那张 done 单的判据，⛔ 不在本单里做。

## 不做

- ⛔ 不重新设计线程模型（音频线程模型是另一条线）。
- ⛔ 不把"等锁"记进别的帧的耗时（口径要一致，见子单 E）。

## 取证与设计已就绪（2026-11-09，只读，**未实施**）

- **`token_wait_ms` 在今天的源码里不存在**（摘要只有 `token :` / `heap :` / `heapstat :`），`.tmp/roomopt/` 也不在
  ⇒ 需求单的 ~85% 与基线**当前不可复跑**；**M1（计量）是前置，不是可选项**。
- HeapAlloc 的令牌粒度 = **整次 shim**（Guard 包住 `local.fn`），而区内只有 µs 级 ⇒ **收益不在 HeapAlloc 自己的临界区**。
- 真因：feeder 等的是**别人持令牌做的阻塞型宿主等待**，共 3 类、全部**没有 Yield**（违反 `guest_token.h` 自己的取/放规则）：
  ① Present：`SDL_WaitForGPUFences` / `SDL_WaitAndAcquireGPUSwapchainTexture` / `SDL_AcquireGPUCommandBuffer` / `SDL_RenderPresent`；
  ② 音频 `mutex_`：DSound shim 在令牌里取它，而 mixer 持它跨整个 `mixStep`；③ 装载帧文件 IO：`fread`。
  相对量级**未确认**，须先计量再排序。只有 feeder 会真的等；mixer **从不取令牌**。
- 计量本身有洞：shim 计时起点在 Guard **之前** ⇒ 等令牌被记进 `defs_[id].ns`；`machine.cpp` 的注释"只统计 shim 实现自身"与该事实矛盾。
- 守卫比看起来弱：`check()` 只覆盖 heap/handles/machine.block/shim.invoke/audio.pump 共 7 处，
  **SurfaceArena / Vfs / ComRegistry / GuestMemory 簿记都没有** ⇒ 先给 SurfaceArena 加 check + 两条用例
  （`arena-outside` / `yield-revalidate`），比"数值断言 token_wait"更有意义。
- 设计三段（均不改并发契约）：M1 计量前置（wait 单开一栏、`ns` 暂不变，口径变更留给 E 单）→
  M2 让出阻塞型宿主等待（Present / 音频 mutex / 文件 IO 各一刀）→ M3 只在 M1 显示"饥饿"时做公平 handoff。
- 分析全文 `.tmp/reveq/01M6XVZG4T9WY8SQ1WS7P8RNW9/analysis.md`（**不入库**；入库的是本节要点）。

## 进展（2026-11-10）：**M1 计量已落地**，M2/M3 未动

- **口径变真**：shim 的计时起点从 `GuestToken::Guard` **之前**挪到**之后** ⇒ `ns` 真的只含 `local.fn`（`machine.cpp` 那条注释
  终于与事实一致）。⚠ **旧 `ns` 读数会变小**，差额 ≈ 等令牌（夹具实测：`wait 300.3 ms` vs `shim.ns 0.011 ms`；旧口径下 `ns≈313 ms`）。
- **新栏位**：摘要 `tokenwait : waits N wait X ms avg Y us`（每轮 `arm()` 清零）+ `--profile` 的 `=== guest token wait ===` 块。
  `waits` 只算**真被挡住**的取令牌（`try_lock` 成功不计、重入不计 ⇒ 未竞争热路径**零额外时钟**）。
- **守卫缺口补上**：`SurfaceArena` 的 create / destroy / find×2 / lock / unlock 全部接 `check("arena.*")`，首违在 stderr
  **响亮喊一行**（CAS 只喊一次）并进 `violations` ⇒ `--assert-guest-token` 判红。新增三条能红用例：
  `guest-token/wait-split`（真竞争，wait 与 ns 不重叠）· `guest-token/arena-outside`（负控 0 违规 + 正控点名 `arena.find`）·
  `guest-token/yield-revalidate`（Yield 期间另一线程真改容器 ⇒ 取回后必须重新校验）。变异一条（起点挪回 Guard 之前）实测红。
- **M2/M3 裁决（2026-11-10，基于新基线，不做了）**：31 遍实测（三种工况 × `--audio off`/`sdl` × 各 2 遍 + 整轮窗口）
  - **默认口径（`--audio off`）三工况 `waits = 0`、`wait = 0.0 ms`（21/21 遍）** ⇒ 令牌只有"主线程块边界 + 音频 feeder"两条取用路径，
    音频关掉就**没有竞争**（未竞争连时钟都不读）⇒ **单里那条"过渡帧 shim 墙钟 ~85% 在等令牌"今天不成立**，M2 在该口径下收益 **= 0**。
  - **过渡帧自身分桶**（`fr110-111` 窗口罩住载荷帧）：`guest 50.4% + interp 31.1%`（解释执行 **81.5%**）vs `shim` 仅 **1.06%（53.0 ms/5022.3 ms）**；
    旧结论点名的 `kernel32!HeapAlloc` 整轮 **6.1 ms / 1586 次** ⇒ 分子本身就不存在。
  - **只有开音频 feeder 才有等待**：空转 `wait/wall = 5.06/5.15%` · 进房间 **16.6/17.5/19.8%** · 出房间 **11.5/12.1%**（`max-concurrent` 全程 1、
    `violations` 全程 0）⇒ 这是 M2 的**收益上限**；要排序还得 N≥8（`avg` 在 324–2017 µs 之间跳）。⇒ 另开单跟踪（音频开启场景）。
  - `--assert-guest-token` **零开销**（只被读一次、运行结束后）：A/B 差 ≤±4%（噪声）⇒ 以后可默认带。
  - ⚠ `host` 桶里 **`input-script` 自己占 wall 的 9–16%**（脚手架每块边界扫动作），`host.audio` 只 2.6–4.4% ⇒ ⛔ 别把它当引擎成本。
- ⚠ **assets 档 CTest 本身没跑**（需 `LIFT_ASSETS=1` 重新 configure）；`option-roundtrip` 那条以后要补跑。
- 另：`README.md` / `main.cpp` 里 `--assert-guest-token` 的说明还没提 arena 检查点（本单未动那两个文件）。
