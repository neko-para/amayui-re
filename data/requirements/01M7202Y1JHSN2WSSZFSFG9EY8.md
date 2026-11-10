# emulator-lift · 交互图解析器 table 化：新增一个键只改一处（今天 parseCond/parseAction/主循环三处 if）

- id: REQ-01M7202Y1JHSN2WSSZFSFG9EY8
- type: req
- status: open
- parent: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- tags: [emulator-lift, interaction, refactor]

## 为什么要它

`repo:docs/03-emulator/design-notes.md` 的 `L3-graphfmt-01` 结论：**换格式省不下维护**（只删得掉 ~110 行词法，
还丢语义错的行号）。真正的杠杆是把 `loadText` 里那套**手写分支**改成**表驱动** —— 今天的形态是
`parseCond` 一组 if、`parseAction` 一组 if、主循环又一组 if：新增一个键要动 3 处 + 补 3 类用例
（实测 `e4a5457` 为加一个按钮词动了 7 个文件、+292/−30，其中解析器只占 41 行、测试占 194 行）。

## 判据（做完后）

1. **新增一个条件 / 动作键只改一处**：键名 →（参数 schema、构造器、touched 字段）写成**一张表**；
   主循环按表驱动，⛔ 不是三处 if。
2. **13 条 `origin:行:` 语义错断言一条不少**（`repo:apps/emulator-lift/tests/interaction_graph.cpp` 的 `parse-errors`）：
   未知键 / 重复键 / `next` 不存在 / `click center` 无框 / `key` 越界 …… 全都要**带行号**响亮报错。
3. **19 条 pure 用例全绿**，且 3 份真图（`repo:apps/emulator-lift/scripts/interaction/*.graph`）解析出的节点与动作
   **逐字段相同**（`graph-scripts-parse` 覆盖）。
4. 变异：把表里某个键的参数校验删掉 ⇒ 对应那条 `parse-errors` 断言**红**（证明表还在起作用，不是摆设）。

## 不做（边界）

- ⛔ 不改文件格式、不改 `interaction-model.md` 的 schema、不碰 trace 格式。
- ⛔ 不为了"少写几行"放宽闭集合：未知键仍然要**响亮**报错并带行号（`AGENTS.md` §10：不许静默）。
- ⛔ 不顺手加 JSON 入口（那是生成器落地时的事，见 `L3-graphfmt-01` 的重开条件）。
