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
  同一脚本把 `--until ea` 设成 `0x4B92A5` 时 run 在 **17.98 s** 内收口（CTest `agerc-ui-state`）。
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
  这条偏差与 SCOPE §1「菜单 / AGERC 设置对话框 = 永久 no-op」一致；守卫 `tests/agerc_ui_state.cpp`。
- **复现**：`./out/emulator-lift --present window --present-hidden --present-driver software
  --raster cpu --skip-logo --audio off --game-dir ../../links/install --appdata ../../.tmp/hover/appdata
  --until ea 0x4B92A5 --trace-shims --input-script ../../.tmp/hover/hover.txt --time-limit 300`
  （脚本两行：`at marker TITLE.BIN` + `mouse_move 640 1`）· 2026-11-12 · macOS arm64 / RelWithDebInfo
