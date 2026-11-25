# 废弃 plan：--input-plan 与 .plan 配方整体退役（输入驱动统一到 JS 插件链）

- id: REQ-01M875NDRETPPNN68VCE3VMCT1
- type: req
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, plan, deprecation]

## 一句话

把 **`--input-plan` 与 `scripts/recipes/*.plan` 整体废弃**：不再维护、逐步删消费方与门禁，
输入驱动统一到 **JS 插件链**（`loadSave` / `ctx.trigger.*` / `ctx.input.*`）。

## 为什么（用户 2026-11-25 口径）

- `plan` 是**废弃工具**，之前已多次研究；但**不禁用就还会被用** —— 本轮 FIELD 的 `loadSave` 失败时，
  第一反应又是"用 plan 绕"，而正确路径是**修 JS `loadSave`**。
- 两套驱动 = 两套门 / 两套失败面 / 两套维护成本；`plan` 的完成度判据（exit 5）与 JS 的
  `ctx.finish(5)` 重复，且 `plan` 不能表达"当前选中角色 / 技能 / 取消路由"这类 VM 逻辑。
- 已有 JS 场景覆盖了 plan 的主用途：`scenario-fellow-switch.js`、`scenario-save10-field.js`、
  `scenario-save07-battle.js`、`field-point-visible.js`（本轮新增）。

## 要做什么（分批，不要一轮删光）

1. **盘点消费点**：`rg '\.plan|--input-plan'` 出 CMake 的 CTest、`tests/*.cpp`、`scripts/recipes/`、
   `script-map.test`、`recipe-driver-parity` 的 replay、stress S1/S2、`save05-move.plan` /
   `save07-battle.plan` / `boot-title.plan`；列出"还有谁在用"。
2. **迁移**：每个仍在用的 plan → JS 插件/场景（有现成 `scenario-*.js` 的优先；S1 的"永不满足门"
   改成 JS 的 `ctx.finish(5)` 负对照）。
3. **删入口**：`--input-plan` 变成**未知参数**（`exit 2`），或至少打一行 deprecated 并拒绝；
   CMake 里 plan 驱动的 CTest 改成 JS 版。
4. **删文件**：`.plan` 与 `recipe-driver-parity` 的逐条 replay 一起删（历史在 git，不用留档）；
   `scripts/recipes/README.md` 改成"已废弃"指针。
5. **守卫**：一条 pure 检查"仓库里没有 `.plan` 被 CTest / 脚本消费"；一条 `--input-plan` ⇒ exit 2 的
   CLI 用例（照 `cli-aot-flags` 的 spawn 形状）。

## 判据

- `rg '\.plan'` 只剩文档 / 历史引用，**无 CTest、无 `--input-plan` 调用**；
- `emulator-lift-aot --input-plan <任意>` ⇒ **exit 2**（或明确的 deprecated 拒绝）；
- 原 plan 覆盖的场景（boot→TITLE、SAVE05 移动、save07-battle、S1 负对照）都有 **JS 版且绿**；
- `ctest` / `pnpm test` / `pnpm verify` 全绿；删掉的 CTest 名没有 `data/requirements` 的
  `verify:` / `repro:` 锚（§12.4）。

## 依赖 / 风险

- **先修 `loadSave` 回归**（`REQ-01M86VRRTPY8W9RP1SFDT0N200`）：JS 路必须先稳定，否则是"拆了旧桥没搭新桥"。
- `save05-move.plan` 的页基漂移（`saves/SAVE.DAT` 持久化页 1）——迁移时用 `loadSave(5)` 自判页。
- 分批删：先删 **CTest 消费方**，再删 `--input-plan` 入口，最后删文件；每批门禁绿。

## 边界

⛔ 不改写 git 历史；⛔ 不用 plan 自己的完成度判据当"废弃完成"的证据；⛔ 不在这一轮删光。
