# 插件 v2 收尾：scenario/lib 迁移 · C5 dispatch · perf A/B · FIELD 体验 · MSVC

- id: REQ-01M8FP9GQBMA7K6JBTQ4YZZ6JB
- type: req
- status: open
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [plugin, emulator-lift]

## 为什么

插件 v2（`REQ-01M8EKFAP4QJY9G12K7KB9YEQB`）的**插件内 API** 已实现并收口；本单收它明确留出的边界与后续。
不把这些挂回父单，是为了让"插件内 API 在不在"有单一判据，而"迁移/控制层/性能/体验"各有自己的判据。

## 收什么

1. **scenario/lib 迁 v2**：`scripts/plugins/scenario-*.js` 与 `lib/*.js` 从 v1 `start`/`onTrigger` 迁到
   `ctx.at/once/task`；迁完才允许删除 v1 兼容层（当前 v1 保留为 deprecated）。
2. **C5 · Run 实体 / dispatch**：`ctx.run.dispatch`、同 endpoint 一个 active Run、新 dispatch ⇒ `busy`（不排队）、
   abort / releaseOwner。父单的"不做嵌套 Run"是边界，本单是它的落地。
3. **perf A/B**：1 条永不命中 hook ≤1.5%（20 轮 CI 上界）、cheap key 档 ≤ 今天读点 poll、JS 回调每帧上限。
   判据必须是 **rel / 同机 / 交错 ≥3 对 / 中位**（AGENTS §12.6），读数进 `.tmp/` 文档，⛔ 不进 test。
4. **FIELD 人工体验**：`--user-run` + `out-user/`（用户侧），agent 不把它当判据。
5. **MSVC（cl）真编译**：当前 win32 preset = clang-cl；cl 下 quickjs / 插件 TU 的编译与 ctest 读数。

## 判据

- 迁移：`ctest -R 'plugin|ui-script'` 全绿 + `rg 'ctx\.trigger|onTrigger' scripts/plugins` 只剩 deprecated 说明。
- dispatch：`plugin/dispatch-busy` / `plugin/abort-release`（新 CTest；落地时补锚）。
- perf：命令 + 读数文件（rel、win32、交错 ≥3 对、中位）；⛔ 不写"全绿"。
- MSVC：configure/build/ctest 的 N/N 读数。

## 怎么查

- 父单：`node tools/requirements.mjs --show REQ-01M8EKFAP4QJY9G12K7KB9YEQB`
- 合同：`docs/03-emulator/plugin-runtime.md`（§4.12–§4.16 / §6 行 44–49）
- 证据：`.tmp/plugin-v2/`（plan / 各片 report / review / fix-report）
