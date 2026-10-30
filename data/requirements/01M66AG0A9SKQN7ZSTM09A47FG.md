# 自动消息（auto-message）计时器未建模：0x2e9 的消费者没有落点

- id: REQ-01M66AG0A9SKQN7ZSTM09A47FG
- type: req
- status: done
- parent: REQ-01M65JT8TYEHQPPNKT1RW747V0
- verify: apps/emulator-cxx/tests/core_selftest.cpp#check_auto_message
- tags: [emulator-cxx, adv, text]

## 结论（已实现并落守卫）

按语料取证后落地（spec 已同步，含两处**纠错**）：

- 自动推进的开关是 `0x1b7` 写的引擎格（`0x1b6` 读回）——**不是** `0x77A14`（那一格是**语音在播**）。
- `0x72` 在开关非 0 时按公式武装计时器：
  `时长 = max(100, Time1 + Pitch1 × (N − 1 − 0x2e9 的格))` 毫秒；`N` = 该层行记录数。
  （选组看"语音在播 + AutoMessageOption bit0"；本模型没有音频后端 ⇒ 语音恒不在播 ⇒ 第 1 组。）
- 到期只做一件事：**放行 `0x72` 的等推进位**（清 bit31 + bit30），⛔ 不换行、不取下一行 ——
  下一行仍由脚本产生（与点击放行完全同一条路）。
- 首次放行 = **1 个周期**（不是 2 个）：定时器起播计数值为 1。

## 守卫

- CTest `emulator-core-auto-message`：
  ① `0x1b7 1` + Time1=2000 ⇒ 1999 ms 不放行、2000 ms 放行并执行下一条；
  ② Pitch1=2、N=0 ⇒ 时长 1998（系数与 `−1−格` 都真的进公式）；
  ③ `0x1b7 0` ⇒ 不武装、等到 5000 ms 也不放行。
- 变异实测：删掉 `step()` 里的 `service_auto_message()` ⇒ ① 当场红。
- spec：公式 / 选组门 / 下限 100 / 开关身份写进 `60-ops-list.md` 与 `60-ops-scalar.md`；
  残差改成"两倍时长的运行时核对"与"N 的领域名"。

## 还没做

- 语音在播那一支（需要音频/语音后端；产品里没有）。
- 自动消息在**真实时间线**下的窗口推进（headless 只证到"位被放行"这一层）。
