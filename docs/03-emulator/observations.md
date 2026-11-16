# 观察账（observations）

> 记 **我们测到过什么**。一条 = 一句话 + 一条能重跑的命令 + 一个读数 + 一个口径 + 一个日期。
> ⛔ 不写"所以要优化 X"（那是需求单的事）；⛔ 读数不是断言（断言在 `contracts-win32.md` 的守卫里）。
> 编号 `OBS-<yyyymmdd>-NN`；**过期不删**，同一条再测出不同读数时新开编号并写"取代 OBS-…"。
>
> **口径A** = RelWithDebInfo（`cmake --preset windows-x64`）/ Win10 x64 / `--present window --present-hidden`
> （启动行 `present : driver=gpu-direct`、`raster : sdl-gpu/direct3d12/DXBC`）/ 有安装树
> （`--game-dir ../../links/install --appdata ../../.tmp/appdata`）/ `--native-overrides all --skip-logo` /
> 输入脚本驱动 / **无** `--profile`、**无** `LIFT_FRAME_TRACE`。命令骨架（`apps/emulator-lift` 下）：
> `./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install --present window --present-hidden --native-overrides all --skip-logo --input-script <脚本>`
> ★ **2026-11-13 起 `--raster` 已删除**（GPU 是唯一光栅实现）：本文早于那天的条目里出现的 `--raster cpu|gpu`
> 是**当时的命令原文**，照抄今天会 `未知参数：--raster` 退出 2 —— 复现那些条目时把它删掉。
> ★ **2026-11-16 起 `--interaction-graph` / `--interaction-trace` / `--interaction-fail-dump` 已退役**
> （配方只经 AOT 臂 `--input-plan`，落点 `apps/emulator-lift/scripts/recipes/*.plan`）：本文里出现的
> `--interaction-graph <图>` 同样是**当时的命令原文**；要重跑同一份配方就换成
> `emulator-lift-aot ... --input-plan scripts/recipes/<名>.plan`（⚠ real-time 档），
> 而那条命令里的 `--interaction-trace` / `--interaction-fail-dump` **没有替代品**（入口已删）。
> 读数属于哪个工作树：`git log -1 --format=%h`（本文取自 2026-11-08 `2bd8eaf` 附近）；换构建后不要混表（`measurement.md` M-01）。

## 过渡（进出 EU-ROOM）

### OBS-20261108-01 · 进 EU-ROOM 的整段墙钟被**一帧**吃掉
- **读数**：过渡帧 `wall=5421.4 ms / 75,900,857 条`，另两次 `5589.7 ms / 75,901,179`、`5171.7 ms / 75,901,984` ⇒ **75.9M 条 / 4.89–5.59 s**。
- **复现**：`pwsh .tmp/roomopt/run.ps1 -Mode enter -Label heapmeas`（脚本 `.tmp/roomopt/enter-clean.txt`）· 口径A + `LIFT_FRAME_TRACE=1` · 2026-11-08 · `[.tmp/roomopt/heapmeas2-enter.log:592]`、`[heapmeas3.log:592]`、`[heapmeas4.log:580]`

### OBS-20261108-02 · 过渡帧两侧的稳态帧只有 ~28.5k 条
- **读数**：`insns=28472 / 28476 / 28485`、wall 20–60 ms；过渡帧 75.9M ⇒ **比值 ≈2.7×10³**（"进房间慢"是单帧内的活，不是整体变慢）。
- **复现**：同 OBS-01 的日志 `[frame] n=102..107` · 口径A + `LIFT_FRAME_TRACE=1` · 2026-11-08 · `[.tmp/room/enter-frames.log:550-575]`（同文件 `:545` 是过渡帧 `n=101 wall=6162.2ms / 75,901,355 条`，那一次开了 frame trace）

### OBS-20261108-03 · 退出 EU-ROOM：点击→TITLE 中位 9.17 s，其中一帧 151M 条
- **读数**：3 次 `9,210.8 / 8,874.9 / 9,171.3 ms`（中位 **9.17 s**，极差 0.34 s）；TITLE 重载帧 `151.05M/8,691.0 ms`、`151.02M/8,356.5`、`151.01M/8,706.0` = **55.3–57.7 ms/M**。逐帧：`109=21.43M/1675 ms` → `110=1.78M/153` → `111–123≈29.5k/17–31` → `124=151.01M/8706` → `125+≈28 万/帧`。
- **复现**：`--input-script .tmp/exitstall/exit-fix.txt`（按下保持 ≥150 ms）· 口径A · 2026-11-08 · `[.tmp/exitstall/report.md:4,45,51-59]`

### OBS-20261108-04 · 同一个 151M 帧在两个留档构建上差 1.1–1.4 s
- **读数**：`.tmp/exitstall` 记 `151.05M / 8,691 ms`（55.3–57.7 ms/M）；`.tmp/clickfix` 记 `151,040,106 / 7325.7 ms`（另两次 7,246.4 / 7,448.5）⇒ 48.1–49.3 ms/M。
- **复现**：两处都用 `LIFT_FRAME_TRACE=1`；结论**只用在这里** —— 跨构建/跨留档不可比，引用 TITLE 重载耗时必须连构建一起引用 · 2026-11-08 · `[.tmp/clickfix/report.md:31-33]`、`[.tmp/exitstall/report.md:55-57]`

### OBS-20261110-01 · 出 EU-ROOM：点「戻る」⇒ TITLE，三次 run 读数逐位相同
- **读数**：`exit 0` / `detail: interaction: task done` / `transitions=11 actions=3 samples=249 changes=44` /
  末帧第 249 帧 `0x94CB492BF7F1AEEB`（TITLE）/ `insns=2,120,728,564`（三次 run 同值）。
  trace：两份 copy 图的 run（`determinism.graph` 两遍）**逐行相同**；本尊 `room-exit.graph` 那份只差
  `task start origin=` 那一行（文件名不同），其余 17 行逐字节相同（`diff` 实测）。
- **复现**：`--game-dir /Users/nekosu/Documents/Projects/amayui-cn/raw --skip-logo --audio off
  --present headless --raster cpu --interaction-graph scripts/interaction/room-exit.graph --interaction-trace <路径>`
  （room 节点 `when screen rect 1200 665 80 45 avg 0xB6957E` + `action click center` ⇒ 实际点 `(1240,687)`）·
  2026-11-10 · macOS arm64 / RelWithDebInfo / `--skip-logo`
- **对照（同一台机、同一构建）**：`click 100 100` / 不点击 / 只 `mouse_move` ⇒ 全部 exit 5（`expect-change(node room)` 或
  `timeout(node back)`）；`click 1100 150`（CG 鑑賞）⇒ 画面变了但不是 TITLE。⇒ 成功可归因于"在按钮上的一次点击"。

### OBS-20261110-02 · 出 EU-ROOM 的时间结构：103 帧退场 + **单帧 730,765,167 条**重载帧
- **读数**（`LIFT_FRAME_TRACE=1`）：抬起帧 n=115 → n=116..218 共 **103 帧** hover 画面（≈29.5k 条/帧）
  → n=219 单帧 **730,765,167 条 / 24.0 s 墙钟** → n=220+ TITLE 动画；重载帧像素 `0xD6ED710894A39FA5`。
- **复现**：同 OBS-20261110-01 的图 + `LIFT_FRAME_TRACE=1` · 2026-11-10 · macOS arm64 / RelWithDebInfo
- **用途**：BUG-16 的误判根因 —— 2026-11-09 那三次 run 的 `quit` 全落在这一个重载帧里（抬起后 +98M/+188M/+350M 条）。

### OBS-20261110-03 · 房间「戻る」的命中区比识别框窄（绘制框左边缘不在命中区）
- **读数**：`(1240,687)`（框中心）与 `(1267,706)`（右下内缩 5px）⇒ exit 0 + TITLE；同框内的 `(1205,670)`
  与框外的 `(1190,690)` 在 900M 条窗口里都 exit 5、无 TITLE。⇒ 识别框 `1200..1272 × 652..711`
  只适合"认出这一屏 + 取中心"，⛔ 不要当精确命中区。
- **复现**：`room-exit.graph` 的 room 节点分别用 `action click <x> <y>` 固定点跑 · 2026-11-10 · macOS arm64 / RelWithDebInfo

### OBS-20261110-04 · 右键点「戻る」⇒ 同一个 TITLE 终态（与左键读数同形）
- **读数**：`room-exit-right.graph` ⇒ `exit 0` / `detail: interaction: task done` / `transitions=11 actions=3
  samples=249 changes=44` / 末帧第 249 帧 `0x94CB492BF7F1AEEB` / `insns=2,120,726,066`。
  与左键版 trace 的差别**只有三处**（`diff` 实测）：`origin=` 行、`action click center right applied`、
  以及 `node back enter … insn=1378353046`（左键版是 `…042`，差 4 条）。
- **复现**：`--game-dir /Users/nekosu/Documents/Projects/amayui-cn/raw --skip-logo --audio off
  --present headless --raster cpu --interaction-graph scripts/interaction/room-exit-right.graph` ·
  2026-11-10 · macOS arm64 / RelWithDebInfo / `--skip-logo`
- **口径**：右键 = 引擎的**取消位**（输入字 bit5，`0x4B9D3F` → `sub_4B8DC0(1)` → `0x411CFE test byte ptr [ebx],20h`），
  取消目标由脚本登记的取消标签决定 ⇒ 这条读数只对"房间里那个取消标签 = 回 TITLE"成立。

## 吞吐与空转

### OBS-20261108-05 · 空转吞吐基线 = 64.7 ms/百万条（≈15.5M insn/s）
- **读数**：`1.5e9 条 / 97 s` = **64.7 ms/M**（无 profiler 的真实速率）。对照 TITLE 重载帧 55.3–57.7 ms/M ⇒ 空转不比"真在装载"更省。
- **复现**：`.tmp/exitstall/exit-y-longpress.txt`（原短按、不落地那一档跑 155 s）· 口径A · 2026-11-08 · `[.tmp/exitstall/report.md:29]`

### OBS-20261108-06 · 房间稳定后不再 Present（帧号不推进）
- **读数**：相邻两窗 `w2=[400M,800M]`、`w3=[800M,1.5e9]` 都是 `Present=0` ⇒ **≥1.1e9 条 0 Present**；逐帧 BMP `frame 100–108` sha256 逐字节相同（`4697b037…`），`frame 109` 才变（差异 bbox = 只剩「戻る」按钮矩形，3022 像素）。
- **复现**：`.tmp/exitstall/exit-shot.txt` + `--dump-frame` · 口径A · 2026-11-08 · `[.tmp/exitstall/report.md:10,19-27]`

### OBS-20261108-07 · 房间的输入采样是**电平**式、约 20 Hz
- **读数**：`GetCursorPos` **880 次 / 43.96 s = 20.02 Hz**；`Sleep` 301,790 次只花 **19.6 ms**（65 ns/次 ⇒ 立即返回）⇒ 按下必须跨过 ~50 ms 采样周期才被看见。
- **复现**：`.tmp/exitstall/logs/prof-stall.log` 的 shim 表 · 口径A + `--profile` · 2026-11-08 · `[.tmp/exitstall/report.md:14,26-27]`

### OBS-20261108-08 · 空转段在做什么（三个热点 EA）
- **读数**（`w3` = 700M 条 / 78.9 s）：`0x412290`（WinMain 主循环）self **9.8%** / incl 27.5%；`0x4BBAB0`（DirectSound 流槽管理）**2,766,573 次 / 78.9 s（35k/秒）**、self 7.1% / incl 18.9%；`0x41BF50`（VM 取操作数）**4,612,006 次**、self 6.5% / incl 14.9%。shim 侧几乎无开销（每 700M 条 `HeapAlloc` 606 次 / `EnterCriticalSection` 308 次）。
- **复现**：`.tmp/exitstall/exit-3win.txt`（`--time-limit 175`，窗口 `[800M,1.5e9]`）· 口径A + `--profile` 窗口表 · 2026-11-08 · `[.tmp/exitstall/report.md:33-38,48-49]`

### OBS-20261114-01 · 派发层固定税 b / 每条 c（`dispatch-tax` 的读数面 —— 判据已移出默认档）
- **读数**（RelWithDebInfo · `windows-x64` · 2026-11-14）：`dispatch-tax: b=66.86 ns/块 c=19.18 ns/条
  lock+unlock=8.25 ns clock=19.88 ns Δb(diag)=10.16 ns dt1=9.91 ns/块`；
  同轮 `microbench` 的 `lock+unlock=8.25 ns` / `steady_clock::now=19.88 ns` / `GuestToken acq+rel=23.35 ns`
  （N=1,000,000 × 7 样本取最小）。
- **复现**：`apps/emulator-lift$ ./out/tests/lift-dispatch-tax.exe`（或 `ctest --test-dir out -R dispatch-tax`）·
  本机 `LIFT_STRESS` **未**开也照跑（当时还在默认档；此后按终裁 3 移档）。
- **口径**：这些是**读数**，⛔ 不是断言；断言面（`kDeltaBTolNs` / `kLockCostFloorRatio` 之类的阈值）随整条
  CTest 移入 `LIFT_STRESS`（效率档），默认 `ctest` 里不再跑它 —— 理由：计时的量级带是效率读数，不是对外合同。

## 测量自身的开销与结构

### OBS-20261108-09 · profiler 的膨胀量级
- **读数**：同工程内实测 **+33~40%**；空转段有一次自报为真速的 **1.7×**（归因给"每块 `steady_clock` + 每块边界的脚本 tick"）。
- **复现**：同一脚本删掉 `profile_begin/profile_end` 两行做 A/B（`enter-prof.txt` ↔ `enter-noprof.txt`，都在 `.tmp/room/`）· 2026-11-08 · `[.tmp/req-bodies/E.md:6]`、`[.tmp/exitstall/report.md:29]`

### OBS-20261108-10 · profile 窗口表的闭式成立，残差 2.1–3.3 ms（一次 22.1 ms）
- **读数**：`buckets : wall=9180.4ms guest=2962.4ms shim=1135.0ms native=22.8ms interp=3041.9ms host=2016.3ms sum=9178.3ms unattributed=2.1ms`；enter 窗 2.2 / 3.3 ms；diag 窗 22.1 ms（该次 `shim` 只 602.7 ms）。
- **复现**：`.tmp/room/exit-prof2b.log`、`.tmp/room/enter-prof.log`、`.tmp/roomopt/diag-enter-diag.log`（脚本 `exit-prof2.txt` / `enter-prof.txt`）· 读法见 `measurement.md` M-03 · 2026-11-08

### OBS-20261108-11 · 过渡帧的 `HeapAlloc` 墙钟 **≈85%（实测 73–98%）不在分配器里**
- **读数**：同一过渡帧 `heap : allocs=17804 … alloc_ms=45.0 free_ms=6.0` 对 `shim : HeapAlloc=1115.1ms×1854 / HeapFree=1267.3ms×1425` ⇒ 分配器自报 51.0 ms、shim 侧 2382.4 ms ⇒ **≈98% 在等**；另一次 `alloc_ms=416.8` 对 `HeapAlloc=1543.5ms×1854` ⇒ ≈73%。平均 **0.6–0.83 ms/次**，比"扫一遍块表"高两个量级。
- **复现**：`.tmp/roomopt/heapmeas4.log:580-582`、`heapmeas3.log:592-594`；`pwsh .tmp/roomopt/run.ps1 -Mode enter -Label heapmeas` · 口径A + 逐帧 `heap/shim` 表 · 2026-11-08

### OBS-20261108-12 · token 等待是**单独一栏**，不在 `shim` 桶里
- **读数**：`heap : … token_wait_ms=1422.4×33234860`（过渡帧）、`1670.2×33208368`（另一次）⇒ 整帧 token 等待 1.4–1.7 s，同帧 `shim` 只 654.9–678.0 ms ⇒ **不可相加**。⚠ 另一次构建 `token_wait_ms=0.0×0`（没开这个计数器）⇒ 只差一栏就足以让两次读数不可比。
- **复现**：`.tmp/roomopt/heapmeas-exit.log:580-582`、`heapmeas2-enter.log:592-594`、`heapmeas3.log:593` · 读法见 `measurement.md` M-04 · 2026-11-08

### OBS-20261108-13 · 堆首次适配 / 收口合并的优化前后（同一过渡帧）
- **读数**：块比较 **47,572,524 → 516,482**（≈92×）；收口合并 **45,812,143 → 2,773**（≈16.5k×）；而 `allocs`（17,808→17,804）、`blocks`（7131→7130）、`zero`（36,519,320 B）**几乎没变** ⇒ 结果面没动，只是扫的量少了。
- **复现**：`.tmp/roomopt/heapmeas3.log:593`（前）对 `heapmeas4.log:581`（后）· 实现 `src/runtime/heap.h:76-82` · 取舍见 L3-heap-01 · 2026-11-08

### OBS-20261114-02 · 同一输入两次 `--trace-out` 的 trace **逐字节相同**（读数；`LIFT_STRESS` 档）
- **读数**（RelWithDebInfo · `windows-x64` · 2026-11-14）：`run1: reason=until-ea eip=004BA890 insns=33499
  hash=BFEBB70095383669 inputs=C7CD139C4714F22A` / `run2:` 同一组数（逐字段相同）；
  `trace: run1=41596 bytes run2=41596 bytes` ⇒ 两次 trace 的**字节数相等且逐字节相同**（断言体就是这个）。
- **复现**：`$env:LIFT_STRESS=1; cmake --preset windows-x64; cmake --build --preset windows-x64;
  ctest --test-dir out -R "^boot-trace$" -V`（要 `links/install` + LFS 拆壳件在场）。
- **口径**：这是**可复现性读数**（同输入两次运行 trace 相同），⛔ 不是"某次 trace 长得对"的断言 ——
  断言仍留在 `tests/boot_trace.cpp`（`LIFT_STRESS` 档，注册在 `if(DEFINED ENV{LIFT_STRESS})` 里）；
  变异清单**不再覆盖**它（终裁 1：默认机器上恒为占位，删条目、读数进本文件）。

## 音频与输入

### OBS-20261108-14 · 音频 feeder 单次 fill 的墙钟 310–450 ms
- **读数**：`[audio] feeder fill slot=11 quarter=0..3` 逐条 **309.7–448.9 ms**（slot=11 = BGM 环；slot=1/2 的 SE 0.03–34 ms）⇒ mixer 每 5 ms 一次 ⇒ 两线程几乎全程同时在执行 guest 代码。
- **复现**：`LIFT_AUDIO_TRACE=1` · 口径A（窗口模式 ⇒ 真线程）· 2026-11-08 · `.tmp/audiord/measure-shimtoken-c1.log.err:4-12`、`measure-wholefill-c1.log.err:4-12`

### OBS-20261108-15 · 令牌三形态的整轮墙钟 A/B（同脚本、同机器；baseline 11 次 / 另两臂各 12 次）
- **读数**：无令牌 **61.3–67.1 s**（**会崩**，BUG-03）/ 整次 fill 持令牌 **146.9–287.3 s**（≈2.4–2.6× 中位、最坏 4.6×）/ shim 边界令牌 **62.2–66.5 s**（回到无令牌水平）。
- **复现**：`.tmp/audiord/{baseline,after2,shimtoken}-summary.txt`（每份 12 条 `elapsed=`）· 口径A · 2026-11-08 · 取舍见 L3-token-01

### OBS-20261108-16 · click 短于采样周期会被**静默吞掉**
- **读数**：`down→up` 隔 20k 条指令（**≈0.7 ms**）⇒ 帧号停 108、不回 TITLE；长按 2M 条（**≈220 ms**）⇒ **3/3 回 TITLE**（frame 125/121/124），hover 非必要。修好后 `release : total=2 auto=2 explicit=0 deferred-ups=0 bounded-no-sample=0`。
- **复现**：`.tmp/exitstall/exit-y-longpress.txt` / `exit-fix.txt`（对照 `.tmp/room/exit-prof2.txt` 的 20k 条间隔）· 口径A · 2026-11-08 · `[.tmp/exitstall/report.md:10-17]`、`[.tmp/clickfix/report.md:27-36]`

## 时间锚与未取证的读数

### OBS-20261108-17 · `at frame` 跨运行会漂（同一 `at insn` 落点差 7 帧以上）
- **读数**：同一脚本的 `at insn 500000000` 一次落 `frame=107`、另一次 `frame=114`；`enter-clean.txt` 把漂移量记成"帧号跨运行会漂 **85–90**"⇒ 跨运行复现必须用 `at insn`（或 `at guest_ms`），`at frame` 只做同一次运行内的相对窗口。
- **复现**：`enter-noprof.log` 尾 `frame=114 … insns=500000006 quit` 对 `enter-prof2.log` 的 `frame=107 … insns=500000013` · 读法见 `measurement.md` M-05 · 2026-11-08 · `[.tmp/roomopt/enter-clean.txt:2-6]`

### OBS-20261108-18 · `input-script` 每 tick 逐条比较量（**未取证**）
- **声称**：`975.9M → 1.6M`（快路径：没有任何条件到期时一次也不扫）。**仓内没找到留档读数** —— 只有计数接口 `src/runtime/input_script.h:229-232`（`tickCount()` / `actionScans()`）与一条守卫 `tests/input_tick.cpp:43`（`actionScans() <= 16`）。⇒ 标**未取证**；要落这条：把 `actionScans()` 打进运行摘要（现在没打）。
- **2026-11-08**（记录的是"找不到落点"，不是读数）。

## 退出尾段（最后一次 Present → 进程退出）

### OBS-20261110-05 · TITLE 点 Quit 之后还有 3.58 s / 131,104,825 条 guest 指令，窗口内 0 次 Present
- **读数**（区间 profiler 窗口 `tail`）：`window : wall=3584.4ms frame 150→150 insns 811344275→942449100`
  ⇒ 尾段 **131,104,825 条**（`942449100 − 811344275`，与 `insnsBlk : block=131104825 window=131104825 diff=0` 闭合）/
  **3.58 s 墙钟** / 窗口内 **0 次 D3D9 Present**（`gpu : frames=0`、`frame 150→150`）/
  `buckets : guest=1470.2ms shim=5.6ms native=0.0ms interp=2099.0ms host=8.9ms sum=3583.8ms unattributed=0.6ms`。
  guest 函数表（按 `self` 取前 12）里 `self ≥ 500 ms` 的**只有一行**：`sub_436550`
  （`self=1265.099ms(35.3%)` / `incl=3106.837ms(86.7%)` / `calls=150086`）；`inclusive ≥ 500 ms` 的两条是
  `sub_4367B0`（`incl=3328.082ms` / `self=60.235ms` / `calls=1`）与 `sub_436550`。
- **复现**（工作目录 `apps/emulator-lift`；脚本 = `at frame 130` / `click 1190 630` / `at frame 150` /
  `profile_begin tail`，不写 `profile_end`、run 停止时自动收口）：
  `LIFT_WINDOW_BLOCK_TOP=512 LIFT_WINDOW_EDGE_TOP=256 ./out/emulator-lift --game-dir ../../links/install
  --appdata ../../.tmp/quit-tail/appdata-tail150 --skip-logo --audio off --present headless --raster gpu
  --input-script ../../.tmp/quit-tail/s-tail150.txt --profile --profile-top 200 --time-limit 300`
- **口径**：macOS arm64 / RelWithDebInfo / `--present headless --raster gpu`（`raster : sdl-gpu/metal/MSL`）/
  `--audio off` / 无 `--real-time` ⇒ `trace-in : clock=virtual` / 每个 run 一个**全新**私有 `--appdata`。
  二进制 `sha256 3459f789…`（mtime 19:09）；同一工作树 `cmake --build` 报 `ninja: no work to do` ⇒ 它就是
  HEAD `1907423` 源码的构建（commit 时间 19:45 只是提交时刻，⛔ 不是"二进制比源码旧"）。
  2026-11-10 19:49–19:58 +08:00。原始日志 `.tmp/quit-tail/A2-tail.out`（窗口表 / 边表全文见 `.tmp/quit-tail/profile.md`）。
- **同脚本两次独立 run**（唯一差异 = `--appdata`）：`insns=942,449,100` **逐字相同** / 末帧 `150` /
  Present 总数 `150`；墙钟类数值不同（`Present` 2977.4 / 3087.2 ms）。

### OBS-20261110-06 · 运行摘要 `present-tail` 这一行自己的读数（量的是同一段尾段）
- **读数**：`present-tail: last_frame=150 insns_at_last_present=811344273 tail_insns=131104827 tail_wall_ms=2626.4`
  （另一次 run 同一行只差墙钟：`tail_wall_ms=2791.4`）⇒ `insns_at_last_present + tail_insns = 942,449,100`，
  与摘要 `insns     : 942449100` 逐字相等。
- **复现**：机器 / 安装树 / 档位与脚本同 OBS-20261110-05，只是**不加** `--profile`：
  `./out/emulator-lift --game-dir ../../links/install --appdata <全新私有目录> --skip-logo --audio off
  --present headless --raster gpu --input-script <at frame 130 / click 1190 630> --time-limit 900`
  · 原始日志 `.tmp/present-tail/runA.out:44` / `runB.out:44`（两次 run 各一份全新私有 appdata）。
- **口径**：macOS arm64 / RelWithDebInfo / `--present headless --raster gpu` / `--audio off` / `clock=virtual`。
  `insns_at_last_present` 比 OBS-20261110-05 的**窗口起点**少 2 条 —— 不是构建差异：窗口起点是
  `profile_begin` 被消费的那个 **block 边界**（在 Present 之后），盖章才是 Present 那一刻。
  **实测对照**（同脚本 + `--profile`）：`window … insns 811344275→942449100` 与同一 run 的
  `present-tail: … insns_at_last_present=811344273 tail_insns=131104827` 并列出现，`tail_insns` 与不带
  `--profile` 的两次**逐字相同**（`.tmp/quit-tail/parent-tail150.out:40,185`）⇒ 2 条差来自边界偏移，
  且 `--profile` 不改变 `tail_insns`。`tail_wall_ms` 是宿主墙钟、单样本（两次 2626.4 / 2791.4 ms）；
  这一档（headless + 虚拟时钟）下 `last_frame` / `insns_at_last_present` / `tail_insns` 多次 run 逐字相同。
  2026-11-10 20:03–20:06 +08:00。

### OBS-20261110-06 · TITLE→Quit：引擎每帧发一笔 0→255 的"黑色"多边形，但顶点色不参与混合
- **读数**：点击（frame 130）后每帧 **2 笔**全屏 TRIANGLESTRIP（点击前 1 笔）。第二笔顶点 diffuse
  `d0=0,0,0,α`，α = `0 / 13 / 27 / … / 241 / 255`（≈14.2/帧 ⇒ 约 18 帧 ≈ 0.3 s）。两笔都是
  `texserial=22`、`blend=5/6`、`stage0=4/4(2/3,2/3)`（color 与 alpha 都是 `MODULATE(TEXTURE, TFACTOR)`）、
  `tfactor=0xDEFFFFFF…0xDFFFFFFF`；帧像素 hash 从 frame 131 到 150 **恒为 `0xF4DC13F10D95EBD7`**。
- **纹理 id**：`sub_4AF1C0`（RenderPolygon）那两帧的 `[ebp-0x24]` / `[ebp-0x20]` = `FFFFFFFF FFFFFFFF`
  （`-1` ⇒ 这一笔**没有纹理**）。调用链（`[draw-call] chain=`）：`sub_49E310 ← sub_49E390 ← sub_4AF1C0
  ← 0x4B4335（帧内 display-list 分派）← sub_412290（主循环）← _WinMain`。
- **stage 状态是谁写的**（`[sts]` 行，带返回地址）：每帧 `0x4C1A29` 写 `COLORARG2=0(DIFFUSE)`、
  `0x4C1A9C` 改写成 `3(TFACTOR)`、`0x4C1A57` 写 `ALPHAARG2=3`；另有约 60 次/帧（每个 D3DXSprite）
  的 `0x4C2346 / 0x4C235D / 0x4C238B` 也写成 `ARG1=TEXTURE / ARG2=TFACTOR / COLOROP=MODULATE`。
- **复现**（`apps/emulator-lift` 下）：
  `LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0 LIFT_DRAW_CALLSITE=1 LIFT_DRAW_LOCALS=004B4335 ./out/emulator-lift.exe
  --game-dir ../../links/install --appdata <私有 tmp> --skip-logo --present headless --raster gpu --audio off
  --native-overrides all --max-frames 133 --input-script <at frame 130 / click 1190 630>`
- **口径**：win32 x64 / RelWithDebInfo / headless + `--max-frames`（压日志；(A) 的墙钟要用窗口档）·
  `--native-overrides all` · 2026-11-10 · 工作树 `c0a0f39` + 本轮诊断（`[draw]` 扩字段 /
  `LIFT_DRAW_CALLSITE` / `LIFT_DRAW_LOCALS`，口径见 `apps/emulator-lift/README.md` §4.3.1）。

### OBS-20261110-07 · 取代 OBS-20261110-06：Quit 那两笔多边形的**实测混合 α = 0**（全透明），且顺序/来源都变了
- **取代**：OBS-20261110-06 的"(a) 同图叠同图任意 α 幂等"与"顺序是先渐变后不透明"**两条都错**，读数保留但结论作废。
- **来源**：脚本 **opcode 0x15（画多边形）**，handler `0x432168`（`mov dword ptr [esi+ecx*8+5D8F4h], 15h`）；顶点色在该层解码
  （`ROR(key ^ ROL(raw,11),25)`，key=`[engine+0x5EC8C]`）⇒ **α 是脚本参数，逐帧重发**。
- **顺序**（`[draw] frame=` 分组）：`Clear(0,0,3,0x00000000,1.0,0)` = **每帧清黑** → 黑多边形（顶点 α=255，**在 sprite 批之前**）
  → 60 笔 sprite（真正画面）→ 黑多边形（顶点 α = 0,13,27,…,255，**在 sprite 批之后** = 渐变覆盖层）。
- **新读数（决定性）**：`[draw] fa=` = 本批真正参与混合的 α（n/均值/最小/最大）——两笔分别
  `894575/0/0/0` 与 `919601/0/0/0` ⇒ **α 恒 0、整笔全透明**。stage0 两段都是 `MODULATE(TEXTURE, TFACTOR)`：
  α 取自**纹理**（当前绑的是几万条指令前留下的 `texserial=22`，α=0），**顶点 α 不参与**。
  ★ 该口径随 CPU 光栅退休已变（今天的 `fa=` 数**顶点 diffuse α**，不再是逐像素混合 α），见 `REQ-01M7B7591WHEVZMYFZ1Q2D8SHQ`。
- **引擎侧**：`sub_49E390` 只在 `edi==1/2`（绑到 1~2 张纹理）时设 `COLORARG2/ALPHAARG2=DIFFUSE`（`0x49E608–0x49E660`）；
  这两笔的纹理 id 是 payload 里的 `-1/-1` ⇒ `edi==0` ⇒ **一条不设**。而 `sub_4C19E0` 是
  `COLORARG2=DIFFUSE(0x4C1A29)` → `ALPHAARG2=TFACTOR(0x4C1A57)` → `COLORARG2=TFACTOR(0x4C1A9C)`（中间无 Draw）⇒ 最终 TFACTOR。
- **没有别的路**（frame 133–150，独立复核）：`Clear`=1/帧、`DrawPrimitive`=2/帧、`DrawIndexedPrimitive`=60/帧；
  `LockRect/UnlockRect/StretchRect/UpdateSurface/ColorFill/SetRenderTarget/GetRenderTargetData/UpdateTexture` **全 0**。
- **复现**（`apps/emulator-lift` 下；`--raster cpu` 才有 `fa=`）：
  `LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0 ./out/emulator-lift.exe --game-dir ../../links/install --appdata <私有 tmp>
  --skip-logo --present headless --raster cpu --audio off --native-overrides all --max-frames 136
  --input-script <at frame 130 / click 1190 630>`
- **口径**：win32 x64 / RelWithDebInfo / headless / `--native-overrides all` · 2026-11-10 · 工作树 `c0a0f39` + 本轮诊断
  （`[draw]` 扩字段 / `frame=` / `fa=` / `LIFT_DRAW_CALLSITE` / `LIFT_DRAW_LOCALS`，见 `apps/emulator-lift/README.md` §4.3.1）。

### OBS-20261111-01 · 固定功能批次全部 `fa=0`（纹理 α=0）⇒ 屏幕内容只来自宿主 sprite
- **读数**（`--raster cpu`，frame 134–136，`LIFT_SLOW_DRAW=1`）：每帧 62 笔 `[draw]` —— 2 笔脚本多边形
  （`va=255` / `va=13,27,41`）+ 60 笔 `DrawIndexedPrimitive` 图层批（`d0=255,255,255,255`）。
  **全部** `fa=<n>:0/0/0`（真正参与混合的 α 恒 0）、`texa=4:0..0`（绑定纹理自身 α 在 4 点采样都是 0）。
- **另一条事实**：这 60 笔图层批在本轮之前**整批失败**（`drawPrecheck` 报"设了顶点声明…只支持 SetFVF"，
  60 笔/帧 `D3DERR_INVALIDCALL`）。本轮实现顶点声明路后成功（`DrawIndexedPrimitive: PrimitiveType=4 …
  ⇒ 三角形 4（光栅化 4），写 921600 个像素`），但帧像素 hash 不变 —— 与 `fa=0` 自洽。
- **复现**（`apps/emulator-lift` 下）：
  `LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0 ./out/emulator-lift.exe --game-dir ../../links/install
  --appdata <私有 tmp> --skip-logo --present headless --raster cpu --audio off --native-overrides all
  --max-frames 136 --input-script <at frame 130 / click 1190 630>`
- **口径**：win32 x64 / RelWithDebInfo / headless / CPU 光栅（`fa=` 只在 CPU 路统计）· 2026-11-11 ·
  工作树 `c0a0f39` + 本轮改动（顶点声明路 + `fa=`/`texa=` 诊断）。

### OBS-20261111-02 · `--raster cpu` 在本机 300 s 只跑到 frame 69（墙钟大头在 CPU 光栅化）
- **读数**（win32 x64 / RelWithDebInfo / `--present headless --raster cpu` / clock=virtual）：
  `--time-limit 300` 到点停止时 **frame 69**（不是跑完）；`--profile` 的 `shim` 桶 **236.5 s**，几乎全在 CPU 光栅化。
  同一份输入脚本换 `--raster gpu` 能跑到点中 Quit（尾段读数见本文件 `OBS-20261110-05` 的本机复现）。
- **复现**（`apps/emulator-lift` 下；副本二进制，避免与构建抢文件）：
  `.tmp/quit-tail/bin/emulator-lift.exe --game-dir .\links\install --appdata .\.tmp\quit-tail\appdata-cpu
  --skip-logo --audio off --present headless --raster cpu --input-script .\.tmp\quit-tail\s-quit.txt
  --input-stall-boot-ms 600000 --profile --profile-top 200 --time-limit 300`
- **口径**：2026-11-11 · 副本二进制（`out/*` 复制到 `.tmp/quit-tail/bin/`，exe sha256 `3818CEB9…`）· 有安装树 · 无 override。
- **用途**：`REQ-01M737M1PPHHXDN65RCTK6XTV6` 的第一条读数 —— "CPU 光栅慢"从此是读数而不是印象；
  也说明**长流程不能用 CPU 光栅跑**（这会反过来决定 oracle 的保留成本）。

### OBS-20261112-01 · M0.1：Present 路径上 7 处 `Acquire`/`Submit` 移出令牌前后的读数（本机 Metal）
- **口径**：**macOS arm64 / Metal（MSL）/ RelWithDebInfo**（`cmake --preset macos-arm64`）·
  `--present window --present-vsync 0 --present-hidden --raster gpu`（启动行
  `sink driver=gpu-direct video=cocoa gpu=metal present=direct/immediate`）· 有安装树 ·
  `--native-overrides all --skip-logo` · **不是口径A**（口径A = Win10 x64 / D3D12 / DXBC）· 40 帧 · 2026-11-12 ·
  工作树 `9378da7` + 本单改动（`d3d9_gpu.cpp` 的 `:456`/`:816`/`:925`/`:1198`/`:1216`/`:1225`/`:1264` 各加一个 `YieldGuard`）。
- **命令 ①**（`pres :` 只在这里出；改前 `--appdata /tmp/m01/appdata`、改后 `/tmp/m01/appdata-after`）：
  `LIFT_FRAME_TRACE=1 ./out/emulator-lift --appdata <…> --game-dir ../../links/install --present window
  --present-vsync 0 --present-hidden --raster gpu --native-overrides all --skip-logo --max-frames 40`
- **读数 ①**（40 帧，每栏取 max / 中位；`direct=1` 两次相同）：

  | | readback | hash | stats | sink | Present shim max |
  |---|---|---|---|---|---|
  | 改前 | 17.0 / 3.4 ms | 0.9 / 0.8 | 0.0 / 0.0 | 1.4 / 0.2 | 4.8 ms（39 次 Present） |
  | 改后 | 14.7 / 3.3 ms | 0.8 / 0.7 | 0.0 / 0.0 | 2.5 / 0.1 | 4.8 ms（39 次） |

- **命令 ②**（`tokenhold`/`holdhist`/`waithist`/`shimmax` 只在 `--profile` 或 `LIFT_AUDIO_TRACE` 下出；
  改前 `--appdata /tmp/m01/appdata2`、改后 `/tmp/m01/appdata2-after`）：
  `LIFT_AUDIO_TRACE=1 ./out/emulator-lift --appdata <…> --game-dir ../../links/install --present window
  --present-hidden --raster gpu --native-overrides all --skip-logo --audio sdl
  --interaction-graph ../../graphs/se-eof.graph --profile`
  （两条都跑到 `detail: interaction: task done`，exit 0）
- **读数 ②**（改前 → 改后）：
  - `tokenhold : main … max` **29.474 → 30.555 ms**；`main hold … = 56.5% → 56.0%`
  - `holdhist : main … >50ms` **0 → 0**（`20-50ms` 7 → 7）
  - `waithist : main … 20-50ms` **1 → 0**、`>50ms` **0 → 0**（`<1ms` 271 → 298）
  - `tokenwait`：waits 519 / 126.7 ms → 542 / 91.5 ms（单次均值 244.1 → 168.9 µs）
  - `shimmax` 前 5：改前 = `IDirectSoundBuffer8::Stop` 52.483 ms×9 / **`IDirect3DDevice9::Present` 32.342 ms×1421** /
    `D3DXCreateTextureFromFileInMemory` 29.471×3 / `DrawIndexedPrimitive` 22.734×20940 / `ID3DXSprite::End` 20.800×10843；
    改后 = `Stop` 61.970×9 / `D3DXCreateTexture…` 29.655×3 / `DrawIndexedPrimitive` 24.028 / `ID3DXSprite::End` 21.526 /
    `Clear` 18.177；**`Present` 掉到第 6 位 17.025 ms×1421**
  - `audio : mode=sdl device=ok …`：`fill_max_ms` **117.19 → 117.89**、`fill_slow` **0 → 0**、`fill_waits` **1 → 1**、
    `fill_wait_timeouts=0`、`underruns=0`；`token : violations=0 max-concurrent=1` 两次相同
  - `tokenhold : other` 的 max 29.202 → 0.723 ms（改后第二遍 0.714）—— 改前只跑一遍，**本单不据此下结论**（可能只是运行间抖动）。
- **改后复跑一遍**（同一份源码、同一二进制；`--appdata /tmp/m01/appdata2-after2`，命令 ② 原文）：
  `tokenhold main max 30.795 ms`、`holdhist main >50ms 0`（`20-50ms 5`）、`waithist main 20-50ms 0`、
  `Present shim max 17.324 ms×1415`、`fill_max_ms=119.48 fill_slow=0 fill_waits=1`、
  `token violations=0 max-concurrent=1`；命令 ① 复跑（`--appdata /tmp/m01/appdata-after2`）：
  `pres : readback 15.5 / 3.3 ms、hash 0.8 / 0.7、stats 0.0、sink 1.5 / 0.1、direct=1`、Present shim `4.9 ms` max。
  ⇒ 改后两遍的**形状一致**（`>50ms 0`、`20-50ms 0`、`Present` 两次都在 `shimmax` 前 5 之外），逐位数值不逐位相同。
- **一句（平台差异，不是"修好了"）**：用户机 Windows 上 Present 内 ~250 ms 的持有**本机不复现** —— Metal 的提交是
  异步 `[commandBuffer commit]`，D3D12 的 `IDXGISwapChain_Present` 才在提交里同步等（`.tmp/audio-token/m01-design-review.md` §5）。
  上表只能说明"本机在这个工况下这一栏是多少"，不能说明 Windows 侧会掉多少；Windows 侧的定案命令见该复审 §6.2 的三臂
  （A `--raster gpu` / B `--raster cpu --present-driver software` / C `--raster cpu`）。
> ⚠ 2026-11-12 复审按同命令（load avg ~17.5）3 次未复现本条组合（`>50ms` 4–5、Present 38.6–65.3 ms、`fill_max` 157–211），见 `OBS-20261112-02`；本条是该时刻（负载较低）的读数，跨负载不可比。

### OBS-20261112-02 · 同一命令在本机**共享负载**下的 3 次复跑（OBS-20261112-01 的范围复核）
- **口径**：与 `OBS-20261112-01` 相同 —— **macOS arm64 / Metal（MSL）/ RelWithDebInfo**（`cmake --preset macos-arm64`）·
  `--present window --present-hidden --raster gpu --native-overrides all --skip-logo --audio sdl` · 40 帧不需要（本命令跑到交互图收工）·
  **不是口径A** · 2026-11-12 · **同一二进制**（工作树 `9378da7` + 本单改动，含仅注释修订后的重编译）·
  三次各新开一个 `--appdata /tmp/m01/rerun3/appdata-{1,2,3}`。
- **命令原文**（三条完全相同；`LIFT_AUDIO_TRACE=1` 是命令的一部分）：
  `LIFT_AUDIO_TRACE=1 ./out/emulator-lift --appdata /tmp/m01/rerun3/appdata-<n> --game-dir ../../links/install
  --present window --present-hidden --raster gpu --native-overrides all --skip-logo --audio sdl
  --interaction-graph ../../graphs/se-eof.graph --profile`
  ⇒ 三次都 `detail: interaction: task done`、exit 0。
- **负载（`uptime` 原样记录；12 核）**：run1 起 `load 17.90 13.04 12.75` / 止 `21.98 14.50 13.29`；
  run2 起 `21.98 14.50 13.29` / 止 `21.57 14.94 13.48`；run3 起 `21.57 14.94 13.48` / 止 `19.30 14.86 13.49`。
  ⇒ **这三次不在低负载下**（复审时 ~17.5，本轮 17.9–22.0）—— 与 `OBS-20261112-01` 的绝对值不可横比。
- **读数（逐次 + 范围）**：

  | 指标 | run1 | run2 | run3 | 范围（min–max） |
  |---|---|---|---|---|
  | `tokenhold main max` | 47.886 ms（55.9%） | 33.187（54.8%） | 32.381（55.2%） | **32.4–47.9 ms** |
  | `holdhist main >50ms` | 0（20–50ms 9） | 0（20–50ms 8） | 0（20–50ms 6） | **0–0**（20–50ms 6–9） |
  | `waithist main 20–50ms / >50ms` | 0 / 0 | 0 / 0 | 0 / 0 | **0–0 / 0–0** |
  | `tokenwait` | 575 次 / 119.9 ms | 556 / 95.9 ms | 486 / 69.6 ms | **486–575 次 / 69.6–119.9 ms** |
  | `Present` max（`shimmax` 名次） | 61.782 ms（**#1**，1377 次） | 115.951（**#1**，1375 次） | 21.872（**#4**，1402 次） | **21.9–116.0 ms，名次 #1–#4** |
  | `shimmax` #1 | `Present` 61.782 | `Present` 115.951 | `D3DXCreateTextureFromFileInMemory` 32.377 | — |
  | `fill_max_ms` / `fill_slow` / `fill_waits` | 159.29 / 0 / 0 | 180.98 / 0 / 0 | 143.16 / 0 / 0 | **143.2–181.0 / 0 / 0** |
  | `token violations` / `max-concurrent` | 0 / 1 | 0 / 1 | 0 / 1 | **0 / 1** |

- **这三次里也没复现 `OBS-20261112-01` 的单次组合**（0 / Present 17.0 ms / `fill_max` 117.89）：
  本轮的 `Present` 至少 21.9 ms、`fill_max` 至少 143.2 ms。⇒ 两处差异的**形状**是"`LIFT_AUDIO_TRACE=1` 这一臂在本机
  随共享负载漂"，**不是**"某一次数字更对"。⛔ 本条不是断言，也不据此判定本单效果（`waithist`/`token` 两栏三次都稳定）。

### OBS-20261112-03 · 第 3 层等待的时长读数：`fill_wait_ms` / `fill_wait_max_ms`（本机 sdl + virtual 两臂）
- **口径**：**不是口径A** —— **macOS arm64 / Metal（MSL）/ RelWithDebInfo**（`cmake --preset macos-arm64`）·
  2026-11-12 · **工作树 = `9378da7` + 本单改动（未提交）**，两条臂都在该工作树上跑（构建于收口前；
  其后的收口只删了 `pump()` 里**不可达**的门与注释，音频路径未变）·
  纯读数（⛔ 不是断言，不据此判定"事务化 fill 值不值"）。
- **命令 ①（thread 臂，唯一可能 >0 的臂：窗口自动 real-time ⇒ 方案 C 真 feeder 线程）**：
  `LIFT_AUDIO_TRACE=1 ./out/emulator-lift --appdata /tmp/req-audio/sdl2 --game-dir ../../links/install
  --present window --present-hidden --raster gpu --native-overrides all --skip-logo --audio sdl
  --interaction-graph ../../graphs/se-eof.graph --profile`
  ⇒ `detail: interaction: task done`、exit 0、`token : violations=0 max-concurrent=1`。
- **读数 ①**（`audio :` 行）：`fills=53 fill_waits=1 fill_wait_timeouts=0 fill_overlapped=5
  fill_max_ms=138.21 fill_slow=0 fill_wait_ms=28.49 fill_wait_max_ms=28.49`。
  自洽：只有 1 次等待 ⇒ 累计 == 单次最大；`fill_wait_timeouts=0` ⇒ 没有 3 s 超时混进时长；
  `fill_wait_max_ms(28.49) < fill_max_ms(138.21)` ⇒ 等的是那一刀的**剩余部分**，不是"吃满一整刀"。
- **同命令的第二次读数（独立实现复审，同日、共享负载）**：`fill_waits=1 fill_wait_timeouts=0
  fill_overlapped=5 fill_max_ms=125.09 fill_wait_ms=78.87 fill_wait_max_ms=78.87`（exit 0，`task done`）。
  ⇒ 两次**形状一致**（`fill_waits` / `fill_overlapped` / `fill_wait_timeouts` 相同），绝对值
  **28.49 vs 78.87 ms** 随**共享负载**漂（本机 12 核，两跑负载不同）—— ⛔ 不可横比，也不取平均。
- **命令 ②（D 臂阴性对照：`--audio virtual` + 窗口 40 帧）**：
  `./out/emulator-lift --appdata /tmp/req-audio/virt3 --game-dir ../../links/install --present window
  --present-hidden --raster gpu --native-overrides all --skip-logo --audio virtual --max-frames 40`
  ⇒ exit 0、`detail: max-frames`。
- **读数 ②**：`mode=virtual device=none fills=4 fill_waits=0 fill_wait_timeouts=0 fill_overlapped=0
  fill_max_ms=113.80 fill_wait_ms=0.00 fill_wait_max_ms=0.00`。
  ⇒ 与"D 模式（`thread_mode_ == false`）没有 feeder 线程、`filling_obj_` 恒 0"一致。
- **边界读数（不当作阴性对照）**：同形状的 `--audio sdl` 40 帧臂（`--max-frames 40`）只跑了 boot
  的 3 刀、没有 Stop 落进 fill ⇒ `fill_waits=0`、两栏 `0.00`、`fill_max_ms=145.59`。⇒ 短跑读不到
  第 3 层的代价，读数要看跑到交互结束的那一臂。
- **口径备注**：两新栏 = 第 3 层"等正在跑的那一刀 fill"的**累计 / 单次最大**墙钟，
  **只量谓词等待本身**（`t1` 取在重取 `mutex_` 之前）⇒ **不含**混音器占用（`mixStep` 跨
  `generateTo`）；⛔ 不要把它当"调用方总卡顿"。`fill_waits == 0` 的运行里 Stop 照样发生很多次、
  只是都落在 fill 间隙 ⇒ ⛔ 不要把 `fill_wait_ms == 0` 读成"Stop 很便宜"。

### OBS-20261112-04 · `_ShowDialog(-1)` 是 AGERC 的 UI **状态查询**，顶边悬停时被 100 ms 计时器轮询
- **读数（本线口径，2026-11-12）**：`at marker TITLE.BIN` 后 `mouse_move 640 1`（客户区顶边 y=1）⇒
  `agerc!_ShowDialog@12(FFFFFFFF,00010000,00000000) -> 00000001  ret=004B92A5`；
  同一脚本把 `--until ea` 设成 `0x4B92A5` 时 run 在 **17.98 s** 内收口（CTest `agerc-ui-state`；
  该 CTest 已于 2026-11-13 随 CTest 瘦身删除 —— 读数仍是当天的实测值）。
- **对照读数（把 -1 分支改成真机 0）**：同一条脚本立刻多出
  `agerc!_ShowDialog@12(00000004,00010000,00000000) -> 00000001`，随后
  `user32!GetWindowRect(00000001)` 报 `窗口句柄 0x00000001 不存在` ⇒ 进程 abort（exit 134）。
- **真机语义（`.lst`）**：AGERC.DLL 的 `_ShowDialog` 体 `0x10038330` 按 `id+1` 做 12 路 switch；
  case -1（`0x100383CB`）只有 `mov eax, dword_10150E70` —— 返回"当前哪些 AGERC 对话框开着"的
  位图（无框 = 0；bit 8 = 終了框、bit 0x10 = 顶边菜单栏、bit 0x100/0x200/0x400 = 其它框），
  **不创建窗口、不阻塞**；case 0/1/5-7（未知 id）走 `or eax,-1` 返回 -1。
- **调用点**：AGE.EXE `0x4B9240`（WM_TIMER(1) / 顶边悬停）在 `0x4B929F` 调它；
  `test eax,eax` 非 0 ⇒ 跳过顶边菜单栏 `sub_408620`（后者先 `or [game+0AAB44h], 400000h`，
  再 `_ShowDialog(4)`，然后对返回值 `GetWindowRect`）。
- **本线口径（2026-11-12）**：**故意返回 1**（不是真机的 0）挡住未实现的 AGERC 菜单栏：
  若返回 0，实测 2026-11-12 macOS arm64 会走到 `_ShowDialog(4) -> 1` ⇒
  `user32!GetWindowRect(00000001)` 报 `窗口句柄 0x00000001 不存在` 并 abort。
  这条偏差与 SCOPE §1「菜单 / AGERC 设置对话框 = 永久 no-op」一致；原来守它的 `tests/agerc_ui_state.cpp`
  （CTest `agerc-ui-state`）已于 2026-11-13 删除（它断言的是我们 stub 的返回值，属 BUG-14 反模式；
  这条取舍现在只记录在本条与 `apps/emulator-lift/README.md` §4.1）。
- **复现**：`./out/emulator-lift --present window --present-hidden --present-driver software
  --raster cpu --skip-logo --audio off --game-dir ../../links/install --appdata ../../.tmp/hover/appdata
  --until ea 0x4B92A5 --trace-shims --input-script ../../.tmp/hover/hover.txt --time-limit 300`
  （脚本两行：`at marker TITLE.BIN` + `mouse_move 640 1`）· 2026-11-12 · macOS arm64 / RelWithDebInfo

### OBS-20261112-05 · 存档缩略图链：`CreateOffscreenPlainSurface`(+0x90) / `GetRenderTargetData`(+0x80) / `UpdateSurface`(+0x78)
- **读数（修前）**：`graphs/sn0000-save.graph` 在 SAVE 写档时
  `stop : unimplemented-slot @ eip=004A0572` / `detail : …IDirect3DDevice9::unsupported+0x90`；
  `SAVE00.DAT` 已被建成 **0 字节**。补上 +0x90 后**前进一格**：`eip=004A479F` / `unsupported+0x78`。
- **真机链（`.lst`）**：`sub_4A0430`（GetTextureSurface）= 源 surface `GetDesc`(+0x30) →
  `CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM=2,
  &ppSurface, NULL)`（**0x4A0570**）→ `GetRenderTargetData(src, dst)`（**0x4A05D2**）；
  随后缩略图写回纹理表面走 `UpdateSurface(src, NULL, dst, NULL)`（**0x4A479D**，4 参数）。
- **读数（修后，同一 graph + 20 s 收口）**：`stop : stopped` / `detail : interaction: task done`；
  `SAVE00.DAT` = **1,048,521 B**、`SAVE00.STH` = **172,854 B**（另 `SAVE.DAT` 48,832 B / `RT.DAT` 276 B）。
- **时序读数**：存档（缩略图 + 1 MB DAT）在模拟器上要 **~7 s**；图原来的最后一段只有
  `wait 5000 wall_ms`（且单 `wait` 的缺省 timeout = 5000 ms）⇒ 图先结束、文件停在 0 字节
  （那是图的时间预算，不是判据红）。已把最后一段拆成 5×`wait 5000 wall_ms`。
- **复现**：`./out/emulator-lift --present window --present-hidden --raster gpu --native-overrides all
  --skip-logo --audio off --game-dir ../../links/install --appdata ../../.tmp/save/appdata
  --interaction-graph ../../graphs/sn0000-save.graph --max-frames 6000 --time-limit 300` ·
  2026-11-12 · macOS arm64 / RelWithDebInfo · 原 CTest `save-graph`（79 s）已按测试模型删除（2026-11-14，
  业务正确性不作判据；它原来在 `repro` 档，现在只留这条命令）。
- `save-graph`：`./out/emulator-lift --present window --present-hidden --native-overrides all --skip-logo --audio off --game-dir ../../links/install --appdata <每次全新私有 tmp> --interaction-graph ../../graphs/sn0000-save.graph --interaction-fail-dump <save.fail.bmp> --max-frames 8000 --time-limit 400` ⇒ 人读判据：`stop : stopped` / `detail : interaction: task done`、`SAVE00.DAT` ≈ 1 MB、`SAVE00.STH` 偏移 54 之后非零字节 > 0（★ 原 CTest 已删，不进测试）。

### OBS-20261112-06 · 两处卡顿的区间 profile：右键进 FELLOW / SAVE 确认保存
- **图**：`graphs/sn0000-save-profile.graph`（`profile_begin fellow-enter` 覆盖右键 + 12 s；
  `profile_begin save-confirm` 覆盖确认保存 + 15 s；窗口表在 `profile_end` 打印）。
- **命令**：`./out/emulator-lift --present window --present-hidden --raster gpu --native-overrides all
  --skip-logo --audio off --game-dir ../../links/install --appdata ../../.tmp/save-profile/appdata
  --interaction-graph ../../graphs/sn0000-save-profile.graph --max-frames 8000 --time-limit 400`
  （macOS arm64 / RelWithDebInfo / 2026-11-12；本机 `--audio off`）。
- **fellow-enter 读数**：wall **12,032 ms**；buckets guest 3,718 / shim 1,940 / interp 3,542 /
  host 2,820（其中 **input-script 2,631**）/ native 10 ms。最大的 guest 项 = `sub_42F560`
  self **1,899 ms** / incl 2,826 ms（**calls=5**，块 `0x42F5F0` 一条 128.8M insns）；`Present`
  1,158 ms / 203 次。
- **save-confirm 读数**：wall **15,039 ms**；guest 4,098 / shim 2,769 / interp 4,354 /
  host 3,747（**input-script 3,548**）/ native 69 ms。存档链：`sub_42D980 → sub_40CD10`
  incl **3,836 ms** → `sub_437480` incl **3,230 ms**；缩略图链 `sub_4A42C0`（ShrinkTexture）
  incl **3,174 ms**（self 1,214 ms；`sub_49EE60` 410 ms）；`j_WriteFile` 仅 **2.5 ms**、
  `sub_4A0430`（D3D 复制）**2.5 ms** ⇒ **卡的是 guest CPU（序列化 + 缩小），不是文件 IO / D3D 拷贝**。
- **单帧读数（`LIFT_FRAME_TRACE=1`，同一张图）**：两处卡顿各是**一帧**：
  `fellow-enter` = 第 **3031** 帧 `wall=7,388.2ms / 240.0M insns`（guest 3,043 / shim 127 /
  other 4,218 ms）；`save-confirm` = 第 **3369** 帧 `wall=7,424.4ms / 220.4M insns`
  （guest 2,926 / shim 87 / other 4,412 ms）。⇒ 不是"持续慢"，是**单帧 7.4 s**；
  `other` 主要是 interpreter + 交互图 tick（手动游玩没有后者）。
- **口径警告**：`host: input-script` 是**交互图自己的** `InputScript::tick`（每块边界一次，
  2.6–3.5 s/窗）——手动游玩没有这一项，**不要把这条图的总墙钟当玩家读数**；要真实墙钟按
  README §4.3.2 做去掉 `profile_*` 的 A/B。窗口 profiler 自身也有开销（每块计时）。

### OBS-20261113-01 · 读档标题乱码 = 老汉化 936/GBK 存档；新汉化 fixtures 正常

- **数据面**：`corpus/fixtures/` 顶层 = **新汉化**（槽 76–79，摘要 `1076–1079`/`2076–2079`）；
  `corpus/fixtures/saves/` = **老汉化 936/GBK**（槽 00/01/03，摘要 `1000–1003`/`2000–2003`）。
  两份 `SAVE.DAT` 都是真机容器；老汉化的槽体只在 `saves/` 里，且与 `saves/SAVE.DAT` 的 0–3 摘要配套。
- **读数（新汉化）**：`SAVE.DAT` 的 `1076–1079` = `8F 98 8F CD`（CP932 `序章`）；
  `2076–2079` = `91 68 90 C1 98 B0 FA 84 90 A2 93 49 8F 97 90 5F`（CP932 `蘇醒于刕世的女神`；
  AmayuiCN 渲染成 `苏醒于现世的女神`）。LoadData 第 70 页：槽 76–79 的标题 / 副标题 / 缩略图正确；
  点槽 76 读取后进剧情，正文中文正确。
- **读数（老汉化 936/GBK）**：`corpus/fixtures/saves/SAVE.DAT` 的 `1000`/`1001` = `A3 B1 D5 C2`
  （GBK `１章`；CP932 拆成 `｣ｱﾕﾂ`）、`2000`/`2001` = `B3 C7 ED CE D3 EB BD E1 D4 B5 D6 AE C1 A6 B9 B2 B4 E6`
  （GBK `城砦与结缘之力共存`）、`1003` = `A3 B2 D5 C2`（GBK `２章`）、
  `2003` = `A3 B2 C8 CB A4 CE C3 A8 AA 95 C8 CB`（GBK `２人の猫獣人`）
  ⇒ 用 `saves/` 的槽 00/01/03 进 LoadData 第 0 页是半角假名乱码；新汉化按 ACP=932 走
  ⇒ **跨汉化版本的数据编码不兼容**。
- **读数（退出写摘要链）**：用顶层 fixtures 构造 appdata + `graphs/sn0000-save.graph` 存到槽 110；
  `close_window`（`--close-confirm exit`）正常退出前 `SAVE.BAK` 的 `1110`/`2110` = `第六章` /
  `降臨的軍神使者`（旧摘要），退出后 `SAVE.DAT` 的同一对 = `8F 98 8F CD` / `91 68 … 90 5F`
  （`序章` / `蘇醒于刕世的女神`）；重进 LoadData 第 110 页显示正确。
- **复现（新汉化）**：把顶层 `SAVE.DAT` / `RT.DAT` / `SAVE7{6,7,8,9}.{DAT,STH}` 拷进
  `<work>/appdata/Local/Eushully/天結いキャッスルマイスター/SAVE/`（无需 `SYS4REG.INI`）；
  `./out/emulator-lift --present window --present-hidden --raster gpu --native-overrides all
  --skip-logo --audio off --game-dir ../../links/install --appdata <work>/appdata
  --input-script <work>/load70.txt --max-frames 700 --time-limit 120`，其中 `load70.txt` =
  `at frame 130` / `click 1070 480` / `wait 60 frames` / `click 605 30` / `wait 20 frames` /
  `click 530 30` / `wait 40 frames` / `click 893 30` / `wait 80 frames` / `quit`。
- **复现（老汉化）**：同上，但 SAVE 文件换成 `corpus/fixtures/saves/` 的 `SAVE.DAT` / `RT.DAT` /
  `SAVE0{0,1,3}.{DAT,STH}`；`load0.txt` = `at frame 130` / `click 1070 480` / `wait 60 frames` /
  `click 605 30` / `wait 20 frames` / `click 530 30` / `wait 40 frames` / `click 612 30` /
  `wait 80 frames` / `quit`。
- **复现（退出写摘要）**：`graphs/sn0000-save.graph` 末尾追加 10×`wait 5000 wall_ms`；
  输入脚本 `<work>/close.txt` = `at wall_ms 105000` / `close_window` / `at wall_ms 125000` / `quit`；
  运行加 `--close-confirm exit`。
- **口径**：macOS arm64 / `apps/emulator-lift`（HEAD `4f1bc7e`）/ 2026-11-13 / RelWithDebInfo /
  `--present window --present-hidden --raster gpu` / `--skip-logo --audio off --native-overrides all`。
- **用途**：修正 `REQ-01M78GAHK7TETS08H4CKAGJKN7` 里「`A3/B1/D5/C2` 是未用槽占位默认串」的误判；
  `corpus/fixtures/saves/`（老汉化 936）的显示要另做数据层迁移（本观察**不**声称 lift 应改编码）。

### OBS-20261113-02 · `--present-screenshot` 的 BMP 导出路径**全仓无守卫**（已知缺口，不补用例）

- **读数**（2026-11-13 / Windows / `apps/emulator-lift` / RelWithDebInfo）：
  `rg -n 'present-screenshot' apps/emulator-lift/tests/` = **0 命中** —— 没有任何测试引用它。
  它仍被 `src/main.cpp`（CLI 解析）与 README / SCOPE 的**人工命令**引用 ⇒ **没有任何自动路径会执行到它**。
- **曾经唯一的真 boot 覆盖**：`overlay-window-smoke`（`tests/overlay.cpp` 的 `--window-smoke` 臂：
  子进程带 `--present window --present-hidden --overlay --max-frames 2 --present-screenshot …`，
  断言 BMP 左上角被叠层精确覆盖；`git show 382e3c4` 可见）。
  ★ 该 CTest 与那条臂已随 2026-11-13 的测试瘦身**删除**（见 `CMakeLists.txt` 的同名注释、
  `docs/03-emulator/defects.md` BUG-14 的"现状"行）。
- **缺口形态**：窗口回读 → BMP 落盘 → `{frame}` 替换 / `--present-screenshot-frame` 这一整条**导出路径**
  现在没有守卫；它坏了不会被任何 `ctest` / `pnpm test` 发现，只能靠人工按 README §4.1 的命令跑。
  叠层的**像素判据**仍由 pure 的 `overlay` ③④ 覆盖；缺的是"真 boot + 真窗口回读 + 真落盘"那一段。
- **口径**：按 `AGENTS.md` §10 这是**已知缺口**（观察 / 缺陷史，不是"变了就是 bug 的静默错误"）
  ⇒ 登记在此，⛔ **不为它新加用例**（原用例正是"用 test 记住当年那次排查"的形态，删掉是对的；
  将来要补的是**设计的合同**，不是把旧 smoke 搬回来）。

### OBS-20261114-01 · 关窗确认三档的复现命令（原 CTest `close-confirm` 已删）

- **读数（2026-11-12，源 = `REQ-01M72JFZ87DJNT52DW4HB8SEV6` 的"收口"段；不是我这次重测的）**：
  三臂各自 `exit -> 1 stop@00405B21` · `title -> 2 stop@00405A82` · `cancel -> 0 stop@00405A95`，
  原 CTest `close-confirm` 18 checks；`ask` 在 dummy 视频下系统框起不来 ⇒ 1 + warn。
- **处置**：按测试模型（`REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）**业务正确性不作判据** ⇒ CTest 与
  `tests/close_confirm.cpp` **已删**（2026-11-14）；纯映射 / 系统框缝 / 2/0 取消关窗那几条**合同**仍由
  pure 的 `tests/messagebox.cpp` 钉住。
- `close-confirm`：`./out/emulator-lift --present window --present-hidden --present-driver software --skip-logo --audio off --game-dir ../../links/install --appdata <每次全新私有 tmp> --close-confirm <exit|title|cancel> --until ea <0x405B21|0x405A82|0x405A95> --trace-shims --input-script <close.txt> --time-limit 600`（`close.txt` = `at marker TITLE.BIN` + `close_window`）⇒ 人读判据：`stop      : until-ea` @ 对应 EA、trace 里 `agerc!_ShowDialog@12(00000003,00010000,00000000) -> 0000000{1|2|0}`、日志有"不弹框、直接返回"（★ 原 CTest 已删，不进测试）。

### OBS-20261114-02 · `--skip-logo` 的钩子点 / 字段（原 CTest `skip-logo` 已删）

- **口径**（`apps/emulator-lift/README.md` §4.1b）：`--skip-logo` **不跳过 boot**，只在 `T_Command` 构造完成、
  指针发布之前（EA `0x004BAA37`，EAX = 新对象）把引擎自己的 show-logo 字段 `T_Command + 0x5EB5C` 写 0；
  字段初值由构造器 `sub_415640` 在 `0x00415AE0` 写成 1。**只写这一格**，⛔ 不按帧号猜 LOGO。
- **处置**：原 CTest `skip-logo`（`tests/skip_logo.cpp`）断的是**我方钩子的内部形态**（只写这一格、邻居不动）
  ⇒ BUG-14 反模式，**已删**（2026-11-14）。钩子万一改坏，boot/TITLE 会当场炸（`README.md` §0.1 的 (b)）。
- `skip-logo`：`./out/emulator-lift --game-dir ../../links/install --appdata <每次全新私有 tmp> --skip-logo --audio off --present headless --max-frames 1` ⇒ run summary 应有 `skip-logo : applied @ T_Command=0x… (+0x5EB5C=0)`；**不带**该开关时整行不出现（★ 原 CTest 已删，不进测试）。

### OBS-20261114-03 · 退出写存档尾段的 LZSS Encode override（原 CTest `native-override-e2e` 已删）

- **读数（2026-11-10，OBS-20261110-05 的同一段 tail）**：`sub_4367B0`（LZSS Encode）
  `incl=3328.082ms` / `self=60.235ms` / `calls=1` ⇒ 它吃掉了退出尾段的大头。
- **处置**：原 CTest `native-override-e2e`（`tests/native_override_e2e.cpp`，assets 档）是**业务 + 效率混装**
  （`hits==1` 是效率读数，`SAVE.DAT` 字段语义 / `stop=stopped` 是业务）⇒ 按测试模型**整条已删**（2026-11-14）。
  override 的语义 / 回退由 `native-override-assets`（真字节两臂）与 `native-override`（pure）继续守；
  效率读数按 `REQ-01M7ATAB…` 的口径归 `LIFT_STRESS`（本批未新写用例）。
- `native-override-e2e`：`./out/emulator-lift --game-dir ../../links/install --appdata <每次全新私有 tmp> --skip-logo --audio off --present headless --input-script <script-quit.txt> --input-stall-boot-ms 600000 --profile --profile-top 200 --time-limit 300`，另跑一臂加 `--native-overrides sub_4367B0`（`script-quit.txt` = `at frame 130` / `click 1190 630` / `at frame 150` / `profile_begin tail`）⇒ 人读判据：`sub_4367B0` 明细行 `hits==1 && fallbacks==0`、`present-tail` 的 `tail_insns` 两臂差、`stop=stopped` + `detail` 含 `ExitProcess`（★ 原 CTest 已删，不进测试）。

### OBS-20261114-04 · 导入覆盖普查的现行入口（原 CTest `base-coverage` 已删）

- **口径**：A5 的判据是"trace 里**执行到的**导入 ⊆ 实现 ∪ `stub`（带 reason）"，原
  `tests/base_coverage.cpp` 还要先钉"真的跑到 L2"（`0x004BA890`）⇒ 那是**跑真镜像的业务结论**，
  按测试模型不作判据（2026-11-14 删）。**缺口普查**本身的价值是清单，不是断言 —— 它现在由
  `emulator-lift --report` 的缺口清单承担（⛔ 不新写工具；静态缺口与动态足迹是两个口径，⛔ 不混）。
- `base-coverage`：`./out/emulator-lift --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE --lst ../../corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst --game-dir ../../links/install --appdata <每次全新私有 tmp> --skip-logo --audio off --present headless --until ea 0x004BA890 --max-insns 200000000 --report` ⇒ 看 `--report` 的缺口明细与 `shims : imported … / called-unimplemented …`（原测试用 `LIFT_AGE_EXE` / `LIFT_AGE_LST` 指同一对文件；★ 原 CTest 已删，不进测试）。

### OBS-20261114-05 · 删 `GpuCoverage::fallback_*` 死账的真实场景 GPU 覆盖读数（130 / 200 / 600 帧）

- **口径**：Windows x64 / `apps/emulator-lift` / **RelWithDebInfo**（`windows-x64` 预设）/ 2026-11-14 ·
  真游戏安装 `../../links/install` · 每次**全新**私有 `--appdata` · `--skip-logo --audio off
  --present headless --native-overrides all`。
- **命令原文**：`./out/emulator-lift --game-dir ..\..\links\install --appdata <全新 tmp> --skip-logo
  --audio off --present headless --native-overrides all --max-frames N`（N = 130 / 200 / 600）。
  ★ 要读 `gpu       :` / `sprite:` 两行时，同一条命令再加 `--profile` —— 它们只在 Verbose 档打印；
  默认档只有 `read :` 那一行（两次跑都在同一台机器、同一 build）。
- **读数（130 帧）**：`read : … gpu=draws 7969/tri 358551/skipped 0 native-ovr=hits 302756/fallbacks 0 …`；
  Verbose 档的固定账 = `gpu       : draws=7969 gpu=7969 recorded=7862 queued=7862 empty=107
  fragments=1075653 skipped=0`、`sprite: draws=3121 gpu=3121 skipped=0`。
- **读数（200 / 600 帧）**：200 帧 = `12239 / 552888 / 4801`、600 帧 = `36639 / 1656958 / 14401`
  （三元组 = `gpu draws` / `tri` / `sprite draws`，顺序同上面 130 帧那两条读数）。
- **逐帧后缓冲 hash**：同一 `--max-frames` 下**改前改后逐帧 hash 完全相同**（130/130、600/600 帧）
  ⇒ 删掉那三个字段没有改变任何一帧的 guest 后缓冲像素。hash 取自 `d3d9!Present` 的 Info 行
  （要 `--log-level info` 才计算/打印），比对的是两次跑**全部帧**的 hash 序列，不是抽样。
- ★ **这是读数，不是断言**：上面每个数字都只是"这条命令在这台机器上跑出多少"，
  ⛔ 不把其中任何一个写成"应该等于"。
- **未测**：① **更靠后的存档 / 战斗**场景（600 帧只到 ADV，存档链与战斗没覆盖）；
  ② 「**源矩形部分越界**」与「**目标 / 纹理非 32bpp**」这两处改行为（从 `fallback` 口径挪进
  `skipped`）之后的**命中情况** —— 这轮没有专门构造能命中它们的场景，只有上面这些真实场景读数。

### OBS-20261114-06 · `LIFT_STRESS` 四场景（S1 / S2 / S3 / S9）的实测读数

- **口径**：Windows x64 / `apps/emulator-lift` / **RelWithDebInfo**（`windows-x64` 预设）/ 2026-11-14 ·
  真游戏安装 `../../links/install` · 每次 run **全新**私有 `--appdata` · `--skip-logo --native-overrides all`。
  **口径 W** = `--present window --present-hidden --present-vsync 0`（real-time ⇒ `guest_ms` = 宿主毫秒）；
  **口径 H** = `--present headless`（虚拟时钟 ⇒ 计数确定性）。⛔ 两套参数不混写（`headless` 拒绝窗口参数）。
- **命令**：`$env:LIFT_STRESS=1; ctest --test-dir out -R "^stress-(boot|title-steady|adv-audio|gpu-coverage)$"
  --output-on-failure`（每条 N=3 遍；逐遍原始值 + 中位落 `.cache/lift-stress/<场景>-<UTC>.json`）。
- **S1 boot→TITLE 首帧**（口径 H，`--max-frames 1`，宿主墙钟）：**4957.98 / 4988.10 / 4918.05 ms**
  ⇒ 中位 **4958.0 ms**；`insns=157,120,091`（三遍逐字相同 ⇒ 读数可比）；`read :` 的
  `heap=11,166,608/15,456,208`；`arena : surfaces 21 in_use 108,595,152 peak 108,595,152 largest-free 22,591,536`。
- **S2 TITLE 稳态**（口径 W + `LIFT_FRAME_TRACE=1`，驱动 = 现成的 `scripts/bench/idle.txt`，
  稳态样本 = `[frame]` 里 **n≥100** 的行）：帧时间中位 **31.3 / 31.5 / 32.0 ms** ⇒ 中位 **31.5 ms**；
  p95 **39.3 / 40.4 / 42.7** ⇒ **40.4 ms**；每帧 `insns` 中位 **221,690**（1208–1212 帧/遍）；
  空转窗口（`at insn 250M` → `450M`）**143.90 / 145.78 / 145.60 ms/M** ⇒ 中位 **145.6**（含帧跟踪开销）。
  ★ 与 OBS-20261108-02 的 28.5k 条/帧**不是同一场景**（那是 EU-ROOM 稳态，这里是 TITLE）⇒ ⛔ 不要横比。
- **S3 ADV 一段 + 真音频**（口径 W + `--audio sdl` + `--log-level info`，驱动 = 现成的 `graphs/sn0000.graph`）：
  帧时间中位 **25.95 / 21.8 / 21.5 ms** ⇒ **21.8 ms**；p95 **87.0 / 87.3 / 86.3** ⇒ **87.0 ms**
  （含 ADV 场景切换帧，单帧最慢 1116 ms；⛔ 不剔除大帧）；
  `audio : mode=sdl device=ok fills=136–138 … fill_slow=0 underruns=0 underrun_silence_events=0
  fill_wait_timeouts=0 slot_reverify_fail=0`；
  **`fill_max_ms` = 441.56 / 428.30 / 431.73**（标定阶段另有一次 **678.60** —— 那一次越过了
  `machine.cpp:2456` 的 500 ms 告警门槛，日志里有 `⚠ audio.fill_max_ms=678.60` 那一行）。
  ★ **裁决（2026-11-14 收尾）：`fill_max_ms` 是读数，⛔ 不进判据（R1）**；`audio :` 行自带
  500 ms 告警门槛（`machine.cpp:2456`），越线时由**产品**自己报警（那行 `⚠ audio.fill_max_ms=…`），
  测试**不重复判**它。为什么：它是**单次最大值**，而 R1 明说墙钟类量只钉中位 / p95、⛔ 不钉单次
  最大值；用一条 R1 不允许的判据去重复产品已经报过的警，只会把"产品报过的那一次"变成第二条红。
- **S9 GPU 覆盖**（口径 H，`--max-frames 600`，`--log-level info`）：三遍全同（虚拟时钟 ⇒ 计数确定性）——
  `skipped_draws = 0`、`sprite_skipped = 0`、`skipped top:` **不打印**（原因表为空）、`⚠ gpu` 行**不出现**
  （⇒ `gpu_failures = 0`）；`read : … gpu=draws 36639/tri 1656958/skipped 0`；
  `gpu : draws=36639 gpu=36639 recorded=36172 queued=36172 empty=467 fragments=4970874 skipped=0`；
  `sprite: draws=14401 gpu=14401 skipped=0`；`unsupported : 0x…0800（bit11=MIPFILTER）`（已知并接受的近似）。
  标定（同一构建、同一命令）：130 帧 = `draws 7969/tri 358551/skipped 0` + `sprite 3121/skipped 0`；
  200 帧 = `12239/552888/skipped 0` + `sprite 4801/skipped 0` —— 与 OBS-20261114-05 的数逐字相同。
- **S9 的"红时逐条列原因"有了机械证据（2026-11-14 收尾）**：把"从一段 stdout 文本里提取 skip 原因
  并按格式报错"抽成纯函数 `tests/stress_skip_report.h` 的 `judgeSkipReport()`（输入 = 子进程 stdout
  文本 + 阈值，输出 = 通过/失败 + 失败消息）；`stress-gpu-coverage` 的红分支只把**真日志**喂给它。
  默认档的 `dispatch-tax`（顶层无条件注册 ⇒ 每次 `ctest` 都跑）里加了 `stress/skip-report`：
  用**合成文本**三组断言 —— ① `skipped=0` 且无 `skipped top:` ⇒ 通过；② `skipped=3` + 三行
  `skipped top:` ⇒ 失败且失败消息里**逐条原样**含那三行；③ `skipped=1` 但 `skipped top:` 缺失
  ⇒ 失败且消息明确说"原因缺失"。⛔ 不开 `LIFT_STRESS`、⛔ 不跑真游戏（真红一次 ~6 min）。
- **阈值怎么来的**（`§0.2` 的规则 + 上面哪条读数；逐条推导也写在 `tests/stress_*.cpp` 的文件头）：
  S1 墙钟中位 ≤**7290 ms**（R1：标定中位 4860 ×1.5）· S1 三遍 `insns` 极差 **0**（R2）·
  S1 `heap in_use` ≤**12,215,184 B**（R4：11,166,608 + 1 MiB）·
  S2 帧中位 ≤**46.95 ms**（R1：31.3 ×1.5）· S2 p95 ≤**57.9 ms**（R1：38.6 ×1.5）·
  S2 吞吐 ≤**178.74 ms/M**（**R3 偏离 ×1.25**：标定中位 142.99 ×1.25 ⇒ 178.7375 取 178.74；
  ⛔ 不是 R3 的 ×1.10，偏离理由见下面"薄余量"那一段与 `tests/stress_title_steady.cpp` 文件头 ④）·
  S3 帧中位 ≤**31.875 ms**（R1：21.25 ×1.5）· S3 p95 ≤**130.05 ms**（R1：86.7 ×1.5）·
  S3 音频四个计数 = **0**（R2：`fill_slow` / `underrun_silence_events` / `fill_wait_timeouts` / `slot_reverify_fail`）·
  S9 `skipped_draws` / `sprite_skipped` / `skipped top:` / `⚠ gpu` 四项 = **0**（R2）。
- ★ **这是读数，不是断言**：上面每个数字都只是"这条命令在这台机器上跑出多少"；
  阈值（断言）在 `tests/stress_*.cpp` 里，每条都注明了规则编号 + 依据读数。⛔ 不把任何一个数写成"应该等于"。
- **同一命令的第二次真跑（`-R "lift-stress|stress"` 整条 gate；**共享负载** 17.76% → 19.38% 忙）**：
  S1 **5486.7 / 5522.1 / 5455.9** ms ⇒ 中位 5486.7（比低负载那轮 +10.6%）；
  S2 帧中位 **40.8 / 32.7 / 31.7** ⇒ 32.7、p95 **47.7 / 46.0 / 39.0** ⇒ 46.0、吞吐 **184.37 / 148.71 / 144.91** ⇒ 148.71；
  S3 帧中位 **21.95 / 21.3 / 21.8** ⇒ 21.8、p95 **86.1 / 87.5 / 85.6** ⇒ 86.1、`fill_max_ms` 446.57 / 468.05 / 429.53；
  S9 与上一轮**逐字节相同**（36639 draws / skipped 0 / sprite skipped 0 / 帧 600）。
  ★ 值得记住的一条：S2 第 1 遍的吞吐 **184.37 ms/M 单遍越门（当时的门槛 157.29）**，而 **N=3 中位 148.71 在门内** ——
  这正是 `§0.2 R1`"只钉中位 / p95、⛔ 不钉单次"的用途；反过来说，**单遍读数不许当基线**。
  ⚠ 同时它暴露一个**薄余量**：门槛由低负载（~7.5%）的读数派生，负载 18% 时中位离门槛只剩 5.5%。
  ⇒ **2026-11-14 收尾裁决：把吞吐门从 R3 的 ×1.10 放宽到 ×1.25（= 178.74 ms/M）**。理由：R3 的
  ×1.10 派生自**空闲机**的 1.2–1.4% 噪声，而本档在**共享负载**下观测到的批间 + 遍间离散远大于此；
  ×1.25 仍能抓住真正的回归（真实回归通常是 2× 量级，不是 5%），同时不会让"机器忙"伪装成回归。
  判据仍落在 **N=3 的中位**上（⛔ 没有改成"挑最好一遍"）；JSON 的 `load.cpuBusyPctAtStart/End`
  与红时的失败消息都带着负载档 —— 红了**先看负载**。
  ★ 收尾轮的 S2 单跑（2026-11-14；按裁决只跑这一条，其余三条阈值没动所以不重跑）：
  吞吐 **157.17 / 143.53 / 143.31 ms/M** ⇒ 中位 **143.53**（门槛 **178.74**，余 24.5%；最慢一遍
  157.17 也在门内）；帧时间中位 **31.4 / 31.4 / 32.2** ⇒ 31.4、p95 **38.3**；
  负载 **9.06% → 6.35%**（**低负载**）⇒ 这一轮没有复现上面 18–19% 的共享负载情形，
  它证明的是"新门槛真的被读进 JSON 且判定通过"，不是"新门槛在 18–19% 下也够"。
- **未落（留给下一批）**：S4（OPTION 往返）/ S5（存档读档）/ S6（EU-ROOM 进出）/ S8（30 min 长跑）——
  理由 = 与已落的四类量**同质且贵**（墙钟 / 内存字节 / 音频计数 / 稳定性），不是"做不出来"。
- **判不准**：① `fill_max_ms` 的归属**已裁决**（2026-11-14 收尾）：只当读数、⛔ 不进判据（R1）；
  理由见上面 S3 那条（它是单次最大值；500 ms 由**产品**自己报警，测试不重复判）⇒ 不再"待裁决"；
  ② S2/S3 的读数**含** `LIFT_FRAME_TRACE` 与 `--log-level info` 的开销 ⇒ ⛔ 不与
  `pnpm tools lift bench idle` 的 105.87 ms/M（无帧跟踪）混表（`measurement.md` M-02）；
  ③ 本机没有 loadavg，负载档记的是 `GetSystemTimes` 忙占比的采样值（S2 那轮 7.5% → 6.9%）；
  ④ `[frame] wall` 只覆盖到"相邻两次 Present"，⛔ 不含首次 Present 之前的 boot（那一帧在 S1 里）。

### OBS-20261114-07 · 三场景耗时结构（Windows）：进入 FELLOW 10.29 s 单帧 / 确认保存 ≥12.6 s 阻塞 / 读档 ≥10.3 s 阻塞

- **口径**：Windows x64 / **RelWithDebInfo**（`windows-x64` 预设）/ HEAD `aeed0d7` /
  `apps/emulator-lift/out/emulator-lift.exe`（mtime 2026-11-14 04:03:26）/ 2026-11-14 ·
  `--present window --present-hidden --native-overrides all --skip-logo --audio off
  --game-dir ../../links/install` · `LIFT_FRAME_TRACE=1` · `--profile --profile-top 40` ·
  每次 run 用 `.tmp/appdata` 的独立副本（9 文件 / 3,203,018 B，基准 mtime 见 `.tmp/profile/readings.md` §5.9）。
- **进入 FELLOW**（`graphs/sn0000-save-profile.graph` 的 `fellow-enter` 窗，右键 `click 500 500 right` + 12 s）：
  窗 wall **12,079.0 ms**；buckets guest 2,607.4 / shim 1,026.8 / **interp 5,878.3** / host 2,488.3
  （其中 input-script 2,454.7）/ native 75.1 / unattributed 3.0。
  **窗内只有 1 帧 > 500 ms**：`[frame] n=1387 wall=10,290.3 ms insns=231,522,324`（`guest=2,394.1 shim=229.3 other=7,666.9`）
  ⇒ 12 s 的 85% 是**一帧之内的转场/装载**。guest self 最大两项：`sub_412290` **756.4 ms**（`calls=0` + `!BLIND`，
  VM 循环头）· `sub_41BF50` **635.8 ms**（612,500 calls，incl 1,749.7）。
  `OBS-20261112-06` 里这一窗的最大 guest 项 `sub_42F560`（self 1,899 ms）已被段级 override 接管 ⇒ 本轮 `native=75.1 ms`。
- **确认保存**（`save-confirm` 窗，`click 650 320` + 15 s）：
  - `--profile` 臂：窗 wall **15,134.8 ms**，guest 5,711.1 / shim 366.5 / native 31.9 / **interp 6,081.7** / host 2,941.9；
    **但这次保存没写完** —— 日志 `stop @ eip=00436D81` + `present-tail tail_insns=197,647,566 tail_wall_ms=15,465.4`，
    落盘 `SAVE00.DAT` = **0 字节**（`SAVE.DAT` 未变）⇒ 15.5 s 只是**下界**。
  - **同一张图去掉 `profile_*` 的臂**（`save-noprof`）：保存**完整跑完**（`SAVE00.DAT`=1,043,873 B、`SAVE.DAT` 49,216→49,288），
    阻塞段是**一帧** `[frame] n=1756 wall=12,626.3 ms insns=220,071,152`。
    ⇒ `--profile` 在保存段至少 **1.225×**（15,465.4 / 12,626.3，且那一臂还没跑完）。
  - guest 工作分布（profile 臂，self / incl / calls）：`sub_4A42C0` **2,767.7 / 7,599.4 / 1** ·
    `sub_436D50` **1,013.6 / 2,822.6 / 1** · `sub_436D00` **679.2 / 2,061.1 / 1** ·
    `sub_49EE60` 496.1 / 960.7 / **921,600**（= 640×480×3，数量级与"逐像素"一致，未证语义）· `sub_40CD10` 435.3 / 5,801.7 / 1。
- **加载存档**（`TITLE → click 1070 480` 进 LoadData）：
  - **点「最新ロード」只弹二次确认框**（脚本身份 `SAVE.BIN` → `SBUNKI.BIN` / `SBUNKIMOVE.BIN`，marker 与截图两路证据）。
    确认框 `是` 的客户区坐标 = **(640, 323)**（青绿 bbox x=[537..742] y=[304..342] 的中心；`否` ≈ (640, 386)）。
    ⇒ **"进 LoadData + 点最新ロード + 等 12 s"这个窗口不是读档**：wall 12,124.9 ms，guest 2,175.1 / shim 3,447.7 /
    interp 4,653.8 / host 1,751.9；142 帧、**max 257.2 ms、没有 >500 ms 的帧**；shim 里**没有 `Sleep`**
    （前 12 项全是 D3D：Present 1,241.8 / DrawIndexedPrimitive 558.9 / Clear 522.7 / DrawPrimitive 318.3，
    `SetRenderState` 35.2 万次 188.3；前 12 项合计 3,106.6 ms vs shim 桶 3,447.7 ⇒ 余下摊在 ≥13 项、每项 ≤26.6 ms；
    `grep -i sleep` = 0 命中）。
  - **点「是」之后才是读档**：窗 wall 12,165.6 ms，但**最后一帧之后 10.32 s / 0 次 Present**
    （`present-tail tail_insns=168,898,371 tail_wall_ms=10,316.4`，进程停在 `eip=0042DEE0`）；
    同一窗口不开 `--profile` 复跑 = 10,336.7 ms ⇒ **读档 ≥10.3 s，且 12 s 窗口没跑完（下界）**。
    guest：`sub_42DDE0` self 1,545.4（无 `--profile` 臂 2,372.6）/ **incl 10,303.3 / calls=1** ·
    `sub_436D50` 1,163.5 / 3,310.3 / 2 · `sub_436D00` 1,039.4 / 3,204.4 / 2。
- **两条读法（不是结论）**：
  ① **`[frame]` 表看不见阻塞段**：保存/读档尾段 **0 次 Present**，墙钟只能从 `present-tail` 读；
  同一件事的侧证是 `[input-script] ⚠ 帧停滞 13386ms/10224ms` 这行告警。只看 `[frame] wall=` 会得出"没有慢帧"的错结论。
  ② `--profile` 的代价**按路径不同**：保存段 ≥1.225×（压缩密集），读档段落在噪声内（12,165.6 vs 12,137.2 ms，+0.23%）。
- **顺带测到（不是本条的判据）**：凡进过 LOAD 界面的 run 都写了 `SAVE.BAK`（= `SAVE.DAT` 的内容）⇒ **"加载存档"不是纯读**；
  进 LOAD 界面 / 点最新ロード / 退出三件事挨在一起，**没辨开**是哪一步写的。
- **复现**：四臂命令 + exit code 见 `.tmp/profile/readings.md` §0（四臂全 exit 0、`detail : interaction: task done`）；
  原始日志 `.tmp/profile/run-{fp,fn,lp,ln,lc,le,lx,ls}.log`；图 `graphs/sn0000-save-profile.graph` 与
  `.tmp/profile/{load,load-confirm,load-noprof,save-noprof}.graph`。
- **单次读数**：每臂 1 次，**没有 N=3 中位数**（`--time-limit 400/200` 一次都没碰到）。⛔ 不与其他会话的数横比。

### OBS-20261114-08 · `--cursor-warp` 在真游戏里的那一次调用（加载存档界面的自动移光标）

- **口径**：Windows x64 / RelWithDebInfo / `--present window`（**可见**，不加 `--present-hidden`）/ `--log-level info` /
  `--native-overrides all --skip-logo --audio off --game-dir ../../links/install` / `--appdata` = `.tmp/appdata` 的独立副本 /
  图 `.tmp/cursor/load-warp.graph`（`await script TITLE` → `click 1070 480` 进 LoadData → `click 475 697` 最新ロード → 等 5 s）。
- **读数（`--cursor-warp` 开）**：启动行 `present : … window=visible cursor-warp=on present=direct/immediate` +
  `cursor-warp: on`；随后**恰好一次**
  `[shim] user32!SetCursorPos: (640,325) 虚拟光标 + 宿主光标（客户区 640,325）`；
  进程 exit 0 / **21.76 s**；**一次性读回自检没有 warn** ⇒ 系统光标真的落到了目标（偏差 ≤1 px，`present_sdl.cpp` 的
  `hostCursorQueryScreen` 自检）。
- **读数（控制臂：同一条命令去掉 `--cursor-warp`）**：同一调用点的日志是
  `[shim] user32!SetCursorPos: (640,325)（虚拟光标位置 / 屏幕坐标；--cursor-warp 未开或没有可见窗口）`、
  `cursor-warp=off`；exit 0 / **21.86 s**。
- **两臂的 `SetCursorPos` 实参逐字相同（都是 `(640,325)`）** ⇒ 开关没有改变 guest 可见调用（这是
  `REQ-01M7BXM46NVYY3QHYW2ZKJZKZN` 判据 4 的现场证据）。
- ★ 那个坐标就是**二次确认框「是」**的位置：`OBS-20261114-07` 用截图量出的 `是` 中心是 `(640,323)`（bbox 高 38 px）。
  两者测量方法不同（引擎实参 vs 像素 bbox），差 2 px ⛔ 不要当矛盾 —— 它证明的是"引擎在加载存档界面把光标
  **自动移到二次确认**上"，与用户描述一致。
- **复现**（`apps/emulator-lift` 下；两份 appdata 副本互不共用）：
  ```powershell
  # 开
  ./out/emulator-lift.exe --present window --cursor-warp --native-overrides all --skip-logo --audio off `
    --game-dir ../../links/install --appdata ../../.tmp/cursor/appdata-warp `
    --interaction-graph ../../.tmp/cursor/load-warp.graph --max-frames 3000 --time-limit 120 --log-level info
  # 关（控制臂）
  ./out/emulator-lift.exe --present window --native-overrides all --skip-logo --audio off `
    --game-dir ../../links/install --appdata ../../.tmp/cursor/appdata-warp2 `
    --interaction-graph ../../.tmp/cursor/load-warp.graph --max-frames 3000 --time-limit 120 --log-level info
  ```
  原始日志 `.tmp/cursor/run-warp-{on,off}.log`。
- **没测到的**：未聚焦时（光标会落到别的 app 上）与多显示器 / 非全屏窗口（`win.x/win.y ≠ 0`）的观感没实测；
  macOS 分支本机没有。

### OBS-20261114-09 · alpha test 拒批的命中面（600 帧 boot→TITLE = 0 次）与 `sn0000.graph` 的首次点击丢帧

- **口径**：Windows x64 / RelWithDebInfo / HEAD `aeed0d7` 之后的当前构建 / 2026-11-14 ·
  `--present headless --native-overrides all --skip-logo --audio off --log-level info --log-cat d3d9` ·
  `--game-dir ../../links/install` · 每次 run 用空的 `--appdata`。
- **读数（alpha test 命中面）**：`--max-frames 600` 跑 boot→TITLE，
  `GPU 不支持 alpha test` **0 次命中**；`unsupported :` 只有 `…0800（bit11=MIPFILTER）`（bit30 不出现）。
  ⇒ 至少在这 600 帧里，**没有任何一批被 alpha test 拒**；这个能力缺口在 boot→TITLE 段**没有可观测后果**。
  ⚠ 但 ADV / 游戏内没有量到（见下条：这次 ADV 跑没走出 GAMESTART）⇒ ⛔ 不许把这条读成"游戏里从不会踩到"。
- **读数（`sn0000.graph` 的首帧点击丢失，`REQ-01M78A0V0X1X1PCK7MP71Q1J81` 的同一症状）**：
  `--present headless --interaction-graph ../../graphs/sn0000.graph --max-frames 1200 --time-limit 200` ⇒ **exit 5**：
  ```text
  stop      : interaction-failed @ eip=004CADE2
  detail    : interaction: timeout · 节点 `step3` 等 `script GAMESTART` 超过 5000 ms
              （frame 8..224，insn 158584410..206610013，guest_ms 15858..20859）；窗口内采样 216（累计 224）；
              条件在窗口内一次都没成立；samples=224 changes=223 last_frame_hash=0xB658DA99AC8E9FDF at frame=224
  ```
  ⇒ **一次就复现**（原单记的是 3 次手工 1 次）。差别只在档位：这里是 `--present headless` + 空 appdata，
  原单是 window/ctest。⚠ 这也让"ADV 段有没有 alpha test 批"这个问题**这次没答上**（画面停在 TITLE→GAMESTART 之间）。
- **复现**（`apps/emulator-lift` 下）：
  ```powershell
  # ① alpha test 命中面（应输出 0）
  ./out/emulator-lift.exe --appdata ../../.tmp/alphatest/appdata --game-dir ../../links/install `
    --present headless --native-overrides all --skip-logo --max-frames 600 `
    --log-level info --log-cat d3d9 2>&1 | Tee-Object ../../.tmp/alphatest/replay.log
  Select-String -Path ../../.tmp/alphatest/replay.log -Pattern 'GPU 不支持 alpha test'
  # ② 首帧点击丢失（应 exit 5 + 上面那段 detail）
  ./out/emulator-lift.exe --appdata ../../.tmp/alphatest/appdata2 --game-dir ../../links/install `
    --present headless --native-overrides all --skip-logo --audio off `
    --interaction-graph ../../graphs/sn0000.graph --max-frames 1200 --time-limit 200 `
    --log-level info --log-cat d3d9
  ```
  原始日志 `.tmp/alphatest/replay.log` / `.tmp/alphatest/adv.log`。

### OBS-20261114-10 · 解释器每条指令的成本（微基准）＋ "25 ns/条" 这个数的重新解释

- **口径**：Windows x64 / RelWithDebInfo / `apps/emulator-lift` / 2026-11-14 · 新增 `tests/insn_cost.cpp`
  （CTest `insn-cost`，顶层 pure；附加臂用 `LIFT_INSN_COST_EXTRA=1` 门控）。
  ⚠ **本机当时有后台游戏在跑**：同一二进制的整轮 `alu-loop` 在 45.5–57.4 ns/条 之间摆，校准 `imul` 链
  ⇒ 有效 **2.26 GHz**（标称 3.2）、`steady_clock::now()` = **27–29 ns/次**（`dispatch-tax` 历史记录 4.8–10）
  ⇒ **下面的绝对 ns 偏大 1.5–2×**，只可用于"形状之间的比值"与"量级判定"。
- **读数**（每遍 1e7 条、3 遍轮转、中位 / 极差）：

  | 形状 | ns/条 | 极差 | ns/轮 |
  |---|---:|---|---:|
  | `alu-loop`（4 条/块，无访存无调用） | **55.74** | [35.86, 56.97] | 222.97 |
  | `mem-loop`（+ 每轮 1 读 1 写） | **63.51** | [42.83, 65.63] | 254.06 |
  | `call-loop`（+ 每轮 1 对 call/ret，跨 3 块） | **99.69** | [69.65, 99.91] | 498.43 |
  | `x87-loop`（+ 每轮 1 条 x87） | **64.20** | [44.96, 68.27] | 321.00 |
  | `shim-tick`（+ 每轮 1 次最轻 shim） | **66.89** | [43.57, 69.86] | 668.86 |

  两点拟合（`alu` 4 条/块 vs `alu-wide` 10 条/块）：**每块 b = 107.0 ns、每条 c = 29.0 ns**；
  残差单价：访存 **+15.6 ns/次**、call+ret **+32.5 ns/对**、x87 **+23.0 ns/条**、shim **+165–208 ns/次**。
- **真实负载对照**（`--present headless --skip-logo --audio off --max-frames 600`，同一构建）：
  `insns = 1,012,065,649`、块执行 **156,820,530** ⇒ **k = 6.45 条/块**；整进程 **70.7 / 81.4 ns/条**；
  另跑一遍 `--profile`（`insns` 逐字相同）：guest-block **43.454 s = 42.94 ns/条**、shim（Present+API）21.924 s、
  其余（块间派发 / 宿主 / 进程）32.78 s。模型外推到 k=6.45 ⇒ 45.6 ns/条，与实测 guest-block 42.94 **差 ~6%**。
- **★ "25 ns/条" 的口径更正**：`interp` 桶按定义 = **每次 dispatcher 迭代里剩下的全部、不含块执行**
  （`runtime/window_profile.h`）⇒ `interp / insns = (每块记账) / k`，**它不是"每条指令的派发底价"**。
  `OBS-20261114-07` 的 FELLOW 窗（`interp 5,878.3 ms` / 236.7M 条 / **16,762,215 次迭代** ⇒ k = **14.1**）
  应读成 **每块 interp ≈ 350 ns**，而不是"每条 25 ns"。装窗口 profiler 时同机实测每块 interp = **250.3 ns**；
  **装 / 不装仪器同循环 480 vs 223 ns/块（+257 ns/块）** ⇒ 窗口 profiler 自己的税也是每块量级。
- **由此得到的成本形状**：解释器 ≈ **每块 ~107 ns + 每条 ~29 ns**（未做仪器时、本机偏大 1.5–2×）。
  真实负载的块间开销实测 **209 ns/块**（是本文件 b 的 2×），最可能是**代码足迹**（本文件永远命中同一个块）。
- **未解释**：整进程墙钟里 **56% 不在 guest 指令上**（shim 21.9 s + 其余 32.8 s）；真实指令混合**没有 opcode 直方图**
  ⇒ ⛔ 给不出"派发 / 解码 / flags / 访存"的百分比分解。**要百分比就得补直方图**，不是靠这五个形状推。
- **复现**：`ctest --test-dir out -R insn-cost --output-on-failure`（默认 9.6 s）；
  `LIFT_INSN_COST_EXTRA=1 ./out/tests/lift-insn-cost.exe`（附加臂）；完整报告 `.tmp/perf/insn-cost-report.md`。
  macOS 臂（未跑）：`cmake --preset macos-arm64 && ctest --test-dir out -R insn-cost`。

### OBS-20261114-11 · `.text` 能不能被静态 lift：间接控制流普查 + 按执行指令数加权的可 lift 覆盖率

- **口径**：语料 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`（只读）· 读数日志
  `.tmp/perf/run-{fp,lp}-top4000.log`（同一构建 / `--profile-top 4000`）· 2026-11-14 · 全部数字由脚本产出
  （`.tmp/perf/lift-census.mjs` → `lift-report.mjs` / `coverage-top4000.mjs`；⚠ 脚本在 `.tmp/`，不入库）。
- **规模**：`.text` VirtualSize **1,163,264 B**；`proc near` **3,810** + IDA **折叠函数 1,397**（168,727 B，正文不在语料）
  ⇒ 函数入口 **5,207**；指令字节 947,271（81.4%）；函数大小 min 3 / **median 96 B** / p90 497 / max 14,552。
- **间接控制流（lift 的生死判据）**：

  | 形态 | 站点 | 能否 lift |
  |---|---:|---|
  | `call reg` | **3,028** | 能 —— 机械 lift 即"间接调用"，被调方按 `EA → lifted 函数指针` 查表（dynarec 常规做法） |
  | `call dword ptr [mem]` | 15 | 能（同一套查表） |
  | `jmp reg` / `jmp dword ptr [mem]` | **26 + 6 = 32** | **不能静态解析**（真·函数指针跳转） |
  | 跳转表 | **62 张 / 663 case**（表长中位 8） | 能；`disasm --cases` 与语料 **41/41** 逐张对账一致 |

  ⇒ 真正"透镜不可靠"的只有 **32 个站点 / 31 个函数 / 788 字节 = 指令字节的 0.08%**，且这 31 个函数**一次都没进过任何 profile 表**。
  尾跳：`jmp <函数入口>` **1,671** 个（其中 521 指向折叠函数）；跳函数中段 113 个；**目标不可解析的 jmp/call = 0**（全部由 PE rel32 算出，不靠符号名）。
- **按执行指令数加权的覆盖率**（分母 = 日志自己的 `total weight`，即"块内执行过的指令数"之和；
  ⛔ 不用 `insns` 当分母——它与 `total weight` 的差是 override/SMC 造成的，override 那部分根本不在块表里）：

  | 场景（日志） | A（直路+直接调用） | B（有界 switch） | C（间接调用） | D（不可解析） | E（语料无正文） | **A+B** | **A+B+C** |
  |---|---:|---:|---:|---:|---:|---:|---:|
  | fellow/save（`run-fp-top4000.log`，total weight 462,531,915） | 51.930% | 11.958% | 32.235% | 0.002% | 3.874% | **63.889%** | **96.124%** |
  | load（`run-lp-top4000.log`，total weight 160,536,535） | 56.435% | 8.489% | 28.485% | 0.005% | 6.586% | **64.924%** | **93.409%** |

  ⇒ 该两个场景里 **lift : fallback ≈ 14–25 : 1**；`ΔC = 28.5–32.2 pp` ⇒ **只做 A+B 的省事方案不成立**（63.9% < 90%）。
- **旧读数的区间被收窄**（不是被推翻）：top-40 口径给的是 `A+B+C = 84.5%` + 表外未知 14.8% ⇒ 区间 **[84.5%, 99.3%]**；
  本轮把表外权重收进来后落在 **96.1%**，在区间内。表外未知桶以 **A 为主**（A 64.7% / C 12.8% / E 20.6% / B 2.0% / D 0.01%）。
- **未测 / 缺口**：① 只有这两段采样（`sn0000-save-profile` 与 load 图），**ADV/战斗/立绘等场景没量**；
  ② **E 类 3.9–6.6% 权重是"语料里没有正文"（1,397 个折叠函数）** —— 不是"lift 做不到"，而是"要用我们自己的解码器补语料"；
  ③ 块级灵敏度（三份日志 top-40 热块里含不可解析间接转移的权重 = **0**）说明 D 类也没进过热点。
- **复现**：`--profile-top 4000` 重跑（`--present window --present-hidden --native-overrides all --skip-logo --audio off`
  + `graphs/sn0000-save-profile.graph` 或 `.tmp/profile/load.graph`），再 `node .tmp/perf/coverage-top4000.mjs`
  （load 场景用 `coverage-lp.mjs`，只换了日志与输出路径）。完整报告 `.tmp/perf/{lift-census,coverage-top4000,coverage-lp-top4000}.md`。

### OBS-20261114-12 · 单块 IR spike：手写 `.ll` + `clang -O2` 与解释器的逐位差分，以及速度比

- **口径**：Windows x64 / `C:\Program Files\LLVM\bin\clang.exe`（22.1.2，与 CMakeCache 的 clang-cl/lld-link 同目录）· 2026-11-14 ·
  两块从真实热点里挑的块（`--profile-top 4000` 表）：**A = `0x436D81`**（`sub_436D50+0x31`，9 条 CRC 内层自环，
  hits 4,570,461 / w 41,134,149 = 8.89%）、**B = `0x4A44D4`**（`sub_4A42C0+0x214`，16 条，末条 `call sub_49EE60`，
  hits 921,600 / w 14,745,600 = 3.19%）。产物与脚本都在 `.tmp/irspike/`（不入库）。
- **差分（判决点）**：同一份初始状态喂「解释器（`Machine` + `placeCode`，`max_insns` 精确停在块边界）」与「lifted 机器码」，
  比较 **8 个 GPR + EFLAGS 的 6 位 + 64 KiB 内存 + 出口 EIP**：块 A **2000 组**、块 B **1200 组**、块 A 长跑 5000 轮
  ⇒ **不一致 0**（含 `edx=0/0x80000000` 的借位/溢出边界、`jns` 两个出口、非对齐访存）。
  另验：lifted 码读**未提交页**走既有 VEH ⇒ `_Exit(3)`，与解释器一致（**无 landing pad**）。
- **速度（同进程、同块、4.45 GHz、3 遍中位 + 每遍内比值）**：

  | | ns/块 | ns/条 | 倍数 |
  |---|---:|---:|---:|
  | 解释器 `0x436D81`（9 条） | 218.0 | 24.22 | 1× |
  | lifted A，一块一次（含 call + 状态往返） | 3.77 | 0.419 | **57×** |
  | lifted A，自环融成一个 IR 循环 | 1.85 | 0.206 | **118×** |
  | lifted B（16 条，含 1 个直接 call） | 5.20 | 0.325 | 110×（k=16 模型折算，无法同进程直测解释器） |

  拆解：**纯执行 1.85 ns/块 + 块边界 1.92 ns/块**（call/ret + 9 load + 9 store）；8.2 周期/轮 = 该 CRC 依赖链的 CPU 下界
  ⇒ 自环那档已经**跑满**。`OBS-20261114-10` 的 ~209 ns/块是**解释器的性质**，不是 lift 的成本。
  ⚠ 用 `OBS-20261114-10` 的 b/c 模型折算会得 k=1 → 325×、k=9 → 97.6×，**比同进程实测高 1.7×**（那是更慢机器状态的读数）
  ⇒ 报结论时**取同进程实测的 57–118×**。
- **工程形态**：IR **零新 runtime 原语**（一个 guest 基址指针 + 9×i32 状态 + 纯 IR flags：`ctpop` / `sadd|ssub.with.overflow` /
  `lshr` / `icmp`）。要新写的是**lift 引擎**（EA→函数派发 / native 代码缓存与 SMC 失效 / 块边界的定时器与输入检查 /
  call-ret 与解释器·shim 的互操作）。体量：`blockA.ll` 305 行、`blockB.ll` 163 行 ⇒ 平均 **13.3 / 6.7 条 IR 行每条 guest 指令**；
  naive 全发 flags 会被 LLVM 的 DCE 掉，**不需要**额外做 liveness。
- **两条必须写进 lowering 约定的坑**：① **EA 必须零扩展到 i64 再接 GEP**（`i32` 下标会符号扩展 ⇒ EA ≥ 0x80000000 全错）；
  ② `fs:`/`gs:` 段覆盖要把 `fs_base` 作为参数传进来。
- **未测**：arm64 实跑、SMC、跨块 CFG（只做了单块）、x87（完全没碰）、真实负载整段墙钟 A/B。
  **唯一能推翻结论的量**：真实块间派发如果是 **20–50 ns/块**（而不是本 spike 的 1.9 ns），那 57× 会掉到个位数。
### OBS-20261114-13 · 三处"CPU 兜底路径"的现行命中面：两个场景里 bit21/22/23 都是 0

- **口径**：Windows x64 / RelWithDebInfo / 2026-11-14 · `--present window --present-hidden --native-overrides all
  --skip-logo --audio off --game-dir ../../links/install` · `--log-level info --log-cat d3d9` · 每次 run 用
  `.tmp/appdata` 的独立副本 · 图 = `graphs/sn0000-save-profile.graph`（TITLE→GAMESTART→ADV→FELLOW→SAVE，**exit 5**：
  后段 `step18 no-sample`，与本节无关）与 `.tmp/profile/load.graph`（TITLE→LoadData→读档，exit 0）。
- **读数**：两次 run 的 `unsupported :` 行**都只有 bit11**：
  `0x…0800（bit11=MIPFILTER）` ⇒ **bit21（透视 invw≠1）/ bit22（mip 缩小）/ bit23（GPU 后端失败）在两段里一次都没置位**。
- **口径说明**：bit11 是**已知并接受**的近似（`MIPFILTER` 的 `max_lod` 处理），不在本条的三个关注位里；
  ⛔ 不要把它算成"命中兜底路径"。
- **未测**：OPTION 界面与战斗场景（仓库里**没有**对应交互图）；fp 那次 `exit 5` 之前只走到 SAVE 之后。
- **复现**：上面两条命令 + `Select-String -Pattern 'unsupported :'`；原始日志 `.tmp/lift-tickets/fallback-census-{fp,lp}.log`。

### OBS-20261114-14 · x87 退化成 `double` 会不会有可见差异：CW 的 PC=53 是决定项

- **口径**：Windows x64 · 2026-11-14 · 只读语料 `corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst` + 二进制
  `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`（sha256 与 `.lst` 头部一致）· 数值实验 = 本机 MSVC 19.51 编的
  **x86（32 位）** 小 harness（真 x87 指令 + 真 `fldcw`；`double` 参照走 SSE2），**不链接模拟器**。产物 `.tmp/irshape/x87/`。
- **决定项：这个二进制把 x87 的 precision control 固定成 53 位，此后从不改。** 唯一路径：
  `_cinit(1)` @`0x4EE154`（唯一调用点 `0x4EDB59`，前一条 `6A 01` = `push 1`）→ 间接 `call ds:[0x52FDA0]`
  （= `0x4E7F38` = `__fpmath`）→ `__setdefaultprecision`(`0x4F2525`) 压 `0x30000`(_MCW_PC) / `0x10000`(_PC_53)
  → `__controlfp_s`(`0x506F28`) → `__control87`(`0x510D62`)；`__control87` 自己的解码证明 `_PC_53 ⇔ CW bits8-9 = 10`。
  全 `.text` 的 CW 站点 = **142 站 / 17 个函数**（`fldcw` 94 + `fnstcw`/`fstcw` 48），其中 47 站是 `or reg,0C00h`
  （**只改 RC=向零**）+ 47 站原样还原；`or …,300h` / `and …,0F3FFh` / `fldcw 立即数` / `fldenv`/`frstor`/`finit`
  全 **0 站** ⇒ **没有任何站点碰 PC 位**。同族 MSVC x86 CRT 的入口 CW 实测 = `0x027F`（PC=53 / RC=就近）。
  - 复现：`node .tmp/irshape/x87/cw-sites.mjs`（142 站逐站上下文）· `node .tmp/irshape/x87/callscan.mjs 0x4F2525 0x506F28 0x510D62`
    （得 `0x4E7F48 → 0x4F2525 → 0x506F28 → 0x510D62`。★ 这些 CRT 函数在 `.lst` 里是 **COLLAPSED**，
    按文本 grep 找不到调用者，必须字节扫 `E8 rel32`）。
  - ⇒ **推论**：在真机上 x87 的 `+ - * /` 本来就每次舍入到 53 位 ⇒ lift 时用 `double` 对四则是**保真**，不是退化。
- **读数**（只翻 CW 的 PC 位；每行 200,000 组随机输入里"结果位型不同"的组数）：

  | 实验 | PC=53 | PC=64 | PC=24 |
  |---|---:|---:|---:|
  | 链式 `(a*b+c*d)/(e-f)` | **0** | 73,640 | 200,000 |
  | (a) `(a/b+0.5)` 再截断成整数 | **0** | 构造 `13-0.75·2⁻⁶⁰` ⇒ trunc=**12**（53 位 ⇒ 13） | 200,000 |
  | (c) 128 项点积 | **0/200** | 173/200 | — |
  | (e) `fstp qword` 再参与运算 | **0** | — | — |
  | (d) `fild qword` | 二进制里 **0 站**（`fistp qword` 6 站，输入已是 53 位 ⇒ 空类） | | |

- **三档**：(i) **保真** = 四则 / 有序与无序比较 / `(int)` 转换 / 长累加 / `fstp` 32·64 位；
  (ii) **不可见** = `+0.5` 再截断的惯用法（`0x467798`/`0x46F71C`/`0x47144E`）、饱和夹紧（`0x4046B8` 起）、NaN 守卫；
  (iii) **可能可见** = 超越函数站点 + **80 位指数值域带**（`1e200*1e200` 在 80 位有限、`double` = inf；
  随机 `(x*y)/z` **16,344/200,000** 不同）+ NaN payload。
- **超越函数本来就不同**（与 fp80→double 决策无关，属既有偏差）：`fsin(1.0)` x87 = `3FFED76A…` / libm = `3FEAED54…`；
  2000 组里 fsin 4 / fcos 2 / fyl2x 150 位不同。二进制里实际用到的站点：`fsin`@`0x4CCC80`/`0x4CCC84`、
  `fcos`@`0x4C8D2E`、`fpatan`@`0x4CF1A1`/`0x4CF1AC`。
- **浮点比较是空类**：全 `.text` 的 `fnstsw` 掩码只有 `41h`×161 / `44h`×46 ⇒ **0 处相等比较**、0 处读 PE 位。
- **给 lift 的四条硬约束**：① 那 47 个站点的 RC=向零必须复现（`fptosi`/`(int)` 天然满足）；
  ② ⛔ 绝不能把 PC 当 64 位（否则四则的"保真"全部失效，36.8% 位差）；③ 内存里的 `REAL4` 槽位按 `float` 建模
  （`fstp dword` 1209 站；统一升 `double` 的差异比 fp80→double **大得多**）；④ 超越函数站点单列。
- **未测**：arm64（软浮点路径）· 真实场景 A/B（只有离线序列，没有跑游戏比存档字节）。
  **唯一能推翻 (i) 的量**：有消费者读 `fistp` 之前的 80 位中间值并靠 **64 位指数**区分（语料里 `tbyte` 只出现在 CRT 自己）。

### OBS-20261114-15 · Shape A 原型（阶段 A+B）：IR 与 shim 链成单一二进制、解释器不在执行路径上

- **口径**：Windows x64 · RelWithDebInfo · AMD64 Family 25 Model 68 · 2026-11-14 ·
  载荷 = `AGE.EXE` 的**真字节**（`sub_436D00` 80 B + `sub_4D4B40` 45 B，`placeCode` 进 guest，**不加载 PE**）·
  `.ll` 由 `clang -c -O2` 编成 obj、与 `emulator-lift-runtime` **链进同一个测试 exe** ·
  门禁 `ctest --test-dir apps/emulator-lift/out -R "lift-abi-probe|lift-ir-proto|lift-ir-shim"`（三条都 pure 档）。
- **形态（本原型证到的）**：入口 `i32 fn(LiftRegs*, u8* mem, u32 resume_ea, void* cpu)`；trampoline 只做
  `EA → 宿主函数` 表查 + 间接调用；**分量内分支直接跳**；`call rel32` = 压 guest 返回地址 + 返回被调方 EA；
  `retn imm16` = 弹返回 EA（`esp += 4 + imm16`）+ 返回它；shim 调用 = `lift_shim(桩 EA)` 薄包装 → `invokeById`。
  解释器**只**在混合模式（阶段 A 的两个建表器）与差分 oracle 里出现。
- **差分 0 不一致**：阶段 A **240 组**（冷 120 / 热 120，`n ∈ {0,1,3,31,4096}` × 随机 buffer，比 8 GPR +
  `EFLAGS & 0x8D5` + 三片内存哈希 + 出口 EIP + ESP）· 阶段 B **80 组**（比 8 GPR + ESP + EIP + `[E,E+0x804)` /
  `[V,V+0x100)` / `[esp-0x100,esp+0x40)`）。外部锚两臂都过：`n=9`、`"123456789"` ⇒ `0xCBF43926`。
- **控制流实测（阶段 B 两臂相同）**：`0x4D4B40 → 0x436D00 → 0x4D4B5D → 7FFE2000 → 0x4D4B63 → 0xFFFFFFFF`；
  `0x436D00` 与桩 EA 各恰好 1 次 ⇒ ③（`call eax` 槽 → 表查）与 ④（IAT → shim 蹦床）走的是**两条不同的路**。
  ★ **两臂是各自独立取的**：lifted 臂 = trampoline 的 dispatch 记录；解释器臂 = `cpu.last_eip` 的真轨迹
  （断言窗口完整 `last_count == insns`、四个 EA 各**恰好 1 次**、顺序递增）。`kLastInsns` 由 24 提到 **64** ——
  本载荷 28 条 > 24，首版只是靠"那三个 EA 恰好在窗口内"侥幸。
  ★ **首版踩过的坑**：解释器臂的序列是拿"期望序列"**合成**出来、再和同一个函数比 ⇒ 240 个 check 恒真、
  打印行也是合成的（复审用"跳过全部观测"的变异证明它**全绿**）。与 `AGENTS.md` §10 同一条：
  **打印出来的"证据"必须先能红**；现在改成真轨迹 + 数次数，`kLastInsns` 改回 24 就该红（实测红）。
- **③ 的实参是单独观测的**（不靠差分）：`[esp]` / `[esp+4]` / `[esp+8]` = 返回地址 / `n` / `buf`，`buf` 用
  `0xA5A51234` 这种非零标记；把 `push ecx` 改压 `0xDEADBEEF` ⇒ **只有这一条红**（差分仍绿）。
  同理 shim 的"整块回读"由探针写 `EDX = 0xABCD0001` 守住（回读只留 EAX+ESP ⇒ 红）——
  这两条都是"差分看不见、必须单独观测"的面。⚠ 这套"从 `last_eip` 拼轨迹"的上限是 `kLastInsns = 64`
  （24→64 是为这一条改的：28 条 > 24 会让前 4 条滚出窗口，而"三个 EA 恰好在窗口里"只是载荷长度的巧合）；
  载荷 > 64 条时会**假红** ⇒ 只能当原型证据，整 `.text` 阶段要把 trace 从 `X86` 拆出去（dispatcher 钩子或可配置）。
- **shim 独立性**：探针（换掉 `timeGetTime` 的 `fn`，返回 `0x5EED1234`）每臂 `calls == 1`、
  `[E+0xFC] == 0x5EED1234`；负控（探针不写 `call.result`）⇒ `[E+0xFC] == 0`。
  ⛔ 不能用真 `timeGetTime` 做差分：它读虚拟时钟 = 累计指令数/10000，两臂指令数不同 ⇒ 那是在比记账。
- **读数**（同进程、3 遍中位）：

  | 量 | 值 |
  |---|---:|
  | 端到端 `n=4096` 热路径：lifted vs 解释器 | **99.6×**（阶段 A 首测 ≈101×，多轮 101–107×） |
  | 每次 trampoline 边界 | **≈0.2 ns**（短载荷控制 6.3 vs 6.1 ns/迭代；`n=4096` 口径 +47.7 ns 被 ±50 ns 噪声淹没 ⇒ **分辨不出**） |
  | `lift_shim` 往返（含物化/回读） | **87.3 ns** vs 直接 `invokeById` 79.1 ns ⇒ 包装开销 **8.2 ns** |
  | `sub_4D4B40` 全链（28 条 guest） | ≈2.25×（`Machine::run` 空跑固定开销 **8.2 µs** 主导；原始 22.75× ⛔ 不可引用） |
  | 冷路径（建表器留解释器） | 解释器 19,333 条；lifted 臂解释器增量 = 19,333（冷）/ **0**（热） |

- **一条会咬人的接口事实（推翻 `.tmp/irshape/runtime-contract.md` §7.2 的建议）**：桩 EA → shim **不能**用
  `(ea - kStubBase) >> 2` 算 id。`shim_id` 是**注册序**（`ShimRegistry` 里的下标），桩 EA 的槽号是**发号序**
  （导入表绑定 / `use()` 分配）；两者只在"注册顺序恰好等于发号顺序"时一致 —— 实测夹具 `kStubBase = 0x7FFE2000`
  下那个公式给出 `0x800`，是一个**不存在的 id**。正确做法：走块缓存 `BlockCache::peek(ea)->shim_id`
  （与 dispatcher 的 `is_shim` 同源），缺块时回退 `idForStub`。
  同理 **桩页范围是 32 KiB**（`GuestLayout`），不是 256 B ⇒ `isShimEa` 用 `[shim_page, shim_page + slots*4)`。
- **SMC**：lifted 代码的 data 写（`mov [esi+800h],1`）调 `lift_note_write` ⇒ `BlockCache` 的 misses/失效 +1，
  与解释器 `writeMem` 同一路径（探针实测）。`push` 不调（解释器 `push32` 也不调）—— 这是**复刻解释器的真实行为**，
  不是遗漏。
- **未测**：lifted 侧的 x87 / SSE / SMC 写自身代码 / 跳表 · arm64 实跑 · 整 `.text`（`.ll` 仍是手写的，
  生成器未做）· 真实负载（本原型是 hermetic 真字节，不是整机 boot）。
- **复现**：上面那条 `ctest -R`（或单跑 `lift-ir-proto` / `lift-ir-shim` 看打印的读数行）；`--lifted-only`
  负控：`apps/emulator-lift/out/lift-ir-shim --lifted-only`（需回解释器 ⇒ exit 9）。报告
  `.tmp/irshape/{impl-stage-a,impl-stage-b,change-review-stage-a,change-review-stage-b}.md`。

### OBS-20261115-01 · Shape A 真机 boot→TITLE：AOT 二进制 3000 帧 `exit 0` / `misses 0` / 窗口里就是标题画面

- **口径**：Windows x64 · RelWithDebInfo（`windows-x64` 预设，Ninja + clang-cl 22.1.2）· 2026-11-15 ·
  二进制 = `apps/emulator-lift/out-h1/emulator-lift-aot.exe`（**干净重建**；Shape A：执行构建期 lift 出来的代码，
  ⛔ 不构造 `Machine`、`cpu.machine=nullptr`）· `--present window` 可见窗口（`window=visible` ·
  `driver=gpu-direct video=windows gpu=direct3d12`）· `--skip-logo` · `--audio off` · `--game-dir ../../dist/install` ·
  每档一个全新私有 `--appdata`（`apps/emulator-lift` 为 cwd）。
  ★ 不是口径 A（口径 A = 解释器 `emulator-lift.exe` + `../../links/install` + `--present-hidden`）⇒ ⛔ 不与其他条目混表。
- **命令原文**（两档只差最后一段）：
  ```powershell
  .\out-h1\emulator-lift-aot.exe --audio off --appdata ..\..\.tmp\aot\h1\appdata-r1-3000 `
    --game-dir ..\..\dist\install --skip-logo --present window --max-frames 3000
  .\out-h1\emulator-lift-aot.exe --audio off --appdata ..\..\.tmp\aot\h1\appdata-r2-3000 `
    --game-dir ..\..\dist\install --skip-logo --present window --max-frames 3000 `
    --native-overrides all --lst ..\..\corpus\disasm\files\AGE.EXE__dumped.sectfix.EXE.lst
  ```
- **读数（① 不带 override）**：`exit 0` · `frames 3000` ·
  `stop : stopped @ eip=00498AE4（正常结局） — max-frames（shim com!IDirect3DDevice9::Present）` ·
  `table : entries=33496 lookups=117715390 hits=117715390 misses=0 avg_cmp=1.06` ·
  `arena in_use=108595152 peak=108595152 surfaces=21` · `insns 1,696,174,952` ·
  `skip-logo : applied @ T_Command=0x100017E0 (+0x5EB5C=0；写前 1；钩子 EA=004BAA33)` ·
  `window : guest primary=00010000 class="ARCGameEngine" wndproc=004B92F0 in-dispatch-table=yes` ·
  `override : hits=0`（未给开关）。
- **读数（② `--native-overrides all`）**：`exit 0` · `frames 3000` · **同一个 `stop`** ·
  `misses=0`（`lookups=81955124`）· `arena` 三数**逐项相同** `108595152 / 108595152 / 21` · `insns 1,204,451,644` ·
  `override : hits=7320047 insns=22257726 skipped-nested=0（函数级 19 项 hits=7320047 回退=0 · 段级 3 项 hits=0 ·
  非 call 帧跳过=351009）` · `⛔ 未装（缺嵌套执行器）：sub_48B090`。
- **两档都没有硬停**：stderr 无 `decode-error`、无"停在派发表里没有的 EA" ⇒ 判据 1（`handoffs==0`，派发面只有
  lifted / shim 两类）与判据 3（无 `_Exit(3/4/9)` / `llvm.trap`）在本条成立。
- **到达 TITLE = 已判定**：帧 3000 的窗口像素（`--present-screenshot` → `.tmp/aot/h1/shots/shot-3000.bmp`，
  转 PNG `shot-3000.png`，sha256 `244f85a030d9d4be6526c3e459d2c1b5bee7ea619ec5f9f995ed231c15192a1c`）
  是**标题画面**：`天結いキャッスルマイスター Version 1.00.0000` 标题 logo + 立绘 + **Game Start / Load Data /
  Eushully-chan Room / Option / Quit** 五个菜单按钮 + 底部 `(c)2017 Eushully All Rights Reserved.` + `FE01–FE05`
  五个 install 槽。判据 = 人看这一帧（本条目由父级会话看图判定），⛔ 不做成 CTest（业务正确性不作判据）。
- **画面不是静止图**：帧 1 / 10 / 100 / 500 / 3000 的 `--present-screenshot` 统计（背景色 = 出现最多的 RGB）——
  帧 1：1,300 色 / 非背景 **0.22%**（近全黑）· 帧 10：204,663 色 / **97.0%** · 帧 100：218,922 色 / **94.2%** ·
  帧 500：223,625 色 / **94.1%** · 帧 3000：223,801 色 / **93.8%**；① 档 3000 帧的逐帧 `lastHash` 有 **2,959 个不同值**
  （最长连续相同 2 帧）。
- **口径警告（会咬人的三条）**：① 末帧 `lastHash` 在窗口档**不可复现**（①/② 与 5 次回读各不同：
  `0x7835…`/`0x8B34…`/`0x773F…`/`0xD727…`）⇒ ⛔ 不是判据；判据是 `stop` / `frames` / `misses` / arena 三数。
  ② **逐帧 `[frame] n=k hash=` 跨 run 不可比**：① 与 ② 的 3000 行只有第 1 帧相同（`0xF3FDFBDFBB518F7F`），
  第 2 帧起全不同 ⇒ 逐帧像素对照**当前做不到**（窗口档 = 真实时钟，每帧含的指令数随墙钟漂）。
  ③ 墙钟只作伴生读数（本机同期有别的构建/进程）⇒ 本条**没有**任何性能结论。
- **与解释器臂的可比项**（同一 `--max-frames 300` 档，见 OBS-20261114-… 与 `.tmp/aot/w1`）：`stop @ 00498AE4` 同址、
  `arena 55412672·55412672·19` 逐值相同、**frame 1 的窗口像素 fnv64 逐位相同**（`0xebf024febe998b05`）。
  ⛔ 不说"两臂等价"，只说这三项相等。
- **未做**：游戏菜单（Game Start / Load Data）没点 · `--present headless/auto`、`--present-vsync 1`、
  `--time-limit`、`--max-insns`、`--dump-frame` 没测 · 输入只投过 `WM_KEYDOWN`/`WM_CLOSE`（见下条）。
- **复现**：上面两条命令（`--max-frames` 可先换 10 试 ≈30 s）；读数存档 `.tmp/aot/h1/readings.md`。

### OBS-20261115-02 · AOT 窗口档的交互链：一个键走到 WndProc 跳表 case 256；点 X ⇒「終了」框 ⇒ `exit 0`

- **口径**：同 OBS-20261115-01（同一 `out-h1` 二进制 / `--present window` 可见 / `--skip-logo` / `--audio off` /
  每档一个全新私有 `--appdata`）· 驱动方式 = 向 **SDL 宿主窗口**投 Win32 消息（`PostMessage`：`WM_KEYDOWN` VK_A +
  `WM_KEYUP`；`WM_CLOSE`）+ `EnumWindows`/`EnumChildWindows` 读窗口与子控件；⛔ 不注入 guest、⛔ 不读 guest 内存。
- **命令原文**（cwd = `apps/emulator-lift`；脚本 `.tmp/aot/h1/probe-interact.ps1` / `close-answer.ps1`）：
  ```powershell
  pwsh -NoProfile -File ..\..\.tmp\aot\h1\probe-interact.ps1 -Tag keyprobe -KeyProbe `
    -ExtraLine '--skip-logo --until 0x4B9367' -ObserveSec 40
  pwsh -NoProfile -File ..\..\.tmp\aot\h1\probe-interact.ps1 -Tag closeprobe -CloseProbe `
    -ExtraLine '--skip-logo' -ObserveSec 25
  pwsh -NoProfile -File ..\..\.tmp\aot\h1\close-answer.ps1
  ```
- **读数（键）**：`PostMessage(WM_KEYDOWN,VK_A)=True` · `stop : until-ea @ eip=004B9367（正常结局）` ·
  `misses=0`（`lookups=5624355`）· stderr 无 `decode-error` · trace 尾 =
  `PeekMessageA → GetMessageA → TranslateMessage → DispatchMessageA`。⇒ 一个键走完
  `SDL 窗口 → input::key → guest 队列 WM_KEYDOWN(0x100) → DispatchMessageA → WndProc(004B92F0) → 跳表 case 256 = 0x004B9367`。
- **读数（关窗，只观察）**：`PostMessage(WM_CLOSE)=True` ⇒ 顶层窗口出现 `#32770|終了`，子控件 =
  `Static|ゲームを終了しますか？` + `Button|キャンセル` + `Button|タイトルに戻る` + `Button|プログラムを終了する`；
  guest 停在模态框上等回答（探针不答 ⇒ 25 s 后 kill，`exit=-1` 是探针 kill 的，不是产品失败）。
- **读数（关窗，答「プログラムを終了する」）**：**`exit=0`** · `stop : stopped @ eip=004123E3（正常结局） — present: 窗口关闭` ·
  `misses=0` · `window : guest 还没有窗口（primary=0）` · stderr 只有
  `[warn] [shim] agerc!_ShowDialog@12: id=3「終了」系统对话框 ⇒ 用户选择 1`，**无** `decode-error` ·
  trace 尾 = guest 自己的关窗路径：`DeleteFileA ret=004B9670 → KillTimer ret=004B967A → DestroyWindow ret=004B9681 →
  ShowWindow ret=004B968F → PostQuitMessage ret=004B9697 → OutputDebugStringA ret=004B96A1`。
- **⚠ 这条读数取代了两处更早的临时读数**（都在 `.tmp/`，未入库、原样保留）：`.tmp/aot/w1/readings.md` 记的
  "guest 驱动的关窗做不到 / 停在 `004B95EC`"、`.tmp/aot/c1` 记的 "`WM_KEYDOWN` 会 decode-error" ——
  那是**旧闭包**（`WndProc` 跳表 12 个 case 未解码）上的读数；闭包补了第五类后两者都通。
- **未做**：框的另两个分支（`キャンセル` / `タイトルに戻る`）· 鼠标消息（`WM_MOUSEMOVE`/点击）· 键盘只投了 VK_A ·
  ⛔ 没有把这些探针做成 CTest（这是观察，不是合同）。
- **复现**：上面三条命令；读数存档 `.tmp/aot/h1/readings.md`。

### OBS-20261115-03 · 最终树复核（含派发哈希表 + `ownsCpu` 守卫）：干净重建 `93/93` CTest、两档 3000 帧仍是 `exit 0` / `misses 0` / 帧 3000 = 标题画面

- **口径**：**当前工作树**（`E:\Projects\amayui-re`，含 T0…H1 全部未提交改动）· Windows x64 / RelWithDebInfo ·
  2026-11-15 · build dir = `apps/emulator-lift/out-final`（**干净重建**：`cmake --preset windows-x64 -B out-final`）。
  命令与读数与 `OBS-20261115-01` 同一参数，只换了 build dir —— 目的是证明**当前树**（不是某个中间态）能构建、测试绿、并复现 TITLE。
- **构建**：configure `exit 0`（12.1 s）· build `exit 0`（257.6 s / 583 步）· **warning 49**（`src/main.cpp` 5 + `tests/*` 44，
  **`src/aot/**` = 0**）· **error 0**。
- **全量 CTest**：**93/93 Passed · 0 Failed · 0 Skipped**（61.86 s）。条数 = `OBS-20261115-01` 那次构建的 92 +
  新增的 `lift-host-services-owns-cpu`（按名字做差集：只在 final 有它；**没 skip**，0.14 s）。
- **两档 3000 帧**（`--skip-logo --present window --max-frames 3000 --audio off`，各自全新 `--appdata`）：
  两档都 `exit 0` · `frames 3000` · `stop : stopped @ eip=00498AE4（正常结局） — max-frames（shim com!IDirect3DDevice9::Present）` ·
  `misses 0` · `arena 108595152 / 108595152 / 21`（`heap_blocks 6674`）· `lookups` 117,691,778（①）/ 81,947,718（②）·
  `skip-logo : applied @ T_Command=0x100017E0 (+0x5EB5C=0；写前 1；钩子 EA=004BAA33)`（两档逐字相同）·
  `insns` 1,696,735,834（①）/ 1,204,409,271（②）；② 的 `override hits=7,320,038 insns=22,249,870`、
  `⛔ 未装（缺嵌套执行器）：sub_48B090`。**无 `decode-error`、无停机。**
- ★ **新读数（派发表已换开放寻址哈希）**：`probe : probes=124472314 avg=1.06 max=7 | table cap=131072 load=0.256 entries=33496 build_max=7`
  （② 是 `avg=1.08`）。⚠ **口径**：`OBS-20261115-01` 的 `avg_cmp=1.06` 是**二分比较**/次，本条的 `probe avg` 是**线性探测**/次
  —— 数值碰巧接近，⛔ 不能当"表没换"的证据；能证明换表的只有 `probe` 行本身 + `cap=131072 / load=0.256 / max=7`。
- **画面**：帧 1 / 10 / 3000 的 `--present-screenshot`（1280×720，背景色 = 出现最多的 RGB）——
  帧 1：1,300 色 / 非背景 **0.002169** / fnv64 `0xd858c45963ccb2d9`（与 `OBS-20261115-01` 的帧 1 **逐像素相同**）·
  帧 10：207,894 色 / **0.969466** · 帧 3000：225,493 色 / **0.940987** / fnv64 `0xd061d7e518638875`。
  帧 3000 的像素内容 = 标题 logo + 立绘 + 五个菜单按钮（Game Start / Load Data / Eushully-chan Room / Option / Quit）
  + 版权行 + `EP.01–EP.05`；与帧 10 **不是同一张静止图**（颜色数/比例/fnv64 都不同）。**"到达 TITLE"仍按人工确认口径**。
- **与 `OBS-20261115-01` 的对照**：`应当相同` 的项**全部相同**（exit / frames / `stop` EA+原因 / `misses` /
  arena 三数 / `heap_blocks` / `entries` / `skip-logo` 行 / override 开关行 / native-ovr 装配 / 帧 1 截图）；
  `允许不同` 的项（`insns` ±0.03% / `lookups` / 墙钟 / 末帧与逐帧 hash / 帧 10·3000 的截图比例）都在墙钟漂移的量级内。
- **未做**：`LIFT_STRESS` 档 · 交互探针（见 `OBS-20261115-02`，本条未重跑）· 逐帧像素跨臂对照（wall clock 下做不到）。
- **复现**：`cd apps/emulator-lift && cmake --preset windows-x64 -B out-final && cmake --build out-final &&
  ctest --test-dir out-final`，然后上面两档命令（`--max-frames` 可先换 10）。读数存档 `.tmp/aot/final/readings.md`。

### OBS-20261115-04 · AOT 墙钟 **84.2 s → 50.0 s（−40.6%）**：大头是**宿主侧每派发一次的 385 项线性扫描**，不是被翻译的代码

- **口径**：`windows-x64` 预设（RelWithDebInfo）· 同一台机器、同一会话、交错 A/B/A/B/A/B/… 共 7 对 · 只差 exe。
  命令（cwd = `apps/emulator-lift`；`--game-dir` 必需，缺了 AGERC 装载失败 ⇒ `exit 2` / `frames 0`；
  `--appdata` 每次给全新空目录做状态隔离）：
  ```
  <exe> --skip-logo --present window --max-frames 300 --game-dir ..\..\dist\install --appdata <空目录>
  ```
- **主表（可见窗口，7 对，中位数）**：墙钟 **84.21 s → 50.00 s**（−34.21 s / **−40.6%**）；
  `TotalProcessorTime` **82.67 s → 48.53 s**（−41.3%）。安静子集（第 3–7 对）配对中位差 −40.7%。
  派生量：`ns / dispatch` 1121.8 → **665.1**；`ns / guest 指令` 90.7 → **53.8**。
  交叉核对（`--present window --present-hidden`，3 对，另一时段）：84.38 → 49.35 s（−41.5%），同量级。
  ⚠ 可见窗口在别的进程全屏时会**阻塞在 Present**（实测同一 exe 有一次 2% CPU / 10 min 未完成）⇒ 隐藏窗口只作核对，主表用可见窗口。
- **语义指纹（20 次运行全部逐位相同）**：`frames 300` · `stopped @ 00498AE4` · arena `108595152·108595152·21` ·
  `table misses 0`。⚠ **`insns` 不是判据**：同一 binary 内就有 **0.22%** 漂移（`927.26–929.26 M`），
  两臂同带 —— 本游戏的帧节奏依赖墙钟（`timeGetTime` / `PeekMessage`），300 帧内的 guest 指令数本身有抖动。
- **根因（代码级 + 量级对账）**：`aot_run.cpp:670` **每次派发**都调 `shims_.byStubEa(ea)`，而它
  （旧 `shims.cpp:263`）= `lock_guard<recursive_mutex>` + 对 `defs_` **线性扫 385 项**（`ShimDef` 含两个
  `std::string` ⇒ 每项 100+ B、整趟 40+ KB），99.9% 的派发扫完返回 `nullptr`。
  实测**单次反查 ≈ 450 ns**（配对微基准 88.3 → 86.3 ns 证明**不是锁**，是扫描/L1 流量）；
  按 75.07 M 次分摊 **≈33.8 s**。`lift_shim` 内的 `idForStub`(+`byStubEa`) 0.842 M 次 ≈ **0.37 s**。
  合计 34.14 s = 实测中位差。
- **改法**（⛔ 无 override、⛔ 未碰 IR 发射器）：槽位索引 `slot → id+1`（**不存指针**，防 `addRuntime`
  令 `defs_` 重分配后悬垂）+ 命中后 `stub_ea` **二次校验**（表过期只降级到慢路径，不会给错答案）；
  登记发号点加**响亮停**断言 `stub_ea == 0 || (在桩页内 && 4 对齐)`；dispatcher 的桩页判据 = **一次无符号减法 + 一次比较**；
  `lift_shim_id()` 让 AOT 把 dispatcher 已解析的 id 递下去（解释器臂的 `cache->peek` 路径**逐字未动**）；
  定时器泵的 `steady_clock::now()` 从"每派发"降到"每 `kCheckStride` 条"（上限 1.6 s；本臂 `定时器回调=0`，实际 ≈0）。
- **上一轮的归因是错的，这里纠正**：`--profile` 的 `dispatch-top` 把 `_memcpy` 算成 46% 是**幻影** ——
  它按"每个派发成本均匀"分摊，而 `_memcpy` 的块占 53% 的派发次数，于是分到 53% 的时间。
  `dispatch-top` 的 `ms` 列**不是**该 EA 的实测成本（`ms ≈ 采样数 × 3.14 ms`，即派发份额）。
  ⇒ 当时"40 s 是实打实的翻译成本"的结论**作废**。
- **剩余 50 s 的构成**（`--profile` 归因，`np2` 运行，**仅归因、不进判据**）：
  `lift_one` **≈42.0 s（85%）** · D3D9 GPU shim 体 ≈6.6 s（13%）· 派发固定项 + lifted 块体 ≈0.5 s（1%，≈7 ns/派发）。
  `lift_one` **47.1 M 次 × ≈892 ns**；其中 **x87 占 93.3%**（Fld 14041 / Fstp 9524 / Fmul 7098 / Faddp 3085 / Fadd 2453 /
  Fxch 1496 / Fsub 813 / Fmulp 683 / Fst 588 / Fsubp 449 / Fnstsw 417 / Fild 430 / …，采样 1/1024）；
  整数侧 `Mov 1136 / Div 1071 / Imul 248 / Rol 157 / Ror 138 / Idiv 121 / Movs 83 / Stos 72 / Push 32`。
  ★ **每种 op 的下限都极高**：`Mov` **517 ns**、`Div` 496、`Rol` 555（x87 普遍 870–1100）⇒ 即使把 x87 全拿掉，
  地板仍在。⇒ 这条把"`lift_one` 到底是解码贵 / 执行贵 / 状态搬昂贵"变成了下一轮必须先测的问题。
- **机器不慢（排除法）**：内联汇编探针（`.tmp/aot/freqprobe.cpp`）—— 依赖链 `add` **0.26 ns/轮**
  ⇒ 核心 ≈ **3.83 GHz**（turbo；`rdtsc` 3.194 GHz 是基频，与 `--profile` 的 `freq:` 一致）；
  8 路独立 add 0.69 ns/轮（≈3 add/cycle）。
- **验收臂（3000 帧，新 exe）**：`exit 0` · `frames 3000` · `table misses 0`（117,821,137 次查找，`avg_cmp=1.01`）·
  `stopped @ 00498AE4` · arena `108595152·108595152·21`；墙钟 **324.4 s** / CPU 313.3 s；
  `insns 1,696,446,245`；`dispatch 124,473,969`（lifted 117,821,137 / shim 6,652,832）；`skip-logo: applied`。
- **CTest**：**94/94**（新增 `lift-shim-stub-index`，196 checks，顶层无条件 ⇒ pure 档）。
  变异：把 `idForStub` 的返回值改成"槽号当 id" ⇒ 该用例 **5 failures / CTest 退出码 8**（红得有意义：
  夹具故意先登记两条无桩的 `add`，所以 id(2,3,4) ≠ 槽号(0,1,2)）。
- **未做 / 未覆盖**：定时器"到期判定粒度变成 4096 条指令"这一语义变化**没有用例**（本臂定时器从不触发）；
  `slot_index_` 的**无锁并发读**没有工具证明（Windows 上无 TSAN；缓解 = 定长表 + `defs_.reserve` + 命中后二次校验）；
  "feeder 线程读的同时主线程 `addRuntime`"没有用例；构建的 exe **字节不可复现**（lld-link 时间戳/GUID）。
- **复现**：脚本 `.tmp/perf-aot/run-case.ps1` / `interleave.ps1`；原始 stdout/stderr 在 `.tmp/perf-aot/{vis,hid,final,accept}/`；
  profile 原始输出 `.tmp/perf-aot/final/np2.out.txt`。

### OBS-20261115-05 · AOT 墙钟 **58.4 s → 19.1 s（−67%）**：`lift_one` 的 79.3% 是**解码器**，加字节校验的解码缓存即可

- **口径**：`windows-x64`（RelWithDebInfo）· 同一台机器、同一会话、交错 A/B/A/B/A/B 共 3 对、只差 exe
  （基线拷出留证：sha256 `6E9A3FEB…76BCD` / 12,045,824 B；连同 8 个运行时 DLL 一起拷，否则 `0xC0000135`）。
  命令（cwd = `apps/emulator-lift`；`--game-dir` 必需；`--appdata` 每次全新空目录）：
  ```
  <exe> --skip-logo --present window --max-frames 300 --game-dir ..\..\dist\install --appdata <空目录>
  ```
- **读数（中位）**：墙钟 **58.42 s → 19.08 s（−67%）**；`TotalProcessorTime` **53.80 s → 15.45 s（−71%）**。
  ⚠ 本轮基线（58.42 s）比 `OBS-20261115-04` 的 50.00 s 慢 —— 同一台机器今天更慢（A/B 是几分钟内交替跑的，
  配对比较才有效，⛔ 不要跨观察直接相减）。
- **`lift_one` 四桶**（`--profile`，单次采样在 4 个段界各取一次时，每桶含 ~21 ns 取时地板；关档仍是一次指针判空）：

  | 桶 | 修前 ns | 占比 | 修后 ns |
  |---|---:|---:|---:|
  | A 换入（`liftRegsToCpu` + eip） | 34.7 | 3.1% | 36.3 |
  | **B 解码**（`Insn`/`why` 构造 + `decode`） | **878.6** | **79.3%** | **101.2** |
  | C 执行（`noteInsn` + `execInsn`） | 160.6 | 14.5% | **135.9** |
  | D 回读（`liftCpuToRegs` + eip） | 34.5 | 3.1% | 37.2 |
  | 合计 | 1108.4 | 100% | 310.6（真值 ≈227，扣掉 84 ns 取时地板） |

  ★ 地板与 x87 无关：连 `Mov` 也有 **B=601.8 ns**（`Div` 526.7）；x87 的增量在 **C 桶**（148 vs 115 ns）。
- **改法**：`src/runtime/decode_cache.{h,cpp}`（新）—— EA → `Insn` **直接映射 512 槽**缓存，条目
  `{ea, len, 原始字节[15], Insn}`；命中须**同时**满足 EA 相同 + `mem.committed(ea,15)` + 那 `len` 个字节
  **逐字节相同**，否则重解并覆盖。只缓存"15 字节窗口完整"的结果（否则 `9B` 前缀合并的长度/op 会随可读字节数变）。
  另把原来**每条指令都调一次**的 `opName()` 改成只在采样时调。
- **`lift_one` 总账**：1108.4 → 310.6 ns/次（总 52.25 s → 14.62 s，−72%），**解释掉 96% 的墙钟节省**；
  命中率 **96.3%**（45,345,150 / 47,079,342），`invalidated=0`、`uncached=0`。
  ⚠ 但 `lift_one` 仍占 19.08 s 的 **14.62 s ≈ 77%**（最大桶已是 C = `execInsn` / x87 SoftFloat）。
- **语义指纹（6 次运行逐位相同）**：`exit 0` · `frames 300` · `table misses=0` · `stopped @ 00498AE4` ·
  arena `108595152·108595152·21`。`insns` 分布 928,339,444–930,355,050（**0.23%**，不作判据）。
  ⚠ `[frame] n=300 hash` 在**同一份二进制自己跑两次之间**就已不同 ⇒ 它本来就不是稳定指纹。
- **CTest**：**95/95**（新增 `lift-decode-cache`，40 条断言、5 组：命中 ≡ 重解 / 异 EA 不污染 /
  SMC 重解（5 字节 `mov`→1 字节 `nop`，长度 5→1、op Mov→Nop、`invalidated` 恰好 +1）/ 页尾窗口不完整 /
  解码失败面一致）。变异（删掉命中时的逐字节校验）**红**：CTest 退出码 8；还原后绿。仓库合同档 546/546。
- **SMC 只在解码层自愈**：缓存**不可能**给出陈旧 `Insn`，但"已发射的 lifted IR 仍执行旧语义"这条缺口
  **本轮没有变化**（`lift_note_write` 在 AOT 里仍是 no-op）⇒ ⛔ 不要读成"SMC 修好了"。
  且 300 帧内 `invalidated=0`、`uncached=0`（guest 一次都没写代码页）⇒ **自愈路径只有 CTest 覆盖，无真负载证据**。
- **新风险**：78 KiB/线程的 TLS（512 槽 × 152 B，`thread_local`）；直接映射冲突 3.68% 未命中（只影响速度，
  语义无损）；页尾附近那条指令永不命中（每帧重复解一次，可忽略）。
- **未测**：B 桶内部**没有**再拆子桶（Zydis vs `toLowerStr`+`unordered_map` vs 三张线性 `strcmp` 表各占多少）
  —— 缓存把 B 整段干掉后不需要，**未实测、不作断言**。
- **复现**：读数与机器可读数据在 `.tmp/liftone-perf/{prof1,prof2,ab,baseline}/`（`ab/ab.json`）；
  profile 行在 `prof1/run.out.txt:780-797` 与 `prof2/run.out.txt:781-784`。

### OBS-20261115-06 · AOT **19.08 → 8.14 s**（x87 换 `double` + 抬进 IR、S7 结构翻译）· **翻译侧已达 native 量级（1.7 ns/条）**，剩余 78% 是图形 shim

- **口径**：`windows-x64`（RelWithDebInfo）· 同一台机器 · 300 帧
  `<exe> --skip-logo --present window --max-frames 300 --game-dir ..\..\dist\install --appdata <每次全新空目录>`
  （cwd = `apps/emulator-lift`）。⚠ **两段的 A/B 性质不同**：x87 那段**没有配对 A/B**（基线 exe 被清理删了，
  用台账跨会话对比）；S7 那段有 3 对交错。
- **x87 → `double` 模型 + 抬进 IR**（D49/D55）：`X87State::regs` `uint8_t[8][16]`（SoftFloat `extF80` 原位字节）
  → **`double regs[8]`**；`fpu_bridge.*` 整份重写成一份 double 模型；四个 native override 改用同一批桥函数。
  生成代码里 x87 算术是**普通 LLVM 浮点指令**（`fadd/fsub/fmul/fdiv/fneg/sqrt/rint/sitofp/fptosi double`、`fpext/fptrunc`、`fcmp uno`）；
  状态全在 `%cpu`，偏移契约集中在 `src/runtime/x87_layout.h` + `lift_abi.cpp` 的 7 条 `static_assert(offsetof(...))`；
  只有超验函数（25 个调用点）走 `@lift_x87_sem` → 解释器**同一份** `execFpu`。
  **读数**：300 帧墙钟中位 **9.01 s**（3 次 9.081/9.005/8.923）、CPU 7.97 s；`lift_one` **47.04M → 3.23M 次（−93.1%）**，
  折算 **14.62 s → ≈0.61 s**；3000 帧验收臂五项全过（65.25 s）；CTest 96/96。
  新守卫 `lift-x87-diff`（22 个真实 x87 站点，逐位比 8 槽 double 位型 / top / tag / 状态字 / 控制字 / 8 GPR / EFLAGS /
  1 KiB guest 内存）；变异 `Fsubr` 反操作数 ⇒ 红。
- ★ **同一轮引入并修回的一处语义回归（必须记住）**：换模型时异常位只剩 IE/ZE，**PE/UE/OE/ES 一律不置**，
  而**guest 确实在读** —— 解释器臂 300 帧 `tri` **828,931 → 828,965**、`insns` −0.06%。
  修回 OE/UE/ES（判据只有一份：`fpu_bridge.cpp` 的 `x87RaiseOverflowUnderflow`；OE = 结果 ±inf 且两操作数都有限，
  `fdiv` 在 `b==0` 时不判（那是 ZE）；UE = 结果次正规；ES = 状态字任一异常位非零，与 D49 **之前**那条同源），
  生成代码发**同一份位型判据**的 IR（`x87RaiseFloatFlags`，OE=0x08 / UE=0x10 都带 ES）。
  **PE 仍恒 0**（double 下几乎每条都与真值不等，除软件浮点或每 op 读 MXCSR 外无法判定）—— 已在 `fpu_bridge.h` §5
  与 `x87_layout.h` 的 `kStatus` 注释里写明"按 PE 分支的 guest 路径会走'精确'那一支"。
  差分扩到两批极端状态（1e308 ⇒ OE/fistp 越界/压满栈；1e-308÷1e8 ⇒ 次正规 UE）：**1390 checks / 0 failures**，
  `OE 7 · UE 2 · ES 21`（两臂同时）；变异删掉 UE 那一行 ⇒ 红（`expected 0x800 got 0x890`）。
  `tri` 改回后 830,001 → **828,499**（−0.18%，⚠ 跨会话散布就有 ~1,000 ⇒ 这个差在噪声边缘，不能全归因）。
- **S7（一对一结构翻译）**（D53/D54）：判据在 `lift-gen` 里跑**它自己的 Zydis 解码**（`src/aot/lift_struct.{h,cpp}`），
  ⛔ 没有搬 `.lst` 普查。`call rel32`（caller+callee 都可结构化）⇒ **宿主 `call`**；`ret` ⇒ 宿主 `ret`；
  跨函数 `jmp` ⇒ 尾调用；间接 ⇒ `lift_dyn`（查表 + **实测 `jmpq *%rax` 的 TCO**）。
  宿主 call/lift_dyn 后的唯一 continuation 判据 = **`%r == resume`**，否则原样上抛 ⇒ **部分转换天然安全**。
  **覆盖率（按 `.ll` 实际站点）**：可结构化 **4,949 / 10,211（48.5%）**；降级 5,262（自身 2,321 + 调用图传染 2,941）；
  首因 = 间接 call **1,641** · 动态返回 628 · 可变长栈 35 · 写返回槽 14 · 表外 callee 3。
  `ret` 7,002 → 宿主 ret **1,777（25.4%）**；`call` 24,388 → 宿主 call **1,230** / `lift_dyn` 5,241 / 其余派发 17,917。
  **`dispatch total` 75,172,837 → 46,875,828（−37.6%）**；`lift_dyn` 查表 14.8M 次（不计入 dispatch，`misses` 仍 0）。
  **墙钟 8.533 → 8.135 s（−4.7%）**、CPU 7.094 → 6.594 s（−7.0%），3 对交错，语义指纹逐位相同。
  ⚠ 派发被换成 14.8M 次 `lift_dyn` 查表，所以 dispatch −37.6% 只兑成墙钟 −4.7%。
  新守卫 `lift-s7-structured`（F `0x4C0980` → G `0x4C0210`，31 checks）：`表查找=1`、`esp` 回位、
  `[esp-4]` 是压入的 resume、`[esp]` 仍是哨兵、与解释器逐位；变异 `structured.clear()` ⇒ `表查找=3` 红。
  ★ **必保语义**：`--skip-logo` / recovery 两个宿主钩子的 EA 被强制留在派发边界（`src/aot/aot_hook_ea.h`），
  否则 S7 会让它们**静默失效**。
- ★★ **本轮最重要的结论**（交付 exe 上跑一次 `--profile`，300 帧，机器上有别的负载 ⇒ 绝对值偏高、比例可用）：

  | 项 | 折算 | 占比 |
  |---|---:|---:|
  | **图形 shim（top-9）** | **≈8.62 s** | **≈78%** |
  | └ `DrawIndexedPrimitive` 18,000 × **174.9 µs** · `Present` 300 × **8.43 ms** · `Clear` 301 × **5.04 ms** · `DrawPrimitive` 323 × **2.28 ms** | | |
  | `lift_one` | ≈0.76 s | 7% |
  | **派发 + 全部 lifted 代码体** | **≈1.6 s** | ≈15% |

  ⇒ **929M 条 guest 指令跑在 ≈1.6 s 里 = 1.7 ns/条 ≈ 6.5 cycles/条**（3.8 GHz 核心）—— **翻译侧已是 native 量级**
  （最早的基线是 55 ns/条 ⇒ 降了 32 倍）。★ 早先那次 50 s 的 profile 里 shim 只占 13%（约 6.6 s）：
  **shim 的绝对值没变，是别的东西变快了**。
  ⇒ **端到端墙钟现在由图形 shim 决定**（`DrawIndexedPrimitive` 175 µs/次、`Clear` 5 ms/次、`Present` 8.4 ms/次，
  比合理成本高 1–3 个数量级，疑似每次 draw 重建 D3D12 管线 / 软件清屏 / 软件呈现转换）。
  这是**另一条工作线**（图形实现效率，不是翻译质量）。
- **未做 / 未覆盖**：`fsub`/`fsubp`/`fsubrp`/`fcom`（4.3% 的 x87 调用）**无逐位证据** —— 静态站点全在 `0x450000+`，
  而 `sectfix` 的 PE 那段没有 raw 字节（`.lst` 头写明来自另一份 dump），只有 3000 帧验收臂兜底；
  S7 的**逐函数动态权重**没有读数（`--profile` 的 `dispatch-top` 是每 EA 的派发份额，不能按函数归因）；
  宿主栈深度已量化（生成函数帧 **80 B/层**，guest 栈 1 MB ⇒ 最坏 ~20 MB）并加 `/STACK:33554432`，**没有**运行期栈探测；
  native overrides 在结构化路径上基本失效（可选性能档，本轮未开）。
- **复现**：`.tmp/aot/s7/{census.md,census.mjs,prof.out.txt}`、`.tmp/aot/s7/base/`（基线 exe + sha256）；
  S7 降级清单逐条打在 `lift-gen` 的 stderr（EA + 原因 + 首因站点 + 传染源，5,262 行，确定性升序）。
### OBS-20261116-01 · AOT 构建 **228 s → 80.8 s（2.82×）**：一半是**重复劳动**，一半是**单 TU 单核**

- **口径**：`apps/emulator-lift`，RelWithDebInfo / win32-x64 / Ryzen 7 7735H（8C/16T）/ clang 22.1.2；
  读数来自 `out/.ninja_log` 的边起止（同机同树）与 `Measure-Command` 计时（两次取中位）。
- **改造前的三段**（2026-11-16 实测）：

  | 段 | 墙钟 | 说明 |
  |---|---|---|
  | `lift-gen --check` | 48.58 s | ★ **纯重复**：它调的就是生成命令下一行要跑的同一个 `buildAll` |
  | `lift-gen`（完整生成，含 134 MB 落盘） | 58.14 s | 发射 + 落盘只占 ≈5.7 s |
  | `clang -c -O2`（134 MB 单 TU） | 120.7 s（111.0 / 130.4 中位） | **单进程单核**，16 核里只用了 1 核 |
  | 链接 | ≈0.5 s | |
  | **合计** | **≈228 s** | |

- **两刀**：① 闭包五条判据并进生成那一趟（`--closure-gate`，`--check` 不再单独跑）⇒ 228 → 197.6 s（−48.6 s，产物**逐字节相同**）；
  ② IR 按分量拆 8 份 + `-j8`（**仍是 `-c -O2`**）⇒ 197.6 → **80.8 s**（clang 段 120.7 → 21.6 s）。
- **8 份并行时单进程慢 ≈20%**（总 CPU 162 s vs 134 s）= **内存带宽**，不是逻辑问题 ⇒ clang 段落在 21.6 s 而非线性预测的 ≈17 s。
  线性度实测：最大分片 18.13 MB / 3,638 函数单独编 = **15.74 s**（线性预测 16.3 s，3% 内）。
- **拆分没有损失代码质量**（三条静态判据）：
  `llvm-objdump -d` 指令行 2,619,129（单份）→ **2,613,209**（8 份）= **−0.226%**；
  8 份 body 拼接与单份 body 的 **sha256 相同**（`5b25e0de…`）；IR 里 `call i32 @lift_cc_*` **2571 == 2571**
  （最后一条是"跨份静态 call 没退化成 `ret i32`"的判据）。
- **运行期没变**：300 帧交错 A/B 中位 4.97 s（单份）vs 4.99 s（8 份）= +0.4%（噪声内）；
  语义指纹 `exit` / `frames` / `stop @ 00498AE4` / arena 三元组 / `table misses=0` 逐位相同。
- **现在改生成器 ≈80 s，改 shim / runtime 只重链接 ≈5 s**（`.ll` 是 CMake 的 generated output ⇒ 只有动 `lift_gen*` 才重生成）。
- **守卫**：`lift-aot-split-manifest`（22.8 s，`pure`）判分片清单自洽（Σfunctions == pin、符号两两不交、
  并集 == 派发表、每份 ≤20 MB、份数 == `LIFT_AOT_SPLIT`）；configure 期 `FATAL_ERROR` 钉住"仍是 `-c -O2`"；
  `LIFT_AOT_SHAPE="split8-O2"` 进二进制并在启动打一行；回退 = `-DLIFT_AOT_SPLIT=1`。
- **仍未解决的两块成本**：`lift-gen` 的 58.6 s（其中 52 s 是**两趟**闭包分析，第二趟由 BUG-24 的跳表识别引入）；
  `lift-jmptab-dispatch` 单条 CTest 就要 59.7 s（同一份分析）。⇒ 要再快得优化 `buildAll` 本身，那需要先有需求。

### OBS-20261116-02 · AOT 缺省 stdout **388 行 → 85 行**：周期性输出只有一条，但它占 78%

- **口径**：`emulator-lift-aot.exe --skip-logo --present window --max-frames 300`，每次全新 `--appdata`；RelWithDebInfo。
- **改造前**：缺省档 388 行，其中 **300 行是 `[frame] n=… hash=…`**（每次 `Present` 一行，**当时没有任何开关**）
  + 3 行 override 读数（`native-ovr:` 1 行 + `override  :` 2 行）。
- **改造后**：缺省档 **85 行**；`--frame-log` 档 385 行（= 85 + 300），行形态与改前**逐字相同**；
  `--profile` 档 733 → 730（只少那 3 条 override；`[frame]` 300 行、`frame-ms` 301 行的字段形态不变）。
- **剩下 85 行的分类**（★ 这是"下一刀要不要收"的判据）：启动头部 4 · `printReadout` **68**
  （exe 1 / PE 9 / 导入 19 / 地址与服务面 11 / 区域表 28）· `[skip-logo]` 1 · `probe` 1 · 指纹摘要 6 ·
  `present` 1 · 钩子结局 4；stderr 1 行（AGERC 冲突）。
- ★ **判据是"这条会在一次 run 里出现几次"，不是"看着吵就删"**：那 68 行是**一次性**的启动快照，
  而且 `pin` / `gaps` / `region` 属于判据面（出问题时是唯一消息面）⇒ 这一轮**故意不收**。
- **守卫**：`tools/test/aot-arm-shape.test.mjs`（`pure`/`contract`）——① `src/aot/` 里不许再出现 override 机制的
  33 个标识符；② `[frame]` 的 `printf` 必须在 `frame_log`（或 `prof_.on`）的条件里。改前红 2/2（194 处命中）→ 改后绿。
- ★ **后续（同日，同一个门禁的第三刀）**：用户判定 **region 表是多余噪音** ⇒ 再加窄开关 **`--region-log`**（缺省关），
  **缺省 stdout 85 → 57 行**。那 28 行（`regions   :` 1 行 + 27 行段表）在源码里是 `printReadout` 的**最后一块**、
  在 stdout 里**连续**（改前第 45–72 行）⇒ 只关它、⛔ 别的 readout 一格没动。
  `--region-log` 档那 28 行与改前**逐字节相同**（块 sha256 `CB9CF03F…`）；语义指纹三档逐位相同。
  守卫加第 ③ 条（**性质**断言："区域表打印必须在 `if (region_log)` 块内"；⛔ 不写成"输出里没有 `regions :`"——
  后者会把"整块删掉"也判成合格）。变异 `if (region_log)` → `if (true)` ⇒ 该守卫 2/2 红。
- ★ **一条口径裁决（父级，写下来免得下一个 agent 猜）**：`[frame]` 的闸门是 `frame_log || prof_.on`，
  而 region 表**只认 `region_log`**（⇒ `--profile` 档也少这 28 行）。理由：`prof_.on` 顺带打开的东西只该是
  **profile 自己的主体**（逐帧时间序列）；region 是启动几何、不是性能归因的对象，没有任何 profile 分析用它。
  ⇒ 想给 `--profile` 加回这 28 行，得先有一条"某次 profile 分析读 region"的证据。
### OBS-20261116-03 · 输入投递层 S1+S4 落地：**执行循环零新增**、行为逐位不变、成本在噪声内

- **口径**：`apps/emulator-lift`，RelWithDebInfo / win32-x64；读数来自 `ctest`、`llvm-objdump -d`、
  AOT 300 帧交错 A/B ×3（每次全新 `--appdata`）。日期 2026-11-16。
- **做了什么**：`runtime/input_plan.{h,cpp}`（计划句柄 + 门 `{Always, Presents}` + flush-before-read + 待抬起）·
  `runtime/input.h` 的四个投递入口（电平 / 队列 / 写点 / 事件源，均 inline + 一条 null 检查）·
  11 个投递点在**读之前**接线（9 读点 + `SetCursorPos` + `ReadFile` 只作事件源）·
  AOT `--input-plan <file>`（解析在 GPU bring-up 之前 ⇒ 文件错不需要 GPU）。
- **行为**：AOT 300 帧语义指纹**逐位相同**（`exit 0` / `frames 300` / `stop @ 00498AE4` /
  arena `55412672·55412672·19` / `table misses=0` / `lastHash`）。缺省 stdout 58 行；
  有计划 59 行，**只多 `input-plan:` 那一行**。
- **静态成本**：`llvm-objdump -d` 指令行 解释器 613,671→**613,823（+152）**、AOT 3,142,049→**3,142,218（+169）**；
  `lift_aot.obj` **0 字节变化**（改的都是 runtime/shim，没碰生成物）。
- **动态成本**：300 帧交错 A/B 3 对 = −3.25% / +4.83% / −0.51%，**中位 −0.51% ⇒ 在噪声内**
  （本机分辨不了 <5%，⛔ 不许把这类差异当结论）。
- **执行循环零新增（本设计的核心判据）**：`src/runtime/machine.cpp` 与 `src/aot/aot_run.cpp` 里
  `InputPlan` / `input_plan` / `input::` / `deliver(` / `atPoint(` / `note(` 命中 **0**。
  ★ 旧 `harness_script` 闸门**故意保留**（删它是后续单）⇒ 守卫把"新层不许进循环"与"旧层还在"分开判：
  5 个新层词两文件必须 0，旧 tick 只许钉在已知的两个位置。
- **守卫**：`tools/test/input-plan-shape.test.mjs`（源码形态）+ CTest `input-plan-read-points`（关系断言：
  `plan.deliverCalls` ⟺ `shims.callCount`，两个独立来源）+ `ctest:cli-aot-flags` 的 `--input-plan` 三组用例；
  三条都**实测红过**再绿，并各入 `tools/mutate-check.mjs` 一条变异。
- **没证明的**：端到端到「加载存档」屏没跑成——机制驱动了 AOT（`fired=1 presents=300`），但**不知道** TITLE 菜单上
  「Load Data」的键/坐标，而仓里现有坐标是**人手写未复核**的 ⇒ 按 §6 不能当证据。
  单 `REQ-01M7GA4B8T9JXFDAYK46YHNH7K`（缺一份经验证配方 + 人手对照读数）。
### OBS-20261116-04 · 屏幕级对照该用什么读数：`region` 三数是**稳定且随屏而变**的指纹，`stop @` 无证明力、`lastHash` 不是指纹

- **口径**：`emulator-lift-aot.exe --present headless --game-dir dist/install --skip-logo --max-frames 400 --appdata <每次全新>`
  + `--region-log`；RelWithDebInfo；13 次 run。日期 2026-11-16。
- **三个读数各自的可用性**（本轮实测，⛔ 别再拿前两个当判据）：

  | 读数 | 结论 |
  |---|---|
  | `stop @ <EA>` | ★ `--max-frames` 下**两侧恒同**（`00498AE4`，停在 Present 桩，与画面无关）⇒ **无证明力** |
  | `lastHash` | 4 次 run **每次不同**（`clock=real-time` + TITLE 是动画屏）⇒ **不是稳定指纹** |
  | **收尾 `region` 三数**（in_use / surfaces / heap_blocks） | **同一命令多次 run 逐位相同**，且**随屏而变** ⇒ 可作屏幕级对照（需 `--region-log`） |

- **实测对照表**（同一命令，只换动作）：

  | 跑法 | `region` 三数 |
  |---|---|
  | 不给输入计划（3 次） | `108595152 / 21 / 6674` |
  | 中性点击 `640 360`（**特异性对照**） | `108595152 / 21 / 6676`（只多 2 个 block ⇒ 不算反应） |
  | `click 1070 480`（2 次逐位相同） | `117490928 / 25 / 6821`（**+8.9 MB / +4 surfaces**） |
  | `click 1186 373`（GameStart） | `112791848 / 24 / 6747` |

- ★★ **一条会改变所有后续命令的前提**：**到 TITLE 的任何自动化都必须带 `--skip-logo`**。
  不带时逐帧 hash 在 **frame 64** 就冻死（`0xBE8BCF5F5F55D3F5` 连续 337 帧）⇒ 那之后的按键**打在静止的 boot 帧上**。
  这正是 S4 那次"端到端"失败的真正原因（当时没带 `--skip-logo`，却被当成"配方不对"）。
- ★ **一个反讽，值得记下来**：`region` 刚被判定为"噪音"并改成 opt-in（`OBS-20261116-02/03`），
  但它同时是**目前唯一稳定、随屏而变**的屏幕指纹。⇒ 收它当**缺省输出**是对的，⛔ 但**不要把它整块删掉**，
  它是"某次 run 到底停在哪一屏"的可机械对照面。
- **用它的代价**：`--region-log` 是诊断档；拿它做对照时，两侧必须**都**带这个开关（否则三数都缺）。
- **另记**：`ESC`（VK `0x1B`）在这条路径上无区分力（两侧落同一 `EA 00513B90` / 同一条 `agerc!_ShowDialog`）。
### OBS-20261116-05 · TITLE 上"点得进去"的位置窗口是**回调体输入端**，不是轮询体；位置门因此可确定

- **口径**：`emulator-lift-aot --game-dir links/install --appdata <每次全新> --skip-logo --present headless
  --max-frames 400 --time-limit 150 --region-log --input-plan <配方>`；macos-arm64 / RelWithDebInfo；2026-11-16。
  配方恒为 `await position TITLE <FROM> <TO>` + `click 1070 480 left`（每次 run 用全新空 appdata）。
- **读数**（每格 3–4 次 run，逐位相同）：

  | 区间 | 这一段是什么 | `fired` | 收尾 `region`（in_use / surfaces / heap_blocks） |
  |---|---|---|---|
  | `[0x0460, 0x0520)` | TITLE 回调体的**输入端** | 1 | **117490928 / 25 / 6821** |
  | `[0x388C, 0x38A4)` | 出口段（轮询期不执行） | **0** | 108595152 / 21 / 6674 |
  | `[0x039C, 0x0460)` | 轮询体（`get-input-type` + `eq/jcc` + `sleep 1` + `jmp`） | 1 | 108595152 / 21 / **6675** |

- **结论**：① 位置门**确定**（同一区间的多次 run 逐位相同 ⇒ 可作判据，与 `wait <ms>` 的 12 跑 4 成相反）；
  ② "到标题菜单再点"的正确判据是 **`[0x0460,0x0520)`**，⛔ 不是轮询体；③ **门命中 ≠ 点击有效**：
  轮询体里门也命中，但点击不产生任何效果（region 停在基线附近）⇒ "输入循环在跑"不等于"点击会被消费"。
- **未验证**：机制。猜测是 TITLE 轮询体的消息泵在 `i2fc` 之前把注入取走/派发掉 ⇒ 点击必须在回调**读输入之前**到位。
- **另记（同一轮取证）**：`header_len` 只能是常量 **`0x3C`** —— 装载器把脚本头与 `sub_header_length` 读进**栈**
  （`0x40EE67` / `0x40EFF2` / `0x40F01C`），`0x40F69E` 从**文件偏移 `0x3C`** 起读进 `frame+0x00`
  ⇒ 头在 guest 内存里**没有落点**，运行时"从头读 header_len"这条路不存在。
