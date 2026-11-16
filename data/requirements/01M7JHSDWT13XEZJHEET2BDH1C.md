# shim 层 · 输入与窗口 / 配方

- id: REQ-01M7JHSDWT13XEZJHEET2BDH1C
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`shim 层 · 输入与窗口 / 配方`：

- **输入投放**（外部数据 → shim 输入分发点，不在执行循环里判断）；
- **配方 DSL 与门**（`await script` / `await position` / `wait` / `click`，以及将来的 `global` / `local` 门）；
- **click 的施加语义**（立即队列、读点、回执）；
- **窗口 / 消息**（user32 消息泵、焦点、光标、关窗与确认框）。

## 不收

- 控制层（暂停 / 采样 / 截图那套**工具**）→ `工具 / 测试 / 文档口径`。
- 图形侧的呈现 → `图形侧`。

★ 归组判据仍然只有一条：**改哪一层 / 判据红在哪**。"配方是给测试用的"不等于它属于工具组 ——
它改的是**输入系统本身**的语义。
