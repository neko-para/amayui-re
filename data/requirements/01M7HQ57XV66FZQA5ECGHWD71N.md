# 解释器边界：冻结为验证臂 · 3 条可搬 AOT · 4 条靠补 --report/--lst · 6 条依赖 --native-overrides 只能退役

- id: REQ-01M7HQ57XV66FZQA5ECGHWD71N
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, boundary, interpreter]

## 一句话

**解释器 = 冻结的验证臂**：不再新增功能，只保留"有 CTest 依赖它"的面；它的**唯一护城河是 `--native-overrides`**（AOT 结构上不该有）。
本单只登记边界与迁移顺序，**不删任何东西**。

## 现状（13 条驱动解释器的 CTest，评估读数）

| 分类 | 条数 | 谁 | 结论 |
|---|---|---|---|
| **可搬 AOT**（不依赖解释器独占开关） | **3** | `lift/no-gpu-refuses-start` · `cli-cursor-warp` · `cli-dump-frame` | 排期搬；判据不变 |
| **靠补诊断面才能搬**（`--report` / `--lst`） | **4** | `cli-time-limit` · `host-encoding` · `exit-code` · `stdout-quiet` | `--report`/`--lst` 与臂无关 ⇒ **按需补到 AOT** 后再搬 |
| **只能退役**（`--native-overrides`） | **6** | `feeder-seg-e2e` · `movie-logo` · `stress-boot` · `stress-title-steady` · `stress-adv-audio` · `stress-gpu-coverage` | ⛔ 不搬、不补（§12.8：AOT 不许有第三个语义实现） |

★ `--native-overrides` 是**结构性解释器专有**：AOT 的执行源只有 lifted IR + shim，override = 用 C++ 顶替某个 EA 的执行 = 第三份语义实现。
★ 那 6 条是"带 override 的压力/性能读数"。

## ★ 裁决（用户 2026-11-16）

> **不需要**那 6 条 override 压力读数 —— "目前新的 aot 版本速度已经远超原来的解释器版本，其测试的目标已经没有了"。

⇒ **解释器进入退役排期**：13 条全部有归属（3 已搬 / 4 靠诊断面搬走 / 6 退役），退完解释器即可删。

## 规则（本单生效）

1. **AOT = 产品臂**：面向用户/自动化的开关只进 AOT；新 CTest 默认挂 AOT，挂不上要写理由。
2. **解释器只做"保绿"**：不新增功能；`overlay` / `profile` / `--dump-frame` 之类**按需再补**（用户口径），今天冻结。
3. **不许给解释器开第二套输入语义**：`--interaction-graph` / `--input-script` 随输入收敛退役（ADR `REQ-01M7HP2Q1N0N004M8R4RPABWSV`）。
4. **两臂比对的现状要如实写**：今天**没有**自动化的两臂比对；它唯一一次真正发挥作用是撞出 AOT 丢 guest 退出码（`REQ-01M7HP2YRZG222E0NE2H6MW6NK`）。⛔ 不要把"可以比对"当成已兑现的收益。

## 判据（每条能红）

1. 搬一条 ⇒ 该 CTest 在 AOT 臂上跑通，且**判据逐条不变**（不是把判据改弱）。
2. 补 `--report` / `--lst` 到 AOT ⇒ 同一份报告/清单内容可用；补完那 4 条能搬。
3. 冻结生效 ⇒ `git grep` 里解释器臂的开关数量**不再增加**（这条只作口径，不建守卫）。
4. 退役前置：`REQ-01M7HP2YZ4CRMQHYVSS8HH611Z`（配方完成度成判据）先绿。

## 退役顺序（每步一条能红判据）

1. **前置**：`REQ-01M7HP2YZ4CRMQHYVSS8HH611Z`（配方未走完 ⇒ exit 5）绿 + `--interaction-graph` 退役。
2. **搬 3 条**（`no-gpu-refuses-start` / `cli-cursor-warp` / `cli-dump-frame`）⇒ 各自在 AOT 上跑通、判据逐条不变。
3. **补 `--report` / `--lst` 到 AOT** ⇒ 搬 4 条 pure（`cli-time-limit` / `host-encoding` / `exit-code` / `stdout-quiet`）。
   ⚠ `exit-code` 先要修 🐞`REQ-01M7HP2YRZG222E0NE2H6MW6NK`（AOT 丢 guest 退出码），否则搬过去必红。
4. **退役 6 条 `LIFT_STRESS`**，并**登记空窗期**：用户口径（2026-11-16）是 AOT 侧压力档"**逐步补充**"
   （boot→TITLE 稳定 · 打开 FELLOW · 加载存档，等输入部分落地后逐条加）⇒ 这一段里端到端速度只能人手跑，
   ⛔ 不许把"没有读数"说成"没有退化"。新档判据见 `REQ-01M7HQJEEENYDJ91FB5DAC4K23`。
5. **删解释器入口 + 独占开关**（`--input-script` / `--overlay` / `--profile(-top)` / `--dump-frame` 等），
   再评估 `Machine` 类本身还有没有别处消费者（测试二进制可能直接用它）⇒ 有就保留、没有才删。

## 未做

本单只登记顺序；**不删任何东西**（第 4 步的"建 AOT 压力档"要单独立单）。

## 用户裁决（2026-11-16，覆盖上面两条细节）

1. **6 份配方保留**，但**挪进子目录**、⛔ 不再散在仓根 ⇒ 落点 `apps/emulator-lift/scripts/recipes/<名>.plan`（内容就是现在的新语法）。
2. **feeder 的覆盖损失不开单**：用户自测（"本身也是性能产生的问题，大概率已经无问题了"）⇒ 退役 `feeder-seg-e2e` 不另挂跟进单。
3. 删除面收敛为：`--interaction-graph` / `--input-script` 的用户面 + 图解析残留 + `scripts/interaction/`（3 张 `room-*`，`node` 语法、无 repro 锚）+ 仓根 `graphs/`。
