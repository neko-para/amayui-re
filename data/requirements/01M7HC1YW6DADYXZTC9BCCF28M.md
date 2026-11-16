# 控制层 CLI（停/继续 · sample 当前执行点 · 截图 · 投递）评估与设计；DSH tool 后续薄包装

- id: REQ-01M7HC1YW6DADYXZTC9BCCF28M
- type: spike
- status: doing
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- tags: [emulator-lift, control-plane, design]

## 一句话

评估并设计**独立的控制客户端进程**（停住 / `sample` 脚本位置 / 截图 / 投递输入 / 继续）+ AOT 侧的端点；
DSH tool 留作**后续薄包装**。产出 = 形态取舍 + 最小改动 + 分阶段计划（每阶段一条能红的判据）。

## 要回答的问题（缺一条就不算结论）

1. **形态与传输**：控制客户端是**独立进程**（用户定）。
   ★ 先给推荐：Node `.mjs`（`tools/emu-ctl.mjs`，符合"仓库工具 = `.mjs`、无构建步骤"）还是 C++ 第二可执行？
   端点：本地 socket / 控制文件+轮询 / 其它；★ 先判定"走子进程 stdin-stdout"在 DSH 沙箱里是否**直接不成立**
   （`AGENTS.md` §5.1：捕获子进程输出=命名管道=EPERM）。需要：命令集、端点发现、同步/异步、超时、多实例、崩溃不留僵尸。
2. **停/继续**：停点只能落在宿主侧汇点（Present / shim 回调内）；每条命令**在哪个点生效**逐条写死
   （"不需要非常精确的时间"⇒ 允许粗粒度，但不许含糊）。暂停期间 `clock=real-time` 对 guest 可见时序的影响；
   ⛔ 怎么保证不变成块边界 pump。
3. **`sample` = 脚本位置**（用户定，⛔ 不是进程/CPU）：返回形如 `{script:"TITLE.BIN", script_id, offset, insn_index?, listing_line?}`，
   offset = **绝对字节偏移、4 字节对齐**（`label_%08x` 同一坐标系，`packages/age-format/src/asm/assemble.mts:16`）。
   ★ "这个数从哪读"由另一个 spike 回答 ⇒ 你只需钉 schema、并在读不到时输出什么（`null` + 原因，⛔ 不许静默给 0）。
4. **动态投递**：`InputPlan` 今天只支持启动前一次性装配 ⇒ 最小改法（外部 step 源 vs 直接调 `input::*`）。
5. **分阶段**：M0 只读控制（停/继续 + `sample` + 截图）→ M1 动态投递 → M2 录制导出 graph，各一条**最小可验证**判据。
6. **DSH tool（后续）**：只给一句"薄包装"形态（按 `plugins/wait-job` 的形状），不展开。

## 判据（本节点收口时）

- 设计文档逐条回答上面 6 问，每条带 `文件:行` 或"未验证"标注。
- 给出 **M0 的判据**：一条能红能绿的命令级断言（例：停住后两次 `sample` 逐位相同；`resume` 后帧号前进）。
- 回答一句：**CLI 形式能不能满足"停住→看→投递→继续"的探索循环**（能/不能 + 为什么）。
- 明确列出**本设计不解决的问题**。

## 未做

实现（M0 起）另开单。DSH tool 不做原型。
