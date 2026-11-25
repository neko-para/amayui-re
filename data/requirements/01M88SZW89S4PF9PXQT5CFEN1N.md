# 插件入口收敛 S2/S3：HUD 变插件（--overlay = --plugin hud）+ 删 --ui

- id: REQ-01M88SZW89S4PF9PXQT5CFEN1N
- type: req
- status: open
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- tags: [emulator-lift, plugin, cli, refactor]

## 需求

把 HUD 变成**真插件**，让 `--overlay` 只做“`--plugin hud` + 要窗口”的预设；然后删 `--ui`。目标：最后只剩**一个装载点**（`--plugin`）+ 一个窗口预设（`--overlay`）。

## S2（HUD 变插件）

- `RenderCtx` 加 `profile`（`plugin.d.ts` + render ctx 构造 + pure 用例）；`scripts/ui/hud.js` 改 `export default { render(ctx) }`（`ui.*` → `ctx.draw.*`、`game.*` → `ctx.game.*`、`host.*` → `ctx.profile.*`）。
- `--overlay` 在窗口 init 之后由 adopt 打开叠层 —— 需要一个“defer hud”选项，否则 `loadUiScript` 会先硬失败；`--plugin hud` 与 `--overlay` 的像素 / readout 等价测试。
- ★ 保留 id `hud`：解析顺序先于普通插件根；若用户根里存在 `hud.js` ⇒ exit 2（⛔ 不静默被内置件遮住）。
- ⛔ 不删 `--overlay`；⛔ 不从模块导出反推“要不要窗口”（parse 期还不知道有没有 `render`）。

## S3（删 `--ui`）

- 删 parse 分支 / 同一字段比较 / `ui_script` 字段 / `loadUiScript` 的显式分支；`kWindowFlagsAot` 去掉 `--ui <path>`。
- `tests/aot_overlay.cpp` 的 `--ui` 用例改成未知参数 / 删除；`cli_aot_flags` 的 `plugin+ui-different` 用例删除；`overlay.md` / `overlay-quickjs.md` / `plugin-runtime.md` D1 / `input-device.md` 同步。

## 依赖与顺序

S2 依赖 S1（已完成）。S3 必须在 S2 之后：`--ui` 现在的测试用途（自定义 UI 脚本）要先由 `--plugin` 承载 render-only 模块接住。
