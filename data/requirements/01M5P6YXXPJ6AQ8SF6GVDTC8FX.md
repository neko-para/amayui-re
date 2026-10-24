# 残差第十六批：帧位置表 · 渲染步外的混合写者

- id: REQ-01M5P6YXXPJ6AQ8SF6GVDTC8FX
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-tables.test.mjs#★ 表项数组 + 线性查找 ⇒ `rec[259]` / `rec[260]` 是**元素下标**（不是位置、也不是位置 − 起点）；当前帧 `rec[260]` 强制 −1
- tags: [spec residual]

# 残差第十六批：帧位置表 · 渲染步外的混合写者

## 一句话

闭合 `RES-SAVE-FRAMEREC`（三张位置表来自脚本头、装载期建表）与 `RES-SCENE-COMMAND-BLEND-INTERFRAME`（命令档前 0 个混合写者）；并把帧位置表落进 `apps/emulator-new`。

## 合同

- 三张位置表 = **脚本文件头自带的反查表**：头里 `[u32 n][3×(条数, 表区偏移)]`；表体是**位置数组**（严格升序），`rec[259]/[260]` 的下标是**线性查找**结果，不是"位置减表起点"。
- `rec[0]` = **父帧号**（根 −1），不是状态码；`rec[259]`（表 1 = `0x71`）/ `rec[260]`（表 2 = `0x03`）不互斥；当前帧 `rec[260]` 强制 −1；`0xAE` 的 +3 = 表 2 项所指换脚本指令长度。
- 混合：命令档入口前无写者（渲染步内外皆然）；CG 只写中立对、文字/影片无写点、Live2D 被状态块还原且只在命令档之后。新增 `RES-SCENE-BLEND-MESH-TIER`。

## 实现

- `machine.ts`：`ScriptTables`（`msg/call/sub` 位置数组）、`indexInTable`（`indexOf`）、`ScriptFrame.tables`、`MachineOptions.rootTables|scriptTables`。
- `runner.ts`：`tablesFromHeader(header, body)` 读表体；根/子脚本接线。
- `handlers.ts`：`rec[0]=parent`、`rec[259]/[260]` 用下标、当前帧强制 −1。
- 测试：新增 `emulator-new-save-tables.test.mjs`（6 pure）+ `emulator-new-save-tables.assets.test.mjs`（真实脚本 2/2）；变异 +1。

## 仍开着

父帧 PC 恢复路径 · `0xAE` 触发时机 · 老格式表 3 未初始化 · `RES-SCENE-BLEND-MESH-TIER` · 真实绘制 S2+。

## 判据

`pnpm test` 1015/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 333/333 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §33 · `tools/test/emulator-new-save-tables*.test.mjs`。
