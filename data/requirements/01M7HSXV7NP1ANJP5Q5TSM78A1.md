# bench 工况驱动缺口：配方语法没有 at frame/insn/marker，pnpm tools lift bench 仍只能靠 --input-script

- id: REQ-01M7HSXV7NP1ANJP5Q5TSM78A1
- type: req
- status: dropped
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- dropped_reason: 不是缺口：旧 bench（pnpm tools lift bench + scripts/bench/*.txt + --input-script）测的是**正在退役的解释器臂**，与被测对象一起死。真要做性能测量 = 按 AOT 侧压力档**重写**（REQ-01M7HQJEEENYDJ91FB5DAC4K23：boot→TITLE 稳定 / 打开 FELLOW / 加载存档，驱动用配方的确定性门）⇒ ⛔ 不需要为旧 bench 给配方补 at frame/insn/marker。用户 2026-11-16 口径：'如果真的需要测试也是重新写'。
- tags: [emulator-lift, bench, input]

## 一句话

`pnpm tools lift bench` 的工况驱动仍靠 `--input-script`（`tools/lift.mjs:1605` 拼这个参数，`scripts/bench/*.txt` 是入库件），
因为它的工况是 **`at frame / insn / marker` 定点**，而配方语法（`runtime/recipe.*`）只有
`await script` / `await position` / `wait <ms>` / `timeout` / `click` / `quit` / `profile_*` —— **表达不了**。

## 影响

- `--input-script` / `InputScript` 因此**不能随 `--interaction-graph` 一起删**（本轮已保留）。
- 但 ADR（`REQ-01M7HP2Q1N0N004M8R4RPABWSV`）把 `InputScript` 定为"解释器臂的动作落点"、并计划在第 5 步随解释器入口一起处理
  ⇒ 到那一步 **bench 会失去驱动方式**。

## 两条路（待裁决）

1. **配方补门**：加 `at frame <n>` / `at insn <n>` / `at marker <name>` 这类**确定性定点门**（与 `await position` 同一层实现，
   ⛔ 不许在循环里 pump）⇒ bench 改用配方，`InputScript` 可以退。
2. **bench 另建驱动**：bench 不依赖 guest 输入的注入语义，直接在 harness 里按 insn/frame 停（类似控制层的暂停点）⇒ 谁都不欠谁。

## 判据（收口时）

- 旧 `--input-script` 工况脚本能用新驱动跑出**同一组读数**（同 insns 区间 / 同帧数 / 同 marker），差在噪声内。
- `tools/lift.mjs` 不再拼 `--input-script`（或明确保留它并写理由）。
- 一条能红的用例：拿一份旧 bench 脚本，在新驱动下必须得到非空读数（⛔ 不是"跑了但什么都没测"）。

## 未做

不实现；`--input-script` 本轮保留不动。
