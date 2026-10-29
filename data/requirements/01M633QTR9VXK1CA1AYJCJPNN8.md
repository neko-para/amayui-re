# 排版记录标志位 2（另一支字号）：置位者与 0x82 选支判据未核

- id: REQ-01M633QTR9VXK1CA1AYJCJPNN8
- type: spike
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-text-record-flags
- tags: [spec, text, emulator-cxx]

## 一句话

已收口（2026-10-29）：位 2 的置位者、条件、消费者与字号支都已取证并落地。
守卫 `emulator-core-text-record-flags` 钉住：主线 run 的记录**位 2 = 0**、振り仮名 run 的**位 2 置起**、
字号支只有 `record_font_size` 一处口径（位 2 ⇒ B 支 10、否则 A 支）、文本目标 id 的位 31 置起 ⇒ 一条记录都不追加。

## 已取证（EA 锚）

- 记录 +0x28 只有三处真写：`0x45EF12`（位 30 = 语音）、`0x45F009`（位 29 = 续段）、
  `0x45F0F6`（调用方原值 + 条件位 0）⇒ 位 2 只能从 flags 实参进来。
- 两个置位点（`or eax,4`）：`0x461C93`（`sub_461A10`，层模式 = 1 的逐字支）、`0x465BF8`（`sub_465A20`，振り仮名 run）。
  条件：振り仮名串非空（`0x46C225` / `0x46C37F`、`0x46CAB7` / `0x46CBA0`）或层的另一个模式格 = 1
  （`0x46BEDC`；唯一写者 = opcode `0x1d9`，`0x42143E` / `0x421431`）；两条受文本目标 id 位 31 约束
  （`0x461EB0` / `0x465E45` / `0x46C208`）。
- 消费者 `0x4666B6`（`sub_466000`，唯一调用方 = `0x82`）：位 2 ⇒ 按对位模式选 `Font+0x448` / `Font+0x355C8`
  （两者都出自 `0x197` 写的 LOGFONT）；位清 ⇒ 满足两个内部门时选 `Font+0x44C`（出自 `0x75`），否则不换字体；
  画完 `0x466F42` 再测一次才归还。
- 位语义表：位 0 = 新段首条、位 1 无写者、位 2 = B 支字号、位 3 = 行推进占位、位 29 = 续段、
  位 30 = 语音、位 31 = 抑制（不落库）。

## 已落地

- 振り仮名 run 追加一条文本记录、flags **|= 4**；主线正文 run 不置位 2。
- 字号支收敛到 `Machine::record_font_size(flags)`：位 2 ⇒ B 支（`font_b_height_`，缺省 10）、否则 A 支（0 = 沿用）；
  振り仮名的待画格也走这个口径。
- 守卫 `emulator-core-text-record-flags`（三条：记录标志 / 字号支 / 位 31 抑制），两条变异验证：
  `record_font_size` 恒 0、振り仮名记录去掉 `|4` ⇒ 各自红。

## 仍未做（不属本单）

- `0x82` 的**真实绘制**仍是"只记请求"（GDI 度量 / 渲染命令未建模）⇒ 位 2 的像素级后果目前只经
  振り仮名的待画格体现；`0x82` 落地时要复用 `record_font_size` 选支。
- 位 2 的其它读者（`sub_45CF00` / `sub_462040` / `sub_4634B0` / `sub_4675A0`）未逐字核。
- 写者枚举是下界（`lea reg,[rec+28h]` 再写的形态看不见）。
