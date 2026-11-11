# emulator-lift · 交互任务：身份电平条件 script、LIFT_LOG_MARKERS、线性 await 语法

- id: REQ-01M74Z3DBK8K5MQVHZPCEX2SXP
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/interaction_graph.cpp#interaction-graph
- tags: [emulator-lift, interaction-graph]

## 一句话

把"等一屏 → 点一下"变成**可自足写出**的两原语任务：身份**电平**条件 `script <名字|id>`、身份边沿诊断 `LIFT_LOG_MARKERS=1`、线性语法 `await <cond>` + 动作行（解析期展开成现有 `Node` IR）。

## 为什么

现在 `at marker <名字>` 是**边沿**触发（进入才发事件、首个样本只播种）⇒ "等待到 TITLE"在**已经位于 TITLE**时永远不成立；且边沿日志只在"等的名字恰好匹配"时才打（`input_script.cpp:1059`）⇒ **名字没有发现路径**。结果是：知道路径的人写不出脚本，只能让模型代跑。名字与 id 的真源已有：`data/routes/screens.json` 的 `arrive`（`TITLE.BIN`=21092 / `GAMESTART.BIN`=21099 / `INITGAME.BIN`=20964 / `SC0000.BIN`=115 / `SN0000.BIN`=116）。

## 判据（实现即契约）

1. `script <文本>` 解析：能解析成 `u32`（十进制 / `0x`）⇒ **按 id** 判；否则按名字（大小写不敏感；**允许省略 `.BIN` 后缀**，即 `TITLE` 命中 `TITLE.BIN`；`id:0x%08X` 退化名始终接受）。
   ★ 与时间线 `at marker` 的关系：**同一份名字表与退化名规则**，但 `script` **更宽松**（允许省 `.BIN`）——`at marker TITLE` 今天**不**匹配 `TITLE.BIN`。文档必须写这条差异，⛔ 不许说成"完全同一套"。
2. **电平语义**：`holds = sample.valid && (by_id ? sample.id == id : 名字命中)`；在**块边界**判定、读的是最近一次 VM 循环头采样（`0x4123F0` / `0x4123E5`）。⛔ 不改 `at marker` 的边沿语义。
3. 采样门控：`Machine` 的 marker 钩子除了 `input_script_.needsIdentitySamples()`，还要覆盖"交互图里有 `script` 条件"与 `LIFT_LOG_MARKERS`（否则条件恒不成立 / 日志为空）。
4. `LIFT_LOG_MARKERS=1`（存在即开，与 `LIFT_FRAME_TRACE` 同口径）：每次身份变化打一行 `[marker] frame=… insn=… cur=… id=0x… name=…`（`name_degraded` 要标出）。
5. 线性语法（同一份文件里可与 `node` 块混用）：
   ```
   await script TITLE
   click 1174 377
   await script GAMESTART
   click 715 576
   await script SN0000
   click 100 100
   ```
   - `await <cond>` 开一个 step；其后的**第一个**动作行属于该 step（没有 `await` 的动作 = `always` step）。
   - ⚠ **连续两个动作行会各自成为 step**（第二个 = 新的 `always` step）：要连点就写两行（这是有意的，文档要写清，⛔ 不要当成解析错误）。
   - 隐式 `next` = 下一个 step；最后一个 step 的 `next` 为空（任务结束）。
   - step id = `step1` / `step2` …（稳定、进 trace）；线性模式下不写 `entry` 时 entry = 第一个 step。
   - 每 step 可选 `timeout <ms>` / `expect <cond>`（作用域 = 当前 step）；缺省 timeout 沿用 `kDefaultNodeTimeoutMs`。
   - ⛔ 不默认 `expect screen changed`（线性程序里，"下一个 await"就是期望；默认加会让 no-op 动作误判）。
6. trace / 失败报告：`script` 条件失败时，报告里要写**最后一次身份**（`cur` / `id` / `name`）。

## 守卫

- pure（`tests/interaction_graph.cpp`）：线性 3 步解析（id/next/entry/timeout/expect）；`script` 的三种写法（名字 / 带后缀 / 数字 id）与非法输入；`condHolds` 用**假身份**注入（valid / invalid / 名字命中 / id 命中 / `id:0x…` 降级命中 / 不命中）。
- assets：`await script TITLE` + `quit`（`--skip-logo` + headless）⇒ exit 0、trace 出现 `step1`；`LIFT_LOG_MARKERS=1` 跑一次 ⇒ 至少一行 `[marker]` 且 `name=` 非空。

## 落点

`runtime/interaction_graph.{h,cpp}`（条件 + 线性解析）、`runtime/machine.cpp`（采样门控 + 传身份 + 日志）、`docs/03-emulator/interaction-model.md`（§1 条件表 + 线性形态、§6 把"`script` 未实现"改掉、§7 分期）、`apps/emulator-lift/README.md` §4.2d 一行指向 + 诊断环境变量列表。

## 未做

- 不做 `include` / `route` / `call` 宏（下一单）。
- 区域 hash 条件（`screen rect … hash`）本单不做。
- 不改 `--interaction-graph` 与 `--input-script` 的互斥。
