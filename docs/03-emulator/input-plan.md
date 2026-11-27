# 组装输入：外部数据投递到 shim 输入分发点（**已退役**）

> **已退役（2026-11-27，用户裁决 `REQ-01M875NDRETPPNN68VCE3VMCT1`）**：
> `--input-plan` / `--input-plan-trace`、`.plan` 配方、`recipe.*` 解析器、`InteractionGraph`
> 机器内驱动器与 `InputPlan` 类已整体删除（连同它们的 CTest 与 JS parity 入口）。
> 现在没有"配方"这个用户面：控制层的 `click` / `move` 走 `runtime/control_input.h` 的立即队列
> （在 guest 读点上施加），其余输入驱动只有 JS 插件链。
>
> **为什么**：两套驱动 = 两套门 / 两套失败面 / 两套维护成本。`.plan` 的完成度判据（exit 5）
> 与 JS 的 `ctx.finish(5)` 重复，而配方表达不了"当前选中角色 / 技能 / 取消路由"这类 VM 逻辑；
> 留着旧入口只会让人在 JS 路径出问题时又绕回已废弃的那条。
>
> **复现改走哪**：`apps/emulator-lift/scripts/plugins/scenario-*.js`
> （共用库 = `lib/boot-to-title.js` / `lib/save.js` / `lib/chains.js` / `lib/script-positions.js`）；
> 只读控制面用 `emu-ctl pause/sample`，输入投递口径见
> `docs/03-emulator/input-device.md` 与 `runtime/control_input.h` 的头注。
> 脚本级位置区间的落点口径见 `AGENTS.md` §12.9。
>
> 历史正文（配方语法 / 门 / 完成度 / `input-plan:` 读数 / 每个 CTest 的判据）在 git 里：
> `git log -- docs/03-emulator/input-plan.md`。
