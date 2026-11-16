# ADR · 维护依赖边界：输入语法收敛（B）、解释器保留、--interaction-graph 先补判据再退役

- id: REQ-01M7HP2Q1N0N004M8R4RPABWSV
- type: decision
- status: done
- done_reason: 裁决 B（语法收敛 + 执行器保留）。理由：废弃 --interaction-graph 只断 2 条 CTest（都走顺序脚本，可机械改写成配方），而废弃解释器要动 13 条（10 条依赖解释器独占开关，6 条 LIFT_STRESS 全传 --native-overrides all，而该面已按 §12.8 从 AOT 删除 ⇒ 今天搬不动，与输入模型无关）。前置条件：配方未走完必须非 0 退出（今天 aot_main.cpp:613 只警告），否则图→配方迁移会把 CTest 变成假绿。InputScript 不退役，降级为解释器臂动作落点。不决定：--native-overrides 是否搬回 AOT。
- tags: [emulator-lift, boundary, input]

## 决策

维护依赖边界 = **B「语法收敛 + 执行器保留」**：

1. **输入只有一份语法**（`runtime/recipe.*` 的解析器 + step 模型），两个入口（`--input-plan` / `--interaction-graph`），推进点仍各自不同。
2. **`--interaction-graph` 退役**，但**不是第一步**：先做下面的前置，再退役。
3. ★ **前置（必须先做）**：**配方"走完数"成为判据** —— 配方没走完 ⇒ **非 0 退出**。
   今天 `src/aot/aot_main.cpp:613` 只是 stderr 警告、不改退出码。没有这条，任何"图 → 配方"改写都会把 CTest
   从"会红"退化成"**假绿**"（两条图驱动 CTest 的判据都会变弱；`stress-adv-audio` 的判据明写"驱动≠判据"，
   配方没走完时帧时间读数会漂成 boot+TITLE 空转，**更容易绿**）。
4. **`InputScript` 不退役**：降级为"解释器臂的动作落点"。
5. **解释器臂保留**：新功能只进共享 runtime / AOT；解释器只保留"有 CTest 或取证需要"的面。

## 事实（评估读数，`.tmp/boundary/assess.md`）

| 读数 | 值 |
|---|---|
| 驱动解释器的 CTest | **13** 条（其中 6 条 `LIFT_STRESS`、7 条 pure） |
| 其中依赖解释器**独占开关** | **10** 条；开关面上可搬的只有 **3** 条 |
| 依赖 `--interaction-graph` 的 CTest | **2** 条（`feeder-seg-e2e` / `stress-adv-audio`） |
| 解释器独占开关 | **34** 个（AOT 24 个里 21 共有、3 独有） |

★ 关键：6 条 `LIFT_STRESS` **全部**传 `--native-overrides all`，而 `AGENTS.md` §12.8 已把该面从 AOT **整条删除**
⇒ 这 6 条今天**不可能搬到 AOT**，且**与输入模型无关** ⇒ "废弃 graph" 与 "废弃解释器" 代价差一个数量级。

## 本 ADR 不决定（留给后续单 / 用户）

- ★ **要不要继续留一个"能用 native override 的臂"**（今天 = 解释器臂）。
  ⚠ 措辞纠正（2026-11-16 用户指出）：**不存在"把 override 搬回 AOT"这回事** —— AOT 的执行源只有
  lifted IR + shim（`AGENTS.md` §12.8 第 1 条），而 override = 用宿主 C++ 顶替某个 EA 的执行
  ⇒ 在 AOT 上就是被明令禁止的**第三个语义实现**。想改 AOT 某个 EA 的行为，杠杆在**构建期**（改闭包 / 改 IR / 写 shim）。
  ⇒ 那 6 条 `LIFT_STRESS`（全传 `--native-overrides all`）**结构性解释器专有**，只有三条出路：
  **留**（解释器冻结、继续产带 override 的读数）· **另写**（给 AOT 写新的压力测试，判据/阈值重定，不是搬运）· **退役**（随解释器一起退）。
- 解释器臂的最终删除。

## 后果

- 3 张 `scripts/interaction/room-*.graph`（`node` 语法）随 graph 退役一并删除，并同步改 3 处 `repo:` 锚。
- 6 张 `graphs/*.graph` 重写为配方语法后，内容本身即可转成 `.plan`。
