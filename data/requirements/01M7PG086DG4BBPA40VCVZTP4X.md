# 工具 · AOT fault 诊断（EIP/寄存器/栈）+ --input-plan-trace + fault 摘要

- id: REQ-01M7PG086DG4BBPA40VCVZTP4X
- type: req
- status: done
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- verify: apps/emulator-lift/tests/fault_report.cpp#aot-fault-site-and-plan-summary
- done_reason: A/B/C 落地（真实 SAVE04 fault 打 eip/lift_cc/8 GPR/32 字栈/input-plan 摘要；trace 开档有行关档零行；fault 摘要含 steps/fired/pending）；D 前提被推翻：click 被消费，卡点是配方门，未改 release 语义；配方 scripts/recipes/save04-fault.plan ⇒ exit 3。ctest 113/113、pnpm test 574、pnpm verify 500/0/2 skip、mutation 两条实测红。
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
4. **click 差异**：给出根因或明确判为“不是缺陷”的读数；若修，SAVE04 配方能走完
   （`pending=0`）并到达 fault。

## 收口（2026-11-18）

- A/B/C 均已落地：真实 SAVE04 fault 打出 `eip=004A50C0` / `lift_cc_004A50C0` / 8 GPR+eflags /
  32 字栈 + ebp 链 + `input-plan` 摘要。
- D **前提被推翻**：click 被消费（trace / 无 click 对照），emu-ctl 同形状；卡点是配方门
  `SC4000 0x5900`（只有点了才会到）⇒ 改为 `[0x5800,0x5860)`。`releaseDue` / `sampled` 不是根因，
  **未改 release 语义**。配方 `apps/emulator-lift/scripts/recipes/save04-fault.plan` ⇒ exit 3。
- 门禁：ctest 113/113（5 skip 与基线同组）· `pnpm test` 574 · `pnpm verify` 500/0/2 skip ·
  `requirements validate` 6/6 · 两条 mutation 实测红。

## 边界

- 诊断只读：⛔ 不改 guest 可见语义；⛔ 不为诊断加周期性默认输出。
- 落点：`src/runtime/fault_state.*` / `src/runtime/memory.*` / `src/runtime/input_plan.*` / `src/aot/*` /
  `tests/*`；⛔ 不动 `saves/`、`corpus/`、`data/**`。
- 先不做的：`emu-ctl` 的 down/up 分离原语 · lldb attach（沙箱拒绝）· BUG-32 根因修复本身。

## 未做

- 修复 BUG-32；win32 对照；`CLI` 的 `--input-plan-trace` 在 POSIX 上的端到端（`cli-aot-flags` 本机 skip）。
