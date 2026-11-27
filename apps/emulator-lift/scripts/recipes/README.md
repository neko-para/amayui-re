# `scripts/recipes/` —— **已退役（2026-11-27）**

`.plan` 输入配方（12 份）已按用户裁决整体删除：`--input-plan` / `--input-plan-trace` 现在是
**未知参数**（`exit 2`），`recipe.*` 解析器 / `recipe_json.*` / `InteractionGraph` /
`InputPlan` 类也一起退场。

- **为什么**：见 `docs/03-emulator/input-plan.md`（退役 stub）与
  `data/requirements/01M875NDRETPPNN68VCE3VMCT1.md`。一句话：两套驱动 = 两套门 / 两套失败面；
  `.plan` 表达不了 VM 逻辑（当前选中角色 / 技能 / 取消路由）。
- **历史**：文件内容在 git 里（`git log -- apps/emulator-lift/scripts/recipes/`），不用留档。
- **复现改走哪**：JS 插件场景 `../plugins/scenario-*.js`（共用库在 `../plugins/lib/`）——
  `boot-to-title.js` / `save.js` / `chains.js` / `gates.js` / `script-positions.js`。
  只读控制面用 `emu-ctl` 的 `pause` + `sample`。
