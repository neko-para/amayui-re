# emulator-lift · 活工作分类（按层 / 侧）

- id: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- type: req
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [org, emulator-lift]

## 这是什么

`emulator-lift` 的**活工作容器**，分类轴只有一条：**改哪一层 / 失效方式**。
⛔ 不按里程碑归组（M1/M2/M3/M4 是交付阶段，不是失效方式）——里程碑节点保留为历史容器，**不再往里挂新单**。

## 组（按层/侧）

| 组 | 收什么 |
|---|---|
| `shim 层 · 图形侧` | D3D9 / DDraw / GDI / GPU 光栅 / sprite 合成 / frame 记录 / 图形 shim 的实现效率 |
| `shim 层 · 音频侧` | DSound / 混音 / feeder / 位置通知 / 解码吞吐 / **AOT 音频管线** |
| `shim 层 · 输入与窗口 / 配方` | user32 消息 / 输入投放 / 配方 DSL 与门 / click 队列 |
| `共用 runtime · 并发 / 内存 / 编码` | guest 令牌 / 容器 / 堆 / 表面 arena / CP932 编码后端 |
| `lift 层 · 语义与覆盖` | 闭包 / 跳表 / opcode 语义 / 入口集 / 停机原因 / EH / SMC |
| `lift 层 · 效率` | 发射器效率（M-lift-2 / 4）/ 压力档 / 端到端墙钟 |
| `工具 / 测试 / 文档口径` | 控制层（AOT inspect 工具）/ 导入桩台账 / 守卫可移植性 / 文档读数 / 证据覆盖类工作 |

## 归组判据（一条，机械）

**这张单要改的代码在哪一层、它的判据红在哪**——不按血缘（父单在哪不决定子单去哪）。
例：`M-lift-3` 虽然挂在 lift 支路下，但它改的是图形 shim 实现 ⇒ 归**图形侧**；`M-lift-5` 的收益是证据覆盖 ⇒ 归**工具/测试**。

## 边界

- 组是**容器**，不是判据；⛔ 不要给组写"完成"判据（它们永不收口）。
- 单子仍遵守本仓规矩：一张单只收一类失效方式（`AGENTS.md` §10 / `amayui-requirements`）。
