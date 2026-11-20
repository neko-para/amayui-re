# SAVE05 移动卡顿基准（配方 + 读数）

> 目的：把 SAVE05「移动」的可复现卡顿做成**可复跑、可判**的评测目标（guest 计算与 shim 两条线分开判）。
> 配方 `apps/emulator-lift/scripts/recipes/save05-move.plan`；测量口径见 `measurement.md` 的 M-01（墙钟）、M-02（仪器）、
> M-12（窗口 profile）、M-13（按帧分桶）。
> ⚠ 下面全是**当时的**读数（命令 + 日期 + 档），不是"现在有多快"的断言；换构建 / 换 `--present` 档先查口径。

## 1. 为什么

- **场景**：SAVE05 = SC0000 第一个地图。`click 440 360` 打发 SO033A 覆盖图后，左键拖拽把镜头从 440 拖到 240；
  `s05move` 窗口里出现 **8 个 70–300 ms 的尖峰**，而 FIELD 稳态是 50 ms/帧（20 Hz）。同一配方同一构建 N=3，
  窗口 wall 差 <0.5% ⇒ 可复现。
- **为什么拿它当评测目标**：尖峰帧的 guest `insns` 是稳态的 3.7–15.4×，其余是贴图 / 文件加载 shim
  （`GetSurfaceLevel` / `D3DXCreateTexture` / `CreateFileA`）；`Present` 恒 ~10 ms/帧、不随尖峰增长
  ⇒ **guest 线与 Present/GPU 线可以分开判**（不是"每一帧都慢"）。
- **判据的出处**：`boundary` 在 `REQ-01M7VQNPY3EAHRZP1F3VJD8QCC`，8 尖峰帧在 `REQ-01M7VQNQ5E3ANQQR0RNSQRVX9K`
  （单号 = `data/requirements/<ID>.md`）。
- **6–8× 从哪来**：那是**用户口径**的"与真机差 6–8 倍"，不是仓内读数（仓内没有真机帧时长基线）。
  当时的评估报告在 `.tmp/perf-reassess/`（**临时区，不入库**）：它把 6–8× 拆成"~15–16 条 host/guest × IPC 比"
  —— 这是**换算**，⛔ 不要当判据，更⛔ 不要写成"已达 native 量级"。

## 2. 怎么跑

构建：`apps/emulator-lift` 的 RelWithDebInfo（macOS `macos-arm64` 预设；`LIFT_AOT_LTO` 默认 ON）。
⛔ 不用 Debug 的墙钟。

```bash
cd apps/emulator-lift
# 每次跑前重建**私有** appdata（saves/ 只读 ⇒ 只复制进去；⛔ 不就地写）
APP=../../.tmp/save05-bench/appdata-n1
scripts/clean-appdata.sh "$APP"
```

```bash
# ① 干净档（评测档 = 隐藏窗口；N=3；A/B 时 ≥3 对交错 b1 a1 | a2 b2 | b3 a3）
./out/emulator-lift-aot --present window --present-hidden \
  --game-dir ../../links/install --appdata "$APP" --skip-logo --audio off --time-limit 400 \
  --input-plan scripts/recipes/save05-move.plan --frame-log
# ② 可见窗口档（**本文的历史读数用这一档**；⛔ 与 ① 不混臂）
./out/emulator-lift-aot --present window \
  --game-dir ../../links/install --appdata "$APP" --skip-logo --audio off --time-limit 400 \
  --input-plan scripts/recipes/save05-move.plan --frame-log
# ③ 仪器档（档位原样保留、只叠加仪器；`--profile-slow-frames` 必须与 `--profile` 同给）
./out/emulator-lift-aot --present window --present-hidden \
  --game-dir ../../links/install --appdata "$APP" --skip-logo --audio off --time-limit 400 \
  --input-plan scripts/recipes/save05-move.plan \
  --profile --profile-slow-frames 60 --frame-log --input-plan-trace
```

- 单跑总 wall ≈ 38 s、窗口 ≈ 7.8 s；完成度看 **exit 0** 且最后的 `input-plan: … pending=0`
  （某个门超时 ⇒ stderr 点名 `文件:行` + exit 5；`wait` 没有守卫）。
- 仪器档为什么是 60：50 ms 稳态会吃满 32 条上限、窗口尖峰一条都留不下
  （实测 `--profile-slow-frames 50` ⇒ `slow=244 listed=32 dropped=212`）。
- 示例的 `--appdata` 用本树 `.tmp/` 下的私有目录（gitignored，跑的人可换；⛔ 别指向安装树）；配方不含任何 `.tmp` 路径。

## 3. 读数（命令 + 日期 + 档；⛔ 不写成断言）

2026-11-20 / macOS arm64（M3 Pro 12 核，机器独占）/ RelWithDebInfo / `--present window`（即 **visible** 档）。
「8 尖峰 Σ」与「862 / 886」来自仪器档的 slow-frame 表（`--profile --profile-slow-frames 60`）；干净档只给窗口包络。
**行与行是各自那一轮的 before/after，⛔ 跨行不可比**（before 二进制不同，档也可能不同）。

| 阶段 | commit | 档 | 当时读数 |
|---|---|---|---|
| **P0 基线** | `dc9cc44`（build 另带 16 个未提交 plugin 改动，未启用） | visible · 干净档 N=3 + 仪器档 prof60 | 窗口 wall 7.751 / 7.789 / 7.788 s（中位 **7.79 s**）· 195 / 195 / 191 帧（中位 195）· 8 尖峰 **Σ1005 ms** |
| **P**（泵判定内联 + lookup-TLS） | `793826b` | visible · 3 对交错 + prof60 2 对 | `boundary` **11.075 → 10.091 ns/派发（−8.9%）**，3/3 同向；`lift` 11.677 → 10.598（−9.2%）；`boundary` 绝对 ms 只报（−2.9%） |
| **C1**（COM 槽计划缓存） | `b48fdad` | visible · 3 对交错（尖峰取自 prof60 慢帧） | `ComRegistry::createObject` 槽计划命中 **100%（712/0）**；**8 尖峰 Σwall 959.1 → 804.9 ms（−16.1%）**，逐对 −13.4…−16.5% · 862 位 −17.5% · 886 位 −11.8% · `D3DXCreateTexture` 213.8 → **<10.5 µs/次**、`GetSurfaceLevel` 196.7 → **<6.9 µs/次**（上界法，≥95%）· 窗口 wall −1.93%（只报不判） |
| **A′**（`%st` 别名 `cpu_`，删每派发寄存器拷贝） | `fbe4760` | visible · 干净 6 对交错 + prof60 2 对 | `boundary` **10.780 → 10.239 ns/派发（−5.02%，6/6 同向）**；逐对中位 **−4.47%**（范围 −2.01…−6.67%）⇒ **与 5% 分辨力同量级，只判方向**；prof60 11.387 → 10.738（−5.70%，2/2）；绝对 ms −0.03%（只报） |

- P 与 A′ 的 `boundary` **绝对 ms** 只 −2.9% / −0.03%：窗口是 20 Hz 忙等，快了以后同一段墙钟里多转派发（+4~6%）
  ⇒ 收益被自己吃回。**可比口径是 ns/派发**，⛔ 不是桶的绝对 ms。
- hidden 档（`--present window --present-hidden`）：`hpa1`（2026-11-20，mac arm64，N=195，7591 ms）
  `Present 1715.401 ms (22.6%)` = readback **7.93** / hash **0.748** / sink **0.117** ms/帧；
  ⚠ 与 visible 档不可互比（§4-4），只判比例与趋势。
- 以上 P/C1/A′ 全是 visible 历史档；hidden 与 visible **不混臂**。

### 3.1 win32 复核（2026-11-20 / win32 x64 / 机器独占 / hidden 档）

这一轮回答的是"mac 上那几项在本机还剩多少"，**不是**单变量归因：before 臂是 **拉代码之前**的构建
（`apps/emulator-lift/out`，`b14b897`，exe mtime 2026-11-20 03:50:44、sha256 前 8 `BFD94F47`），
after 臂 = HEAD `911fe2b` 重建（22:02:29、`split8-O2-lto-abi`）。两臂之间**五个变量**：
`793826b`(P) · `b48fdad`(C1) · `fbe4760`(A′) · `7f47235`(C6) · `ca0b877`(Phase A) ⇒ 只能判"合臂方向与量级"。

命令（cwd `apps/emulator-lift`，每跑前 `scripts/clean-appdata.sh ../../.tmp/win32-perf/appdata-<臂><n>`）：

```bash
# before 臂：<repo>/.tmp/win32-perf/baseline/emulator-lift-aot.exe（exe + 8 个 DLL）
# after  臂：out/emulator-lift-aot.exe
<exe> --present window --present-hidden --game-dir ../../links/install \
  --appdata ../../.tmp/win32-perf/appdata-<臂><n> --skip-logo --audio off \
  --time-limit 400 --input-plan scripts/recipes/save05-move.plan --frame-log
```

- **主判据 `boundary` ns/派发**（= 该表 `boundary_ms×1e6 / iter=`；⛔ 没有现成字段）：3 对交错
  `b1 a1 | a2 b2 | b3 a3` ⇒ 17.199→15.850（−7.84%）· 17.068→15.850（−7.13%）· 18.730→15.904（−15.09%）；
  逐对中位 **−7.84%**，臂中位 17.200→15.850 = **−7.85%**，**3/3 同向**。
- **窗口墙钟** 7538→7492 ms（−0.61%，逐对 −0.70…−0.01%）⇒ 低于本机 5% 分辨力，**只报不判**。
- **绝对 `boundary` ms +0.78%、`iter` +9.1%**（64.0M→69.9M）：与 mac 同型（wait-closed，快了以后多转派发，
  收益被自己吃回）⇒ 可比口径仍是 ns/派发。
- **尖峰**：`save05-move.plan` 在 win32 上只有 **3 帧**过 `dInsns≥50M`（不是 mac 的 8）——
  两对仪器档 Σwall 973.0→662.4 ms（−31.9%）· 866.6→649.4 ms（−25.1%）；三帧 `insns` 两臂逐位同
  （174.0 / 67.3 / 66.7 M）⇒ 同一事件。top-8 慢帧 Σ 1293.5→1002.1、1189.5→973.1。
  那 174M 事件里 `CreateFileA` 38.3→15.6 ms / 30.2→16.6 ms（−45…−59%）。
- **Phase A 本档不可分离**：干净/仪器档都带 `--frame-log` ⇒ `needsGuestPixels()` 为真、逐帧强制回读，
  门控不生效；`LIFT_PRESENT_EAGER=1` 与缺省两臂的 `readback` 6.64/6.79 vs 6.88–6.90 ms/帧（before 6.89–7.04）
  ⇒ 三组重叠（≤6%）。要分离得换"无 `--frame-log`"的档。
- **指纹**：`stop @ eip=00412383` · arena `89668632/119683880/182` · `table misses=0` · `exit 0` ·
  `pending=0` 在 **14/14 跑逐位相同**；只有 `frames` 823→839（+1.9%，wait-closed 的必然，§4-2 已把它排除在指纹外）。
- **与 mac 方向**：boundary 同向（mac P −8.9% / A′ −5.02%），尖峰 Σ 同向且量级更大（mac C1 −16.1%），
  C6 的 `CreateFileA` 同向（mac −7.8%），Phase A 两边都测不出来（mac −1.7% <5%）。
  ⚠ 档不同（mac = arm64/visible/点击版；本轮 = x64/hidden/拖拽版）⇒ **只比方向，不比绝对值**。
- **与既有 win32 读数不可比**：`W1`（LTO `/Ob2` −9.25%，FELLOW init）与 `W4`（点击版 SAVE05、
  pre-P、visible、28.6 ns/派发）都是**不同配方 / 不同 commit / 不同档** ⇒ 只当方向参照。
- ⚠ 这是 `save05-move.plan`（拖拽版）在 **win32 上的第一次运行**：四个位置门全部成立，
  14/14 `exit 0` + `pending=0`（无 exit 5）。

## 4. 判据

1. **主判据 = `boundary` ns/派发**（同档、A/B 只差一个变量、≥3 对交错，比臂中位）。wait-closed ⇒ 绝对 ms **只报不判**；
   "同派发数折算"可作旁证，但⛔ 不换主判据。
2. **8 尖峰帧**：窗口内**同一事件顺序**的 8 个尖峰（Σwall、最大位、拖拽起步位分别看）。帧号会平移（本轮 +2…+4），
   `frames` / `insns` 也不逐位稳定 ⇒ ⛔ 不按字面帧号匹配，⛔ 不把 `frames` / `insns` 列进指纹。
   ⚠ 「8」是 mac visible 档的读数；**win32 hidden 档同一配方只有 3 帧过门**（§3.1）⇒ 尖峰个数**随平台/档变**，
   判的是"同一事件顺序的那几帧"，不是"必须是 8 个"。
3. **语义指纹**（逐位同）：`exit 0` · `stop @ eip=00412383`（`input-plan: quit`）· arena 三元组
   `89668632 / 119683880 / 182`（in_use / peak / surfaces）· `table misses=0`。
4. **hidden 不混臂**：两臂必须同档（都 visible 或都 hidden）；visible 的旧数不能当 hidden 的 before；
   要换档就两臂重跑。仪器档与干净档也分表（prof60 与干净档的窗口 wall 差 −0.4%，属同一现象的另一面）。
5. **完成度**：exit 0 + `pending=0`；门超时是 exit 5（点名 `文件:行`），⛔ 不读成"解析失败"。

## 5. 不做 / 待核

- **win32 只复核了 hidden 档的合臂**（§3.1，2026-11-20）：visible 档、单变量归因（逐 commit 的 before/after）、
  以及 `/Ob1`+LTO 的反汇编计数（`bl liftRegsToCpu` 那类）仍**只有 mac 读数** ⇒ 要单变量归因就在 win32 上
  逐 commit 重建（每个 commit 一次构建 + ≥3 对），⛔ 不要拿合臂数字反推单项。
- **hidden 档要重取就两臂都跑**（见 §4-4）；Present/GPU 的绝对数在 hidden 与 visible 之间**不可互比**
  （只判比例与趋势；`Present` 在尖峰里是常数项，不随尖峰增长）。
- 装弹帧（窗口前）与 `click` 之前的转场不在基准目标内，⛔ 不列判据。
- ⛔ 不改产品代码、不改 `saves/**`：本文只描述"怎么跑、怎么判、读到过什么"。
