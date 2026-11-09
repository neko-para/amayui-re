# 测量口径（measurement）

> 这个文件记 **"怎么量才算数"**。每条 = 能比什么 / 不能比什么 + 一条能重跑的命令。
> ⛔ 仪器化读数不是性能成绩；⛔ 跨口径的读数不许放进同一张表；⛔ 读数过期不删（见 `observations.md` 的编号规则）。
> 编号 `M-NN`。口径的**判据**是"两个人各自按这条跑，读数能对齐"。

---

### M-01 · 墙钟只能在**同口径**内比较
**能比**：同一构建 / 同一 `--present` 档 / 同一时钟档 / 同一输入脚本 / 同样开没开 profiler 的两次运行。
**不能比**：换构建、换 `--present window` ↔ `headless`、换 `--real-time` ↔ 虚拟时钟、换有无 `--profile`。
- **反例（实测）**：同一条 151M 条的 TITLE 重载帧，`.tmp/exitstall` 留档读 `55.3–57.7 ms/M`，
  `.tmp/clickfix` 留档读 `48.1–49.3 ms/M`（`observations.md` OBS-04）⇒ 差 14%，纯口径差异。
- **另一个反例**：窗口模式会**自动**开 real-time（启动行 `⚠ 窗口模式（自动 real-time）`），
  于是 `--present window` 的读数天然带宿主时钟；`--present headless` 不带 ⇒ 两者不可混。
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
  build=<preset>/<RelWithDebInfo|Debug> · git=<sha> · present=<headless|window> · raster=<cpu|gpu> · clock=<virtual|REAL-TIME> · instr=<运行摘要 instr: 行逐字>
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
  for i in 1 2 3 4 5; do ./out/lift-bench 50000000          > ../../.tmp/profile-ab/bench-off-$i.log 2>&1; done
  for i in 1 2 3 4 5; do ./out/lift-bench 50000000 --profile > ../../.tmp/profile-ab/bench-on-$i.log  2>&1; done
  grep '^bench' ../../.tmp/profile-ab/bench-*.log   # 每臂 5 个 MIPS 读数 ⇒ 取中位数
  grep '^instr' ../../.tmp/profile-ab/bench-*.log   # 两臂的标签必须只差 profile=
  ```

- **怎么量（有安装，真实工作负载）**：两臂 = 同一份时间线，只差一个开关；宿主墙钟 N=5 取中位数：
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
    --present window --present-hidden --raster gpu --native-overrides all --skip-logo \
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

### M-06 · "动作发出去"不等于"guest 看见了"：用采样数 / 像素差分，不用帧号
- **为什么**：`click` 的按下短于 guest 采样周期会被**静默吞掉**（BUG-04）；此时帧号不动、退出码也可能是 0。
- **判据（三条，任取两条交叉）**：① 脚本侧 `waited_samples` / `hold_ms` / `post_sample_ms`（`[input-script]` 行）；
  ② guest 侧 `kDownSeenPolls`（守卫里）；③ **像素差分**（逐帧 BMP 的 sha256 + 差异 bbox，OBS-06 的 `29035ad3…` 就是这么来的）。
- **怎么量**：`--dump-frame` + 逐帧文件名，再比 sha256；点击前后**同一内容**的连续帧才是"没落地"的证据。
  ⛔ 只看"运行结束退出码 0"不算证据（脚本的兜底 `quit` 会正常收口）。

### M-07 · 音频：`underruns` 是**弱指标**，`fill` 墙钟才是；Debug 一律不许用
- **能比**：同一构建下 `fills` / `fill_bytes` / `mixed` / `muted_bytes` 与**每次 fill 的墙钟**；`--sound` 开/关的交付字节数应**相等**。
- **不能比**：`underruns` 跨版本（它只在 SDL 队列 == 0 时计）；Debug 构建的任何音频结论
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

### M-10 · "两次动作的 `guest_ms` 差 = 宿主墙钟"这条口径怎么核
- **为什么能用**：窗口模式 = real-time 时钟 ⇒ 宿主 `steady_clock` 直接进 guest 时钟，`guest_ms` 就是宿主毫秒。
- **怎么核（做过一次，误差 ≤1 ms）**：把 profile 窗口的 `wall=` 与同一对锚点的 `guest_ms` 差逐字对照 ——
  `enter-prof2` 的 `8085.1 ms` 对 `guest_ms` 差 `8085`；`exit-prof2b` 的 `9180.4 ms` 对 `9181`
  （`.tmp/roomopt/run.ps1` 文件头记着这次核对）。
- **代价**：这条只在 real-time 档成立；`--present headless` / `--audio virtual` **不成立**（虚拟时钟）。
  ⇒ 用 `guest_ms` 当墙钟前，先确认启动行有 `⚠ 窗口模式（自动 real-time）`。
- **怎么量**：`pwsh .tmp/roomopt/run.ps1 -Mode enter -Label <名字> -Runs 3`（脚本里没有任何 `profile_*`，
  从 `[input-script]` 行取 `guest_ms` 采样）；两次动作之间取差，就是那段的宿主墙钟。
