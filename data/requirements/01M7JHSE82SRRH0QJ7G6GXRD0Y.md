# 共用 runtime · 并发 / 内存 / 编码

- id: REQ-01M7JHSE82SRRH0QJ7G6GXRD0Y
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`共用 runtime · 并发 / 内存 / 编码`：两条臂**共享**、且不属于某个 shim 族的失效面。

- **guest 令牌**（容器互斥）的作用域、公平性、等待归因；
- **容器 / 堆 / 表面 arena** 的生命周期与越界（`GuestHeap` / `HandleTable` / `SurfaceArena`）；
- **编码后端**（CP932 要走系统接口还是自带表；它同时喂 font / vfs / kernel32，不是某个 shim 专属）。

## 不收

- 某条 shim 族的可见行为（→ 对应 shim 组）。
- 纯"哪个访存原语读错了"（那属于语义登记 → `lift 层 · 语义与覆盖`）。

★ 归组判据：**失效方式是"共用机制"还是"某一族接口"**。共用机制的线程/锁/内存问题即使由音频触发，
也归这里 —— 音频只是**暴露者**。
