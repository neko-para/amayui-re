# emulator-lift · 装载帧性能：先把 enter 75.9M / exit 151M 两帧切成精确窗口，再按 inclusive 树拆 sub_48D610 / sub_428A60

- id: REQ-01M6XVZ8AXJNG492WA2QAMTA35
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 3
- tags: [emulator-lift, performance, load-frame]

## 现状（只有总窗）

进 / 出 Eushully-chan Room 各有一帧的指令数与墙钟远超稳态（硬数字见母单表格）。**现在只有"总窗"**：

- enter 侧有 inclusive 树：`.tmp/room/enter-prof2.log` 里 `sub_48D610` inclusive 4255 ms / 266 次是绝对主导；
- **exit 侧没有分解**：`sub_428A60` inclusive 3920 ms、`calls=1` 是唯一线索（`.tmp/room/exit-prof2b.log`）—— 这一帧到底花在哪几步，目前不知道。

## 三步走（每步都要 A/B + 像素等价）

1. **切成精确窗口**：把 enter / exit 各切成"点下去 → 界面提交完成"之间的**逐段**窗口（现有 `profile_begin/end` + `at insn` 就够，不需要新工具）。产出：两帧各一张"段 → 指令数 / 墙钟"表。exit 那一帧按 `sub_428A60` 的调用分段（`calls=1` ⇒ 一次调用内部的边界要另找）。
2. **按 inclusive 树往下拆**：enter 从 `sub_48D610`（266 次 / 4255 ms）往下；exit 从 `sub_428A60`（1 次 / 3920 ms）往下。每拆一层记一句"这段是不是 guest 语义必需的"。
3. **再决定是否 native override 叶子**：只对"叶子 + 纯计算 + 与输入无关"的做；⛔ 不改 guest 可见语义。
   ★ 这条路走通过的样子已有先例：OPTION 装载帧的一个热点函数 override（单 `REQ-01M6V1KDFTE933AD3YB5P6XJK2`，A/B + 像素等价都做了）—— 照它的证据形态做，别发明新口径。

## 判据

- 每一段都能用**一条命令**复跑（脚本落 `.tmp/room/` 或 `repo:tools/lift.mjs`），读数落文件；⛔ 不接受"只在会话里出现过"的数字。
- **A/B**：改动前后同一脚本、同一窗口、N 遍中位数；enter / exit 各一份。
- **像素等价**：改动前后 `--dump-frame` / `--present-screenshot` 逐像素相同（或不超过既有判据），工具见子单 E。
- 收口条件：两帧各有"段 → 归因"表，且每条被质疑的段都有"为什么它必须存在"的一句话。

## 不做

- ⛔ 不动 `0x4BBAB0` 那类**每帧固定税**（子单 F1）。
- ⛔ 不动解释器吞吐（子单 D）—— 本单只吃"这两个帧里的具体热点"。
- ⛔ 不用 `--skip-logo` 之类的旁路把时间抹掉当"优化"（那只是不测）。
