# 排版 / reflow 机械化（AGE 脚本文本层）

- id: REQ-01M47SGPFRBWPEQA8AF1WYQ8KP
- type: req
- status: open
- parent: REQ-01M3TCENZ0BCBBFEEDJNMDR75K
- order: 20
- tags: [translation age-format]

## 背景（已定的部分）
旧仓 `scripts/reflow*.js` 的职责是**脚本文本的排版层**规则（实测口径：每视觉行 ≤25 中文字、
专名不许腰斩、标点后不留全角空格、折行刷新等）。`packages/script-dsl` 这个曾用名下的占位包**已删除**：
它无代码、无人依赖，声称的"解析 / 组装"本来就在 `packages/age-format/src/asm/`
⇒ **reflow 的落点是 `packages/age-format`**（见其 README §3.4），不再另造包。

**下游对它的依赖是软的**：翻译域当前接受**手工折行**（`.agents/skills/amayui-translate/SKILL.md` §8 明写
"不要假定 `reflow` / `assemble` / `find-untranslated` 存在"）⇒ 本题**不阻塞**任何一批迁移。

## 判据
（待写实。方向：若裁决"要机械化"，则落 `packages/age-format` 的文本层，
判据须与往返同源 —— reflow 只允许产出**能反解回字节**的文本：`assemble(disassemble(bin))` 逐字节相同。）

## 要裁决什么
1. **要不要机械化**：手工折行是否已够用（现状），还是排版一致性必须由工具保证。
2. 若要做，**规则的真源从哪来**：旧仓 `reflow*.js` 只能当**参考**（工具一律按"重建"处理），
   规则必须重新表述成可再校验的观察（行宽 / 折行点的判据），而**不是**抄旧脚本。
3. 与「旧式注释层」（`REQ-01M3XP1ZV3YF5EYHTK2A3KV4CE`，已 dropped）的边界：
   那一层是页边界 / `// FROM:` / 原文存档块；本题**只管排版**，不重新引入注释层。
