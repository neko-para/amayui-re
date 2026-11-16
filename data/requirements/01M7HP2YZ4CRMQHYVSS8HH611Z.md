# 配方完成度成判据：未走完的 step ⇒ 非 0 退出（--interaction-graph 退役的前置）

- id: REQ-01M7HP2YZ4CRMQHYVSS8HH611Z
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: apps/emulator-lift/CMakeLists.txt#input-plan-read-points
- done_reason: 配方完成度成判据落地：pending != 0 或某步 timeout 到期 ⇒ exit 5 + 点名（FAIL incomplete / FAIL timeout + step 名与行号）；顺带把两臂能力面收成一份（RecipeFeatures/kPlanFeatures/kGraphFeatures 整块删除，'这个入口不认'的解析期硬错删净）。读数：反例 await position TITLE 0x388C 0x38A4 + click ⇒ 改前 exit 0/pending 缺 ⇒ 改后 exit 5 + pending=2 + 点名 step1（父级复跑确认）；正例 [0x0460,0x0520) ⇒ exit 0 + fired=1 + pending=0 + region 117490928/25/6821；超时例 ⇒ exit 5 + '守卫 2000 ms…已等 2001 ms ⇒ 到期'；--input-plan 现在能直接吃 graphs/sn0000.graph（含 timeout/profile_*）；ctest 102/102、pnpm test 552、verify 501、validate 6/6；变异 1/1 红。三处取舍见正文（quit 靠 postQuit+stop_requested、timeout 缺省按入口、profile_* 落成墙钟区间）。
- tags: [emulator-lift, input, recipe, exit-code]

## 一句话

让**配方的"走完数"成为判据**：配方里还有没走完的 step（门没满足 / 超时）⇒ 进程**非 0 退出**，而不是只打一行 stderr 警告。

## 为什么（ADR 前置，`REQ-01M7HP2Q1N0N004M8R4RPABWSV`）

- 这是"`--interaction-graph` 退役"的**前置**：图今天有每步 `timeout` + `exit 5`（任务没走完 ⇒ 响亮失败，`src/main.cpp:1312/1334`）；
  配方只有 `fired=` 计数（`src/aot/aot_main.cpp:613` 仅 stderr 警告）。
- 没有它，图 → 配方的改写会让两条图驱动 CTest **从"会红"退化成"假绿"**：
  - `feeder-seg-e2e`：配方没走完 ⇒ 只是没触发那条路径，`bov` 读数照样能取到；
  - `stress-adv-audio`：它的判据明写"驱动≠判据"，配方没走完时帧时间读数漂成 boot+TITLE 空转 ⇒ **更容易绿**。

## 要做的（最小）

1. 配方跑完时给一个**完成度**读数（`steps=N fired=M pending=K`）。
2. `pending != 0`（或任一步 timeout）⇒ **退出码非 0**（沿用图那套的 5，或另定，需与既有 `exit-code` 用例对齐）。
3. 两条图驱动 CTest 改成配方后，**必须仍能红**：把"故意不满足的门"当作变异/反例用例（`pending != 0` ⇒ 非 0 退出）。

## 判据

- 单测/CLI 用例：门永不满足 ⇒ 非 0 退出 + 点名哪一步没走完。
- 反例：`await position TITLE 0x388C 0x38A4`（永不执行的区间）+ click ⇒ 非 0 退出（今天 `fired=0` 但 exit 0）。
- 两条图驱动 CTest 在改写成配方后**仍能红**（给出可红的证据，不是"只贴绿"）。

## 未做

不在这里改图、不退役 `--interaction-graph`（那是下一步）。

## 取舍（2026-11-16，实现后确认；已写进代码注释）

1. **`quit` 不能只投 `WM_QUIT`**：实测 guest 的停机路径**在闭包外**（WM_QUIT 之后停在 `00513B90` ⇒ decode-error / exit 2）
   ⇒ 落点 = `postQuit(0)`（与解释器臂同一交付点）+ `AotRunOptions::stop_requested`（下一个块边界 `Stopped`，与 `machine.cpp:1270` 同口径）。
   ★ 这条同时说明**闭包缺 00513B90**（退出对话框路径）—— 已另开单。
2. **`timeout` 的缺省按入口不同**（解释器 5000 ms / AOT 无缺省）：宽度与到期公式只有一份，差别只在入口传的 default。
   理由 = AOT 只有一把墙钟尺，给 5 s 会把正例打成超时（判据 2 会红）。⛔ **显式 `timeout` 两臂语义相同**。
3. **`profile_*` 在 AOT 没有分项 profiler**（`WindowProfiler` 依赖 BlockCache / ShimRegistry / 派发循环）⇒ 落成宿主墙钟区间读数
   （begin/end 各一行），输出里明说"不是分项"。
