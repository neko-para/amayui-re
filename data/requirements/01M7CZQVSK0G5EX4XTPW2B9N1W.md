# emulator-lift · L · 原生执行（整 .text lift / 手写 native override）

- id: REQ-01M7CZQVSK0G5EX4XTPW2B9N1W
- type: req
- status: dropped
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- dropped_reason: 2026-11-16 拍平：L 的两支只剩一支（B · 手写 native override 已 drop —— AOT 执行来源只有 IR 与 shim，解释器退役）。中间层没有存在意义：A 已迁到 'lift 层 · 语义与覆盖'，其子单按类分散到 语义/效率/工具 三组。边界文字随 A 保留。
- tags: [emulator-lift, lift, perf]

## 这一支回答什么

「guest 代码怎么变成宿主代码、以及怎么把个别热点换成手写实现」：

- **A · 整 `.text` 静态 lift**：构建期把 `.text` 编成 IR / 宿主码，最终**一个二进制**、解释器不在执行路径上。
  形态口径见 ADR `REQ-01M7CHE8HBPVEYTNSD05SJSW47`（含"发货不含解释器 / `.ll` 不入库 / 从 `entry` 起"三条附加口径）。
- **B · 手写 native override**：语义已知、且比 lift 更优的局部替换（CRC-32 是已落地的样板，`L3-crc-01`）。

## 不放在这里（放错地方会让这一支又变回平铺）

- **输入 / 交互图**：`REQ-01M6XVYA68DQWWMQPY795WPN0Q` 那一支（交互可靠性）与输入注入重做，留在 M2。
- **导入桩台账**（`data/lift/overrides.json` —— 这里的 `lift` 是**工具名**，不是本支）：留在 M2。
- **音频 / 影片 / D3D 的语义与吞吐**：留在 M2 / M3 / M4 的对应子单；只有当它变成"手写 override 落地"时才挂到 B。
- **arm64 兼容**：属 A 的验收面，不单独立单。

## 读数在哪

`docs/03-emulator/observations.md` 的 `OBS-20261114-{11,12,14,15}`；
ADR 是 `REQ-01M7CHE8HBPVEYTNSD05SJSW47`（形态）与 `REQ-01M7CHE9T4NKHGKY2TQGVX6WK3`（浮点模型）。
