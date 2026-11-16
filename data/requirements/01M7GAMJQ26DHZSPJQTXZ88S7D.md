# 同一输入两臂结果不同：TITLE 上 click 1070,480 在 AOT 生效、在解释器无反应

- id: REQ-01M7GAMJQ26DHZSPJQTXZ88S7D
- type: bug
- status: dropped
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: docs/03-emulator/defects.md#BUG-26
- severity: S2
- dropped_reason: 不是缺陷：这是**机制未统一**，不是同一实现下的行为分歧。实测解释器根本不接 --input-plan（未知参数 + exit 2；setInputPlan() 全仓只有 aot_main.cpp:484-485 两处调用），所以「两臂同一输入」今天不可执行；原来的对比是 AOT 的 --input-plan 与解释器的 --input-script/--interaction-graph 两条不同施加路径。观测本身（TITLE 上 click 1070 480 在 AOT 生效、在解释器无反应）保留在 docs/03-emulator/defects.md#BUG-26 与 input-plan.md §6。要让这条重新成立：先给解释器接同一条 deliver* 路径（另单）。
- tags: [emulator-lift, input, divergence]

## 现象（2026-11-16 探测时撞到）

**同一个输入（TITLE 屏上 `click 1070 480`）在两条臂上给出不同结果。**

| 臂 | 现象 |
|---|---|
| **AOT** | `region`（in_use / surfaces / heap_blocks）从基准 `108595152 / 21 / 6674` 变成 **`117490928 / 25 / 6821`**（+8.9 MB、**+4 surfaces**），两次 run 逐位相同 |
| **解释器** | 同一份计划"1 条动作全部应用"，但 `arena` / `surfaces` 与**无动作逐位相同**；`engine_marker` 停在 `TITLE.BIN`、**无新 identity**（`frame=` 全是 0） |

⇒ 要么解释器没把那次点击真的送到 guest，要么 AOT 那次点击落在了不同的 guest 时间点上。

## 两个候选原因（都未定，按此顺序查）

1. **时间步长**：两臂的 guest 时间步长差 ~**15 倍**（探测时观察到的量级）⇒ 同一条 `await presents N` 在解释器里
   对应的 guest 时刻与 AOT 差很远。
2. **解释器侧 `bounded-no-sample=1`**：它可能让 `region` / marker 在实际渲染前就收口 ⇒ 读数看不到变化。

★ 还有第三种可能，必须一起排除：**AOT 的 `--input-plan` 与解释器的 `--input-script` / `--interaction-graph`
走的是两条不同的施加路径**（AOT 是本轮新接的 `deliver*`，解释器是老的 `InputScript::tick`）⇒
如果两条路径对"同一动作"的语义不同（例如按下与抬起的时机、坐标缩放、客户区 vs 屏幕坐标），那就是**实现分歧**，
比时间步长严重得多。

## 判据（收口时）

一条命令，两臂都跑，**必须给出同一个机械读数**：

```
# 两臂各跑一次（关键：都要 --skip-logo、都要全新空 --appdata、都要 --region-log）
emulator-lift    --skip-logo --present window --max-frames 400 --appdata <新> --region-log <同一份输入>
emulator-lift-aot --skip-logo --present window --max-frames 400 --appdata <新> --region-log --input-plan <同一份输入>
```
- **最小可判**：同一次点击后，两臂的 `region` 三数**要么都变、要么都不变**。
  只有一侧变 ⇒ 分歧成立。
- 若定位到"施加路径不同" ⇒ 让两臂共用同一条路径（`deliver*`），并把老的 `InputScript::tick` 逐步退役。
- ⛔ 不许用"画面看起来一样"当判据；⛔ 不许改弱读数口径。

## 为什么它可能比配方本身更重要

如果两臂对同一输入给出不同结果，那么**所有"解释器能跑通 / AOT 跑不通"的结论都可能是输入路径差异而不是引擎差异**。
这条不查清，后面每一个"某处 AOT 表现不符"都要重打一遍折扣。

## 附带（本单不管，但记下）

- `--input-plan` **表达不了"同一份计划去掉动作"**：只有门没有动作 ⇒ 解析报 `线性 step step1 缺动作行`、exit 2。
  对照只能用"不给 plan"或"惰性点击"两档。若将来要更干净的对照，给 plan 加一个 `noop` 动作。
- `ESC`（`VK 0x1B`）在这条路径上**无区分力**：两侧落同一 `EA 00513B90` / 同一条 `agerc!_ShowDialog`（返回 IDOK = 退出程序）。
