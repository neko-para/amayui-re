# SC0000 前置：加载确认框（SBUNKI）不接受注入点击 ⇒ SC0000 到不了

- id: REQ-01M7M45WSPSYB6XC82TS3X4YM2
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- done_reason: 确认框已过：根因是 InputPlan::atPoint 在同一读点消费相邻 click（kForceReleaseReads=16 兜底把按下提前抬起），不是 shim/坐标/gate 数据。可用配方 load24.plan（三次回调形状：无按键一拍 → 按下 → 抬起），读数 rc=0 steps=12 fired=4 pending=0，确认后 script=SC0000.BIN。已记进 docs/03-emulator/scripts/SC0000.md「第 3 轮」。
- tags: [emulator-lift, input, sc0000]

## 结论（2026-11-17 收口）

**已过**。根因不在 shim、不在坐标、不在 `i12e`、也不在 gate 数据，而是**输入计划的施加形状**：

- `InputPlan::atPoint` 的 while 会在**同一个读点**里把相邻两条 `click` 都消费掉；
- 按下只活约 17 个读点（`kForceReleaseReads=16` 兜底），在脚本自己的
  `read-mouse-pos/button`（`0xAF8/0xB0C`）之前就抬起了；
- ⇒ SBUNKI 的 **3979 次** `GetAsyncKeyState(VK_LBUTTON)` 全返回 0，`(local b)` 只到 4（bit2），
  从没到 5（bit0+bit2）⇒ `0x0DA8` 的 `b & 1` 恒 0 ⇒ 跳 `0x1040` ⇒ `0x0F60` 从未进入。

**形状必须是三次回调**：① 一拍无按键（置 `b` bit2、`9=0`）→ ② 一拍按下（置 `b` bit0，`b=5`）→
③ 一拍抬起（`b&1` 清、`b&4` 过 ⇒ `0x0F60`）。可用配方 `.tmp/sc0000/load24.plan`（已入库进
`docs/03-emulator/scripts/SC0000.md` 的「第 3 轮」），读数：`rc=0` · `steps=12 fired=4 pending=0`
· 确认后 `script=SC0000.BIN`。

## 排除过的（都留了读数）

注入链路（WM 进 WndProc、`VK_LBUTTON` 在 `read-mouse-button` 处 0x8000）、按住时长、
`GetForegroundWindow` 返主窗口、`i2fc`（触摸队列，台账 `KN-01M7M6B3477V7H2Z1Z0S04475G`）、
`i12e`（命中测试，台账 `KN-01M7M7BZZB2R1F512K3M0V5437`；はい = `[517,764]×[300,347]` 或按字节宽度
`[473,807]`，两者都含 600/690）。

## 派生

- 单次 `click` 无法保证"按下被某一拍看到" ⇒ 另立单（输入计划的 press/release 形状）。
- `local 9c = 6`（`_mbstrlen` 返回字节数）⇒ 文本度量偏差，另立单。
