# 需求台账预算口径漂移：README §6 与技能文档写「活节点 ≤ 40」，真源是 ≤ 100

- id: REQ-01M6WWA1K218GV57TB0N3PXTTP
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- tags: [requirements docs]

## 现状

- `repo:data/requirements/README.md` §6 原写"活节点 ≤ 40" ⇒ **本轮已就地订正**为 ≤ 100，并把真源指向
  `pnpm tools requirements describe`。
- `repo:.agents/skills/amayui-requirements/SKILL.md` **仍有两处**写 ≤ 40（`plan` 那行命令注释、预算那条不变量）——
  **未改**：本轮写权限只到 `data/requirements/**`。
- `repo:apps/workbench/README.md` 还有一处转述"≤40 活节点" —— 同样不在本轮写权限内。

真源：`pnpm tools requirements describe` 的不变量 #5 与模型里的 `BUDGET`（单节点行数上限 / 活节点上限）。
`validate` 的 #5 与 `describe` 共用**同一份措辞**（`BUDGET_TEXT`）⇒ **工具侧不会漂**，会漂的只有转述它的文档。

## 判据

1. `rg '≤\s*40' .agents/skills/amayui-requirements/SKILL.md` 为空，且预算那句**指向** `describe`
   而不是自己复述数字。
2. `rg '≤\s*40' data/requirements/README.md` 为空（已满足）。
3. `rg '活节点.{0,16}40'` 全仓只剩历史记录（本单 / `git log`），没有第二份"当前口径"。
4. ⛔ 判据**不是**"这次数字对上了"：转述本身就是漂移源。这两处文档今后应只指向 `describe`，
   把数字写在工具里一处。

## 为什么现在不做

`.agents/skills/**` 与 `apps/workbench/README.md` 都是**只读**（本轮 subagent 的写权限被限定在
`data/requirements/**`）。改技能文档是环境级动作，要父级 / 用户那一层去落；台账 README 那半已经修好。

## 现在绕到了哪里

- 读 `data/requirements/README.md` 的人现在看到 ≤ 100，会去 `describe` 复核。
- 读技能文档 / 工作台 README 的人仍看到 ≤ 40 ⇒ 按**错的预算自我限制**：活节点到 80 出头就开始担心撞墙，
  或者反过来以为"还能再开 40 个"。这正是本单要消掉的那条缝。
- 为什么当时会漂：数字被抄进了三份散文，而真源在一次模型改动里从 40 变成 100（工具侧同步改了，散文没改）。

## 边界

- ⛔ 不改 `BUDGET` 的取值（本单只治"文档转述"，不讨论预算该多大）。
- ⛔ 不往 README / 技能里写"当前活节点 N"这类会过时的计数（那是 `pnpm tools requirements plan` 的回答）。
