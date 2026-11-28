# 插件 v2 收尾：scenario/lib 迁移 · C5 dispatch · perf A/B · FIELD 体验 · MSVC

- id: REQ-01M8FP9GQBMA7K6JBTQ4YZZ6JB
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [plugin, emulator-lift]

## 为什么

插件 v2（`REQ-01M8EKFAP4QJY9G12K7KB9YEQB`）的**插件内 API** 已实现并收口；本单把仓库里剩下的 v1 用户
（scenario / lib / 真实入口）迁到 v2，并**删除 v1 兼容层** —— 用户 2026-11-28 裁决：直接以最终形态体验，
避免兼容层造成损失/模糊；**不要求 MSVC(cl)，统一 clang-cl**。

## 收什么

1. **迁移**：`scripts/plugins` 下 `scenario-*.js` / `lib/*.js` / `field-point-visible.js` / `load-save.js` /
   `quit-at-title.js` 全部改成 v2（`ctx.at/once/task/off` + `Tick` + `RenderCtx` + 全局 `log`），
   行为与现有 e2e readout 逐条等价（点位/次数/slot/分支）。
2. **删 v1 兼容层**：`PluginModule.onTrigger`、`start` 返回 `Trigger|Trigger[]|null` 的契约、
   `ctx.trigger.*`、`ctx.now`、`ctx.state`、RunCtx 的 `ctx.ui`、`ctx.hook.at/list/clear`、
   `InstrHookHandle.onTick` / `InstrTick`、`ctx.log`/`tick.log`（全局 `log` 取代）、`TriggerRunner` /
   `TriggerFactory` / `Trigger` 中**插件面**的部分。删除后 `plugin.d.ts` 不再有 deprecated 面。
3. **保留**（不是兼容层）：`ctx.read`（getter 查询，scenario/lib 需要）、`ctx.params`、`ctx.config`、
   `ctx.appdata`、`ctx.profile`、`ctx.finish/quit/fail`、读点 `IntentExecutor`（input 下一读点落地 / timeout 泵）。
4. **C5 dispatch / Run 实体**（`ctx.run.dispatch`、同 endpoint 一个 active Run、busy）——按需，可与迁移分开。
5. **perf A/B**：hook ≤1.5%（20 轮 CI 上界）等；rel / 同机 / 交错 ≥3 对 / 中位，读数进 `.tmp/`，⛔ 不进 test。
6. **FIELD 人工体验**：`--user-run` + `out-user/`（用户侧，不是 agent 判据）。

## 判据

- 迁移：`ctest -R 'plugin|ui-script'` 全绿，且 `plugin-headless` / `plugin-field-switch` / `plugin-overlay`
  的 readout/像素断言**逐字未改弱**；`rg 'ctx\.trigger|onTrigger|ctx\.now|ctx\.state|hook\.at' scripts/plugins`
  只剩 0（或文档性说明）。
- 删兼容层：上述符号在 `src/**` / `plugin.d.ts` 中 0 命中（守卫/形态测试可加一条）；全量 `ctest` 144/144。
- perf / FIELD：命令 + 读数文件；FIELD 由用户做。

## 怎么查

- 父单：`node tools/requirements.mjs --show REQ-01M8EKFAP4QJY9G12K7KB9YEQB`
- 合同：`docs/03-emulator/plugin-runtime.md`（§4.12–§4.16 / §6 行 44–49；删兼容层后要同步 §4.2/§4.5/§4.6/§5/§7）
- 盘点：`.tmp/plugin-v2/final/{scripts-inventory,tests-inventory,runtime-inventory}.md`
