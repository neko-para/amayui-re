# emulator-lift · 令牌等待：过渡帧 HeapAlloc 的 shim 墙钟 ~85% 在等 guest 令牌（单令牌是上一轮修堆损坏引入的）

- id: REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 7
- tags: [emulator-lift, performance, token]

# emulator-lift · 令牌等待：过渡帧 HeapAlloc 的 shim 墙钟 ~85% 在等 guest 令牌（单令牌是上一轮修堆损坏引入的）

- id: REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 7
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
