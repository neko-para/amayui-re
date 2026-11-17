# blind wipe（场景命令族 kind 2）· 卡顿剖面

> 场景：转场时的**纵向切条抹除**（blind wipe，边界从左向右推进）。本文回答两件事：**这段每帧在做什么**
> （静态，锚 EA）与 **它慢在哪、怎么量**（配方 + 读数 + 未定）。锚一律**二进制 EA**
> （执行源 `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`），⛔ 不拿 `.lst` 行号当锚；读数口径见
> `measurement.md`（M-01 / M-02 / M-05 / M-12）。⛔ `## 每帧工作量模型`**整节是模型**，只有标「读数」的数字出自 run。

## 身份（静态 · 锚 EA）

### 写侧：脚本把一条 kind 2 记录写进场景命令表

- 脚本 opcode **`0x24f`**（10 操作数）→ handler **EA `0x4AF6A0`**（`SetBlindWipe`；两条错误串
  `"関数：SetBlindWipe エラー：分割幅が０以下です．"` / `"…タイプが不正です．"` 在 `0x4AF6B1` / `0x4AF855`）；
  唯一调用方 = VM 派发 **EA `0x4258F0`**（`call sub_4AF6A0`）⇒ 它是**派发块入口**（能当 `--until` 停点）。
- 记录字段（写点 EA）：`+0x00 = 2`（种类）`0x4AF759` · `+0x04 = 0` `0x4AF778` · `+0x08 = 延迟` `0x4AF784` ·
  `+0x0C = 持续` `0x4AF799` · `+0x10 = 纹理` `0x4AF7AE` · `+0x14 / +0x18 / +0x1C / +0x20 = 另 4 个操作数`
  `0x4AF7C0` / `0x4AF7EA` / `0x4AF7D5` / `0x4AF7FF` · `+0x34 = 类型` `0x4AF814` · `+0x38 = 分割幅` `0x4AF829` ·
  `+0x3C = 命令键` `0x4AF842`；收尾 `[edi+0B5ACh] = 1`（场景脏）`0x4AF845`。
- 两条校验：**分割幅 < 1** ⇒ 夹成 1、类型置 −1、延迟 / 持续清 0、**照写**（`0x4AF6AC`..`0x4AF6DC`）；
  **类型 ∉ [0,11]**（两端闭、含负数）⇒ 响亮错误、不写记录（`0x4AF6EE cmp eax,0Bh` / `jg 0x4AF855`）。

### 渲染侧：每帧把整场喂进两块画布，再按图案逐单元落两笔

- 帧入口 **EA `0x4B4460`** ← 主循环 **`0x423EC0`** → 命令档 **EA `0x4B06D0`**（31 个间接调用点）。
- ① 两段绘制键区间各喂一块**全屏画布**：`sub_4A50C0` = SetRenderTarget 包装（画布 **36 / 37**），站点 **`0x4B17BE`**。
- ② 换到命令自己的纹理、按**图案**逐单元落笔：分派 = 跳转表 **`0x4B1832`**（`jmp ds:jpt_4B1832[esi*4]`，**12 个 case**）：
  `0x4B1839`(0) · `0x4B1A2B`(1) · `0x4B1C08`(2) · `0x4B1DB9`(3) · **`0x4B1F99`**(4) · **`0x4B21DB`**(5) ·
  `0x4B2402`(6) · `0x4B263D`(7) · **`0x4B286F`**(8) · **`0x4B2AB5`**(9) · `0x4B2CF5`(10) · `0x4B2F35`(11)。
  4/5/8/9 = **纵向条带**（6/7/10/11 用高 ⇒ 横条）。
- ③ 每单元 **2 笔** = **`0x4A2D50`**（DrawTexture）：先画布 **36**（`push 24h`）、后画布 **37**（`push 25h`），
  相接不重叠。纵向族落笔站点：mode 4 = **`0x4B2131` / `0x4B21BD`**；mode 8 = **`0x4B2A00` / `0x4B2A86`**。
- 本命（`SC0000` 的 wipe 块）：`f8045 = 8` ⇒ **mode 8**（case EA **`0x4B286F`**）、纵向条带、从左向右。每次
  DrawTexture 的接口面 = sprite `SetTransform` → `Begin(D3DXSPRITE_ALPHABLEND)` → `SetSamplerState`×3（MIN/MAG=LINEAR、
  MIP=NONE）→ `SetRenderState`×1..3（ALPHATESTENABLE=0、SRCALPHA/INVSRCALPHA）→ 四边形提交 → `End`；
  **没有** StretchRect / LockRect / guest 的 DrawPrimitiveUP。
- 排除项：`SetLineFade`（`0x443B20`）/ `SetRandomFade`（`0x445D70`）是**另一套**老的淡入淡出；`0x203 SlideBlur`
  支在 `0x4B34F9` 之后（与本族共用命令表但不同图案）。

## 每帧工作量模型（⛔ 模型，不是实测）

- **条带数 `⌊A/S⌋ + 1`**（`S` = 记录 `+0x38` 分割幅，≥1；纵向族 `A = 1280`，横向族 `A = 720`）。证据（mode 8 体内）：`0x4B2882 idiv ecx`（`eax = ⌊A/S⌋`、`edx = A%S`）
  → `0x4B2888 lea edx,[eax+1]` 存成循环上界 → `0x4B2AA4 cmp eax,[ebp+var_104]` / `0x4B2AAA jl` 回循环头；
  错相步长的**分母** = `S + 条带数`（`0x4B288B add ecx,edx` → `0x4B2893 fidiv`）。
- **条带笔画 = 2 × 条带数** 次 `0x4A2D50`（每次 `S` 像素宽 × 整高 ⇒ 合成总覆盖 ≈ **2 个全屏**）；**画布填充**：
  两块全屏画布**每帧各自**把自己那段键区间的全部绘制项画一遍 ⇒ 每帧把**整场重画两遍**（draw ≈ 2 × 场景项数）。
- **lift 侧的结构性成本**（不是 guest 侧）：每次换 target ⇒ flush + submit + **等 fence** + 整幅回读
  （1280×720×4 = **3.69 MB**，`d3d9_gpu::rasterPrepare → downloadInto`）；画布被当纹理采样 ⇒ staging 拷贝 +
  整幅哈希 + 上传。wipe 窗**每帧 target 切换 ≈ 4 次**（36 → 37 → 命令纹理 → 后缓冲）。
- ⇒ 模型结论：一帧 ≈ 4×3.69 MB 回读 + 2×3.69 MB 上传/哈希 + 4 次 fence + 2×(场景项数) 次 draw，
  **与 guest 指令数无关**；条带数只抬高 DrawTexture 次数。
- **模型给得出可分辨的预测**（S 的两个候选读法，见 `## 待核` 2）：带宽 20 ⇒ 65 带 / **130 笔/帧**；
  条带数 20 ⇒ 20 带 / **40 笔/帧**。一次 `LIFT_AOT_SHIM_TRACE=1` 数 `ID3DXSprite::Draw` 就能定案。
- ⚠ 四个数（4 次切换 / 两条画布 / 2× draw / 一次整幅回读）都还没有同 run 的计数去对 —— 那正是 H1 / H2。

## 假设与证伪方式

| # | 假设 | 怎么证实 / 证伪（判据落在读数上） |
|---|---|---|
| **H1**（最高）| **换 target 的往返**（整幅回读 + 重传 + fence）是主因 | 窗口 profile 的 `shim` 份额 + 具名行是否 `d3d9!SetRenderTarget` / `d3dx9_*!End`；同 run「wipe 窗 vs pre 窗」的**切换次数**与 ms。两窗切换次数相同而 wipe 仍慢 ⇒ **证伪** |
| **H2** | **整场画两遍**让 draw 数与 T&L 翻倍 | `LIFT_AOT_SHIM_TRACE=1` 按帧数 `ID3DXSprite::Draw` / `SetRenderState`；`lift` 桶在 wipe 窗是否 ≈ 2× pre 窗；wipe 窗 draw ≈ 2× 稳态 + 2×条带数 ⇒ 成立 |
| **H3** | guest 侧条带算式自己贵 | `dispatch-top` 里 `0x4B06D0` / case 体是否进 top-20；`lift` ≥70% ⇒ 成立（**条带段已否掉**：那 ~10 帧 guest 只 0.02–0.06 M insns） |
| **H4** | **present**（回读 + hash）是恒定开销 | `present` 子行（readback / hash / freeze / sink / stats）占比；headless vs window 的差（⚠ M-01：跨档不可混比，只作形状） |
| **H5** | **帧率门**：vsync 把 >16.7 ms 切成 33.3 ms | `--present-vsync 1` vs `0`：Δt 是否钉在 16.7 / 33.3 的整数倍；vsync=0 的 Δt 才是这段的真成本 |
| **H6** | `0x203 SlideBlur` / 软件模糊（`PixelShaderVersion = 0`）一起跑 | `--until 0x4A62A0`（`BlurTexture`）/ `--until 0x4B36EF` **是否命中**（命中 ⇒ 软件路径进了；macOS / caps=0 那次 `0x4A62A0` 在 frame 1344 命中过，见 `scripts/SC0000.md`）；再补一档 `LIFT_D3D9_PS_VERSION=0x200` 对照 |

## 实测（2026-11-18 / win32 / dev `@847ebb8` / **LTO OFF**）

口径：`apps\emulator-lift\out\emulator-lift-aot.exe`（mtime 2026-11-18 02:31、15,943,168 B）· `--skip-logo --audio off` ·
appdata = **一份专用副本**（从现有 appdata 复制；⛔ 不写用户的 appdata；跑完两棵树 `git status` 均干净）·
`--game-dir` 指向 `dist\install` 的只读 junction。

### 槽位校准（★ 先说这个，否则下面全部错位）

加载屏 10 行的**行中心 y**（`emu-ctl screenshot`，frame=523，`script=SAVE.BIN pos=0xB39C`）：

| 行 | 行中心 y | 内容 |
|---|---|---|
| LOAD 000 / 001 / 002 | ≈ 91 / 150 / 209 | 旧存档（01/02 19:54 · 01/02 20:03 · 01/03 21:22） |
| **LOAD 003** | **≈ 269** | **`NEW!` 01/06 21:23 = `SAVE03.DAT`（第 4 槽 = 用户那份）** |
| LOAD 004..009 | ≈ 328 起 | `保存情報なし` |

- **行距 ≈ 59.5 px（⛔ 不是 40）**：`click 300 220` 落在 002 与 003 之间、偏 **002** ⇒ 那是 `SAVE02`。⇒ 早期用 `220` 的那批探针（下面的 P0/P1）走的是 **SAVE02 的链**；用户那条用 **`click 300 269`**，加载后**只点 1 次**就转场。
- 配方落成 tracked 件 `apps/emulator-lift/scripts/recipes/sc0000-wipe.plan`（⛔ 不再引用 `.tmp` 里的副本）。

### P0 / P1 · `--until` 探针（**SAVE02** 那条链，机制读数）

| run | present | `--until` | 结局 | `frames` | input-plan |
|---|---|---|---|---|---|
| mode 4 | headless | `0x4B1F99` | time-limit @ `0x4904E0` | 921 | `steps=14 fired=6 pending=0` |
| mode 5 | headless | `0x4B21DB` | time-limit @ `0x4127D2` | 663 | `pending=0` |
| mode 8 | headless | `0x4B286F` | time-limit @ `0x4127D2` | 670 | `pending=0` |
| mode 9 | headless | `0x4B2AB5` | time-limit @ `0x4904E0` | 739 | `pending=0` |
| 阳性对照 | headless | `0x4B06D0` | **`until-ea @ 0x4B06D0`** | **0** | `fired=0`（boot 期就停） |
| window 档 | window（visible） | — | time-limit @ `0x4904E0` | 569 | `pending=0` |

- 四条 mode 探针**全部 time-limit**，但 `pending=0` ⇒ 配方跑完了，**不是**"没到 wipe 就卡住"；这是 `--until` 的盲区（见下节）。
- 阳性对照 `0x4B06D0` 在 **`frames=0`（boot 期）**就命中 ⇒ 只证明"until 机制没坏"，⛔ 它**不是**这段的停点。
- 同一条链到达 wipe：`--until 0x4AF6A0` + 80 组推进点击 ⇒ `until-ea @ eip=004AF6A0` · `frames=2096` · `insns=14,745,160,926`
  · `steps=171 fired=32 pending=105`；同配方不给 `--until` ⇒ `unimplemented @ eip=7FFE0098`（`d3dx9_43!D3DXMatrixDecompose`）、
  `frames=2134`（另两跑 1704 / 2028）⇒ **wipe → 3D 场景 → `D3DXMatrixDecompose`**。命中帧在 **2008–2096** 之间漂（≈4%）。

### 用户那条（**SAVE03 + 1 击**）

- `sc0000-wipe.plan` + `--until 0x4AF6A0` ⇒ **`until-ea @ eip=004AF6A0`** · **`frames=292`** · `steps=12 fired=5 pending=0` ⇒ 配方走完、没有门超时。

### 条带态（截图 · **SAVE02 链** · 同一 run 内逐帧）

- `cap-2007` 转场前（角色立绘 + 对白框）· `cap-2014` **条带态**（左 = 新场景遗迹全景 / 右 = 旧画面 / 中间一列竖条）·
  `cap-2040` / `cap-2071` / `cap-2096` 已全换成新场景 ⇒ 整段 ≈ **2008 → 2039 帧（≈31 帧 ≈ 1 s）**。
- **图案测量**（把 2014 的每一列与 2007 旧 / 2071 新比对，按 y = 120/240/360/480 分类）：x 0..559 = 新（已让出）、
  x ≈ 901..1280 = 旧（未让出）；中间是 `新(len 18,17,16,…) + 旧(len 3,4,5,…)` 交替，**旧段起点间距恒 19 px**、旧段长度**每格 +1** ⇒ **三族错相**（第 i 带晚 i 像素步）、**从左向右**、**S ≈ 20 px** ⇒ mode 8 ✓。
- ⚠ 采样不完整（1843–2096 之间的一批，`2008..2013` 未逐帧全采）；且这批来自 **SAVE02** 链 —— 用户那条只拿到停点
  `frames=292`、**没有截图** ⇒ 图案参数是否相同**未核**（见 `## 待核` 2）。

## timed 读数（2026-11-18 · 9 run · 三档交错 · 每档 3 次）

`--profile --frame-log` 全开；A = `--present headless` · B = `window --present-vsync 0` · C = `window --present-vsync 1`。
下表是**每档 3 次的 min–max**（逐帧 `frame-ms` 与两张 `=== profile window ===` 表）：

| 档 | 最大单帧 Δt（该帧 insns） | "卡"帧数 | 这些帧 Δt 中位 | `pre` ms/帧（`lift`%） | `wipe` ms/帧（`lift`% / `shim`%） | `present` ms/帧 |
|---|---|---|---|---|---|---|
| A headless | 2364–2678 ms（**855 M**） | 10–11 | **121.1–126.6** | 41.6–100.0（49.6–60.2） | 15.0–16.9（21.8–30.1 / 51.9–64.3） | 4.08–4.33 |
| B vsync 0 | 2274–2588 ms（**855 M**） | 9–10 | 121.8–128.1 | 51.7–100.0（50.6–58.7） | 15.1–15.4（24.8–29.6 / 52.2–58.8） | 4.72–4.88 |
| C vsync 1 | 2452–2565 ms（**855 M**） | 10–13 | **118.6–141.2** | 68.2–88.2（54.3–57.0） | 16.9–24.9（29.4–34.7 / 45.8–52.5） | 5.36–6.90 |

- **第一段卡 = 一次性 guest 计算**：一帧 **855 M insns / 2.3–2.7 s**，9 次 run 彼此一致（855 / 854.9 M）
  ⇒ 确定性 guest 工作（更像一次性场景 / 画布建立，**不是**条带逐带绘制）。
- **第二段卡 = ~10 帧 × ~120 ms（≈8 fps），guest 空闲**：这些帧 guest 只跑 **0.02–0.06 M insns** ⇒ 时间**不在 guest**，
  在 shim / GPU 路径上。这一段才是条带动画。
- **H5 否掉**：C 档（vsync=1）同帧仍是 **118.6–141.2 ms**，比 16.7 ms 大一个数量级 ⇒ 不是帧率门量化。
- **H4 只是常数项**：`present` **4.1–6.9 ms/帧**（headless 也在，readback / hash 为主）⇒ 解释不了 120 ms。
- **与 H1 同向**：`ID3DXSprite::End` **0.12–0.31 ms/次**（常规 0.05–0.08 的 **2–6×**，N=1163–1943）；
  `SetRenderTarget` 偶尔 **3.99 / 4.57 ms/次**（只有 2 个 run 出现，N=12 / 4）。
- ⚠ **`wipe` 窗被稀释**：窗宽 1800 ms 覆盖"转场 + 之后的新场景"，128 帧里只有 ~10 帧是动画（其余 ~7 ms/帧）
  ⇒ 表里的 `wipe` ms/帧（15–25）**不是**动画帧时长（那是 **~120 ms**）；`pre` 窗落在装载后的初始化段
  （41.6–100 ms/帧）⇒ **wipe/pre 比值 <1 没有意义**，⛔ 不要据此说"wipe 比平时快"或"这段没问题"。
- **未定**：那 ~10 个 120 ms 的帧里**具体是哪个 shim**。现有 `--profile` 只有整轮 / 窗口聚合（1800 ms 含 ~118 个轻帧），
  缺"**按帧**的 shim 分桶"；855 M 那一帧的 EA 归因同样缺（`dispatch-top` 只有整轮）⇒ 这是本轮真正的**仪器缺口**。

## `--until` 的盲区（详见 `defects.md#BUG-31`）

- **跳转表 case 体停不住**：`lift_dyn` 直接尾调用目标函数、**不检查** `until_ea`；生成代码块内也不检查。★ 注意：
  `0x4B286F`（mode 8 case）**在** `out/aot/lift_table.inc` 里（可当派发块入口），但它由 `jmp ds:jpt_4B1832` 经
  `lift_dyn` 进入 ⇒ 派发主循环的 until 比较根本不经过 ⇒ 探针 0 命中。"在派发表里"与"停得住"是两件事。
- **`call` 的返回点不是块入口**：`0x4A9F50` / `0x4AF05A` 在派发表里没有条目 ⇒ 永远不命中；改用 `call` 之后紧邻的
  块入口 **`0x4A9F55`** / **`0x4AF05F`**（后者尚未取得命中读数 ⇒ 待核 5）。
- **能用的是什么**：VM 派发的**块入口**（本例 `0x4AF6A0` 已实测命中）⇒ 停"第一帧"打**写侧 handler** 可靠；
  停"渲染侧第 n 帧"只能靠配方里的 `profile_begin` + 窗内相对判据，不能靠 case EA。

## 测量配方

**前置**：① appdata 用**副本**（⛔ 不写用户的真 appdata；跑完 `git -C . status` 干净），副本里**删掉
`SAVE.DAT` / `SAVE.BAK` / `RT.DAT`**（有它们时加载屏第一页是 `110..119` 全空）；② 载入判据是**位置门**
（`TITLE [0x0460,0x0520)` → `SAVE [0x1104,0x11C4)` → `SBUNKI` 三段，确认框要三次回调，形状见 `scripts/SC0000.md` 第 3 轮）；
③ 槽位 `click 300 269`（见上表）；④ 每次把 `SC0000.BIN` 的大小 + sha256 写进日志头（`dist/install` = 504,312 B 汉化修订版，原版 `links/raw` = 511,688 B）。

```bash
cd apps/emulator-lift && ./out/emulator-lift-aot \
  --game-dir ../../links/install --appdata <专用副本> --skip-logo --audio off \
  --present headless --profile --frame-log --max-frames 4000 --time-limit 240 \
  --input-plan scripts/recipes/sc0000-wipe.plan
```

`sc0000-wipe.plan` = 加载链（`TITLE` → `click 1070 480` → `SAVE` → `click 300 269` → `SBUNKI` 三门）→ `wait 4000` →
`profile_begin pre` / `wait 1500` / `profile_end pre` → `profile_begin wipe` / `click 500 500` / `wait 1800` / `profile_end wipe`。
**三档**只差一个开关：A = `--present headless` · B = `--present window --present-vsync 0` · C = `--present window --present-vsync 1`；
每档 3 次（M-02：N≥3 取中位），另跑一档**不带** `--profile`、用 `Measure-Command` 取总墙钟做量级校核（⛔ 开 / 关仪器混进同一张表）。
⛔ **禁用 `--max-frames F0+K`** 当窗边界（F0 本身在漂 2008–2096 / 292 两条链各自不同）——窗只由 `profile_begin/end` 划定。

**读什么**：逐帧 `frame-ms:`（`n / t_ms / insns`）是**帧时长判据**；`=== profile window: pre|wipe ===` 的
`wall / lift / boundary / shim`（+ `present` 子行、`shim:<module!name>` 具名行）只作**归因**；`gpu :` 是
`Present` 的内部拆解、**⊂ `shim`**，⛔ 不能与闭式相加。

## 判据

- **主判据 = 逐帧 Δt + 同帧 insns**（不是窗均值）：Δt ≥60 ms **且**该帧 guest insns ≪1 M ⇒ "卡在宿主 / shim 侧"；
  Δt 大**且** insns 同量级（如 855 M）⇒ guest 计算。窗级 `ms/帧` 一律**不作**帧时长判据（窗被轻帧稀释）。
- **归因门（单窗，只用于归因）**：`lift` ≥70% ⇒ guest 计算 · `shim` ≥50% 且 top 具名行是 `SetRenderTarget` / `End` ⇒ H1 ·
  `present` 子桶 ≥30% ⇒ H4 · `boundary` 最高 ⇒ 派发 / 边界税，与这段无关。**H2**：wipe 窗 draw ≈ 2× 稳态 + 2×条带数，且 `lift` / `shim` 同向上涨。
- **H5**：C 档 Δt 是否 16.7 的整数倍。**实测已否**：C 与 B 同量级（118.6–141.2 ms vs 121.8–128.1）。
- ⛔ **本文不给"与真机差 X 倍"**：本仓**没有**真机帧时长读数。允许的措辞只有"相对本模拟器**同场景稳态**偏慢"。要下真机结论，
  先得有：同存档 + 同一下点击、真机逐帧时长（RTSS / Fraps / 引擎叠层）+ vsync 状态 + 分辨率 / 窗口模式，并且比**中位数与掉帧分布**（⛔ 不比均值）。
  ⛔ 不拿 `--profile` 的墙钟当结论；⛔ 不把 headless ↔ window 的差当这段的属性（M-01）。

## 待核

1. **120 ms 那 ~10 帧的 shim 归因**（本文最大的缺口）：要"按帧的 shim 分桶"，现仪器没有（`--profile` 只有整轮 / 窗口聚合）
   ⇒ 登记为"需要什么仪器"，⛔ 不自行改代码。
2. **分割幅 / 条带数**：`+0x38` 是带宽（⇒ 65 带 / 130 笔/帧）还是条带数（⇒ 20 带 / 40 笔/帧）—— 见「每帧工作量模型」；
   一次 `LIFT_AOT_SHIM_TRACE=1` 的 `ID3DXSprite::Draw` 计数即可定案。控制层也读不到 `record[13]/[14]`（没有读 guest 内存的出口）。
   ⚠ 图案读数来自 **SAVE02** 链，用户那条的 S 是否同为 20 **未核**。
3. **855 M 那一帧与条带帧的先后 / 是不是同一段**：两者在帧号上相邻，但 `--until` 命中帧（292）与 timed 跑的最大
   Δt 帧（n=105–120）来自**不同 run**，帧号会漂 ⇒ 同一段的可能**未核**；⛔ 不跨 run 拼时间轴。
4. **`F0` 漂移来源**：加载时长 + `wait` 走宿主钟 ⇒ 2008–2096（≈4%，SAVE02 链）/ 292（SAVE03 链）；来源未定。
5. **`0x4AF05F`**（`0x4AF05A` 之后紧邻的块入口）是否真能命中 —— BUG-31 记的是"尚未取得命中读数"。
6. **与 `SC0000` 黑段是不是同一段转场**：`scripts/SC0000.md` 第 4 轮的黑段（`sample` 读到 `script=SC0000.BIN pos=0x6EAA8 frame=1996`）
   与本文的 wipe 窗帧号相邻，但那是**另一台机器 / 另一次 run / 另一个脚本修订版** ⇒ 同一段的可能**未核**。
7. **wipe 的窗边界（位置门）**：`await position SC0000 0x7201C 0x725DC` 是候选门，**未取得命中读数**；若该块在转场期间
   不停留（脚本直接往下走），这门就不成立 ⇒ 只能退回"在最后一个推进 click 前开窗"。
