# emulator-lift · 装载帧性能：先把 enter 75.9M / exit 151M 两帧切成精确窗口，再按 inclusive 树拆 sub_48D610 / sub_428A60

- id: REQ-01M6XVZ8AXJNG492WA2QAMTA35
- type: req
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- order: 3
- done_reason: 两帧都切到精确窗口并出了"段→指令数/墙钟/归因"表：enter 单帧 N=5 = 64.97 ms/M（载荷帧号漂 106-110）；exit 4 段 N=5 = 64.94 ms/M（151M 3/3 复现）；块级 insns 闭合（85 窗口 block+override==window、unattributed 全 0）。exit 的 sub_428A60 切成 12 段：seg01-11 是同一个 8 指令块自旋 641,875 次（块级 insns 100%、edges 空、三遍恒等），桶恒定 guest 46%/interp 32%/host(input-script) 21% ⇒ inclusive 不能整个当 guest 工作量。语义取证：sub_48D610 = 音频源接口解码出 int16 PCM（32767.0/clipping/ReadFile/同虚表第4-5项是缓冲 reader）⇒ 引擎必需；sub_428A60 = 结束当前场景+重装根脚本（其自身抛出点 0x428D9C；0x4123C4 属 sub_412290 主循环，已纠正早期正文）；00428AD0 = flip pool bank 指针数组重填（环大小由场景数据 this+0x5D7E8 给）。裁决：不做 native override（唯一候选 __ftol2 上限 2.4% 且归属存疑）。
- tags: [emulator-lift, performance, load-frame]

## 这张单要什么

把 enter（75.9M 条）/ exit（151M 条）两个装载帧**切成精确窗口**，出"段 → 指令数 / 墙钟 / 归因"表，再决定是否对叶子做 native override（⛔ 不改 guest 可见语义）。

## 判据（本轮全部达成）

- 每段可用**一条命令**复跑（脚本 + `run5.ps1`/`extract5.ps1` 落 `.tmp/`，exe sha256 + 脚本 sha256 逐次记录）；⛔ 不接受"只在会话里出现"的数字。
- **A/B**：同一脚本、同一窗口、N=5（enter）+ N=5（exit）+ 12 段 × 3 遍；⛔ 不用 profiler 读数当基线。
- **像素等价**：本轮**没有**任何性能改动（只有测量与工具），故不适用；若将来做 override，照 `REQ-01M6V1KDFTE933AD3YB5P6XJK2` 先例做 A/B + 像素等价。
- 收口条件：两帧各有"段 → 归因"表，且每条被质疑的段都有"为什么它必须存在"的一句话 —— 见下。

## 读数（RelWithDebInfo，`--present window --present-hidden --present-vsync 0 --raster gpu --native-overrides all --skip-logo --audio off`）

- **enter 单帧 N=5**：insns 中位 75,901,424 · wall 中位 4931.7 ms ⇒ **64.97 ms/M**（极差 63.40–67.38）；载荷帧号漂 **106–110**（⇒ 候选窗下界 100 才稳，帧号不能当判据）。
- **exit 4 段 N=5**：152,285,018 条 / 9888.9 ms ⇒ **64.94 ms/M**（极差 64.50–66.05）；重载帧 = 151,015,150 一类（3/3 复现）。
- **块级 `insns` 闭合**：85 个窗口 `block + override == window` 逐位成立、`unattributed` 全 0（16,786 条块行无 `insns=0`）。
- ⚠ `input-script` 这个 host 桶占**墙钟 9–21%**（脚手架在每块边界扫动作，不是引擎账）；profiler 在巨帧上膨胀 +33~40% ⇒ ⛔ 别拿单次比值当定论。

## 段 → 归因（本轮新增，带语义）

- **enter 帧（60.5–62.7% 墙钟）**：`sub_48D610` incl 2936–2953 ms、其中 **73% 走 `→sub_4D90C0`（268 次）**、99.6% 再落 `→sub_4E2A30`。
  语义（取证）：**`sub_48D610` = 音频源接口的"解码出 int16 PCM"虚方法**（`OggStream` 0x528FD0 / `OggVorbis` 0x52917C **共用第 3 个表项** 0x528FD8/0x529184）；
  铁证 = `dbl_5291B8 = 32767.0`、串 `"Clipping in frame %ld"`、`ReadFile 4096/次`、饱和 `cmp 7FFFh`/`cmp 0FFFF8000h`，且同虚表第 4/5 项正是
  "输出缓冲地址/字节数"的 reader ⇒ **产出有 guest 读者**。`sub_4D90C0` = 单包合成入口（分派到 mapping inverse），`sub_4E2A30` = **真正算力**（floor/residue 反向重建）。
  ⇒ "为什么必须存在"：**引擎本来就要解码**；优化空间在"同一语义更便宜的等价执行"，不在"它不必要"。
- **exit 帧**：`sub_428A60` 的 inclusive **不能整个当 guest 工作量** —— 12 段 × 3 遍实测桶恒定 **guest 46.2–46.8% / interp 31.6–32.0% / host(input-script) 20.8–21.3%**，
  按次摊 **414 ns/圈**。语义（取证）：**`sub_428A60` = "结束当前场景 + 重装根脚本（统一文件 id 0）"**，**不是**进程退出；它自己的抛出点是 **0x428D9C**（`Command_Exit_Exception`）。
  ★ **纠正**：`0x4123C4` 的 `__CxxThrowException` **属于 `sub_412290`（主循环）**，不在 `sub_428A60` 里（本单早期正文写错过）。
  - **那 849 ms 的形状**：切成 12 段（每段 5.135M 条）后，**seg01–seg11 的块表只有 1 行** `00428AD0`（占块级 insns **100%**、`iters=641,875` **三遍一字不差**、`edges` 为空）
    ⇒ **同一个 8 条指令的块自旋 641,875 次**；`seg12` 才有循环退出与同函数的尾块（`428B66`/`428B13`）。
    语义：`00428AD0` = **flip pool bank 的指针数组重填循环**（`arr[i] = [this+0x5EC90]`，8 条/圈，圈数 = `this+0x5D7E8`+1，该字段唯一写点 0x41515D 从 `this+0xAAB2C` 抄）
    ⇒ **环大小由场景数据给定**，合计 ≈ 2 bank × 3.7M 槽（与 `set:EnableMemFlip`=非 0 自洽）。吞吐校验：37.5M/0.849–1.115 s ≈ 34–44 M/s，与同窗 interp 32.7 M/s 同量级
    ⇒ **真的在退休指令，不是宿主忙**。
  - ⚠ **`calls=0` 有两种成因**（别再一律叫"尾调用盲区"）：① 同一个 call 被窗口边界切开（inclusive/calls/边只在 `ret` 结算）；② 真尾调用进入（thunk `0x476596` = `jmp sub_428A60`）。
    判据：窗内有没有 `Op::Call` 进 `0x428A60`/`0x476590`；本轮 `open_at_end=1` 佐证 ①。
- **入口段**：p303 probe `incl 20.189 / self 9.456 / calls 1`（覆盖的 0.6%）。

## 决定（本轮裁决）：**不做 native override**

- 唯一"叶子 + 纯计算"候选是 `__ftol2`（199,232 次 × 12 条 = 2.39M 条）：self 191.9 ms / 8085 ms ⇒ **上限约 2.4% 墙钟**，且要复现 x87 80 位 `fmul`、控制字截断、有符号饱和次序、`"Clipping"` 副作用。
- ⚠ 归属仍 `<待核>`：`0x48D772` 调的是 `__ftol2_sse`(0x4E82C0, COLLAPSED)，块 EA 是 `__ftol2`(0x4E82F6)，只有前者跳进后者才接得上。
- ⇒ **2.4% 的天花板 + 归属存疑 + 逐条一致性风险**，收益不抵风险。要真提速就得打 `sub_4E2A30`（mapping inverse）那一层，那是"重实现解码"，属语义单。
- ⛔ 没有为提速改任何 guest 可见语义；本轮**零性能改动**（只有测量与 profiler 工具）。

## 残差（⛔ 不要当"没做"重复记）

① 12 段的 `incl` 读不到（窗口起点落在函数帧内部 ⇒ `!BLIND`），`self` 仍准；② 逐段墙钟遍间极差 15.6%（最大 37.3%）⇒ 要按段比墙钟需 N≈10–13；
③ 3920 ms 那个数**本轮未复现**（同范围单窗 3244.8/3153.3，当时是 `[300,400M]` + `audio=on` 的另一窗口）；`exseg1` 的 1852 已复现（1882.8）；
④ 语义侧仍未证：`this+0x5D7E8` 的运行时值（3.7M 是 insns/8 反推）、PCM 之后被谁消费、60% 里非 `4D90C0` 那 ~27% 的逐块切分。
