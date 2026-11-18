# 工具 · AOT fault 诊断（EIP/寄存器/栈）+ --input-plan-trace + fault 摘要

- id: REQ-01M7PG086DG4BBPA40VCVZTP4X
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, tooling, fault, input-plan]

## 一句话

为 `BUG-32`（SAVE04 → 教程选“否” → guest mem fault）补**只读诊断面**：AOT 臂的 guest fault 报告
（EIP / 寄存器 / 栈 / 块 EA）· `--input-plan-trace` · fault 路径的 `input-plan` 摘要；
并定位 `InputPlan` 的 click 在 `SC0000 0x194FC` 不被消费（同坐标 emu-ctl 可推进）的差异。

## 判据

1. **AOT fault 报告**：在现有 `why/access/region` 之后追加 `eip`、32-bit 寄存器组、32 字栈、
   当前块 EA（`lift_cc_<EA>` 或最近派发块）；默认档**零周期输出**。pure CTest 用合成 fault 夹具钉字段。
2. **`--input-plan-trace`**（opt-in）：每条 step 成立 / 施加时打一行（`文件:行`、`kind`、`script`/`pos`、
   `frame`、`state_seq`）；默认档一次不打印。有 CTest 钉“开档有行、关档零行”。
3. **fault 路径摘要**：`std::_Exit(3)` 之前把 `input-plan: steps=… fired=… pending=…` 写进 fault 文本
   （或紧邻 stderr）；默认档形状不变。
4. **click 差异**：给出根因（最小复现：`await position SC0000 …` + `click 500 500` 不推进，
   emu-ctl 暂停态入队 + `resume` 可推进）或明确判为“不是缺陷”的读数；若修，SAVE04 配方能走完
   （`pending=0`）并到达 fault。

## 边界

- 诊断只读：⛔ 不改 guest 可见语义；⛔ 不为诊断加周期性默认输出。
- 落点：`src/runtime/memory.cpp` / `src/runtime/input_plan.{h,cpp}` / `src/aot/*` / `src/aot/aot_main.cpp` /
  `tests/*`；⛔ 不动 `saves/`、`corpus/`、`data/**`。
- 依赖：M0（overlay）先提交，`aot_main.cpp` 不在两条车道同时改。
- 先不做的：`emu-ctl` 的 down/up 分离原语 · lldb attach（沙箱拒绝）· BUG-32 根因修复本身。

## 未做

- 修复 BUG-32；win32 对照；配方转正为 `repro` / 守卫。
