# lift 层 · 语义与覆盖

- id: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`lift 层 · 语义与覆盖`：执行来源那一侧（构建期 lift + 派发）的**语义正确性**与**覆盖**。

- **闭包 / 跳表 / 入口集**：某条路径没被抬升 ⇒ 走到就响亮停（`decode-error` / 表外 EA）；
- **opcode 语义未登记**（含"名字是猜的"那一类）；
- **停机原因 / 退出码**的映射；
- **EH / SEH** 与合成 EA 泵；
- **SMC（自修改代码）**：guest 写已发射的代码页后仍执行旧 IR 这一类**静默**语义缺口；
- **整 .text 静态 lift** 这件事本身的形态判据（`A · 整 .text 静态 lift` 及其子单）。

## 不收

- 执行**速度**（→ `lift 层 · 效率`）。
- shim 内部语义（→ 对应 shim 组）。

★ 判据：**"跑错了 / 跑不到"归这里；"跑得慢"归效率组。**
