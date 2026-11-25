# 业务外策略入口整合：插件运行时 + ControlBus（plan-as-callbacks，C0–C5）

- id: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- type: req
- status: open
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, control, qjs, input, design]

## 一句话

把三个“业务外策略入口”（`--input-plan` / `emu-ctl` / QJS）整合成 **一个插件运行时 + 一个 ControlBus**：
插件用 **plan-as-callbacks**（具名 Trigger 链）驱动；输入/编辑/读/触发按**操作类**分开；
`emu-ctl` 只是 ControlBus 的一个传输；历史 `.plan` 冻结，由 `InputPlan` 兼容执行、JS driver 只做 parity。
本单跟踪 **C0–C5** 整个重构与开发。

## 已入库（2026-11-19，合同 v1）

- 合同文档：`docs/03-emulator/plugin-runtime.md`（1400+ 行；含 v5.3.2 + Timeout）。
- API 声明：`apps/emulator-lift/scripts/plugins/plugin.d.ts`（368 行，`@amayui/lift-plugin`）。
- 示例：`apps/emulator-lift/scripts/plugins/examples/`（9 个 `.ts`，strict `tsc` 全过；含 load-save / global-edit /
  hud-at-pos / dispatch / keep-others / maxfires / rearm / timeout / concurrent）。
- 可用性验证：5 轮“只看 d.ts 的翻译测试”（原始报告在 `.tmp/control-unify/usability*.md`，未入库）；
  `P0 = 0 / P1 = 0`，剩余 P2 是收尾打磨。

## 核心模型（合同）

- **单插件运行时**：一个 ES module `export default PluginModule<State>`；`render`（每帧画）/`start`/`onTrigger` 可选；
  `ctx.edit`（立即）/`ctx.input`（串行读点）/`ctx.read`（无暂停）/`ctx.trigger`/`ctx.ui`/`ctx.run`/`ctx.finish`/`ctx.quit`。
- **具名 Trigger**：`name` = 状态名；`start/onTrigger` 返回 `Trigger | Trigger[] | null`；数组 = 完整新 armed 集合（替换）；
  一读点最多一个；`null/[]`=done；重复 name=fail；`maxFires` 对象级跨 arm；Once/Reenter 的 rearm 表已写死。
- **控制面 5 组件**：`EditSink` · `EngineRead` · `IntentQueue` · `RunnerControl` · `RunControl`（+ 可选 `HostAction`）；
  `sample`/`screenshot` 不是核心类，是 `emu-ctl` 的组合（pause + EngineRead / pause + HostAction）。
- **历史 plan**：文本 recipe 冻结、不再加能力；`InputPlan` 保留为冻结兼容执行器（summary/failure/fault/trace/exit5 不动）；
  C3 的 JS driver 只做 parity（语义等价 + 已知差异表，含 `wait`/`>=` 1ms）。
- **`emu-ctl`**：9 op 逐个映射到核心操作；`control_ws` 保留控制点胶水（screenshot auto-pause/state_seq、resume hook、
  click 暂停准入）；一个能力只有一处实现。

## 切片（C0–C5）

- **C0**：**已完成（main）**：能力表/接口/9 项同源（`hello`/`caps=`/`--help`/`.d.ts` 一致；
  `caps=` 6→9 readout 修正）；守卫 `tools/test/capability-matrix.test.mjs`。
- **C1**：**已完成（main；独立复审“C1 可收”）**：`TriggerRunner` + `IntentQueue` + executor +
  `EngineRead` + C++ handler（`after position → global 读改写 → click`；不接 `input.h`、不迁 control click）；
  用例 `tests/trigger_runner.cpp`（361 checks / 0 fail）。
- **C2a**：**已完成（main；四轮独立复审收口）**：QJS loader/`start`/`onTrigger`/`ctx` 基础、
  `--plugin`/`--plugin-param`、读点 observer、`plugin-runtime`（170 checks）与 `plugin-headless` e2e。
- **C2b**：**已完成（main `68ac4ae`；独立复审 PASS-with-fixes）**：C2b-1 `RenderCtx`/`ctx.draw`/`ctx.ui`；
  C2b-2 `adoptOverlayScript` 窗口装配 / FontEngine 进 PluginHost / `game.mouse().buttons` 生产读数 /
  `plugin-overlay`（pure，30 checks）+ `aot-overlay` 子用例（44 checks）。
- **C2c**：**已完成（2026-11-20）**：`scripts/plugins/lib/save.js` 的 `loadSave(slot)` + pure chain + `plugin/load-save-e2e`（mac 真跑；偶发 gate timeout 已单独立 `REQ-01M7W2X81F1ASB488EJFQGMQSH`）。
- **C3**：**已完成（dev `02d3b73` → main `eecbfac`）**：`src/runtime/recipe_json.{h,cpp}`（`Step[]` 唯一 JSON 出口）·
  `scripts/plugins/lib/recipe.js`（`runRecipe`，只翻译不判门）· `recipe-driver-parity`（pure；oracle=`InputPlan`）·
  `tools/test/recipe-freeze.test.mjs`；已知差异 10 条（D5：显式 `mouse_down/up` 不施加 ⇒ save05-move NON-DIFFABLE）。
- **C4**：ControlBus 收编 `emu-ctl`（`EditSink`/`EngineRead`/`HostAction`/控制点；WS 薄传输）。
- **C5**：UI→Run（`read → edit → input → dispatch` 的 e2e）。
- 最小闭环 **C0 → C1 → C2**；C3/C4/C5 可并行/后置。

## 已裁决（D1–D8，按推荐）

D1 单插件入口/`--plugin-param`/headless；D2 具名集合/one-per-point/replace；D3 宿主墙钟 + `NON-DIFFABLE`；
D4 recipe 冻结 + `InputPlan` 保留 + C3 parity；D5 错误/超时/owner/预算（2 ms / 100k ops）；
D6 ControlBus 5 + WS 控制点；D7 Run 只收 module+params、不 eval、busy 不排队；D8 Run status/events/owner/一个 active。
★ C2b-2 复审的裁决点：`--overlay` + 插件 render ⇒ 插件 render **取代** hud.js（一次性 stderr，⛔ 不静默）；
adopt 失败（字体缺失 / 窗口没建）⇒ exit 2（与 `--overlay` R5 同口径）。

## 残留 / 未做

- 14 条 P2（`ctx.fail` 单参陷阱、initialState 的 ctx 引用只能 optional+guard、耗尽态不可类型化、stringly name、
  `UiApi.get` 无校验、`ProfileApi.end`、measure 估算等）——在 C0/C1 落类型/文档时处理。
- C0/C1/C2a/C2b/C2c/C3 已在 main；C4/C5 未完成；`emu-ctl` 尚未迁移；C5 e2e 未做。
- C2b-2 的 docs 同步已完成（`da3d845`）：`plugin-runtime.md` §4.6/§8.1 与 `overlay.md` 的 `game.mouse` 形状（`buttons` 可选键）。
- ~~`plugin-overlay` 未覆盖“adopt 丢弃 owned hud 脚本”那条路径~~（2026-11-25 S3 删除 `--ui` 后该路径不再存在；HUD 由 `--overlay` 预设 / 内部回退覆盖）。
- 蓝遮罩：`REQ-01M7SW90PC5GXBWZVJ2FJH7DY8`（独立；2026-11-19 真机确认为预期表现，已 dropped，不再是残留）。

## 怎么查

- 合同：`docs/03-emulator/plugin-runtime.md`
- API/示例：`node_modules/.bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json`
- 进度：`pnpm tools requirements plan`；本单的 `C0–C5` 逐个关。
