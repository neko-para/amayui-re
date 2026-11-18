# emulator-lift · 子系统 · UI 插件宿主（overlay / QuickJS）

- id: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- type: req
- status: doing
- parent: REQ-01M7JHSCSEX4Q58YPY462RNJQW
- tags: [emulator-lift, ui, overlay]

## 一句话

overlay / UI 宿主是 `emulator-lift` 的**新子系统**：一个 `UiHost`、同一份 display list、同一条输入路由；
插件 = 一个 JS 脚本文件（`apps/emulator-lift/scripts/ui/<name>.js`），宿主独占校验 / 求值 / 布局 / 命中 / 光栅 / 合成。
设计真源：`repo:docs/03-emulator/overlay.md`（产品面 / 里程碑）与 `repo:docs/03-emulator/overlay-quickjs.md`（绑定 / 沙箱 / 预算）。

## 边界（从设计真源摘出的不变量）

- 一个 host + 同一套 API：内置 HUD 与游戏内小窗口是同一类插件；⛔ 不为 HUD 单开渲染 / 输入路径。
- 每帧 display list（立即模式）：没有跨帧控件树、脏矩形、GPU 资源生命周期。
- 坐标：声明用 1280×720 参考系，合成用窗口像素（letterbox 之后）；两条呈现路径统一。
- 输入单一 router：OS / 注入 down / 注入 up 三个入口都先问同一个 `UiClickRouter`；`input::*` 仍是唯一施加路径。
- ⛔ 不写 guest；`--present-screenshot` 含 UI，`--dump-frame` / `arena.hash` / 控制层 `screenshot` 不含。
- 失败响亮：启动期 `exit 2` + `origin:行:列`；运行期卸载该插件 + 一次性 stderr。
- ⛔ 无 Promise / async；`render` 必须同步产出 display list。

## 里程碑（判据真源 = overlay.md §4）

| # | 内容 | 状态 |
|---|---|---|
| M0 | 接线与统一合成（无脚本、无输入；AOT `--overlay` + `presentOverlay` + 两条路径窗口像素） | 待复审结论后开子单 |
| M1 | `ui_core` + display list + 布局 / 条件 / 命中（无语言运行时） | open |
| M2 | QuickJS 宿主 + HUD 迁脚本 + 点击路由 | open |
| M3 | `script/pos` 条件 + 数据查询 + 控制层面 | open |
| M4 | 图片 / 锚点进阶 / 热重载收尾 | open |

## 未做（边界）

- 本容器**不写判据**：每个里程碑的判据在 `docs/03-emulator/overlay.md` §4，子单只挂 `verify:` 锚。
- M0 是否具备开工条件（有无阻塞裁决）以独立复审 `.tmp/overlay-review/review.md` 的结论为准。
- 实施落点只在本仓；MaaFramework 是只读外部参考，⛔ 不进依赖 / 构建。
