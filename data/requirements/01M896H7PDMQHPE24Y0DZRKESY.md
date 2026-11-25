# 脚本工具函数收拢到 lib（配置提取 / 归一 / UI 控件）

- id: REQ-01M896H7PDMQHPE24Y0DZRKESY
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- done_reason: 纯重构（无新判据）：plugin-runtime / plugin-headless / ui-* 全绿 + tsc 0 条作证；边界与未做写在 docs 与 utils.md

# 脚本工具函数收拢到 lib — 已交付

## 已交付（2026-11-25）
- 新增 `scripts/plugins/lib/settings.js`：`openConfigDomain`（谓词收窄）/ `intSetting` / `boolSetting` /
  `textSetting` / `scriptListSetting` / `normalizeScriptList` / `slotSetting` / `registerSettings`；
  `load-save.js` / `scenario-save10-field.js` / `field-point-visible.js` 的逐字复制全部换掉
  （`slot 900/-1/1.5/NaN`、行 y `328/388/507`、`expect` 默认值等旧口径逐字保留）。
- 新增 `scripts/plugins/lib/gates.js`：`awaitPosition` / `awaitScript` / `awaitReadPoint` /
  `failOnTimeout` / `failOnUnknown`；迁移 `boot-to-title`、`lib/save.js`（只两处收口）、`save07` /
  `save10` / `fellow` / `field`（文案、`finish(5)`、`ctx.fail` 前缀逐字不变）。
- `lib/guest-globals.js` 导出 `hex`（`field` 不再自带一份）。
- ⛔ 不抽 `chains.js` 的 `route`、⛔ 不给 `script-positions.js` 加函数、⛔ 不抽 `parseTypes`
  （边界写进 `utils.md` / README）。

## 判据
无新增专用守卫（纯重构，行为不变）：由 `plugin-runtime`（含 `plugin/field-switch-apply`）、
`plugin-headless`、`ui-*` 全绿 + `tsc` 0 条作证；`on=2` / `null` 这类口径变化已在 README 写明。

## 未做
- `lib/save.js` 里剩下的 6 处 `ctx.trigger.*` 裸调用未收口（形状与 gates 有意分离）。
