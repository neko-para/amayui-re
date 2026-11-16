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

> "这里历史的 bench（解释器侧）并不需要了？如果真的需要测试也是重新写，为什么说需要按旧的测试补充输入能力？"

⇒ **旧 bench 不保留**：`pnpm tools lift bench` + `scripts/bench/*.txt` + 它拼的 `--input-script` 与**被测的解释器臂**一起死。
⇒ 性能测量只在 AOT 侧**重写**（`REQ-01M7HQJEEENYDJ91FB5DAC4K23`），⛔ **不为旧 bench 给配方补 `at frame/insn/marker`**。
⇒ `--input-script` / `InputScript` **可以删**（含只测它的用例），前提是保留的用例（`input-expect` / `profile-window` / `screen_sample`）不依赖它；依赖就报告最小保留面。

## 落地实测（2026-11-16，`--input-script` 那一轮）

- **退役 = 8 个时间线参数整族**（`--input-script` / `--anchor-script` / `--input-stall-ms` / `--input-stall-boot-ms` /
  `--input-max-hold` / `--input-marker-timeout-ms` / `--expect-change-after` / `--expect-change-window`）⇒ 给了就是"已退役"+exit 2。
  理由（已认可）：它们共用**同一条时间线**，只留一半会留下半死面。
- **`InputScript` 类保留最小面**：9 处结构性依赖 —— `profile-window`（窗口边界=脚本动作）· `input-expect`（扫脚本动作）·
  `InteractionGraph::actuator`（**配方动作经 `appendText` 落地**，自动 hold / 延迟抬起 / quit 唤醒 / profiler 请求只有这一份）·
  `input-tick` / `injection` / `unfinished` / `marker` / `anchor` / `insn-cost` / `messagebox`（用 `loadInputScriptText` 造确定性时间锚）。
  ⇒ 它从"输入驱动"变成"**可测试的动作落地**"，无 CLI 生产者。
- **CTest 102 → 102**（退役面里没有一条 CTest 只测 CLI；旧 bench 只有 `add_executable` 没有 `add_test`）。
  node toolchain 档 **501 → 491**（删了 `tools/test/lift-bench.test.mjs` 的 10 个 `test()`）。
- 若将来连机器内机械也删，会失效的锚：`REQ-01M710E6PV4PPHECPPCCTRF5RA` · `REQ-01M72QSNQCH3GHH3158A7YX96V`（`REQ-01M701PEB38VDQJD4QR2JEBA74` 的 profile_window 不要删）。
