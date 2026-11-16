# lift 层 · 效率

- id: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`lift 层 · 效率`：**执行来源**的每指令/每块成本，以及端到端墙钟的归因。

- 发射器效率（逐指令宿主调用、跨块 SSA、`call/ret` 真调用、`lift_tick` 记账）；
- 派发层税（`lookup` / 合批 / 每块时钟）；
- **压力 / 性能档**的重建（替代随解释器退役的 6 条 `LIFT_STRESS`）；
- 读数口径本身（什么能当效率判据、什么不是）。

## 不收

- 语义 / 覆盖 / 停机（→ `语义与覆盖`）。
- **某个 shim 实现**的成本（→ 对应 shim 组；例如 `M-lift-3` 的图形 shim 效率归图形侧）。

★ 判据：这条单改的是**发射器 / 派发 / 读数**，还是**某个 shim 的实现**。
