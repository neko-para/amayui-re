# A · 整 .text 静态 lift（构建期 AOT：guest 代码 → IR → 与 shim 联编成单一二进制）

- id: REQ-01M7CZQZ5RR0WV8CQE2ZEKJHHQ
- type: req
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- tags: [lift, aot]

## 范围

构建期把 guest `.text`（从 PE `entry` 起）lift 成 IR，与 runtime + Win32/D3D shim 编成**一个二进制**；
解释器只以测试构建存在（差分 oracle）。四条硬口径 + 三条落地附加口径见
`REQ-01M7CHE8HBPVEYTNSD05SJSW47`。

## 判据的方向（细则在子单）

- 发货二进制里 **`handoffs == 0`**（一次都不交回解释器），且它是**会失败的负控**（原型已有 `--lifted-only`）；
- 与解释器臂在同一 `--clock=virtual`、同一 present 计数下**逐位一致**；
- 一个字：**可 lift 性不是瓶颈**（96.1% / 93.4% 加权覆盖，`OBS-20261114-11`），瓶颈在发射器与钩子。

## 为什么它是独立一支

它换的是**执行后端**，不属任何一条内容阶段（M2 画面 / M3 3D / M4 音频）；挂在阶段下会被那些子单埋掉。
