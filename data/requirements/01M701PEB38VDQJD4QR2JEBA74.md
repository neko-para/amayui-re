# 修区间 profiler 的运行期 arm：图脚本经 appendText 追加的 profile_begin/end 不重新 arm ⇒ 静默全 0 残表

- id: REQ-01M701PEB38VDQJD4QR2JEBA74
- type: bug
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: apps/emulator-lift/src/runtime/machine.cpp#window_prof_.setArmed
- severity: S3
- tags: [emulator-lift, tooling, profiler]

## 判据（存在性 + 观测）

- 现象链（代码证据）：`repo:apps/emulator-lift/src/runtime/interaction_graph.cpp`（`action profile_begin/end` 的追加处在 810–821 行） 把图里的
  `action profile_begin/end <label>` 经 `actuator_.appendText(...)` **在运行期**追加进 `InputScript`；
  而 `repo:apps/emulator-lift/src/runtime/machine.cpp` 的 `prepare()` 在 **`prepare()` 时**只按
  `window_prof_.setArmed(input_script_.hasProfileActions())` arm 一次。
  ⇒ 用 `--interaction-graph` 跑带 `profile_begin/end` 的图：窗口能开、能收，但 `armed_` 仍是 false
  ⇒ `tests` / `interrupts` / `function` 表 / `interp` 全 0 的**静默残表**（不报错、不警告）。
- 与时间线脚本的差别：`--input-script` 的 `profile_begin` 在 parse 期就在脚本里 ⇒ `hasProfileActions()` 为真，正常。
- 与 `REQ-01M6XVYY5TCKSAS80WS7PADF8P` 的关系：该单的 trace 只记 `profile_begin` 行，不校验表非空 ⇒ 不暴露本缺陷。

## 修法边界（给将来的 agent）

- 最小修法：在 `InputScript` 消费 `ProfileBegin` 的那一刻（或 `appendText` 解析完成后）把窗口 profiler **重新 arm**
  （`machine` 侧提供一个 `armWindowProfile()`；⛔ 不要在每次 tick 检查，那会加无谓分支）。
- 或：`appendText` 返回"是否含 profile 动作"，调用方据此 arm。两条都行，**选一条并在注释里写明"运行期追加"的原因**。
- ⛔ 不许把"表全 0"变成静默：至少要在窗口收尾时检查 armed，未 arm ⇒ 打一行警告（响亮失败优先，但这属于工具面，警告即可）。
- 守卫（必须能红）：pure CTest —— 用合成输入走"**运行期 appendText** 追加 `profile_begin/end`"的路径，
  断言窗口表的 `insns` / shim 计数**非 0**（修前为 0 ⇒ 红）；变异 = 去掉重新 arm 那一行。
  现有 `repo:apps/emulator-lift/tests/profile_window.cpp` 是合适落点。

## 与别的单的关系

- 发现于 `REQ-01M6XVZE3AN0FKFZA52VV23WD9`（分析工具与测量可信度）的只读设计；本单只修"运行期追加不 arm"这一条。
- 与 `REQ-01M6XVZ8AXJNG492WA2QAMTA35`（装载帧性能）相邻：C 单的窗口若用图脚本插入，就会命中本缺陷。
