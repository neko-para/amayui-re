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
GAME="$APP/Local/Eushully/天結いキャッスルマイスター"
rm -rf "$APP" && mkdir -p "$GAME/SAVE"
cp ../../saves/SAVE*.DAT ../../saves/SAVE*.STH ../../saves/SAVE.DAT ../../saves/RT.DAT "$GAME/SAVE/"
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
- hidden 档（`--present window --present-hidden`）**本轮没有读数**：以上全是 visible 历史档；hidden 要重取就两臂都跑。

## 4. 判据

1. **主判据 = `boundary` ns/派发**（同档、A/B 只差一个变量、≥3 对交错，比臂中位）。wait-closed ⇒ 绝对 ms **只报不判**；
   "同派发数折算"可作旁证，但⛔ 不换主判据。
2. **8 尖峰帧**：窗口内**同一事件顺序**的 8 个尖峰（Σwall、最大位、拖拽起步位分别看）。帧号会平移（本轮 +2…+4），
   `frames` / `insns` 也不逐位稳定 ⇒ ⛔ 不按字面帧号匹配，⛔ 不把 `frames` / `insns` 列进指纹。
3. **语义指纹**（逐位同）：`exit 0` · `stop @ eip=00412383`（`input-plan: quit`）· arena 三元组
   `89668632 / 119683880 / 182`（in_use / peak / surfaces）· `table misses=0`。
4. **hidden 不混臂**：两臂必须同档（都 visible 或都 hidden）；visible 的旧数不能当 hidden 的 before；
   要换档就两臂重跑。仪器档与干净档也分表（prof60 与干净档的窗口 wall 差 −0.4%，属同一现象的另一面）。
5. **完成度**：exit 0 + `pending=0`；门超时是 exit 5（点名 `文件:行`），⛔ 不读成"解析失败"。

## 5. 不做 / 待核

- **win32 无本轮读数**：`/Ob1` + LTO、`offsetof` 的条件支持合法性、`bl liftRegsToCpu` 这类反汇编计数都只在 macOS 验过，
  win32 要各自重取（同一配方、同一档；⛔ 跨平台不混表）。
- **hidden 档要重取就两臂都跑**（见 §4-4）；Present/GPU 的绝对数在 hidden 与 visible 之间**不可互比**
  （只判比例与趋势；`Present` 在尖峰里是常数项，不随尖峰增长）。
- 装弹帧（窗口前）与 `click` 之前的转场不在基准目标内，⛔ 不列判据。
- ⛔ 不改产品代码、不改 `saves/**`：本文只描述"怎么跑、怎么判、读到过什么"。
