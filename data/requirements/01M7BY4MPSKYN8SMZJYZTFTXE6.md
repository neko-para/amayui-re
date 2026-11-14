# emulator-lift · 写档 / 读档 / FELLOW 三处 >1s 阻塞段：能否 native override（评估）

- id: REQ-01M7BY4MPSKYN8SMZJYZTFTXE6
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, native-override, profile]

# emulator-lift · 写档 / 读档 / FELLOW 三处 >1s 阻塞段：能否 native override（评估）

读数来源：`OBS-20261114-07`（Windows x64 / RelWithDebInfo / HEAD `aeed0d7` / 单次快照）。
判据口径：**只看 `--profile` 窗口表里的 guest 函数**（`self` 与 `incl`；`interp` 桶不是函数，⛔ 不能当 override 候选）。

## 裁决表

| 场景 | 阻塞段 | 最大 guest 项（self / incl / calls） | 裁决 |
|---|---|---|---|
| 进入 FELLOW | 单帧 `n=1387 wall=10,290.3 ms` / 231.5M 条；`interp=5,878.3 ms(48.7%)` | `sub_412290` 756.4 / — / 0（`!BLIND`，VM 循环头）· `sub_41BF50` 635.8 / 1,749.7 / 612,500 | **没有候选**：无一个 guest 函数 self>1s 或 incl>3s；那 5.88 s 是解释器本身的开销 ⇒ 只能靠"解释器更快 / 更大的 native 单位"，不是本方向 |
| 确认保存 | 一帧 `n=1756 wall=12,626.3 ms` / 220.1M 条（`--profile` 臂是 15.47 s 且没跑完） | `sub_4A42C0` **2,767.7 / 7,599.4 / 1** · `sub_436D50` 1,013.6 / 2,822.6 / 1 · `sub_436D00` 679.2 / 2,061.1 / 1 | **两个候选**：`sub_4A42C0`（ShrinkTexture，缩略图）与 CRC-32 两变体 |
| 加载存档 | `present-tail tail_wall_ms=10,316.4`（≥10.3 s 下界，12 s 窗口没跑完） | `sub_42DDE0` 1,545.4 / **10,303.3** / 1 · `sub_436D50` 1,163.5 / 3,310.3 / 2 · `sub_436D00` 1,039.4 / 3,204.4 / 2 | **1 个候选**：CRC-32 两变体（读写两条路径共用）；`sub_42DDE0` 是读档事务本体（含文件 I/O + guest 控制流）⇒ ⛔ 不做 |

⇒ 一句话：**三处里能被 native override 拿走的只有 CRC-32（读、写各 ~3.2 s incl）与 ShrinkTexture（写，2.77 s self / 7.6 s incl）；FELLOW 与读档事务的剩余时间是解释器 / 事务本身，不是"某个 guest 函数"。**

## 子单（本单在它们都收口前不能收口）

- `REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ` —— CRC-32 两变体（`sub_436D00` / `sub_436D50`）函数级 override（读写共用，先做）。
- `REQ-01M7BY4FMA9VPETYEEV4V7RF2Y` —— 存档缩略图 `sub_4A42C0`（ShrinkTexture）函数级 override（收益更大、风险高一档）。

## 为什么"评估"也要立单

`OBS-20261114-07` 只记读数、不写"所以该做什么"（观察账的纪律）。**做什么、不做什么、为什么不做什么**必须有落点，
否则下一个人会把"FELLOW 还慢 10 s"重新算一遍，或者去 override `sub_42DDE0` 那种不该动的东西。
