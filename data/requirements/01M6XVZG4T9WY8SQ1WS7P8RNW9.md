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
