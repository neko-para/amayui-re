# @amayui/age-format — AGE 二进制格式

## 1. 职责

AGE 引擎的容器 / 资源格式读写：**ALF / AGF / ASM / UIMAP** 等（解析、组装、校验）。

## 2. 来源（旧仓，只登记不迁移）

旧仓 `scripts/{alf,agf,asm,uimap}/`（登记为 `corpus/assets.json` 的 `tooling/format-scripts`）。

## 3. 本轮状态

只有最小 `package.json`（`type: module`）+ 本 README，**没有代码**。

## 4. 语言口径

本包**用 `.mjs`**（纯 JS，无构建步骤）—— 只有 `apps/emulator` 用 TypeScript，见 `AGENTS.md` §3。

## 5. 迁移批次

**M2**：搬 `scripts/{asm,alf,agf,uimap}` 的**能力**（按新结构重写，不是逐文件搬运）+ 其守卫。
前置：M1（反汇编语料入位）。

## 6. 纪律

* 格式层只做**容器**：不含任何游戏语义结论（字段含义 / 脚本角色…）——那些属于待重建的知识层；
* 解析器**不得改写原始素材**：任何"解析友好化"只允许是派生的内存视图（见 `AGENTS.md` §1 第 5 条）。
