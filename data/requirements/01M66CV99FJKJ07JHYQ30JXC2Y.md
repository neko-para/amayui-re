# 🐞 SN0000 旁白渲染在边缘（应居中）

- id: REQ-01M66CV99FJKJ07JHYQ30JXC2Y
- type: bug
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- repro: docs/02-engine/repro/sn0000-narration-align.md#观测点
- severity: S2
- tags: [emulator-cxx, adv, render]

## 现象（用户实测，窗口模式，序章 SN0000）

旁白应**居中**（屏幕中部、无消息窗框）；当前渲染在**边缘**（贴左 / 贴顶）。

## 与既有缺陷单的关系

`REQ-01M65A72Y5ZHJPXMGPMHB0KH92` 记的是"这些旁白一句都不出现"；本轮让它们出现之后，
**位置**成了新的可观测分歧。两件事同一条链：文本层矩形 / 原点 / 绘制键基数 / 揭示游标。
