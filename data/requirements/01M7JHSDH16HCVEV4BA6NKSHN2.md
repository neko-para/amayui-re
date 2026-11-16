# shim 层 · 音频侧

- id: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`shim 层 · 音频侧`：`dsound` / `winmm` / `src/runtime/audio.cpp`（设备、混音器、feeder 线程、位置通知、解码）,
以及 **AOT 臂接入音频管线**（同一失效面，不同执行来源）。

- **缺陷**：跳段 / 错位 / 静音晚到 / 尾声重播 / 位置通知与 Stop 的时序。
- **效率**：解码吞吐（ADV 每句 ~1.7 s 那一类）。
- **能力**：AOT 臂从"恒 off"到"真出声"（含它与暂停 / 输入注入的契约）。

## 不收

- 令牌本身的作用域与公平性（→ `共用 runtime · 并发`；音频只是它的一个**触发源**）。
- 图形侧的呈现（→ `图形侧`）。

★ 口径：`audio.cpp` 是**共用 runtime**，不是"解释器专属" —— 解释器退役不等于音频缺陷作废；
但⛔ 不要把"只有解释器才能跑到的路径"当成仍然可达（AOT 臂今天没有 feeder）。
