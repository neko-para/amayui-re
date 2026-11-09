# emulator-lift · 交互可靠性 + 装载性能：未收口工作汇总（识别驱动状态机 / 装载帧 / 吞吐 / 分析工具）

- id: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- order: 0
- tags: [emulator-lift, interaction, performance]

# emulator-lift · 交互可靠性 + 装载性能：未收口工作汇总（识别驱动状态机 / 装载帧 / 吞吐 / 分析工具）

- id: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- order: 0
- tags: [emulator-lift, interaction, performance]

## 这张单收什么

这一轮（emulator-lift 交互 / 性能）插进来的**所有待处理工作**收在一处：交互（识别驱动状态机 · 脚本身份事件源 · 点击真的被 guest 采到）、装载帧与指令吞吐、测量与分析工具，以及本轮已经开出来的缺陷单。新发现先挂这里再往下拆 —— 不再散在树各处。

## 现在到哪了（已完成的 4 件 + 可复核落点）

| # | 已落地 | 判据 / 落点 |
|---|---|---|
| 1 | **输入注入**：`--input-script` 指令式时间线（`at frame/insn/guest_ms/wall_ms`、`click`、停滞兜底）+ `--present-screenshot-frame` | 单 `REQ-01M6WSBJXVEN3E63J2P3H41QRP`（done）；落点 `repo:apps/emulator-lift/src/runtime/input_script.h`、`repo:apps/emulator-lift/src/runtime/input.h`；守卫 `repo:apps/emulator-lift/tests/input_injection.cpp` |
| 2 | **区间 profiler**：脚本里 `profile_begin` / `profile_end` 定界，`at insn` 可在**帧内**切窗口 | 落点 `repo:apps/emulator-lift/src/runtime/window_profile.h`；守卫 `repo:apps/emulator-lift/tests/profile_window.cpp`（闭式残差 ≤3.3 ms 的口径在该守卫与 `show 1QRP` 的正文） |
| 3 | **三条快路径**：input-script tick 逐条比较、`GuestHeap` 首次适配 / 收口、GPU 合批脏矩形 + 切段、`GetTextMetricsA` 度量缓存 | 单 `REQ-01M6X4KST5FJ3R4W06TRFZYCGR`、`REQ-01M6WXEDGGJY1Q1YDEM07S1YR3`（done）；守卫 `repo:apps/emulator-lift/tests/input_tick.cpp`、`repo:apps/emulator-lift/tests/heap_layout.cpp`、`repo:apps/emulator-lift/tests/gpu_batch.cpp`、`repo:apps/emulator-lift/tests/gdi_font_metrics_cache.cpp` |
| 4 | **测量口径**：RelWithDebInfo + 窗口 real-time 时钟 ⇒ `guest_ms` 差 = 宿主墙钟（A/B 与 N 遍中位数都靠它） | 跑法 `.tmp/roomopt/run.ps1`（参数即口径）、解析 `.tmp/roomopt/parse.ps1`；**`.tmp/` 不入库、脚本随会话消失** ⇒ 要长期用就落 `repo:tools/lift.mjs`（子单 E） |

★ 四件都**不靠本单自证**：上表的单号与守卫文件自己会说话；本单不写会过时的门禁数字。

## 硬数字（测量事实，不是门禁；不同运行有抖动）

| 量 | 读数 | 怎么量 |
|---|---|---|
| 进 Eushully-chan Room 的那一帧 | 75.9M 条指令 ≈ 5.5 s | 脚本 `.tmp/room/enter-prof.txt`（`click 944 562` + `at insn` 定界）；一次读数 `.tmp/room/enter-prof2.log` |
| 退出房间的那一帧 | 151M 条指令 ≈ 8.7 s（真实退出中位 9.17 s） | 脚本 `.tmp/room/exit-prof2.txt`；**窗口只有总窗、没有分段**（子单 C 的第一件事） |
| 空转 / 装载帧吞吐 | 64.7 ms 每百万条指令（≈15.5 M insn/s）；装载帧 55–58 ms/M | 同一 `input-script` + profiler 窗口；`.tmp/roomopt/run.ps1` 的 `guest_ms` 差 |
| 过渡帧 `HeapAlloc` 的 shim 墙钟 | ~85% 是**等 guest 令牌**（进房间窗 605.8 ms / 过渡帧 849 ms） | 该 shim 自己记 `token_wait_ms` 与调用数：`.tmp/roomopt/heapmeas-*.log`（口径见子单 F2） |
| 进帧热点（inclusive） | `sub_48D610` 4255 ms / 266 次（self 115 ms）；`sub_4E0B40` self 243 / incl 1239 / 366 次；`sub_4E0760` self 334 / 5520 次；`__ftol2` 192 ms | `.tmp/room/enter-prof2.log`（同脚本另几次 `enter-prof.log` / `diag-enter-diag.log` 数值同量级、不逐字相同） |
| 出帧热点 | `sub_428A60` inclusive 3920 ms，**calls=1**（self 1615 ms） | `.tmp/room/exit-prof2b.log` |
| 每帧固定税 | `0x4BBAB0`（DirectSound 槽管理器，3 槽轮询）空转 79 s 内 2.77M 次 | 帧级调用计数（子单 F1） |

★ 这些是**量级判据**：C / D 的目标是让它们各自可 A/B 复现，而不是把上面某个数当门禁。

## 子单索引（id ← 子单：为什么它单独成单）

| 子单 | 为什么它单独成单 |
|---|---|
| `REQ-01M6XVYY5TCKSAS80WS7PADF8P` 识别驱动交互状态机 | 换的是**交互模型**（节点图），不是修一条脚本；判据是"识别 → 动作 → 再识别"的闭环，且 pure CTest 可测 |
| `REQ-01M6XVZ4ANQRC215ACG90A35NR` 脚本身份 marker 事件源 | **不装游戏**就能用纯 CTest 钉住，与"必须真安装"的消费者（DPZVY）分开 |
| `REQ-01M6XVZ8AXJNG492WA2QAMTA35` 装载帧性能 | 只动**两个具体帧**的 inclusive 树，判据 = A/B + 像素等价 |
| `REQ-01M6XVZC086PFXJRWE5AASRJ8G` 指令吞吐 | 改的是**核心解释器** ⇒ 影响所有路径，不能与装载帧混在一起裁决 |
| `REQ-01M6XVZE3AN0FKFZA52VV23WD9` 分析工具与测量可信度 | 工具缺失会把"两次运行差在哪"退化成手写脚本；也是**静默失败**的通用检测面 |
| `REQ-01M6XVZF3VDC5NSBJ20J2451VJ` 每帧固定税 | 空转也交的税，与"某一个装载帧慢"是不同问题 |
| `REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9` 令牌等待 | 等锁 = 并行契约的代价，修法与"查得太勤"完全不同 |
| `REQ-01M6XVZH2HA6VYTV0X6F3QAGHB` 点击必须被 guest 采到 | **测量前提**：点击没落地 ⇒ 之后所有优化都在假数据上（已经发生过两轮） |

## 本批结果与下一轮入口（2026-11-09）

- 收编的 7 张原单：6 张已收口（三个 lifetime / 资源耗尽退出码 / OOM 夹具；语法统一 dropped），只剩 TITLE spike。
- **已收口 9 张**：13 / 14 / 15 / 12 / 10 / E / 16 / B + A 的卡点 1（后缓冲 `LockRect`）。
- **仍在 6 张**：A（`blocked`：戻る + C++ EH）· C · D（已裁 `X=4.0 s`）· F1 · F2 · TITLE spike。
- **新开 5 张**：ddraw 同形泄漏 · surface 非 RT 寿命 · EA 认对象残余 · 图 profiler 静默残表 · C++ EH 展开。
  下一轮入口：C/D/F1/F2 的设计要点已写进各自单正文；TITLE 需 4 条真机读数；A 只等两张前置单。

## 明确不做什么

- ⛔ **不为提速改 guest 可见语义**（要改语义就另开语义单，走准入门 `AGENTS.md` §6）。
- ⛔ **不砍帧、不砍音频**（性能不是靠少画 / 少算换来的）。
- ⛔ **不重走 ASAN 那条路**（已经走死；结论留在既有单里，不再投入）。
- ⛔ 不把本单当"知道什么"的仓库：测量口径与逆向结论进 `data/ledger/` / `docs/`，这里只留"还要做什么"。
- ⛔ 不为了收口把子单并成一张（识别源、装载帧 / 吞吐、固定税 / 令牌等待的失效方式互补，合并就没人知道该看哪个判据）。

## 只在正文里链接、**不动**它们（不属于本批）

`REQ-01M6RQ2XNWRDV35E72D7G2ZV7D`（TITLE sprite 合批 / 按需回读，上一批性能单）· `REQ-01M6RH9CDPM8PG2SFEG0PYT46T`（TITLE 黑场渐变 ~1 fps，doing）· `REQ-01M6X4M4NSRSNX2B0G0DQSFAD1`（option-roundtrip 堆损坏，仍挂 M2）。M7 探针 / 翻译 / 语料三支一律未动。
