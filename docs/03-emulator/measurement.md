# 测量口径（measurement）

> 这个文件记 **"怎么量才算数"**。每条 = 能比什么 / 不能比什么 + 一条能重跑的命令。
> ⛔ 仪器化读数不是性能成绩；⛔ 跨口径的读数不许放进同一张表；⛔ 读数过期不删（见 `observations.md` 的编号规则）。
> 编号 `M-NN`。口径的**判据**是"两个人各自按这条跑，读数能对齐"。
>
> ★ **时钟只有一档：墙钟**（2026-11-27 起；`--real-time` / 虚拟时钟整档移除，见 `runtime/clock.h`）。
> ⇒ 同一条命令跑两次的 `insns` / trace / 状态 hash **不承诺相同**；要跨次可比的判据只用
> **输入清单**与**计数器**，⛔ 不用"逐字节相同"。

---

### M-01 · 墙钟只能在**同口径**内比较
**能比**：同一构建 / 同一 `--present` 档 / 同一输入脚本 / 同样开没开 profiler 的两次运行。
**不能比**：换构建、换 `--present window` ↔ `headless`、换有无 `--profile`。
- **反例（实测）**：同一条 151M 条的 TITLE 重载帧，`.tmp/exitstall` 留档读 `55.3–57.7 ms/M`，
  `.tmp/clickfix` 留档读 `48.1–49.3 ms/M`（`observations.md` OBS-04）⇒ 差 14%，纯口径差异。
- **怎么量**：口径四件事写进日志第一屏 —— 构建（`cmake --preset`）/ `present :` 那一行 / `raster :` 那一行 /
  **运行摘要的 `instr :` 行**（三个仪器位 + `expect=`，见 M-02）。这些行 `apps/emulator-lift` 都会打出来，
  **抄它，不要凭记忆**。

### M-02 · **仪器化读数**：三个开关、口径标签、N=5 中位数（⛔ 开/关不许混进同一张表）
**能比**：同一档（同一二进制 / 同一 `present`+`raster` / 同一 `clock` / 同一输入脚本）里，**只差一个开关**的两臂。
**不能比**：跨构建、跨 `present` 档、跨时钟档、以及把"开着的 `ms/M`"与"关着的 `ms/M`"排成同一条序列。

- **"开没开仪器"不是一个布尔量，是三个开关**（只写"带 profiler"不足以描述读数）：

  | 开关 | 打开方式 | 量的是什么 | 相对"全关"的**边际** |
  |---|---|---|---|
  | `--profile` | CLI flag（`RunOptions.profile`） | 整轮热点：块命中 / 块墙钟 / shim 墙钟（跑完打表） | 每块 `++hits` + 每块 `b->ns +=` + **每次 shim 调用两次 `steady_clock`** |
  | 窗口 profiler | 脚本里的 `profile_begin` / `profile_end` | 一段区间的闭式（guest/shim/native/interp/host） | 每迭代两次时钟 + 每块边界最多 3 处宿主段计时（各两次）+ `addGuest` |
  | `LIFT_FRAME_TRACE=1` | 环境变量 | 逐帧"墙钟去哪了"一行 | shim 计时 + 每次 Present 一次汇总 + 一行输出 |

  三者**共享** shim 计时开关（`shims_.setTiming`）⇒ 任意组合都可能出现 ⇒ 标签必须三位都写。

- **膨胀从哪来**（2026-11-09 读代码更正；旧归因是错的）：
  - **"每块两次 `steady_clock`"不是 `--profile` 的开销**：块计时**无条件**取（基线同样付这笔钱）。
  - **"每块边界的脚本 tick"也不是**：没有脚本时它根本不跑；有脚本时基线也跑同一条 tick。
  - `--profile` 真正的边际 = 每块 `++hits` + 每块 `b->ns +=` + **每次 shim 调用两次时钟**；
    纯解释器回路上前两项接近零 ⇒ 膨胀应接近零，shim 密集路径上由第三项主导。
  - 窗口 profiler 的边际见上表；`LIFT_FRAME_TRACE` 比 `--profile` 小但**不为 0**。
  - ⇒ **窗口 profiler 与 `--profile` 是两种仪器**，它们的膨胀比不可互换；`lift-bench` 的读数只代表
    "解释器 + 块缓存"（没有 shim / Present / 宿主泵），⛔ 不能拿它推断真实 boot 的膨胀。

- **口径标签（唯一写法；一行、字段顺序固定、可 grep）**：
  ```
  build=<preset>/<RelWithDebInfo|Debug> · git=<sha> · present=<headless|window> · raster=<cpu|gpu> · clock=wall · instr=<运行摘要 instr: 行逐字>
  ```
  `instr :` 行由**运行摘要无条件打印**（`profile=… frame-trace=… window-prof=… expect=…`），
  读的是**本轮真实 armed 位**（`window-prof` = `WindowProfiler::armed()`，⛔ 不是"脚本里写了 `profile_begin`"）。
  ⇒ 照抄它，⛔ 不要凭命令行参数回填。

- **N=5 中位数**（至少 3 次）：报 **median + min/max**；两臂比值 = **中位数之比**。
  ⛔ 用均值、⛔ 挑最好的一次、⛔ 只跑一次就下结论。

- **A/B 的唯一合法形状**：同一 build（**同一个二进制**）/ 同一 `present`+`raster` / 同一 `clock` /
  同一输入脚本，**只差一个开关**；一次只 A/B 一个开关。两臂读数**分行写，各带自己的 `instr=` 标签**。

- ⛔ **禁止把开 / 关 profiler 的数字混进同一张表**：同一张表只放**同一口径**的读数。一次 A/B 报告里
  两臂可以同时出现，但必须**分成两行 / 两组**、各自带标签；⛔ 不许排成一条序列、⛔ 不许把"开着的
  `ms/M`"当"这份实现多快"（那只是"这份实现 + 这台仪器"的读数）。

- **怎么量（无安装，实验室）**：`tests/bench.cpp` 的 `lift-bench`（顶层 pure，自造字节，不建窗口）：
  ```
  cd apps/emulator-lift && mkdir -p ../../.tmp/profile-ab
  for i in 1 2 3 4 5; do ./out/tests/lift-bench 50000000          > ../../.tmp/profile-ab/bench-off-$i.log 2>&1; done
  for i in 1 2 3 4 5; do ./out/tests/lift-bench 50000000 --profile > ../../.tmp/profile-ab/bench-on-$i.log  2>&1; done
  grep '^bench' ../../.tmp/profile-ab/bench-*.log   # 每臂 5 个 MIPS 读数 ⇒ 取中位数
  grep '^instr' ../../.tmp/profile-ab/bench-*.log   # 两臂的标签必须只差 profile=
  ```

- **怎么量（有安装，真实工作负载）**：两臂 = 同一份时间线，只差一个开关；宿主墙钟 N=5 取中位数。
  ★ **2026-11-16：`--input-script` 的 CLI 已退役**（`docs/03-emulator/input-plan.md` §7）⇒ 下面这条
  **解释器臂**的 A/B 命令**跑不了**；保留为历史方法 / 读数口径。重开＝AOT 侧压力档按
  `REQ-01M7HQJEEENYDJ91FB5DAC4K23` 重写（配方驱动），⛔ 不复活 `--input-script`：
  ```
  # ① 窗口 profiler 的 A/B：.tmp/profile-ab/room-off.txt 与 room-on.txt **只差**这两行
  #      at insn <A> / profile_begin enter / at insn <B> / profile_end enter
  #    ⚠ 锚点按 M-05 在真安装上录（`at insn`），⛔ 不要抄别人的帧号。
  # ② --profile 的 A/B：同一份脚本，on 那一臂只多一个 --profile（⛔ 不要同时开窗口 profiler）。
  for i in 1 2 3 4 5; do
    /usr/bin/time -p ./out/emulator-lift --game-dir <安装> --skip-logo ... \
      --input-script ../../.tmp/profile-ab/room-off.txt \
      > ../../.tmp/profile-ab/room-off-$i.log 2> ../../.tmp/profile-ab/room-off-$i.time
    /usr/bin/time -p ./out/emulator-lift --game-dir <安装> --skip-logo ... \
      --input-script ../../.tmp/profile-ab/room-on.txt \
      > ../../.tmp/profile-ab/room-on-$i.log  2> ../../.tmp/profile-ab/room-on-$i.time
  done
  ```
  Windows 用 `Measure-Command { ... }` 取墙钟。⛔ Debug 构建的墙钟一律不许当判据（AGENTS §5）。
  开跑前先核对两臂的 `instr :` 行**只差一个字段**；不满足 ⇒ 这次 A/B **作废**（⛔ 不"尽量比"）。

- **读数落点**：命令输出与 `time` 落 `.tmp/profile-ab/`（gitignored）；**结论文本**进
  `observations.md` 的新 `OBS-<yyyymmdd>-NN`（旧条不删，按该文件头注规则）。本 M-02 不保存任何读数。

- **旧读数（历史，⛔ 不是不变量）**：`OBS-20261108-09` 记的 **+33~40%**（另有一次 1.7×）是**那次构建 /
  那份脚本**的读数，它引用的复现路径（`.tmp/room/enter-prof.txt` ↔ `enter-noprof.txt`、`.tmp/req-bodies/E.md`）
  **今天已不存在** ⇒ 按本文件自己的判据（"两个人各自按这条跑，读数能对齐"）它**不可复现**。
  保留它作历史；⛔ 不许把它写成"当前膨胀 X%"。它的**归因**（"每块 `steady_clock` + 每块边界的脚本 tick"）
  已由上面"膨胀从哪来"更正 —— 那两项在基线同样付。

- **守卫（不变量，不是量级）**：CTest `profile-window` 的 `profile-instrumentation-invariant`：
  四臂（全关 / 仅 `--profile` / 仅窗口 profiler / 两者）跑同一份程序，
  **`insns` 必须全等**、C/D 两臂的窗口 begin/end insns 相等、窗口报告只在 C/D 出现。
  ⇒ 它守的是"**仪器不许改执行流**"（改了 ⇒ 两个读数根本不是同一条路径上的）；⛔ 它不断言耗时或百分比。

### M-03 · profiler 窗口表的闭式怎么读
- **闭式**：`wall = guest + shim + native + interp + host + unattributed`，六项**互斥**、来自 dispatcher 的同一条时间轴。
  `guest` = 块执行；`shim` = `ShimRegistry::invoke` 整段；`native` = 函数级/段级 override；
  `host` = 音频泵 / 事件泵 / 脚本 tick / 定时器；`interp` = 一次迭代里剩下的**全部**；`unattributed = wall − 其余`。
- **残差怎么判**：正 = 有没归因的时间，负 = 有重叠/重计，**两种都不许吞**。实测正常态 **2.1–3.3 ms**（`observations.md` OBS-10）；
  见过 **22.1 ms** 的一次（那一窗 `shim` 只 602.7 ms，属"短窗 + 边界错位"的量级）⇒ 残差相对 wall 看，别只看绝对值。
- **三条读法红线**：① `gpu :` 行是 `Present` 的内部拆解（readback/hash/stats/sink），**⊂ `shim`**，再加一遍闭式立刻破；
  ② `guest fns` 的 `inclusive` 是调用树（父含子），⛔ **不可相加**、不进闭式，只有 `self` 求和 = `guest`；
  ③ 窗口**未闭合**（没写 `profile_end` / 提前停）会**自动收口 + 告警 + 照样打表**，看到告警就别把这张表当完整窗。
- **怎么量**（在 `apps/emulator-lift` 下）：
  ```
  LIFT_FRAME_TRACE=1 ./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install \
    --present window --present-hidden --native-overrides all --skip-logo \
    --input-script ../../.tmp/room/enter-prof.txt
  ```
  窗口表在 `profile_end` 时打一次（`=== profile window: <label> ===`）。守卫只剩"未闭合必须告警照打"一条（CTest `profile-window`）。

### M-04 · 令牌 / 等锁的墙钟会**污染 shim 计时**，必须分开看
- **为什么**：`shim` 桶量的是 `ShimRegistry::invoke` 的整段墙钟，而某个 shim 的墙钟里可能**大部分在等令牌**
  （`GuestToken` 是"任一时刻最多一个宿主线程持有令牌"）。看 `shim` 排行会得出"这个 API 很慢"的错结论。
- **判据**：把"分配器/缓存自己的账"与"等"分开。实测（过渡帧）`HeapAlloc` shim **1115.1 ms × 1854** 而同一帧
  `heap : … alloc_ms=45.0 free_ms=6.0` ⇒ 分配器自报 51.0 ms ⇒ **≈98% 在等**；另一次 `1543.5 ms` 对 `416.8 ms` ⇒ ≈73%
  （`observations.md` OBS-11/12）。
- **怎么量**：运行摘要里 `heap :` / `arena :` 两行是**自己的账**，`shim :` 行是**含等的账**，两张表都打出来对照；
  token 侧另有独立一栏 `token_wait_ms=N×次数` 与 `token : violations=0 max-concurrent=1`（`--assert-guest-token` 判红）。
  ⛔ 三栏不可相加；⛔ 换一个没开 token 计数器的构建，那一栏会变成 `0.0×0`（OBS-12 实测）⇒ 别把"没开"读成"没有"。

### M-05 · 跨运行复现用 `at insn`（或 `at guest_ms`），**不用** `at frame`
- **为什么**：帧号跨运行会漂（实测同一 `at insn 500000000` 落在 `frame=107` 与 `frame=114`；`at frame` 的漂移量在
  `.tmp/roomopt/enter-clean.txt:2` 被记成 **85–90** 帧）。原因是同一条时间线在不同构建/不同仪器下每帧的指令数不同，
  而**指令数**才是稳定的量。
- **另一条更硬的理由**：房间里帧号**根本不推进**（OBS-06）⇒ 退出房间的场景用 `at frame` 连窗口都对不上。
- **怎么量**：脚本里所有窗口边界写 `at insn N`；`at frame N` 只用于**同一次运行内**的相对窗口（`wait k frames`）。
  现成样式：`.tmp/roomopt/enter-clean.txt` / `exit-clean.txt`（注释里写清了为什么）。
  `--input-script` 的触发集合 = `at frame / at guest_ms / at insn / at wall_ms / after / wait`（启动行会打出来）。
  ★ 2026-11-16：该入口已退役 ⇒ 本条原则（**指令数比帧号稳**）保留；重开时用配方 / AOT 压力档的同口径锚。

### M-06 · "动作发出去"不等于"guest 看见了"：用采样数 / 像素差分，不用帧号
- **为什么**：`click` 的按下短于 guest 采样周期会被**静默吞掉**（BUG-04）；此时帧号不动、退出码也可能是 0。
- **判据（三条，任取两条交叉）**：① 脚本侧 `waited_samples` / `hold_ms` / `post_sample_ms`（`[input-script]` 行）；
  ② guest 侧 `kDownSeenPolls`（守卫里）；③ **像素差分**（逐帧 BMP 的 sha256 + 差异 bbox，OBS-06 的 `29035ad3…` 就是这么来的）。
- **怎么量**：`--dump-frame` + 逐帧文件名，再比 sha256；点击前后**同一内容**的连续帧才是"没落地"的证据。
  ⛔ 只看"运行结束退出码 0"不算证据（脚本的兜底 `quit` 会正常收口）。

### M-07 · 音频：`underruns` 是**弱指标**，`fill` 墙钟才是；Debug 一律不许用
- **能比**：同一构建下 `fills` / `fill_bytes` / `mixed` / `muted_bytes` 与**每次 fill 的墙钟**；`--sound` 开/关的交付字节数应**相等**。
- **不能比**：`underruns` 跨版本（2026-11-13 起它 = **设备队列空 ∪ 环陈旧** 的 episode 计数；
  "环陈旧" = 宿主替 guest 插静音，判据是**写见证** `[stream+0x2468]` 在那一刀前后变了，
  ⛔ 不是内容比对。分项看 `underrun_silence_events`（episode）与 `underrun_silence_bytes`（输出字节）；
  `guest_silence_fills` 是**guest 自己**"放完 + memset 0"的静音，⛔ 不要和宿主插的静音混读）；Debug 构建的任何音频结论
  （AGENTS §5：Debug 下解释器慢到 boot skip-logo > 30 s，fill 必然 underrun）。
- **怎么量**：`LIFT_AUDIO_TRACE=1` 打每次 `[audio] feeder fill slot=N quarter=Q <ms> ms`（实测 BGM 环 **309.7–448.9 ms**，OBS-14）；
  摘要 `audio :` 行给 `fills/mixed/played/notify/dropped/underruns/muted_bytes`。
  并发修复的 A/B **必须用同一份代码的"令牌开/关"对照**（`.tmp/audiord/*-summary.txt` 的 12 次整轮墙钟，OBS-15）。

### M-08 · CTest 耗时的读法（`CTestCostData` 会失真）
- **能比**：同一棵 `out/` 树、同一档（顶层注册 = pure / 条件分支里 = assets）之间的相对耗时。
- **不能比**：跨树、跨构建类型；也不能拿 `CTestCostData.txt` 当绝对耗时 —— 它按**上一次运行**滚动累计，
  且 `out/` 可能是 **stale** 的（实测 `apps/emulator-cxx/out` 的注册数比 `CMakeLists.txt` 少 3 条，
  ⇒ "写了守卫"与"守卫在跑"是两件事，`.tmp/test-audit/report.md` §1.1）。
- **怎么量**：`ctest --test-dir apps/emulator-lift/out -N` 数注册条目；`ctest -R <name> --output-on-failure` 单跑；
  要耗时就用 `ctest --test-dir … --output-on-failure` 的本次输出，不看缓存文件。

### M-09 · 仪器化**计数**与**墙钟**分两栏记，永远不换算
- **为什么**：计数是"做了多少次比较/合并/提交"，换一份实现后可以差几个数量级而行为完全不变（OBS-13：首次适配 **47.57M → 0.52M** 次比较、
  `allocs`/`blocks`/`zero` 三个结果面**一个都没变**）。把计数当性能成绩，会让"优化"变成数字游戏。
- **能比**：同一实现的两次运行（判"这次真的走了快路径吗"）；同一 A/B 的两臂（判"省的是哪一段"）。
- **怎么量**：`heap : allocs=… scan=… coal=… alloc_ms=… free_ms=…` / `arena : surfaces N in_use U peak P largest-free F` /
  `token : violations=… max-concurrent=…`。★ `arena` 的 `largest-free` 是**最大连续**空闲块：
  `in_use` 没满但 `largest-free` 很小 = **碎片化**，不是"总量用完"（BUG-01 的判别就靠这一格）。

### M-10 · "两次动作的 `guest_ms` 差 = 宿主墙钟"
- **为什么能用**：时钟唯一档 = **墙钟** ⇒ 宿主 `steady_clock` 直接进 guest 时钟，`guest_ms` 就是宿主毫秒。
- **怎么核（做过一次，误差 ≤1 ms）**：把 profile 窗口的 `wall=` 与同一对锚点的 `guest_ms` 差逐字对照 ——
  `enter-prof2` 的 `8085.1 ms` 对 `guest_ms` 差 `8085`；`exit-prof2b` 的 `9180.4 ms` 对 `9181`
  （`.tmp/roomopt/run.ps1` 文件头记着这次核对）。
- **代价**：无（墙钟是唯一档；headless 与 window 同一条时钟）。
  （原句写的 `--real-time` / "虚拟时钟"已于 2026-11-27 整档移除。）
- **怎么量**：`pwsh .tmp/roomopt/run.ps1 -Mode enter -Label <名字> -Runs 3`（脚本里没有任何 `profile_*`，
  从 `[input-script]` 行取 `guest_ms` 采样）；两次动作之间取差，就是那段的宿主墙钟。

### M-11 · 令牌 Phase-0 的四读数：**只读比例/分布**，且分母不是 run wall
- **字段**：`tokenhold`（持有占空）· `holdhist`（一次不间断持有的五桶：<1/1–5/5–20/20–50/>50 ms）·
  `waithist`（每次等待的五桶）· `shimmax`（每 shim 的最大墙钟 + **记账期内**调用数）。
- **为什么要有**：M-04 说清"shim 墙钟含等"，但没回答"一次到底持了多久"。实测 SE 播完那次收尾：feeder 在 **355 次边界**上共等 **13.79 s**，
  单次等待中位 **43.1 ms**、最大 440.8 ms，而那几刀的真活只有 **1.4–2.5 ms** ⇒ 1.3 s = **边界数 × 交接延迟**，
  既不是锁开销、也不是解码慢。⇒ 修的是"**持有上界**"，不是"公平性"。
- **口径（⛔ 三条不许混）**：
  ① `tokenhold` 的分母是**令牌观测窗口**（首次持有 → 最后一次释放），**不是 run wall**（两者差 boot 前后一小段）；
  ② `shimmax` 的 `calls` 是**记账期内**调用数，⛔ 不是总调用数；
  ③ `shimmax` 的读时钟门比字面宽（`timing_on || audio_trace`，后者含 `LIFT_FRAME_TRACE` / window-prof / `--overlay`）。
- **代价**：默认（无 `--profile` / `LIFT_AUDIO_TRACE`）零额外时钟、零原子写，摘要逐字节不变；
  **打开时 ≈ +45–50 ns/块 ⇒ ≈ +5–6% 墙钟** ⇒ ⛔ **`--bench --profile` 的 MIPS 不能当性能基准**。
- **怎么量**：`--profile` 或 `LIFT_AUDIO_TRACE=1` 后读摘要这四行，与 `tokenwait`（M-04）**并列看**，⛔ 不要相加。
- **A/B 的判据（⛔ 别拿"第 N 帧的 insns"）**：guest 时钟 = 墙钟（F 唯一档）⇒ 两臂的 guest 时钟**各自**跟宿主墙钟走，
  **帧号 / 逐帧 `insns` 不可比**（两臂跑到同一帧号时并不是同一 guest 状态；
  实测两臂只到 1542 / 1463 帧，且同一帧号的 `insns` 早期吻合、后段分叉）。A/B 只能用：
  ① `insns_unattributed == 0`（native override 补账与块级闭合的机械判据）② **计数器**（`insns` / `fills` / 命中数）
  ③ assets 档的 `cpu.insns` 精确相等。

### M-12 · AOT 窗口 profile（`profile_begin/end` 之间的分桶账）：闭合靠**包络**，份额靠**采样**

- **字段**（`=== profile window: <label> ===`，`end` 之后打一次）：
  `wall`（T0→T1，`steady_clock`，已扣控制层暂停）· `lift` · `boundary` · `shim`（+ `present` / `shim:<module!name>` 子行）·
  `partial` · `instrument` · `unaccounted` · `coverage` · `lift_dyn` · `verdict`。
- **桶的定义（AOT 专有；⛔ 不要照抄解释器的六项）**：
  · `lift` = `fn(...)`（一个 lifted 根函数的执行）；
  · `boundary` = **块顶 → 分派前**：控制层闸门 / 停机请求 / `--time-limit` / `pumpHostEvents` / 三个合成 EA 泵 /
    定时器泵 —— 这是解释器那套里 `interp` 的对偶（派发循环自身的税）；
  · `shim` = `lift_shim` 整段（**精确**：每次调用 2 次取时）；
  · `partial` = 窗口两个边沿的 sliver（开/关落在某个区间中间 ⇒ 那半截单独记）；
  · `instrument` = 仪器自己花掉的时间（每次取时 ~20–35 ns，**单列**，⛔ 不塞进任何业务桶）；
  · `unaccounted` = 定义式 = `wall − 上面五项`；**`>3%` ⇒ 本次读数作废**（打印 `INVALID`）。
- **⚠ 2026-11-17 的实测教训（这条是 M-12 存在的主要理由）**：**不能**把采样均值直接外推成绝对时间。
  每次采样自带的取时开销落在**被计时的区间里**，而外推要乘 `iter/sampled`（≈64）⇒ 30 ns 被放大 64×。
  实测 FELLOW init：直接外推得 **3.1 s**，而墙钟只有 **2.0 s**。⇒ 现在的口径是
  **绝对量取精确包络 `wall − shim − partial − instrument`，采样只决定 `lift:boundary` 的比值**；
  `unaccounted` 因此结构上恒 ~0，**取代它的自检是 `coverage`**（= 采样外推 / 包络，1.00 才吻合；
  实测 1.0–1.5 波动、只作诊断，离谱到 `[0.5, 2]` 之外才判作废）。
- **采样方式**：每迭代一次取时决策，用**乘性散列取高 6 位**（平均 1/64）。⛔ 不许写 `iter % 64 == 0`：
  实测那会与 guest 循环周期对齐、恒定采到同相位（外推偏高 1.6×）。`iter` 是**窗口内迭代数**（含未分派的迭代）。
- **成本**（实测，`--profile` **不需要开**）：`shim` 2 次取时/调用；`lift`/`boundary` 2 次/64 迭代；
  `present` 子桶 **0**（`presentBreakdown()` 本来就一直在计时）；`lift_dyn` 一次 `bool` 判断 + 一次 `++`。
  整体 ≈ +2–4% 墙钟（见 `instrument` 行）。**默认档（不开窗）一行不打、一次取时都不做**。
- **`lift_dyn` 只报次数**：它是**尾调用**（`return fn(...)`）⇒ 区间含目标函数体，报时间会与 `lift` 双计。
  今天它是唯一能看见"间接跳转那一大批派发"的读数（不计 dispatch、不进 liftone 采样）。
- **优化候选的判定（写死，单口径）**：任何**具名行**占 `wall` **≥10%** ⇒ 候选；3–10% ⇒ 观察名单（需两次读数复现）；
  <3% ⇒ 噪声。最高具名行 <10% ⇒ 结论写"**纯 CPU 均匀分布，暂不优化**"（合格交付，⛔ 不为凑产出挑 6% 的行）。
  `lift` 是**聚合**行，⛔ 不进候选（要看它内部就上按 EA 展开）。
- **读数（2026-11-17 / win32 / RelWithDebInfo / N=3 / 配方 `.tmp/wprof/fellow.plan`）**：
  `fellow-init` `wall` **2209 / 2302 / 2270 ms**（中位 2270）· `insns` **612.4 / 612.4 / 612.7 M**（±0.06%）·
  `lift` **50.0 / 50.1 / 49.7%** · `boundary` **28.7 / 29.9 / 29.1%** · `shim` **18.6 / 17.5 / 18.5%** ·
  `coverage` 1.20–1.22 · `frames = 0`（整段零 Present）。
  最大的单条 shim 是 `user32!PeekMessageA` **5.3%**、`gdi32!DeleteObject` **5.1%**、`winmm!timeGetTime` **3.8%**
  ⇒ **没有单条 shim ≥10%**；`boundary` 是唯一 ≥10% 的具名行。
  `fellow-enter`（转场，~200–260 ms）：`shim` **92–94%**，其中 `D3D9::Present` ~32% / `D3D9::Clear` ~31% / `ID3DXSprite::End` ~16%。
- **怎么量**：配方里写 `profile_begin <label>` / `profile_end <label>`（`docs/03-emulator/input-plan.md`），
  跑 `emulator-lift-aot --present headless --skip-logo --game-dir links/install --appdata <本树 .tmp> --input-plan <plan>`；
  ⛔ 必须显式给 `--appdata`（否则往安装树里写）。守卫：CTest `aot-window-profile`（pure，纯算术夹具）。

### M-13 · AOT **按帧** shim 分桶（`--profile-slow-frames <ms>`）：窗口的补集，不是替代

- **要解决的问题**：窗口桶（M-12）按**窗口**切，一段 1800 ms 的窗里只有 ~10 帧是真正要看的、其余 ~118 帧把
  `ms/帧` 与具名行的 `ms/次` 都稀释掉（实测 wipe：`ID3DXSprite::End` 窗口 **0.223 ms/次** vs 条带帧 **0.86–1.34 ms/次**，
  ~4× 差）。⇒ 需要一个"**只保留慢帧**、且按**单帧**分桶"的读数。
- **CLI**：`--profile-slow-frames <ms>`，**必须与 `--profile` 同时给**（缺 ⇒ `exit 2` 并点名两者；值 0 = 关）。
  缺省 0 ⇒ **一行不多打、一次取时都不多做**；`--profile` 的产物（`frame-ms:` 等）逐字不变。
- **字段**（run 结束打**一张**表，⛔ 不每帧打）：`slow-frames: frames / slow / listed / dropped` +
  每帧 `slow-frame: n / wall_ms / insns / shim_ms / shim_calls / presents` + 每帧 top-8 `shim（名字 + ms + calls）`。
- **时间口径（唯一一份）**：
  · `wall_ms` = `frame-ms:` 的 `t_ms` **帧间差** —— 同一把尺 ⇒ 阈值可以直接与既有 Δt 读数对照；
  · `shim_ms` = **M-12 的 `shim` 桶那条精确计时**（`windowNowNs()` 包住 `lift_shim_id` 的同一对取时）⇒ 一次调用只付一笔取时税；
  · **只统计主线程**：打点在主执行循环里 ⇒ 音频 feeder 线程的 shim **结构上进不来**（要量 feeder 看 `tokenhold` / `shimmax`）；
  · `insns` = `cpu_.insns` 的帧间差（该帧 guest 指令数）。
- **有界**：慢帧 ≤ 32 条、每帧 ≤ 8 个名字；超出**只报数**（`dropped=N` / `另有 M 个 shim`）⇒ 撞上界时
  "没看到"≠"不存在"。
- **归因红线**：本表只回答"**这一帧**是哪个 shim 花掉的"，不回答 shim **内部**怎么花的（没有子桶）；
  且它是**诊断档** ⇒ ⛔ 不进任何性能判据、⛔ 不与关档 run 比墙钟。
- **怎么量**：`--present headless --skip-logo --audio off --profile --profile-slow-frames <ms> --max-frames N --input-plan <plan>`
  （`--appdata` 用**副本**，⛔ 不写用户原件）。守卫：CTest `aot-frame-bucket`（pure：名字合并 + 有界 top-N + 阈值门 + 关档）
  与 `cli-aot-flags` 的 `profile-slow-frames-*`（纯 CLI 面）。读数见 `wipe-profile.md` 的
  `## 按帧 shim 分桶`（2026-11-18）。

### M-14 · AOT 的两个诊断环境变量（`--help` 指向这里）

两个都**只在启动期解析一次**（早于 `AotHost::init` / 音频线程），未设 / 空 ⇒ 零新增输出。
**超限 / 语法错 ⇒ `exit 2`**（⛔ 不静默截断、⛔ 不静默当成普通段）。

**`LIFT_FAULT_DUMP=<spec>`** —— guest fault 时按 spec dump 内存（缺省不设 = fault 文本一条 dump 行都不多）。

- 每段三种写法，逗号分隔：
  - `EA:LEN` —— EA 纯 hex（`0x` 可省）；LEN 十进制，`0x` 前缀 = hex；
  - `EA:*LEN` —— 先读 EA 处 4 字节当 guest 指针，再 dump 该指针起 LEN 字节（`EA:4*LEN` 同义）；
  - `EA:*<delta>:LEN` —— 跟随指针并加有符号 delta（`|delta| ≤ 1 MiB`），例如 `*-8:16` = 从 `P-8` 起 dump。
- 上限：**8 段 / 每段 1..256 B / 总 ≤ 1024 B / spec ≤ 1024 字符**。
- 例：`LIFT_FAULT_DUMP=0x1005AB58:64,0x10050000:*32,0x1005AB58:*-8:16`

**`LIFT_WATCH=<spec>`** —— 观察 guest 侧写入（`lift_note_write`）与 shim 侧写入（`writeBytes` / `fillBytes` /
`wr8..wr64`）（缺省不设 = 一条 watch 行都不打）。

- 每段：`EA:LEN`（EA 纯 hex；LEN 十进制 1..16）或 `EA:LEN>follow`（仅 LEN=4：命中后把**下一次写**开头读到的
  指针 `P` 的 `[P-8, P+8)` 加进观察集 —— 用来点名"谁在 `P-8` 写了那 16 字节"），逗号分隔。
- 上限：**8 段 / spec ≤ 1024 字符**。
- 命中打一行：`watch      : WRITE 0x<EA> n=<N> block=lift_cc_<EA>`（guest）或 `… shim=<名字>`（shim 侧；
  名字由 shim 调用点设，未设 ⇒ `?`）。
- 例：`LIFT_WATCH=0x1005AB58:4>follow`

### M-15 · AOT `--profile` 的采样率与 `--profile-slow-frames` 的字段

- **`--profile` 是采样档**（不是逐次计时）：三个 stride 是常量，真源在 `src/aot/aot_host.h` 的 `AotProfile`：
  - 派发归因 `dispatch`：**1/4096**（`kDispatchStride`；`ms` 列是"派发份额"的估计，⛔ 不是该 EA 的实测成本）；
  - `lift_one` 单次耗时：**1/1024**（`kLiftOneStride`；逐次计时与被测对象同量级 ⇒ 压到 ~0.1%）；
  - shim 归因：**1/64**（`kShimStride`，按**每个 shim 自己**的调用序号 —— 全体 1/1024 会让只调几百次的
    `D3D9::Present` 拿到 0–1 个样本）。
  - 三个 stride 也在 `window-dispatch-top` 的头部行按 `stride 1/N` 打出（⛔ 不手抄数字）。
- **`--profile-slow-frames <ms>`**（必须与 `--profile` 同时给；值 0 = 关；缺省 0 ⇒ 一行不多打）：
  run 结束打**一张**表 `slow-frames: frames / slow / listed / dropped`，每帧一行
  `slow-frame: n / wall_ms / insns / shim_ms / shim_calls / presents`，紧跟该帧 **top-8 shim**
  （`module!name · ms · calls`）。口径与有界规则见 M-13。
