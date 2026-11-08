# emulator-lift · 点击必须被 guest 采到：按下时长要覆盖界面采样周期，且要有「真的采到了」的守卫

- id: REQ-01M6XVZH2HA6VYTV0X6F3QAGHB
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 8
- verify: apps/emulator-lift/tests/input_injection.cpp#input-injection/click-spans-sample
- done_reason: 自动 hold（缺省）：click 把 down 与 up 之间保证为 [按下后 guest 采样计数至少 +1] 且 [从'看到按下'的那次采样起再等一个采样周期（= 最近采样间隔，夹 1..200 ms）]；上限 --input-max-hold（缺省 1000 ms guest 时间，0 = 不设限），到上限仍未采样 ⇒ 仍然抬起 + stderr 告警 + 脚本继续；显式 hold N / hold N frames / hold N ms 完全覆盖自动值（不看采样）；显式 mouse_up/key_up 同样推迟到被采样后。守卫 input-injection/click-spans-sample（另有 click-honors-explicit-hold / click-bounded-without-sampling）；变异实测：把自动判据改回'同一 tick 内 up' ⇒ 三例全红、guest 侧 kDownSeenPolls=0。端到端：.tmp/clickfix/exit-click.txt 一次 click 点「戻る」3/3 回 TITLE（点击帧 → 单帧 1.51e8 条 TITLE 重载 → 28 万条/帧动画）。未做：本单判据第三条'动作之后必须观测到变化'的通用检测属下一轮状态机，需父级裁决是否拆单。
- tags: [emulator-lift, interaction, input]

## 已确证的分歧

`mouse_down` / `mouse_up` 相隔 **0.7 ms**，而界面按 **20 Hz** 采样按键（周期 50 ms）⇒ 整次按下落在两次采样之间，**guest 一次也没看到"按着"**。点击被静默吞掉、界面什么都没发生，而运行**照样往下跑** —— 于是两轮优化基于假数据。

## 现状

- 语法里已有 `click X Y [left|right|middle] [hold N]`，`hold` 是**帧**数（`repo:apps/emulator-lift/src/runtime/input_script.h` 的注释区）。
- **缺的是证据**：没有任何守卫断言"guest 真的采到了按下"；缺省 `hold` 够不够覆盖采样周期也**没有定论**（帧率与 20 Hz 采样不是同一把尺子）。

## 判据

- 一条守卫（可复用 `repo:apps/emulator-lift/tests/input_injection.cpp` 的同款夹具）：注入一次点击 ⇒ 断言 guest 侧采样**至少看到一次按下**；把 `hold` 压到 0 帧、或把采样短路 ⇒ 该断言**当场红**。
- 缺省值有裁决：要么"缺省保证 ≥ 一个采样周期"，要么"缺省不保证、脚本必须显式给 `hold`" —— 二选一，写进本单与帮助文本，⛔ 不许两套口径并存。
- 交互层面加一条**通用**检测（与子单 A / E 共用）：动作之后必须观测到变化，否则响亮失败。

## 不做

- ⛔ 不把"引擎内部命中测试点中了"当判据（那不是"guest 采到了按键"）。
- ⛔ 不用"加大空闲等待"当修法（等待不产生采样边沿）。
- ⛔ 不与 A 的节点图混做：本单是 A 的**前置**（没有可靠的"按下"，识别驱动的动作一样是假的）。
