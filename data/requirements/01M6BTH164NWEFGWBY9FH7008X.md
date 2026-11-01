# 批 S1r · 按需补齐：shape need <T>[@<off>] 一条命令给出 载体候选 / 未声明偏移 / 可粘贴 claim 草稿

- id: REQ-01M6BTH164NWEFGWBY9FH7008X
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 54
- verify: tools/test/shape-need.test.mjs#★need-01
- tags: [engine shape tooling]

## 一句话

把"用到时补一格"变成一条命令：给定类型（可带 `@off`），打印
①已有载体 · ②传播已推断、尚无载体的候选（带**调用点 EA**，可直接当 G16 witness）·
③未声明偏移 × 触碰站点 EA × 观测宽度 · ④**可粘贴的 claim 草稿**。
⛔ 本域只读：草稿打到 stdout，不落 `data/shape/`。

## 为什么先做这条（评估结论）

批处理式扩充对"结构初稿"的边际收益低：S1l（跳表）是**闭合度指标**，做完**不新增任何站点 / 载体 / 成员**
（站点逐指令抽；类型传播只走直接 `call`）。而 `T_FadeTimer` 实测只需 **3 条载体 + 7 条成员**，
一次 `claims` 就落位 —— 缺的不是"再扫一遍"，是把 `--by-type`（缺哪些偏移）与 `propagate.next`
（该由谁当载体）**合成一条命令**并给出草稿。

## 判据（做完的样子）

1. `pnpm tools shape need <T>[@<off>]` 能跑：列出已有载体 / 候选（带 `viaEa`）/ 未声明偏移（EA + 宽度）/ 草稿。
2. 草稿满足：`carriers[].witness` 用**调用点 EA**；`members` 只含**未声明**偏移（落在已声明窗口里的不许出现）；
   每条成员带观测宽度（宽度是**观测值**，⛔ 不是判断）。
3. 给定 `@off` 时只报该偏移（仍列载体）；该偏移**已声明** ⇒ 明说"落在成员 `<off>` 的窗口里"，而不是给空草稿。
4. 守卫：`pure`（合成 types/carriers/sites/inferred：候选排除已声明、成员排除窗口内、`@off` 过滤、空输入）+
   `assets`（动态取一个零成员类型：草稿非空、且成员集 == 未声明偏移集）+ 变异一条。
5. ⛔ 不写盘、不猜类型、不自动应用；粘贴之后由 `shape claims` 判（G1/G2/G4/G8/G16 是最后一道闸）。

## 不做什么

- ⛔ 不把草稿写进 `data/shape/`（本域没有任何写操作）；⛔ 不按偏移命中率猜类型。
- ⛔ 不为了"补得更全"放宽 G1 / G4 / G8。

## 相邻单

- 缺口清单（本单落地后按"哪条 spec 真的要"逐条补）→ `REQ-01M6AKCBTRNBZQNERZGPT8VMPD`。
- 暂缓：S1l（闭合度 / 消费者缺失）→ `REQ-01M57VNC7QD83CVADYT0ERFA6G`（已降为 open）。
