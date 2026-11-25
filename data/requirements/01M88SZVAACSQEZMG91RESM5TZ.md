# 插件入口收敛 S1：--plugin <id|path>（非递归两根）+ 入口/库重排

- id: REQ-01M88SZVAACSQEZMG91RESM5TZ
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/plugin_resolve.cpp#resolve/id-both-roots-ambiguous
- tags: [emulator-lift, plugin, cli, refactor]

## 需求

`--plugin` 收 **id 与路径**两种 spec；id 只查两个**非递归**根；`lib/` 只被 import、**不是 id 目标**；入口插件与库分开存放。

## 判据

- id 语法 `^[A-Za-z0-9_-]{1,64}$`；含 `.` / `/` / `\` / 绝对路径 ⇒ 当**路径**（`.js` 后缀天然是路径）；路径形式行为逐字不变。
- 搜索（非递归，仅两处）：`<exeDir>/scripts/plugins/<id>.js` → 编译期 `LIFT_PLUGIN_DIR/<id>.js`；⛔ 不查 `lib/`、⛔ 子目录、⛔ cwd、⛔ PATH。
- 歧义（两根同 id）⇒ exit 2 列全部候选；0 命中 ⇒ exit 2 列搜过的目录；命中打一行 `--plugin id=… ⇒ <绝对路径>`。
- 布局：4 个 `scenario-*.js` 在 `scripts/plugins/` 根；`lib/save.js` 只留核心（无 `export default`），根 `load-save.js` 是参考入口（域 `save` > params > 默认）；插件内 import 全为 `./lib/…`（QJS 模块根 = 入口目录，`..` 越根被拒）。
- `--ui` 只加一行 deprecated warn（行为不变、不删；删在 S3）。

## 落点（已交付）

- `src/plugin/plugin_search.{h,cpp}`（纯函数、roots 可注入）· `aot_main` 接线 · `CMakeLists` 的 `LIFT_PLUGIN_DIR` · `tests/plugin_resolve.cpp` · `cli_aot_flags` · `script-map` · `plugin-runtime.md` · `FIELD.md` · `scripts/plugins/README.md`。
- 守卫：`plugin-resolve`（syntax / path-forms / id-exe-root / id-dev-root / id-both-roots-ambiguous / id-not-found-lists-candidates / id-case / duplicate-roots-deduped / directory-is-not-a-hit）· `plugin/load-save-e2e`（根入口）· FIELD 的 id 验收 `forced=3/3`。

## 读数（2026-11-25 / win32 / RelWithDebInfo）

- `ctest` 全量 **144/144**；`pnpm test` **633/633**；`plugin/load-save-e2e` exit 0。
- FIELD：`--plugin field-point-visible`（id 形式、配置文件、无 `--plugin-param`）⇒ `field-points: forced=3/3`、`exit=0`。
- main 侧锚点已跟随（`repo:` / `repro:` / 正文命令），`requirements validate` 6/6。
