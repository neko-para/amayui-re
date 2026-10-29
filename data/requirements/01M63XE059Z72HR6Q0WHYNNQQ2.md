# spec 成篇 · 标量/字符串/数组族（0xc0/0x142/0x1bb/0x1a6/0x2c7/0x2de/0x12f）

- id: REQ-01M63XE059Z72HR6Q0WHYNNQQ2
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 3
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- tags: [engine spec]

## 判据（可判定）

`audit.gaps[]` 里不再出现：`0xc0` `0x142` `0x1bb` `0x1a6` `0x2c7` `0x2de` `0x12f`；
它们各有规则标题、`bindings`、`audit.rules`；`pnpm tools spec validate` 13/13 绿 + 精确划分守卫绿（收口凭据）。

## 已落（2026-10-29）

- `60-ops-voice.md`：`CORE-OPS-AUDIO-MUSIC-FADE-CELL`（0xc0）—— 纯读一个引擎整数格；三条读法上的排除（不清、不触发）。
- `60-ops-scalar.md`：`CORE-OPS-FLAG-CELL-142`（0x142）—— **原值**写（⛔ 不折成布尔），复位 1。
- `62-adv-text.md`：`ADV-TEXT-GATE-1BB`（0x1bb）—— 三个取值 + 三支行为；第 31 位；判据是"≠ 0"。
- `60-ops-text.md`：`CORE-OPS-TEXT-HALF-LEN-1A6`（0x1a6）· `CORE-OPS-TEXT-SUBSTR-2C7`（0x2c7，含首 / 尾修正与双字节判据）·
  `CORE-OPS-TEXT-FONT-NAME-2DE`（0x2de，含"跳一个 @"与"不读标志字节"）。
- `60-ops-array.md`：`CORE-OPS-ARRAY-SORT-12F`（0x12f，含操作数映射、读序、A[0] 原始写、N=0 的终态、重叠未定）。

## 独立复审（规则面）与处置

共 25 条，高 / 中优先项全部处置：

- **`0x2c7`**：把"起点不在 `[0,长度)`"改成"**起点 ≥ 长度**"（负起点进残差）；补"首修正 → 尾修正"的**顺序**与
  "从头扫描"的判定算法；两条警告点名走日志通道。
- **`0x12f`**：补操作数映射（A/B/C/N）与读序（A→B→C→写 A[0]→N）；写清 **N=0 时不进收尾循环、A[0] 保持原始 0**；
  重叠进残差；元素按 4 字节字 + 整数编解码解释（浮点族同处理）。
- **`0x1bb`**：与 `0x1d2` 的"文本目标 id"统一成"文本显示门"；补第 31 位、读者三处、初值 / 复位进残差。
- **falsifier 修正**：`0x1a6`（3 字节 ⇒ 1）· `0x2c7`（两个双字节 → 空串）· `0x12f`（解码后比、N=0/N=1 对照）·
  `0x2de`（去掉"两个 @"那条恒真项）· `0x142`（改用引擎状态转储）。
- **写法统一**：整数操作数一律写"按整型读出"；"字符串族 5/8/11"改成"按「数据池」的写整型支持表"。

## 已知欠账

- 参考实现里 `op_2c7` 是空返回、`op_2de` 恒 −1 ⇒ 已登记成 `REQ-01M63YS3T54VYCXCBJ6RPCHDAY`。
- `0x2c7` / `0x2de` 的证据引的是 `emulator-core-config-screen`（未给具体断言）；升 reviewed 前要具名用例或 KN 锚。
