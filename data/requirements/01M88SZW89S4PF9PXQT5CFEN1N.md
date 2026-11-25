# 插件入口收敛 S2/S3：HUD 变插件（--overlay = --plugin hud）+ 删 --ui

- id: REQ-01M88SZW89S4PF9PXQT5CFEN1N
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/cli_aot_flags.cpp#ui-removed-unknown-arg
- tags: [emulator-lift, plugin, cli, refactor]

## 需求

把 HUD 变成真插件，让 `--overlay` 只做“`--plugin hud` + 要窗口”的预设；删除 `--ui`，只留 `--plugin` 一个装载点。

## 已交付（2026-11-25）

- **S2**：`RenderCtx.profile`（`begin/end`；`end` 回传 `{ok,text,error}` 并走 `HostPause`）；`scripts/ui/hud.js` = `export default { render(ctx) }` + 旧 `globalThis.render` 双入口，共用同一份 `drawHud`；`--overlay` 无 `--plugin` ⇒ 按保留 id `hud` 解析并装载；有 `--plugin` ⇒ 插件赢（有 `render` 才 adopt 取代；无 `render` 时保留默认 hud 的**内部回退**）。
- **S3**：`--ui` 从 CLI 删除（未知参数 `exit 2`）；`kWindowFlagsAot` 去掉 `--ui <path>`；`ui-hud-script` 保留（改测内部 hud 回退的同一个 UiScript loader）；`--ui` 相关测试/文档迁移；`--ui-input` 仍是窗口参数。
- 保留 id `hud`：`<exe>/scripts/ui/hud.js` → `LIFT_UI_SCRIPT_DIR/hud.js`；用户插件根里同名 `hud.js` ⇒ `exit 2`；ASCII 大小写不敏感。

## 判据（守卫）

- **像素等价**：`--overlay` = `--plugin hud` = 内部 hud 回退，BMP 2764854 B / sha256 `01468478066F2C2DC728E535569FCFA412C6266DD8954BB951A55DC18FC818E7`（★ 2026-11-25 D4 版式后重算；旧 `3E72F989…` 作废）、guest dump 逐字节同（`tests/aot_overlay.cpp` ⑦ 三方 `equal=1`）。
- `--overlay` + 无 `render` 的 `--plugin` ⇒ HUD 仍在（`apps/emulator-lift/tests/aot_overlay.cpp#overlay-with-run-only-plugin`）。
- `--ui` ⇒ `exit 2` + `未知参数：--ui`（`apps/emulator-lift/tests/cli_aot_flags.cpp#ui-removed-unknown-arg`）。
- `--overlay --input-plan` ⇒ `exit 5`；`--overlay --plugin hud --input-plan` ⇒ `exit 2`（预设不改互斥语义）。
- `RenderCtx` 运行期恰好 **8** 成员（含 `profile`；2026-11-26 起加回 `config` —— `--overlay` 的 HUD 插件也要能读写 `plugin-config.json`，与经典回退 HUD 的 `host.config` 同一份桥）。

## 读数（2026-11-25 / win32 / RelWithDebInfo）

- D4 之前：全量 `ctest` **144/144**；`pnpm test` **633/633**；`pnpm verify` **472 pass / 2 skip / 0 fail**；`requirements validate` **6/6**。
- D4 之后（设置模块 / 左中版式 / F9）：全量 `ctest` **146/146**（present-window 取样点随版式更新）；`tsc`（scripts + examples）**0 条**。

## 剩

- 发行打包 R10（`--overlay` / hud 的 release 脚本布局）仍未做；`--overlay --control-root` 无专用 CTest。
