# 🐞S2 音效槽量不一致：实现按 0..14，引擎侧 T_SE 只有 10 格且不检查越界

- id: REQ-01M5Z9SYN0AF3QC61B41AEZPTP
- type: bug
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- repro: apps/emulator-cxx/src/core/vm.h#se_slot_resource_
- severity: S2

## 分歧

`0xb4` / `0xb6` / `0xba` 在 `emulator-cxx` 里按 **0..14** 判槽（`se_slot_resource_[15]`），
而取证的引擎侧槽数组是 `T_SE+0x4BC` 的 **10 项**，且两个写入口（`sub_4B4F60` / `sub_4B5050`）**都没有 0..14 的边界检查**。

⇒ 实现与汇编有**两处**不一致：
1. 槽数上限（15 vs 10）；
2. 我们**拒绝** `slot > 14`，而引擎根本不检查（越界写属未定义行为）。

## 为什么值得单独一张单

它不是"少一条指令"，而是**已登记规则的字段解释偏了**（`CORE-OPS-AUDIO-WAV` 的"槽"到底是 10 个还是 15 个）。
照现状继续加音频指令，会把错的槽量当成合同复制到新规则里。

## 判据（要落到能红的守卫）

- 一条合成脚本：`slot = 10..14` ⇒ 引擎格**不该**被写到"第 10 格"，而实现应当按取证处置（拒绝 or 记日志）；
- 或者反向：把 `T_SE+0x4BC` 的项数读出来（`data/shape/types.json` 里 `T_SE` 的 size），
  让守卫断言"实现的槽量 == shape 声明的项数"。后者更硬。
