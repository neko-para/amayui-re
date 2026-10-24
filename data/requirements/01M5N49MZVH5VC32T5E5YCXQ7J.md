# 残差第九批：命令档执行合同 · S0 规划器

- id: REQ-01M5N49MZVH5VC32T5E5YCXQ7J
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene-command-plan.test.mjs#★ 命令键升序：输入乱序也按命令键（不是插入次序）执行
- tags: [spec residual]

# 残差第九批：命令档执行合同 · S0 规划器

## 一句话

把命令档执行的**结构**写成合同（入口次序 / 空表 / 前置 / 出档 / 中止 / 命令表生命周期 / 区间之下口径），并落地 S0 规划器（只规划、不真实绘制）。真实绘制（画布 36/37、表面拷贝、遮罩、图案、技法）留后续。

## 合同

- **两档**：命令档（命令键严格升序）→ 收尾扫描；空表无动作。
- **每条前置**：取记录 → 时间戳 0 盖章 → 用**区间 A 起点** + 场景键窗算「跳过额外变换」→ 按种类分派。
- **正常出档**：目标不同才恢复；「有动画 ∨ 强制完成」⇒ 置脏。
- **中止**（种类 2 缺同键绘制项）：清持续时间后整档返回、不恢复不收尾。
- **生命周期**：渲染步收尾检查「有动画」=0 ⇒ 整表清空；随后「强制完成」清 0；各命令推进期会置「有动画」。
- **区间之下**：种类 1 仅首帧 ∧ 设备就绪 ∧ 能力档<2；种类 0/2 每帧每块 ∧ <2（跳过另一段）；种类 3 不发生；不置已消费（单画布两次 / 双画布三次）。

## 闭合 / 残差

`RES-SCENE-DRAW-COMMAND-COVERAGE` 关闭；`KIND1-BELOW-RANGE` 并入 `RES-SCENE-COMMAND-BELOW-RANGE-ROLE`；新增 `TABLE-LIFETIME` / `EFFECT-TECHNIQUE`。

## emulator-new 落地（S0/S1）

- 新纯函数 `renderer/scene-command-plan.ts`：commands / now / 键窗 / 三表键 / 能力档 / 有动画 / 强制完成 → 规划（阶段、目标 36/37/own/movie、区间 A/B、覆盖键、consumed）、`clearTable`、`stamps`。
- `protocol.ts` + `session.ts` 投影 `SceneSnapshot.commands`；`scene-order.ts` 去掉 `scene-command-order-unmodeled`，改报 `scene-command-pass-planned` 与 `scene-command-pass-render-unmodeled`；`pixi-host.ts` 只加统计。
- 守卫 `emulator-new-scene-command-plan`（11 条）；变异 +5（320 条）。

## 仍开着

真实绘制（画布 36/37 / 表面拷贝 / 遮罩 / 12 图案 / 技法 / 进度插值 / 真实清表）· `shaderTier` 映射 · `forcedComplete` 核心格 · `RES-SCENE-COMMAND-BELOW-RANGE-ROLE` / `-TABLE-LIFETIME` / `-EFFECT-TECHNIQUE` / `KIND0-ALPHA` / `KIND1-FLAG`。

## 判据

`pnpm test` 974/0 · `pnpm test:assets` 200/0（1 skip）· `pnpm test:mutation` 320/320 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。复审：一轮 S1 4，已逐条修订。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` · `docs/02-engine/spec-audit-2026-10.md` §26 · `tools/test/emulator-new-scene-command-plan.test.mjs`。
