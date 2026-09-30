# @amayui/script-dsl — AGE 脚本解析 / 组装 / reflow

## 1. 职责

AGE 脚本（`.BIN` 里的 VM 脚本）的**解析 / 组装 / 文本 reflow**：
把二进制脚本 ↔ 可编辑文本视图，并保证 round-trip 不丢信息。

## 2. 来源（旧仓，只登记不迁移）

旧仓 `scripts/` 里的 reflow 系列与脚本相关脚本（`tooling/translation-pipeline` 与 `tooling/re` 条目覆盖）。

## 3. 本轮状态

只有最小 `package.json`（`type: module`）+ 本 README，**没有代码**。

## 4. 语言口径

本包**用 `.mjs`**（只有 `apps/emulator` 用 TypeScript）。

## 5. 与知识层的关系

脚本**骨架 / 调用图 / opcode 派发表** 属于知识线的 **A 类（可机械再生成）**：
不是知识 ⇒ 数据丢弃，生成器按"重建"处理（见 `docs/00-origin/knowledge-rebuild.md` §2）。
**不得**把旧仓脚本的输出当结论搬进来。

## 6. 迁移批次

随 M2/M4 一起落地（格式层与模拟器都需要它）。
