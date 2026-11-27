# FELLOW 立绘切换的单帧 ~1.0 s 卡顿（profile 归因 + 逐 EA 取证）

> 目的：「FELLOW 里切角色立绘明显卡顿」的形状、钱花在哪个桶、那 433 M 条 guest 指令的形状、不改 guest 语义还能省多少。
> ⚠ 下面全是**当时的**读数（命令 + 日期 + 二进制 sha + 档），不是"现在有多快"的断言。口径来源：`measurement.md` 的 M-12 / M-13 / M-01；
> 同类基准见 `perf-bench-save05.md`；profile 读数**不进任何判据**（`AGENTS.md` §12.5）。

## 0. 快照（先冻结二进制，再谈读数）

| 项 | 值（2026-11-21） |
|---|---|
| 执行源 | `.tmp/fellow-profile/bin/emulator-lift-aot.exe` + 8 个 DLL（`apps/emulator-lift/out/emulator-lift-aot.exe` 的冻结副本）· `sha256` `128A7E169FCCEEA12C90359C78817A812C116BE73DCE9A9F24569D5172442B96`（前 8 = **128A7E16**） |
| 冻结时 `HEAD` | `04a6e24`，**工作树另有 12 个未提交改动**（别的车道在改 `src/**`）⇒ exe 由脏树构建；读数绑这份 sha，**不绑** HEAD 源码 |
| 平台 / 构建 | win32 x64；`out/` 的缺省预设产物。★ 冻结记录**没有单列 build 类型** —— 拿它的墙钟当基线前，先按 `AGENTS.md` §12.2 确认不是 Debug |
| 档位 | `--present window --present-hidden`（= hidden）· `--skip-logo --audio off --time-limit 240` · `--scale` 缺省 1.0 · guest 1280×720 |
| 原始件 | 日志 `log-a1n` / `log-a2n` / `log-b3w` / `log-ctl` / `log-c2`、配方 `save09-fellow-switch.plan` / `-nowin.plan` / `control-steady.plan` —— 都在 `.tmp/fellow-profile/`（**gitignored，不入库**；仓内 `scripts/recipes/` 没有它 ⇒ 文件不在原地读数就不可复跑） |

★ **复跑的两个前提（2026-11-22 实测，缺一个就跑不出来）**：

- **存档集要用 2026-11-21 那份**：`saves/SAVE.DAT` + `RT.DAT` 在 2026-11-22 09:33 被重写过、并新增了 `SAVE10` ⇒ 读档界面的**持久化页号变成 1**（每页 10 行 ⇒ 第一屏是 `LOAD 010..019`），于是"点 `y=626` 选槽 009"**必然落空**（`await position SBUNKI` 超时、exit 5）。复原办法：`git archive 04a6e24 saves/ | tar -x -C <私有 appdata 的 SAVE 目录>`（实测 4/4 通过，含 `--audio sdl/off`）。
- **`--audio` 只认 `off` / `sdl`**（`parseAudioMode`）；⛔ 没有 `on`（会 exit 2）。"产品态" = **`--audio sdl`**（`audioThreadMode()` = Sdl && realTime ⇒ 真 feeder 线程）；`--audio off` 不开设备、不跑混音。本文 §3 那批读数是 **`--audio off`** ⇒ 是**下界**，不是产品态。★ 原第三值 `virtual` 档已于 2026-11-22 **整档移除**（它没有 feeder 线程、走主线程 D 泵，会被误读成产品态音频）。

## 1. 现象（形状）

- **单帧 ~1.0 s、每次切换一帧**：6 次切换 = 6 个 0.95–1.18 s 的单帧（帧内 `insns` 433–437 M）；该帧之前还有一帧 ~17–24 ms 的「轻帧」（0.26 M）⇒ 不是"每帧都慢"。**30/30 复现**（5 跑 × 6 切换，逐次差 ≤~20%，两方向同量级）；同跑内切换之间稳态 8.0–9.0 ms/帧（≈28.6 K insns），⛔ 不是常说的 ~50 ms（FIELD 20 Hz 旧口径）。
- **首次切换多一帧**：FELLOW **首次进入**自带一帧 ~1.7 s / **614 M**（零 Present 的密集计算），之后才是正常那帧 1.00 s ⇒ 首次 = 两帧、之后每次 = 一帧（窗口只覆盖后半段，见 §3.2 的 s1-to-avaro 行）。旁证：M-12 在 2026-11-17 用
  另一配方记的 `fellow-init` 是 612.4 M。
- ★ **真机对照（用户口述，2026-11-22）**：真机上同一次切换「**有一点卡顿，但不明显**」。
  ⚠ 这是**口述、无截图无读数** ⇒ 只作**定性**证据、⛔ 不据此算倍数。它说明卡顿**不是我们造出来的**：
  同一份 433 M 条指令在真机上也存在，只是更快跑完（推断 0.1–0.3 s ⇒ 差 3–10 倍；**这是推断，不是读数**）。

## 2. 怎么跑（命令逐字）

cwd = `apps/emulator-lift`；二进制 = §0 的冻结副本（⛔ 不是 `out/` 的新 exe —— 换了二进制就是另一张表）。每跑前 `scripts/clean-appdata.sh <dir>`（`saves/**` 只复制不就地写）；win32 上该脚本用 Git
bash（`C:\Program Files\Git\bin\bash.exe`）。

```bash
cd apps/emulator-lift
BIN=../../.tmp/fellow-profile/bin/emulator-lift-aot.exe
APP=../../.tmp/fellow-profile/appdata-rerun
scripts/clean-appdata.sh "$APP"
"$BIN" --present window --present-hidden --game-dir ../../links/install --appdata "$APP" \
  --skip-logo --audio off --time-limit 240 \
  --input-plan ../../.tmp/fellow-profile/save09-fellow-switch-nowin.plan \
  --profile --profile-slow-frames 100 --frame-log --input-plan-trace
"$BIN" --present window --present-hidden --game-dir ../../links/install --appdata "$APP" \
  --skip-logo --audio off --time-limit 240 \
  --input-plan ../../.tmp/fellow-profile/save09-fellow-switch.plan \
  --profile --input-plan-trace
```

- 第一条 = **A 档**（干净墙钟档：看 `frame-ms:` 尖峰 + slow-frame 表）；第二条 = **B 档**（窗口档：`[profile-window]` 分桶表 = 归因主读数）；§3.2 的 arm C 是第二条加 env `LIFT_GPU_SUBMITS=1`。完成度判据 = `exit 0` + `input-plan: pending=0`（门超时 = `exit 5` 点名 `文件:行`，⛔ 别读成"解析失败"）。
- 配方：SAVE09 → 确认 → REIGN 右键进 FELLOW → 每轮 `click 330 130`（阿瓦罗）/ `click 400 130`（菲亚）×3 轮，6 对 `profile_begin/end`，`quit` 收尾，**一条 `wait` 都没有**；A 档用孪生件
  `-nowin.plan`（逐字相同，只删 6 对 `profile_begin/end`）。窗口 = `click` → 等 `SETTOOLLIST` → 等回 `FELLOW` → 等主轮询体 `[0x1570,0x15A8)` → `profile_end`（**脚本内位置**，不是
  EA）。
- ★ REIGN→FELLOW 的右键必须 `mouse_down` → 门 → `mouse_up`：单发 `click ... right` 实测 3/4 —— 失败臂按下→抬起的读点跨度 **6** vs 成功臂 **1778** ⇒ 按下没被任何 `read-mouse-button`
  看见（停在 REIGN / `exit 5`）；拆开后 **2/2**。

## 3. 读数

以下全部 = §2 的档 + §0 的快照（`128A7E16` / win32 x64 / 2026-11-21 23:14–23:25；计时臂无并发进程，但整轮非独占机器 ⇒ 墙钟只作量级）。「A1」= `log-a1n`（A 档），其余类推。

### 3.1 卡顿帧（A 档：`--frame-log` 的 `frame-ms:`）

| 跑 | 6 次切换的卡顿帧 ms（加粗括号 = 首次切换那两帧） | 该帧 `insns` | 同跑稳态 |
|---|---|---|---|
| A1 (`log-a1n`) | 1764.7**(+1001.9)** · 1005.7 · 958.4 · 1076.4 · 1008.2 · 1005.0 | 436.6–436.7 M（首次那帧 614.0 M） | 8.99 ms |
| A2 (`log-a2n`) | 1668.3**(+1035.7)** · 1054.5 · 992.4 · 949.5 · 1067.4 · 1041.8 | 436.7 / 432.9 M | 7.90 ms |
| B3 (`log-b3w`) | 1700.6**(+1042.2)** · 986.7 · 1154.0 · 965.0 · 1171.6 · 993.4 | 437.0 / 433.2 M（`→阿瓦罗` 恒 436.6–436.7 M、`→菲亚` 恒 432.9–433.0 M ⇒ 方向差稳定 **3.7 M**） | — |

### 3.2 一次切换的花法（B 档：`[profile-window]` 分桶，6 个窗口）

| 窗口 | `wall` ms | `lift` | `boundary` | `shim` | `present`（⊂shim） | `PeekMessageA` ms/N | `timeGetTime` ms/N |
|---|---|---|---|---|---|---|---|
| s1-to-avaro | 1103.4 | 38.7% | 18.6% | 38.2% | 1.9%（readback 17.1） | 150.1 / 162450 | 137.3 / 487347（`insns` 437.0 M = 首次切换后半段） |
| s1-to-fia | 1012.6 | 39.6% | 19.4% | 36.0% | 1.9%（14.9） | 142.7 / 161634 | 129.7 / 484901 |
| s2-to-avaro | 1176.7 | 41.2% | 18.1% | 34.9% | 1.5%（13.8） | 170.6 / 162449 | 150.4 / 487346 |
| s2-to-fia | 986.0 | 39.0% | 18.6% | 35.4% | 1.9%（14.6） | 138.0 / 161634 | 124.8 / 484898 |
| s3-to-avaro | 1194.3 | 42.0% | 17.6% | 35.3% | 1.4%（12.5） | 169.3 / 162450 | 151.1 / 487346 |
| s3-to-fia | 1016.6 | 38.9% | 18.4% | 36.0% | 1.9%（15.1） | 140.5 / 161634 | 126.6 / 484899 |

- 闭式自检：`unaccounted = 0.000 ms (0.0%)` · `coverage = 1.32–1.44×` · `instrument` 4.5–5.8% ⇒ 读数有效（M-12；`coverage` >1 = 外推偏高，只比**份额与量级**，⛔ 不拿 `lift` 的绝
  对 ms 当判据）。其余 5 个窗口都是完整一次切换。
- **`lift` + `boundary` = 56.1–61.1%**；`shim` 34.9–38.2%，其中 **≈26 点 = guest 自己的忙等**：`PeekMessageA`（13.6–14.5%）+ `timeGetTime`（12.4–12.8%），一帧分别被调 ≈16.2 万 / ≈48.5 万次 ⇒ 单次宿主成本 ≈0.9 µs / ≈0.26 µs。⛔ 不是"某个慢 API"。
- **present / GPU 不是大头**：1.4–1.9%（readback 12.5–17.1 ms/窗口）；`CreateFileA` 只 65 次 / 13–23 ms。A 档 slow-frame 表同形（`n=168`：`PeekMessageA` 151.2/162386 ·
  `timeGetTime` 146.7/487160 · `GetGlyphOutlineA` 15.2/2795）。arm C：每次切换 `uploads` **+48**、`full` **+6**、`cuts=0` ⇒ 48 次 dirty 上传，**没有**管线切割 / 回读风暴。

### 3.3 那 433 M 条指令的形状（边际法：`log-b3w` 减 `log-ctl`）

同一份前缀、同一份存档：`log-ctl`（进 FELLOW **不切换**）与 `log-b3w`（6 次切换）。整跑差：`insns` **+2.552 G**、`dispatch` **+55.85 M**、`timeGetTime` 调用 **+2.844 M**、
`PeekMessageA` **+0.953 M** ⇒ 每次切换 ≈ **9.31 M 派发 / 433 M insn / 474 K 个主循环回合**（= **914 insn/回合**、**19.6 派发/回合**）。★ 独立核对：`timeGetTime` 桩 `0x7FFE001C` 边
际样品 701 × 4096 = 2.87 M ≈ 2.844 M 次调用（**1% 内**）。

| 边际入口（1 样品 = 4096 次入口；全表 13,636 个边际样品） | 占边际 | 是什么（EA） |
|---|---|---|
| 主循环自身 7 个块 | **35.6%** | `0x412496` / `0x412498` / `0x4124C1` / `0x4124CD` / `0x4127FC` / `0x41280C` / `0x41284C`（每个 ≈690） |
| 3 声道定时器泵 | **20.3%** | `0x4BBB55` 15.3%（每回合 3 槽）+ `0x4BBAB0` 5.0% |
| 10 槽定时器 + 1 格 flag 查询 | **10.1%** | `0x4B5230` 5.0% + `0x404C20` 5.1% |
| `timeGetTime` 桩 | **5.1%** | `0x7FFE001C` |
| 逐像素转换 + guest `_memcpy` | **2.2% / 2.5%** | `0x48B4FC` 1.9% + `0x48B495` 0.3%（`sub_48B090`）/ `0x4E7530`（`rep movsl` + 两张跳转表） |
| 长尾 | **26.3%** | 685 个 EA，每个 <1.2%，**未展开** |

（几项"每回合 1 次"的 ctl 侧样品是 `≤404` ⇒ 边际只能给下界，故各行相加略超 100%。）

- ⇒ 433 M **不是**"逐像素合成"，而是**引擎主循环在 ~1 s 里跑了 ≈474 K 个调度回合**：每回合读一次 `timeGetTime`（`0x412496`）· 两个定时器泵（`sub_4B5230 @0x4124BC` / `sub_4BBAB0 @0x4124C8`，
  后者每槽 1–2 次 `idiv @0x4BBAFC`）· 一次 flag 查询（`sub_404C20 @0x4127F7`）· **执行 1 条脚本指令**（`0x41284A` 的 `call edx`，1024 项命令表；返回点 `0x41284C` ≈1/回合）· 约每 3 回合排空一
  次消息队列（`PeekMessageA @0x41237D`，0.34 次/回合）。
- 主循环 = **`sub_412290`**，唯一调用方 `_WinMain@16 @0x4BA890`：32 槽线程化状态机，转槽条件 `([engine+0x5EB60] & [engine+0xAAB4C]) == 0`（**不是定时**）；消息泵退出 = `PeekMessageA`
  返回 0 或 WM_QUIT；idle 分支各有 `Sleep(5)`，切换帧**没走**。

## 4. 归因口径（三条最容易读错的）

### 4.1 `dispatch-top` 是**整跑**统计，% 是**采样时间份额**（不是该 EA 的独占时间，也不是入口频次）

- 打印时机：`dispatch_by_ea` 只在收尾打一次 ⇒ 那张表是**整跑**（B3 = 启动/读档 0.80 G + 装载 ~1.48 G + FELLOW 首次进入 0.61 G + 6×433 M），⛔ 不是窗口口径。EA = 派发表的一条**入口**（
  `out/aot/lift_table.inc` 的 `LIFT_AOT_ENTRY(...)`）：可以是函数起点，也可以是函数内 jump-table / indirect 入口（`0x4E7530` / `0x4E7617` 就是 `_memcpy` 体内的尾表入口）。
- 采样 = 每 **4096** 次派发取一次 TSC，把「距上次采样」的整段 ticks 记到**这次采样点的入口 EA** 上（`src/aot/aot_run.cpp` 的 `profileDispatchSample`，`kDispatchStride = 4096`；文件前注自称「自计时间隔法 / self-time」，实现就是 `slot.second += delta`）⇒ 一个样品 ≈ 4096 × 42.4 insn ≈ **0.35–0.40 ms**。
- ⚠ 所以 % 是**采样时间份额**：分子是 ticks 不是次数 ⇒ 它**不是**入口频次；整段 4096 派发的 ticks 都记给采样点那**一个**入口 ⇒ 它也**不是**该 EA 的独占时间。两个副作用要记住：① 粒度 = 4096 派发（≈0.35–0.40 ms）⇒ 比这更便宜的单次调用看不出来；② **相位别名** —— 周期循环里只有落在 ≡ 0 (mod 4096) 相位的块才会被采到（§3.3 里那些「每回合 1 次」的块正是这样被采到的）⇒ 单个 EA 的数只能读作「以它为采样点的那些 stride 的时间」，⛔ 不能读作「这个块自己花了多久」。
- ⚠ **它回答"多常被进"，不回答"每次多贵"**：步长恒为 4096 次派发 ⇒ 稳定循环里各 EA 的采样次数份额 ≈ 入口次数份额（整数账：主循环 7 块 + 泵 4 + 定时器 2 每回合各 1 次 × 474 K 回合 = 35.6% / 15.3% / 5.0% / 5.1%，与表里逐格吻合）⇒ ⛔ 不能拿它给"某个函数贵"排序；"每次多贵"要另取每入口 `insns/派发` 或块转移计数（今天没有这两格读数，见 §8）。
- ⇒ **窗口归属只能用"带窗口跑 vs 同前缀不切换的对照跑"做差**（§3.3）。这条差法用 `timeGetTime` 桩的样品数**独立核对**过（701×4096 = 2.87 M vs 2.844 M 次调用，**1% 内**）—— 两个独立来源对得上，差法才可信。整跑表可先看形状
  （`log-b3w.txt:614-616`：`0x4BBB55` 7.1% / `0x4E7530` 5.3% / `0x4E7617` 4.9%），但⛔ 别拿整跑份额回答"切换帧里谁贵"：做差后 `_memcpy` / 逐像素掉到 2–3%，`0x4BBB55` 反而升到 15.3%。

### 4.2 `--frame-log` 会强制 readback；**墙钟（唯一档）**是这条臂的前提

- `--frame-log` ⇒ `needsGuestPixels()` 为真 ⇒ 每帧强制整幅回读（口径见 `present-readback.md`）⇒ A 档的 1.0 s 是**仪器档**读数；但卡顿由 guest 计算主导（`lift+boundary` 56–61%），关档只会更
  短、份额方向不变。⛔ A 档墙钟不与关档 run 混表。
- ⚠ **前提写出来**：guest 时钟 = **宿主墙钟**（`src/runtime/clock.h` 只有这一档；运行 readout `services … clock=wall`，⛔ 没有 `--clock=` 开关）。
  ⇒ "`insns` 恒定而 wall 变 ⇒ 不是时间门"这条推理成立（没有第二个时钟档可躲）。
- 真墙钟下的正面证据（同方向 3 次切换）：`insns` 437.0 / 436.6 / 436.6 M，`wall` 1103 / 1176 / 1194 ms（wall 差 8%、insns 差 0.09%）；两方向差 3.7 M ⇒ 指令数由**切到谁**决定。反证：若存在 1 s 超
  时门，wall 986→1194 ms 之间 insns 必须跟着变 ~20%，实测 0.02%；且主循环里唯一的 `now − t0` 比较是 **60000 ms** 看门狗（`0x41241F`），不是 1 s。

### 4.3 稳态基线有**两套**口径 ⇒ 倍数必须给两套

- 口径①（**同跑内**、切换之间的 FELLOW 帧）：8.99 ms（A1）/ 7.90 ms（A2）⇒ 卡顿帧 **≈107–135×**。
- 口径②（**空闲对照跑** `control-steady.plan`：进 FELLOW 不切换、停 4 s）：末 60 帧中位 **18.7 ms**（p10 14.9 / p90 22.8）⇒ **≈51–58×**。
- ⛔ 不要只写一个：①是"同一次操作序列里前后帧的差"，②是"同一屏完全空闲时一帧有多长"。两条基线的 `insns`/帧还差 7.5×（28.6 K vs 216 K），**这一差本轮没有归因** ⇒ 倍数只作量级参照，⛔ 不当判据。⚠ 对照跑的逐字命令行没保留（只留 plan 与
  `log-ctl.txt`）⇒ 要复跑先补那条命令。

## 5. 不是被我们的 shim 逼上 CPU（caps / 格式查询 = 代码事实）

- **`GetDeviceCaps` 只在渲染器初始化问一次**：`0x499126`（`sub_498CC0`，唯一调用方 `sub_406730 @0x406ADA`），只读 `+0x58/+0x5C` 当**下限闸** —— `0x499157`/`0x499163` 比不过就跳
  `0x4991E5` 报"テクスチャ処理能力が不足"并 `mov eax,80004005h` 返回 `E_FAIL` ⇒ 把 caps 报小，症状是**初始化直接失败**，不是"退 CPU 路"。
- **`CheckDeviceFormat` 也只在同一处问一次**（`0x49918A`，问 `D3DFMT_X8R8G8B8`），答案存进 `renderer+0x464`；唯一消费者 = `NormalTexture::Create`（`sub_48AC40`）的 `0x48AC8F`
  ⇒ 只决定 **X 格式 22/30** 还是 **A 格式 21/26** —— **bpp 相同**、都是 GPU 纹理、都进同一段 `sub_48B090` 逐像素填图 ⇒ **没有 CPU/GPU 分叉**。引擎自己的路 = `D3DXCreateTexture @0x48AD07`（
  `MipLevels=1`）→ `LockRect @0x48B238` → guest 内存里逐像素写 2/4 字节（`_memcpy @0x48B4F7`，宽 = `dword_5518C0/8`）→ `UnlockRect @0x48C18x` → draw 时 dirty 上传；**不走
  `GetSurfaceLevel`**（所以那项计数 0）。⚠ "每次切换不被调用"的依据是**唯一调用点在启动期**，缺一条运行时计数（§8-4）。同一组 caps 事实已登记在 `caps.md`，本文是在它之上追到"谁消费答案"。

## 6. 与既有结论的关系

1. **与 SAVE05 那次尖峰不同族**（`perf-bench-save05.md`）：那边是**贴图 / 文件加载 shim**。这次切换帧里 `D3DXCreateTextureFromFileInMemory` 与 `GetSurfaceLevel` **各 0 次**（对照同一
   跑的启动帧：3 次 / 106.6 ms）、`CreateFileA` 只 **65 次 / 13–23 ms**（启动帧 473 次 / 95.6 ms）、`ReadFile` 209 次 ≤0.4% ⇒ 落点完全不同，只同到"一段重活把一帧拉到秒级"。⛔ 不要用 SAVE05 的归因套这次。
2. **与 `defects.md` 的 BUG-38 是两回事**：BUG-38 是 ADV 切立绘那 **2 帧**的**几何 / 采样**缺陷（整层偏 (−0.48,−0.46) px + 锐度双峰，走 `ID3DXEffect` 的 `Fade` / RTT 路），
   **没有墙钟症状**；本文是一次切换的**单帧 ~1.0 s 时长**归因。⛔ 不要混成一条，也不要拿本文读数去解释那 2 帧的像素。
3. **`sub_48B090` 的逐像素转换在装载帧才是大头**：对照跑整跑 `dispatch-top` 前 5 名 = `_memcpy` 两入口 6.0 / 5.6% + `sub_48B090` 三入口 4.4 / 4.3 / 4.1%，而 `0x4BBB55` 只 3.6%；同一
   份表里 `0x4E7617` / `0x48B490` 在切换帧的边际甚至是**负的**。装载类量级：ctl 最大那一帧 `n=107→108` = Δinsns **1.485 G** / Δwall 4841 ms；FELLOW 首次进入 `n=162→163` = **614 M** / 1700 ms。⇒ 切换帧里它只有 **1.9–2%**（§3.3）⇒ 归因成"切换时在逐像素合成"**不成立**。

## 7. 不改语义时还能省多少

| 可省项 | 量级 | 依据 / 限制 |
|---|---|---|
| 忙等 shim 的入口/出口税（26 点里可省的部分） | **10–20 点（100–200 ms）** | 单次宿主成本 0.9 / 0.26 µs、guest 一条指令 ≈2.3 ns ⇒ 一次调用 ≈100–400 条指令的等价时间；大头是桩派发 + `lift_shim` + 注册表/参数搬运（拆开这三段才有"能省多少"的读数，现在没有） |
| `boundary` 17.6–19.4%（派发税） | **数点（~30–80 ms）** | 433 M / 10.2 M 派发 = 42 insn/派发；合并相邻块（更少派发入口）线性缩这个桶，属 AOT 分块形状 |
| `_memcpy` 2.5% / `present` 1.4–1.9% / 逐像素（切换帧 ≤数点） | ≤2 / ≤1.5 / **≈0** 点 | 前两项已是小块；逐像素是 guest 代码，不改语义就只能更快地执行它，没有"跳过"的合法路径 |
| ≈474 K 回合的结构性开销（~70% 入口） | 不可省 | 引擎自己的控制流（1 条脚本指令/回合 + 定时器 + 取时），改它就是改语义 |

⇒ 合计上界 ≈ **30 点（~300 ms）**、现实可达 **10–20 点** ⇒ **量级 15–25%**。结论：**这不是 shim 缺陷、也不是我们造出来的卡顿**（guest 侧 `lift + boundary` 56–61% 是地板）；能压的只有**模拟器自身税**（一次
shim 调用的宿主成本 + 派发边界），而**逐像素转换对每次切换几乎不可省**。

⇒ **裁决（2026-11-22，`data/requirements/01M7ZFEWVMAPFGVJQEE8S80VCT.md` 已收口）：记录不修、不排期。**
依据三条：① 真机上也有（有一点、不明显，见 §1）⇒ 不是我们的缺陷；② 可省上界 15–25% **改变不了观感**；
③ `≈474 K 回合` 的结构性开销是 guest 控制流，改它就是改语义。要再动，先补 §8 的两件读数（窗口级
`dispatch-top`、慢帧 `script` / `pos`），并**另开 AOT 吞吐线**的单 —— 那与"这一帧的归因"无关。

## 8. 未做 / 待核（缺的是读数，不是结论）

1. **长尾 25–26% 是谁**：685 个 EA（每个 <1.2%）在整跑表里读不出窗口归属 ⇒ 缺**窗口级 `dispatch-top`**（现在 `dispatch_by_ea` 只在收尾打一次；最短的路是在 `endWindow` 里同时打一张窗口增量 top-20）。
2. **≈475 K 条脚本指令属于哪个脚本 / 哪段循环**：`0x41284C` 是"每回合 1 条脚本指令"的返回点，`0x41284A` 的 `call edx` 查 1024 项命令表 —— 但**执行的是哪段脚本**未定（缺慢帧内的 `script` 名 + `pos` 采样；判
   据是 pos 落在 `FELLOW.BIN` 主轮询体 `[0x1570,0x15A8)` 还是切换体内部）。
3. **48 张上传的尺寸**：每次切换精确 +48 uploads / +6 full，但 `full` 只说明"6 张写区未知"，**不能**从 `+48` 反推像素数；缺每条上传的 `{texture serial, w, h, rect, whole}`。§6-1 的"0 次"来自"
   不在 top-8 / top-15"，**不是**计数 0 ⇒ 要一条窗口级具名行才能回答"48 张是新建还是复用"。
4. **caps 查询的运行时计数**："`GetDeviceCaps` / `CheckDeviceFormat` 只在启动期"目前只有"唯一调用点"这条静态依据。另：两套稳态基线的 `insns`/帧 差 7.5×（28.6 K vs 216 K）没有归因；mip / Live2D 重算
   这两个假设，本文读数既不排除也不证实，⛔ 不要当结论引用。

## 9. 2026-11-22 微优化批（另一批二进制；单价来自 boot 窗口，不是切换帧）

> ★ **本节与 §3 不可直接比**：§3 的绝对值是 FELLOW 切换帧（10.2 M 派发 / 685 K shim 调用/帧、19.95 ns/派发）；
> 本节的单价来自 **boot→TITLE 窗口**（≈2.4 s / 41.3 M 派发 / ≈43 K shim 调用/趟、11.8 ns/派发）。
> 二进制 = dev 车道 `66a8ee7` + 两个 perf 提交（`f70fa96` shim chrome · `722784b` 派发序言 + 消息泵）；
> 读数日期 2026-11-22 / win32 / RelWithDebInfo / `--audio off` / 每配置 5 趟取中位。

### 9.1 改了什么（对应 §7 里"能省"的那几项）

| # | 落点 | 改了什么 |
|---|---|---|
| 1–4 | `runtime/shims.cpp` | 每次 shim 调用：两把递归锁 → 一把；两份 `ShimDef` 拷贝（每个两个 `std::string`）→ 一份；两份无条件 trace string 拷贝 → 只在 `trace_on` 分支；`byStubEa` 加 1-entry 桩 memo（存 id、命中后仍做 `stub_ea` 二次校验） |
| 7 | `runtime/host_window.cpp` | `pumpTimers` 空定时器表直接返回（旧形状取一次 QPC 后一次都没用） |
| 8 | `host_window.h` + `shims/user32_window.cpp` | `PeekMessageA` 的过滤条件从 `std::function` 换成 `MessageFilter`（三个 `uint32` 的 POD）；匹配循环体仍在 `.cpp` |
| 5 | `aot/aot_run.cpp` | 派发序言：循环不变量外提（`time_limit` / `until` / `max_insns` / `stop_requested` / `audio_enabled`）、Present 闸门先比 `insns`、`control_` 两次判空并一层、两个一次性 latch 并一个 bool |
| 6 | `aot/aot_dispatch.cpp` | `lookup` / `lookupUncounted` 各加 1-entry `ea→fn` memo；`lookups / hits / misses / probes` 原样补记（`table misses` 是语义指纹） |

### 9.2 读数（同负载、同一轮内的前后对照）

| 读数 | 基线 | 本批 | Δ |
|---|---:|---:|---:|
| `timeGetTime` ns/次 | 175.2 | **152.4** | −22.8（−13.0%） |
| `PeekMessageA` ns/次 | 596.6 | **533.4** | −63.2（−10.6%） |
| `boundary` ns/派发 | 11.827 | **11.320** | 配对差中位 −0.278（−2.4%） |

拆开：#1–#4 = −22.8 ns/`timeGetTime` 次（它是最纯的样本：shim 体本身只占 ≈35 ns）；
#7+#8 = −31.4 ns/`PeekMessageA` 次（空表不取 QPC ≈28 ns + 不建 `std::function`）；
#5+#6 = −0.28 ns/派发（3 对交错 A/B 的配对差 −0.509 / +0.071 / −0.278）。

⇒ **调研估计偏乐观**：每条 shim 的固定 chrome 实测 ≈ **23 ns**（估 45–95）；派发序言只有估计的 **1/10**
（LTO + `-O2` 已经把 `const` 引用上的循环不变量提前取了；memo 多一次比较 + TLS 读，与"哈希 + 1 次探测"打平）。

### 9.3 折算到切换帧（估算，不是实测）

685 K shim/帧 × 23 ns ≈ **15.6 ms**；162 K `PeekMessageA` × 31.4 ns ≈ **5.1 ms**；10.26 M 派发 ×
0.28 ns ≈ **2.9 ms** ⇒ ≈ **−24 ms/帧（≈2.2%）**。
⚠ 单价来自 boot 负载（混合比例与切换帧不同）⇒ 只当量级；与 §7 的"10–20 点"上界相比，这一批只吃到 2 点量级。

### 9.4 口径警告（这一批踩到的）

- **并行车道会污染墙钟**：2026-11-22 21:00 之后的一轮 5 趟**作废** —— 另一条车道同时跑 exe + ninja，
  同一份二进制全线慢 15–25%，**连本批改动碰不到的 `timeGetTime` 一起慢**。⇒ 本机只有
  **交错 A/B 的配对差**与同一轮内部的相对关系可信；跨轮绝对值只当方向，Δ 只当**下界**。
- 语义判据不变：`exit` / `frames` / `stop @ EA` / arena / `table misses` 逐位相同；`insns` 不作判据。
- 门禁：`ctest --test-dir out` **137/137**（360 s，`MessageFilter` POD 形态 + 收紧后的
  `input-plan-shape` 守卫）· `pnpm test` 614/614 · `pnpm verify` 500 pass + 2 skip。
