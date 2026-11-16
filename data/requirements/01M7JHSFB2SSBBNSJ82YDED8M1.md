# 工具 / 测试 / 文档口径

- id: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [org]

## 收什么

`工具 / 测试 / 文档口径`：不改变 guest 可见行为、但决定"我们能不能看见/复现/证明"的那一类。

- **控制层**（`--control-root` + `emu-ctl`：停 / 采样 / 截图 / 投递 / 收口）——它是**工具**，不是游戏输入；
- **导入桩台账**（`data/lift/overrides.json` 的 kind / scope / 模块集）；
- **守卫与测试可移植性**（Windows 专有守卫、assets 档成本、变异清单）；
- **文档读数与口径**（哪些数字可以写、哪些必须改成"怎么查"）；
- **证据覆盖类**工作（例如 `M-lift-5`：补逐位证据，收益是"能证明"而不是"更快"）。

## 不收

- 任何改变 guest 可见行为的实现（→ 对应层组）。
- 需求树工具本身的缺陷（那是仓库工具；若涉及本仓门禁仍可挂这里，但要在正文写明）。
