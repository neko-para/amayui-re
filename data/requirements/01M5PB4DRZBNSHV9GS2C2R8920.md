# 残差第十九批：辅助表 A/B 身份（A = Live2D，排除）

- id: REQ-01M5PB4DRZBNSHV9GS2C2R8920
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene.test.mjs#★ 0x344 场景条目：置存在位 + 写值 + 标脏
- tags: [spec residual]

# 残差第十九批：辅助表 A/B 身份（A = Live2D，排除）

## 一句话

辅助表 A = **Live2D 模型条目表**，按用户要求排除、不展开；辅助表 B = **3D 网格模型条目表**（领域名，字段级只作证据）。`0x344` 写 A 而不是绘制项表。

## 结论

- A（`+0x448`）：记录 `+4` 索引 `scene+0xDA04` 模型池；发射器 `sub_4B0360` → `sub_4783D0`（startMotion/updateParam）；自装正交投影并还原。⇒ Live2D，排除。
- B（`+0x438`）：`DrawModel`；572 B 记录，有效/时间窗/已消费位、模型号、混合档、窗、两色、矩阵槽。`0x333` / `0x338` 写。
- `0x344` 写 A（`+0x448`），不是绘制项表（`+0x408`）；正文“是否同表”收口为**不同表**。
- 平键序是固定测序，不由内容派生。

## 实现

`0x344` 注释改为“辅助表 A（Live2D，不展开）；本实现只当不透明桶保存”，行为不变；`scene-order` 的 `auxA=null` 是显式“不建模 A”。

## 仍开着

`RES-SCENE-DRAW-AUX-B-FIELDS`（B 矩阵槽读出点）· 同键叠放真机采样 · 真实绘制 S2+ · 保存读侧。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` / `CORE-OPS-SCENE-ENTRY-344` · `docs/02-engine/spec-audit-2026-10.md` §36。
