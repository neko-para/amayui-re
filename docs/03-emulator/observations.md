# 观察账（observations）

> 记 **我们测到过什么**。一条 = 一句话 + 一条能重跑的命令 + 一个读数 + 一个口径 + 一个日期。
> ⛔ 不写"所以要优化 X"（那是需求单的事）；⛔ 读数不是断言（断言在 `contracts-win32.md` 的守卫里）。
> 编号 `OBS-<yyyymmdd>-NN`；**过期不删**，同一条再测出不同读数时新开编号并写"取代 OBS-…"。
>
> **口径A** = RelWithDebInfo（`cmake --preset windows-x64`）/ Win10 x64 / `--present window --present-hidden --raster gpu`
> （启动行 `present : driver=gpu-direct`、`raster : sdl-gpu/direct3d12/DXBC`）/ 有安装树
> （`--game-dir ../../links/install --appdata ../../.tmp/appdata`）/ `--native-overrides all --skip-logo` /
> 输入脚本驱动 / **无** `--profile`、**无** `LIFT_FRAME_TRACE`。命令骨架（`apps/emulator-lift` 下）：
> `./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install --present window --present-hidden --raster gpu --native-overrides all --skip-logo --input-script <脚本>`
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
