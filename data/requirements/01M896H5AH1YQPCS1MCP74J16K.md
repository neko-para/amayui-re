# FIELD 开关不得驱动读档（常驻施加派生位）

- id: REQ-01M896H5AH1YQPCS1MCP74J16K
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/plugin_field_switch.cpp#field-switch/static-contract

# FIELD 开关不得驱动读档（语义偏移）— 已交付

## 现象
`field-point-visible.js` 名为开关，却自带 `bootToTitle` + `loadSave(slot=10)` + 点行 + 确认框：
`--plugin field-point-visible` 会**自动读档**进入 FIELD。用户裁决：开关不许产生任何输入。

## 已交付（2026-11-25）
- 插件重写为**常驻开关**：不 import `save.js` / `boot-to-title.js` / `chains.js`，不发 `ctx.input.*`，
  不写盘（唯一写 = `settings.schema` 的机器域），正常路径**不 `finish`**。
- 每 `poll`（缺省 250 ms）一轮；`ctx.now.script === "FIELD.BIN"` 时才读/写派生位 `0x50ff9f` 的
  `HIDE|ALPHA0`（`after === before` 的槽不发写回执）⇒ 每次进图/换图自动重新施加。
- 配置：`on` / `types` / `diag` / `poll`（旧 `slot` / `hold` 无害忽略）；并在 `start` 用
  `registerSettings` 注册到 `settings.schema`。
- 自动化复现 FIELD 仍归 `scenario-save10-field.js`；`--plugin` 与 `--input-plan` / `--control-root`
  互斥（`src/plugin/readings.h:19`），因此不设计"输入计划驱动"的验收。

## 判据
- `apps/emulator-lift/tests/plugin_field_switch.cpp#field-switch/static-contract`（剥注释后代码里
  没有 `ctx.input` / `loadSave` / `bootToTitle` / `ctx.finish` / `ctx.quit`；且仍写着
  `forceFieldPointsVisible` / `afterDelay` / `DEFAULT_POLL_MS`）。
- 同一文件 `#field-switch/resident-headless`：真子进程、无 `--input-plan` ⇒ `run=waiting`、
  `stop: time-limit`、exit 0，stdout/stderr 无 `boot:` / `load:` / `load-save:`。
- `apps/emulator-lift/tests/plugin_runtime.cpp#plugin/field-switch-apply`：假 guest 上判施加 /
  不重复写 / 换图重施加 / `on=0` 不写。

## 未做
- 真机（人读档进 FIELD）`on=1` 的 `forced=N/M` 读数：没有自动化入口（互斥），已记
  `docs/03-emulator/scripts/FIELD.md` 的 `## 待核`。
- `on:2` 现在按非法值 throw（`lib/settings.js` 的并集语义只收 bool / 0-1 / `"true"|"false"`）。
