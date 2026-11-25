# FIELD.BIN

## 身份

- 清单：`dist/views/data/FIELD.BIN.txt`（**13053 行**；头 `signature = SYS4450`、`local_vars = { 81b6 4 3 13 1 5 }`，行 1–3）。
  该名字在 `dist/views/data/` 里**只有一份**（没有 `$1$` / `$2$` 副本）。
- ALF 条目 **21095**：`arc=0(DATA1.ALF) fileIndex=15 offset=1085133632 length=282940`
  ```bash
  node .tmp/field-skill-hint/alf-scan.mjs links/install/SYS4INI.BIN 'FIELD' --limit 10
  # 读数：idx=21095 FIELD.BIN arc=0(DATA1.ALF) fileIndex=15 offset=1085133632 length=282940
  #       同族另有 SETFIELD.BIN(20745,380 B) · DEBUGFIELD.BIN(21102) · DEBUGFIELD2.BIN(21103)
  ```
- ★ **两份副本不同构，锚必须带副本**：松散件 `links/install/FIELD.BIN`（282940 B，引擎优先读）vs ALF 原件（idx 21095）。
  同一对绘制在松散件是 `instr#10029/#10038`、在 ALF 原件是 `instr#10002/#10011`（差 27 条）。
  本文所有 `instr#` / `0x…` 若未注明 **= 松散件**。
- `script` 身份名怎么来的：运行期 marker 文件名匹配（ASCII 大小写不敏感、两侧剥 `.bin`），判定在
  `apps/emulator-lift/src/runtime/recipe.cpp` 的 `scriptNameMatches`；★ **插件 trigger 侧是精确 `==`**（`trigger_runner.cpp`），
  所以链里必须写 `FIELD.BIN`（带后缀）。
- 进图装载链：**未取证** ⇒ `## 待核` 1（只有 `SETFIELD.BIN`(20745) 这个旁证）。
- 结构锚（本文区间都靠它，地址 = `label_%08x`）：
  - **进图初始化段** `0x000D54..0x001004`：连排 `fill-zero` 清 32 槽表 ——
    `fill-zero (global-int 6202a) 32`@`0x000E30` · `fill-zero (global-int 50ff9f) 32`@`0x000FA0` ·
    `fill-zero (global-int 585ff) 32`@`0x000FB4`（完整清单见 `## 地块特效渲染模型` ③）。
  - **格属性表构建** `0x00FCE4`（`instr#2864`，见模型 ②）。
  - **32 槽点位环** `0x011354`（`instr#3074`）。
  - **格标记绘制** `0x03A6DC`（`instr#9852`）。
  - **主轮询体** `0x579C`（唯一被用过的门，读数与失败面见 `## 位置区间`）。

## 位置区间

读数口径：判据只有 `{script, pos}` 与插件的 `fired/pending/exit`。`pos` 与清单 `label_%08x` 同一坐标系。
执行源 `apps/emulator-lift/out/emulator-lift-aot.exe`（win32，2026-11-25 构建）。
★ `--appdata` **必须让存档落在嵌套层** `<appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/`
（`--appdata` 指 `%AppData%` 根；建法见 `saves/README.md` §`--appdata` 与目录结构）。
把 `SAVE*.DAT` 直接放到 `<appdata>/` 根目录时，引擎会新建**空的**嵌套 `SAVE/`、读档界面能开能翻页，
但**点行不弹确认框** ⇒ `load-save: gate timeout (load:sbunki)` —— 2026-11-24/25 的失败全是这一条，
⛔ 不是“点击被吞”、⛔ 不是“页基算错”、⛔ 也与 `--present headless/window` 无关
（同一嵌套 appdata 下 headless 与 window 的读数逐行相同）。

| 区间 `[lo,hi)` | 是什么 | 复现命令 | 读数日期 |
|---|---|---|---|
| `[0x579C,0x5860)` | **主轮询体**：进图完成、开始等输入。★ 目前唯一被用过的门（`await position FIELD 0x579C 0x5860`） | 见下 ①（开关的常驻读数）/ ②（读档链到 FIELD）/ ③（配方里的同一区间） | **2026-11-25 门 2/2**（headless 与 window 各一次，都命中本区间；★ 当时用的是 `--plugin field-point-visible` 的**自驱读档版本** —— 那条自驱链已删，同一区间现在用 ② 复现）；历史：2026-11-22 配方侧 4/4（出处 `apps/emulator-lift/scripts/recipes/save07-battle.plan` 的 ①–④ 头注释） |

- **`[0x579C,0x5860)` 只证明"到地图就绪"，不证明"点击被消费"**：FIELD 上没做过点击负对照 ⇒ `## 待核` 3。
- ★ **`field-point-visible` 的触发区间不是本表这一条**：它改成 **setmap 的两个返回点** ——
  `[0x18280,0x18284)`（`FIELD_SETMAP_A_*`，`label_00018274` 里 `call-script 511a`=SETOBJ 的返回点）与
  `[0x35168,0x3517C)`（`FIELD_SETMAP_B_*`，`0x035130` 块里同一条 `call-script 511a` 之后）。这两个区间
  目前**只有静态取证、没有 run 读数** ⇒ 按上一段的收录口径进 `## 待核` 9（⛔ 不写进本表）。
- 常量位置：`apps/emulator-lift/scripts/plugins/lib/script-positions.js` 的 `FIELD_MAIN_LOOP_LO/HI`（仅复现入口用）
  与 `FIELD_SETMAP_A_LO/HI` / `FIELD_SETMAP_B_LO/HI`（开关用）；数值只写这一处。

```bash
cd E:/Projects/amayui-re-dev/apps/emulator-lift
# ① `field-point-visible` = **常驻开关**（用户裁决 2026-11-25：目标 = **在 setmap 时无条件显示所有采集/挖掘点**）：
#    ⛔ 不读档 / ⛔ 不点鼠标 / ⛔ 不写盘 / ⛔ 不 finish、⛔ **不轮询**。它常驻 arm **两个**位置触发
#    `afterPosition("FIELD.BIN", …, { mode: "reenter" })`（setmap 完成、SETOBJ 已算完、FIELD 还没建 `65f2` / 画）：
#      `[0x18280,0x18284)` = `label_00018274` 里 `call-script 511a` 的返回点（进图主路径）；
#      `[0x35168,0x3517C)` = `0x035130` 块里同一条 `call-script 511a` 之后的 `mov b22a 1`。
#    每次**外→内** fire 一次；对类型命中（缺省 採集 1f / 発掘 20）的槽把派生渲染位
#    `50ff9f[slot]` **置 `|= 0x5`**（bit0=画、bit2=正常 alpha；DRAWOBJ 画 `0xFA+类型` = `0x119` MO031B 绿草 /
#    `0x11A` MO032B 岩石）。⛔ **不清任何位**，尤其 bit1（A 链格表与 FIELD 的战利品抽取消费它）。
#    Reenter 的边沿挂在**同一个 trigger 对象**上、跨 arm 保留 ⇒ 不需要 re-arm（`trigger_runner.cpp` 的
#    `exhausted()` 只耗尽 Once / `maxFires>0`）。判据 = **已知脚本位置**，不是时间 ⇒ 没有 poll 这个设置项。
#    ★ **`?` / `!` 不是点位**：它们是 A 链格标记（`SO030.AGF` 槽 `0x49`，按帧内 `65f2` 的每格 dword 画），
#      `on=0` 与 `on=1` 的同一帧上都会出现；本开关不改它们（判据是 B 链精灵）。
#    ★ 开关参数写在 **appdata 根**（与 Local/ 同级；⛔ 不在 SAVE/ 下）：
#      {"version":1,"domains":{"field.point-visible":{"on":1}}}
#      域 key：on（bool/0-1，缺省 0）/ types（hex 串或数组）/ diag（0/1）。
#      ★ 旧配置里的 slot / hold / poll 被**无害忽略**（不再读档、不再一次性结束、不再轮询）。
#    ★ 验收 = **玩家自己读档**：--overlay --plugin field-point-visible（`--plugin` 与 --input-plan /
#      --control-root 互斥 ⇒ 没有"输入计划驱动"这条验收）；FIELD 的自动化复现归 ②。
mkdir -p ../../.tmp/field-model/appdata
printf '{"version":1,"domains":{"field.point-visible":{"on":0}}}\n' \
  > ../../.tmp/field-model/appdata/plugin-config.json
./out/emulator-lift-aot.exe \
  --game-dir ../../links/install --appdata ../../.tmp/field-model/appdata \
  --present headless --skip-logo --audio off --time-limit 20 --log-level warn \
  --plugin field-point-visible
# 读数（2026-11-26 / win32 / dev / headless / on=0 基线；★ 没人读档 ⇒ 进程一直停在标题）：
#   [plugin] info: field-points: on=0 types=0x1f,0x20（setmap 时显示所有采集/挖掘点）
#                                                          ↑ 只这一行（关着不做事）
#   readout：file=…/field-point-visible.js entry=default render=no callbacks=yes
#            params=- calls=1 run=waiting
#   stop   ：time-limit @ eip=…（正常结局；★ 落点 eip 随墙钟时点变，不是判据）；exit=0
#   ★ calls=1 = 只有 start：两条 setmap 触发只在玩家进 FIELD 的 setmap 时 fire（没人读档 ⇒ 一次都没 fire）。
#     常驻 → 由 --time-limit 收口，⛔ 不是 finish。
#   ★ on=1 时进 FIELD 的效果面判据在**纯档**：CTest `plugin-runtime` 的 `plugin/field-switch-apply`
#     （假 guest：0x3→0x7 / 0x5→0x5 / 类型不命中不动 / 留在区间内不再 fire / 读到有效"外"再"内"（另一条
#     setmap 钩子）⇒ 重新施加 / `on=0` 一个 global 都不读、都不写）。真机（人读档进 FIELD）的
#     `field-points: setmap forced=` 读数见 `## 待核` 8。
#   ★ 常驻/不自驱的守卫：CTest `plugin-field-switch`
#     （静态：代码里没有 input / loadSave / bootToTitle / chains / finish / quit，且**没有** FIELD_MAIN_LOOP /
#      afterDelay / poll；e2e：headless 真子进程无 --input-plan ⇒ 没有 boot:/load: 行、run=waiting、
#      首行 on=0）。

# ② 自动化到 FIELD 的复现（不是本开关）：读档链 + 场景入口 `scenario-save10-field.js`
#    ★ 存档必须落在**嵌套层** `<appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/`
#      （放根目录 ⇒ 读档列表为空、点行不弹确认框 ⇒ `load-save: gate timeout (load:sbunki)`）。
#    ★ 命令形状取自该入口的文件头（域 `scenario.save10-field`，缺省 slot 10 / expect FIELD.BIN）：
#      {"version":1,"domains":{"scenario.save10-field":{"slot":10,"expect":["FIELD.BIN"]}}}
#      ⚠ 本轮**没有复跑**这条（本轮改的是开关，不是复现链）；读数口径见
#      `docs/03-emulator/plugin-runtime.md` §C2c（同一个 lib/save.js 链：page → row → SBUNKI → FIELD.BIN）。
bash scripts/clean-appdata.sh ../../.tmp/field-model/appdata
./out/emulator-lift-aot.exe \
  --game-dir ../../links/install --appdata ../../.tmp/field-model/appdata \
  --present headless --skip-logo --audio off --time-limit 90 --log-level info \
  --plugin scenario-save10-field
# 本表那一行 [0x579C,0x5860) 的 2026-11-25 读数（headless / window 各 1 次）当时就是用
# 这条链读档后取到的 —— 与"谁点的鼠标"无关，区间判据只有 {script,pos}。
# ③ 配方里的同一区间（历史读数）
rg -n "await position FIELD" apps/emulator-lift/scripts/recipes/save07-battle.plan
# 读数：51:await position FIELD 0x579C 0x5860 timeout 300000
```

## 输入点

- **未取证**：FIELD 上**没有**做过"哪一段吃 click"的读数（本文不给区间）。
- 已知形态（静态，⛔ 不是语义结论）：
  - 主轮询体 `0x579C` 与 TITLE / GAMESTART / FELLOW 同形（`get-input-type` + `sleep` + `jmp`）——
    在那三份上它有"门命中但点击不生效"的读数，**FIELD 上未做**，⛔ 不要直接搬过来（`## 待核` 3）。
  - 32 槽点位的**选中/命中**走局部表 `71f2`（列）/ `7576`（行）/ `65f2`（格属性），绘制环另见模型 ③；
    命中判定在哪一段**没有定位**（`## 待核` 3）。
- 与"地块特效"的关系：`!` / `?` / 环 / 53×53 都是**每帧按格属性重画**，与点击无关（读 `65f2`，不读输入）。

## 地块特效渲染模型

★ 本节的结论**全部落在脚本指令上**（松散件 `dist/install/FIELD.BIN`、`SETOBJ`/`DRAWOBJ`/`SETMAPTEX` 各一份），
每条都带 `instr# + 0x偏移`；语义没有读数的一律进 `## 待核`。
★ **VM 闭环**：这条链的读/写全在 VM 全局 + 帧内 local 上。引擎侧**没有**任何一处引用这些表或这两个 imgid
（`AGE.EXE` 1,007,104 B 与 `AGERC.DLL` 里按 4 字节小端搜 `14d740/50ffd1/5b7e5/6f158/56d69/4a5f0/eb5c1/513d/513e/20797/20798` ⇒ **0 命中**；
`.data:1014D740` 这类命中是**地址串**，不是常量）⇒ 不要在引擎里找这条链的判据。

### ① 三条**互不相干**的链路（先分清，别再互相推）

| 链路 | 图 | 槽 | 谁画 | 判据 |
|---|---|---|---|---|
| **A 格标记** | `SO030.AGF`（ALF **20786**，2000×2000） | **`0x49`**（`SETMAPTEX` instr#3 `0x000090`） | `FIELD.BIN` `instr#9852..10045`（每帧、每可见格） | 帧内 local 格属性表 `65f2` |
| **B 点位精灵** | `MO031B.AGF`（ALF **20797**，64×82）/ `MO032B.AGF`（ALF **20798**，70×88） | **`0x119` / `0x11A`**（`SETMAPTEX` instr#78 `0x0007DC` / #80 `0x000814`） | `DRAWOBJ.BIN` `instr#58` `0x00066C` | **纹理槽 = `0xFA + 点位类型`**（`instr#44` `0x0004E4`）+ `50ff9f[槽]` 的位 |
| **C 演出定点图标** | `AE202AA/AB`（ALF 11719/11740，32×64） | `0x3`（`SC4700`/`SC4900` 的 `set-texture` 立即数） | `SC4700`/`SC4900` | 剧情 flag（与技能/地块无关；见 `report.md`） |

- **B 的槽号是算出来的，不是常量**：`DRAWOBJ.BIN` `instr#44` `add (local-int 11) fa (local-int 1)`
  ⇒ `0xFA + type`。`type=0x1F`(採集) ⇒ `0x119`；`type=0x20`(発掘) ⇒ `0x11A`。
  ```bash
  node .tmp/field-skill-hint/dis-one.mjs .tmp/field-skill-hint/DRAWOBJ.BIN --all
  # 读数：#44 0x0004e4 `add (local-int 11) fa (local-int 1)` · #58 0x00066c `draw-texture (local-int a) (local-int 11) 0 0 …`
  #       ; fa(hex)=250 ⇒ 250+0x1f=0x119 · 250+0x20=0x11a
  ```
- **B 的两张图是什么（已核，像素级）**：`MO031B` = 一丛**绿草**、`MO032B` = 一块**带叉的岩石**，
  且**底对齐**（下缘贴格）。
  ```bash
  node .tmp/field-skill-hint/so030-png.mjs --idx 20797 --out mo031b-4x.png --scale 4 --bg checker
  node .tmp/field-skill-hint/so030-png.mjs --idx 20798 --out mo032b-4x.png --scale 4 --bg checker
  # 读数：MO031B.AGF 64x82 / MO032B.AGF 70x88 ⇒ 两张 4× PNG（绿草 / 岩石）
  ```
  ⇒ **A 链的 `!`/`?` 与 B 链的草/岩不是同一张图、不是同一套判据**（`report-so030` 把两者混谈过，这里按槽号与绘制点分开）。
- **C 链与地图无关**（`report.md` / `report-2.md` 已核：两个立即数各只有 1 处，且在 `show-text` 之后、由剧情 flag `2f2f` 门控）。

### ② A 链的判据：一张**帧内 local** 格属性表（`65f2`）

- **形状（纠正前几轮的口径）**：`65f2` 是 **local**（帧内）数组，不是 global；**每格 1 个 dword**、**每行 `0x20` 格**：
  ```bash
  node .tmp/field-skill-hint/dis-one.mjs dist/install/FIELD.BIN --all | Select-Object -Skip 2863 -First 8
  # 读数：#2865 0x00fcf8 `mul (local-int 819f) 20 20`（= 0x20*0x20 = 1024 格）
  #       #2866 0x00fd14 `fill-zero (local-int 65f2) (local-int 819f)`（清 1024 个 dword）
  #       #2868 0x00fd44 `fill-zero (local-int 6df2) (local-int 819f)`（第二张同形表：槽号图）
  ```
  ⛔ **不是"每格 0x20 字节"**（`report-so030` 的原话）：`lookup-array-2d` 的语义是
  `base[op3*行宽 + op5]`（`apps/emulator/src/vm/ops.ts` 的 `opLookupArray2d`，EA 锚 `sub_42EFD0`），
  元素步长按目标操作数类型取 4 ⇒ `0x20` 是**行宽（每行 32 格）**，每格 1 个 dword。
  ```bash
  node .tmp/field-skill-hint/mask-hist.mjs      # 读数：`and (local-int) (local-ptr) <mask>` 155 处 / 41 种掩码
  ```
- **位表（谁置哪一位）**：`bit-set` 的第二操作数是**位号（十六进制）**，掩码 = `1 << 位号`；
  极性按 `jcc` 口径读（`jcc` 的 op2 是 `0xffffffff` ⇒ **cond 为 0 才跳**，即"条件成立才执行后面的 `bit-set`"）。
  ```bash
  node .tmp/field-skill-hint/bit-sets.mjs --from 2864 --to 3200
  ```
  | 掩码 | 置位条件（逐条读过 `jcc` 极性） | 置位点 |
  |---|---|---|
  | `0x10000` | `510d73[格上要素] != 0` | #2886 `0x00FF1C` |
  | `0x100` | `25c00[b222*64 + 行]` 的第 `列` 位**置位** | #2891 `0x00FFC0` |
  | `0x200` | 邻格 `510d73 != 0` **且** `25c00` 那位置位 | #2920 `0x0102D4` |
  | `0x400` | `57dff[行][列]` 的第 1 位**置位** | #2927 `0x010398` |
  | `0x200000` | 两支：①`510d73[格] == 3` 且 `25c00` 那一位为 0 且（`6f158[61c72] != 1` 或 `62d72[61c72][0x0B] == 0`）②`510deb[格][0] != 0` | #2940 `0x01051C` / #2945 `0x0105B0`（复位 #2956） |
  | `0x20000` | `(6202a[槽] & 0x40001) != 1` | #2991 `0x010A40` |
  | `0x40000` | `5b7e5` 的第 `6f158[槽]` 位**置位** | #2997 `0x010AF0` |
  | `0x2000000` | `62d72[61c72][0x0C] == 1` 且 `(格 & 0x100) == 0`；另一支同形（`6202a & 4` 那一路） | #3006 `0x010C14` / #3059 `0x0111D0` |
  | `0x20` | 邻格有要素 + `56d7e` 那格非零 + `57dff` 的第 `56d69` 位**置位** | #3051 `0x0110E8` / #3137 `0x011A20` |
  | `0x1000000` | `511468[54524c[61fc6[槽]]] > 0` 且 `62d72[61c72][该 id] == 0` | #3072 `0x011334` / #3185 `0x011F78` |
  | `0x100000` | ★ **每个** `50ff9f[槽] & 2` 的槽（"该槽要参与绘制"） | #3089 `0x0114D8` |
  | `0x10` | **取反**：**不**满足（`50ffd1[槽]` ∈ {2,3,4} 且 `4a5f0[b222][槽] == 56d69`） | #3100 `0x011624` |
  | `0x200000` | `510deb[格][0] != 0`（元素组） | #3154 `0x011BDC` |
  | **`0x400000`** | `50ffd1[槽] == 0x1F`（採集）**或**（`== 0x20`（発掘）**且** `62d72[61c72][0x33] == 1`，`0x33`=51=**発掘**技能） | **#3165 `0x011D28`** |
  | `0x1000000` | 同 #3072 的第二处（另一条类型分支） | #3185 `0x011F78` |

  上面 `#3165` 那一条就是 `?` 的"类型 + 技能"半边（`report-so030` 的 `report` 口径一致：`50ffd1` = 32 槽类型表、
  `62d72[61c72][…]` = 逐地图 × 技能表）。

### ③ A 链的绘制段：`instr#9852..10045`（每帧，按 `78fa` 个可见格循环）

```
9855 0x03A708  循环头  for (2fcf < 78fa)                ; 78fa = 本帧"要画的格数"（由 32 槽点位反推，见 ② 的 0x100000 位）
9858 0x03A75C  格坐标  (71F2[i], 7576[i]) → 屏幕 = 格 * 64
9864 0x03A7F4  jcc (global-int b22b) → 0x3A9F8          ; ★ b22b = 0 ⇒ 走"b224 分档"那半（见下）
      0x03A810  格 & 0x80      → 画槽 a5 的 112×64 + 槽 49 的 (0x237,0x459,0x3C,0x30)
      0x03A91C  格 & 0x20000   → 槽 49 的 (0x69C,0x176,0x64,0x19)
9879 0x03A9F8  and (b224) 100 → 五档（0x100/0x200/0x400/0x800/0x1000），每档: 环槽 a2..a5 + 槽 49 标签
9941 0x03B250  ★ 从这一条起**没有 b224 门**，逐位判：
      0x03B27C  格 & 8        → 槽 a4 + 49 的 (0x702,0x15B,0x64,0x19)
      0x03B3C8  格 & 4        → 槽 a3 + 49 的 (0x636,0x140,0x64,0x19)
      0x03B508  格 & 1        → 槽 a5 + 49 的 (0x636,0x176,0x64,0x19)
      0x03B9E0  格 & 0x70102 == 0x70102 → 槽 a3 + 49 的 (0x636,0x191,0x64,0x19)
      0x03BB48  格 & 2        → 槽 a2 + 49 的 (0x636,0x1AC,0x64,0x19)
      0x03BC88  格 & 0x2000000 → 槽 49 源矩形 (0x3E2,0x232,0x35,0x35)      ; ★ 第三块 = (994,562,53,53)
      0x03BD90  格 & 0x300 == 0        → 槽 49 源矩形 (0x4A4,0x643,0x1C,0x37)   ; ★「!」=(1188,1603,28,55)
      0x03BEB4  格 & 0x400100 == 0x400100 → 槽 49 源矩形 (0x4A4,0x67C,0x1C,0x37) ; ★「?」=(1188,1660,28,55)
      0x03BFCC  格 & 0x1000102 == 0x1000102 → 继续后面的块
```
- 复现（源矩形按**数值**扫全语料，各 1 处）：
  ```bash
  node .tmp/field-skill-hint/draw-scan.mjs --src 4a4 643 1c 37 --all
  # 读数：# 扫 492 个脚本 · draw-texture 共 6493 处 ⇒ 命中 1 处
  #       FIELD.BIN instr#10029 0x03be38 slot=49 src=(4a4,643,1c,37)
  node .tmp/field-skill-hint/draw-scan.mjs --src 4a4 67c 1c 37 --all
  # 读数：命中 1 处 FIELD.BIN instr#10038 0x03bf5c slot=49 src=(4a4,67c,1c,37)
  ```
- **`!` 与 `?` 的分工（按掩码，不按剧情）**：
  - `!` = `(格 & 0x300) == 0` ⇒ **这一格既没有 `0x100`（`25c00` 地形位）也没有 `0x200`（邻格链）**。
  - `?` = `(格 & 0x400100) == 0x400100` ⇒ **有"类型+技能"位（`0x400000`）**并且**有 `0x100`**。
  - ⇒ 两者**互斥**（`!` 要求 `0x100` 为 0，`?` 要求它为 1）；`?` 才是挂在"採集/発掘 + 技能"上的那一个。
  - ⛔ 这两块**不是**"采集点 / 发掘点"这一对：它们是 A 链的**格标记**（同一张图集 `SO030.AGF` 的槽 `0x49`），
    判据是帧内 `65f2` 的每格 dword；**采集/发掘点的本体是 B 链精灵**（`MO031B.AGF` / `MO032B.AGF`，
    见 ④）。⇒ `field-point-visible` 改的是 B 链的派生渲染位，`?` / `!` 照旧按格属性出现在 `on=0` 与 `on=1` 的帧上。

### ④ B 链（真正的"点位精灵"）：`DRAWOBJ.BIN` 全文 119 条

```bash
node .tmp/field-skill-hint/dis-one.mjs .tmp/field-skill-hint/DRAWOBJ.BIN --all   # 该文件从 ALF idx 20761 取出
```
- 槽循环 `2 = 1..31`（`instr#1/#4`）；每槽：
  - `instr#7` `and 50ff9f[槽] 1` → `jcc`（**cond==0 才跳**）⇒ **bit0=1 = 画；bit0=0 才整段跳过**；
  - `instr#9/#11/#14` 取 `类型 = 50ffd1[槽]`、列 `14d73c[槽]`、行 `14d76e[槽]`；
  - `instr#15` `511080[类型] == 1` ⇒ `缩放 = 4a5f0[b222][槽] * 510fb8[类型]`（否则 0）；
  - `instr#23` `add 9 = 0x3A98 + 槽`：**动效句柄**（每槽一个；静态句柄是 `0x3ACA + 槽`，`instr#43`/`#58`）；
  - `instr#25` 先 `call 0x89C` 画**动效**（`510439[类型]`，7 帧），`instr#26` `and 50ff9f[槽] 4` **为 0**
    → `instr#29` `set-draw-color-alpha(0x3A98+槽, 0)` ⇒ **bit2=0 ⇒ 动效全透明**；bit2=1 ⇒ 不动（动效可见）。
    ⛔ bit2 不是"静态的 alpha"：静态走 `0x3ACA+槽`，全脚本从不设它的 alpha（恒不透明）。
  - `instr#30..#39` **静态门 ①**：`511080[类型]==3` **且** `eb5c1[b222][槽]==1` **且** `(50ff9f&6)==2`
    ⇒ 成立才继续（不成立 ⇒ 跳到循环尾，**不画静态**）。`(…&6)==2` = **bit1=1 且 bit2=0**。
  - `instr#40/#41` **静态门 ②**：`check-bit` 读 `25c00[b222][行]` 的第 `列` 位 → `jcc`
    ⇒ **位=1 ⇒ 画静态（`instr#44..#58` 的 `0xFA+类型` = `MO031B/MO032B`）；位=0 ⇒ 跳过**。
    ★ 2026-11-26 订正：本节早先把这一条读反了（写成"置位⇒跳过"）。`jcc` 极性 = cond==0 才跳、
    `check-bit` = 位值 0/1（`apps/emulator/src/vm/ops.ts` 的两条 handler），`DRAWOBJ.BIN.txt:56-58` 的
    `jcc` 目标是循环尾 ⇒ 位=0 才跳。同一个位在 `DRAWOBJ.BIN.txt:148-156`（动效那段）是
    **位=0 ⇒ alpha 0、位=1 ⇒ alpha `5110e4[类型]`** ⇒ `25c00` 是"这一格有地块要素"的**共同门**
    （`SETLAND.BIN.txt:47-49` 置 / `DELLAND` 清 / 进图由 `510d73` 重建），**不是**静态/动效的选择位。
    ⇒ **清 `25c00` 的位会让静态与动效同时不画（点直接消失）**；两态的选择位是 `50ff9f` 的 bit2。
  - `instr#44` `槽 = 0xFA + 类型`；`instr#47..#57` 目标 = `列*64+32 - w/2` / `行*64+32 - h/2 - 14`（**底对齐**）；
  - `instr#58` `draw-texture (0x3ACA+槽) (0xFA+类型) 0 0 w h dstx dsty`。
- ⇒ **B 的可见性判据 = `50ff9f[槽]` 的 bit0（画）/ bit2（动效 alpha = 两态选择位）+ 静态门 ①/②**；`50ff9f` 是 VM 在**每次进图**重建的派生表：
  `FIELD.BIN` #187 `0x000FA0` `fill-zero (global-int 50ff9f) 32` · `SETOBJ.BIN` #2 `0x00005C` 再清一次 ·
  然后 `SETOBJ` #34 `bit-set …0`（= 画）/ #78 / #84 `bit-set …2`（= 动效 alpha 正常）/ #86 `bit-set …1` 逐槽写。
  ★ **2026-11-25 订正**：本节早先把 `jcc` 极性读反了（写成"置位=不画 / 置位=alpha 0"）。以
  `apps/emulator/src/vm/ops.ts` 的 `jcc`（cond 为 0 才跳）+ 这两支的极性为准：**bit0=1 画、bit2=1 动效可见**。
  **bit1 不清**：A 链格表（`0x100000` 位）与 `FIELD` `label_00030E54` 的按 `flags & 2` 抽取战利品都消费它。
  ```bash
  node .tmp/field-skill-hint/dis-one.mjs .tmp/field-skill-hint/SETOBJ.BIN --all | Select-Object -First 8
  # 读数：#1 memcpy (50ff9f)… · #2 fill-zero (50ff9f) 32 · #3 memcpy (14d7a0) (50ffd1) 32
  ```
- `SETOBJ` 的输入（**这些才是"该不该显示"的上游**）：`14d8cc[槽]` 的第 `a9bb` 位（`a9bb` 取值 0/1/2：
  `GAMESTART`/`$1$SC1820`/`DEBUG*` 写）、`14d8fe[槽][0..6]` / `14da5c[槽][0..4]` 两组 id × `13d7[id-1] == 1`、
  `3e2a0[b222][槽]` 与 `5111ac[类型]` 的大小、`511080[类型]`。
  ⛔ 它们的**语义**（尤其 `13d7` 那 10000 项是什么）没有读数 ⇒ `## 待核` 5；本节只说"B 的可见性最终由这些算出来"。

### ⑤ 纹理绑定（`SETMAPTEX.BIN`，ALF 20776，115 条）与本模型相关的四个槽

```bash
node .tmp/field-skill-hint/dis-one.mjs .tmp/field-skill-hint/SETMAPTEX.BIN --all
```
| instr# | 偏移 | 指令 | 含义 |
|---|---|---|---|
| 3 | `0x000090` | `set-texture 5132 49 (local-int 7)` | `SO030.AGF`（ALF 20786）→ 槽 **`0x49`**（A 链的图集） |
| 29..43 | — | 循环：`14d728[i]` / `510edb[i]` → 槽 `0x52+i`（i<20） | 逐槽绑定 |
| 46 | `0x0004F4` | `fill-zero (global-int 510439) 64` | 类型→槽 登记表清零 |
| 52..63 | — | 循环 32 槽：`14d7a0[i]` 当类型 → `call 0x9EC` | 类型→图（`510ef0` / `54524c`）绑定到 `0x66+` |
| **78** | **`0x0007DC`** | `set-texture 513D 119 (local-int 7)` | `MO031B.AGF`（ALF 20797）→ 槽 **`0x119`** = `0xFA+0x1F` |
| **80** | **`0x000814`** | `set-texture 513E 11A (local-int 7)` | `MO032B.AGF`（ALF 20798）→ 槽 **`0x11A`** = `0xFA+0x20` |
- 复现读数：`node .tmp/field-skill-hint/img-usage.mjs --prefix MO031` ⇒ 全语料 **1 个 `set-texture` 站点**、**0 个 draw 站点**。
  ⇒ 这两个槽**不是**任何脚本用立即数 draw 的（`rg "draw-texture .* (119|11a) "` 在 492 份清单里 0 命中）；
  **唯一消费者是 `DRAWOBJ` 的"槽 = `0xFA+类型`"**（④）——那条指令的槽操作数是变量，所以按立即数扫不到。

### ⑥ 否定（这几条能省掉重复取证）

1. **"第三个 `32×64` 特效"不存在**：全 AGF 目录里 `32×64` 只有 `AE202AA`/`AE202AB` 两条（`small-agf.mjs --wh 32x64`）。
2. **`SO030` 的两块不是 `25×50`**：引擎用的源矩形是 **`(1188,1603,28,55)`** 与 **`(1188,1660,28,55)`**（宽高 `0x1C=28` / `0x37=55`）；
   紧 bbox 分别 `26×53` / `28×55`（`report-so030` 的像素统计）。
3. **`MO031B/32B` 不在 `SO030` 里**：它们是各自独立的 AGF（`64×82` / `70×88`），`SO030` 是 2000×2000 的图集。
4. **引擎里没有这条链**（见本节开头那 11 个常量的 0 命中）。
5. **`AE202A*` 与地块无关**（C 链）。

### ⑦ 静态 / 动效两态 + `field-point-visible` 的 `static`（2026-11-26）

| | 静态（无工具图标） | 动效（锄头 / 镐 + 7 帧） |
|---|---|---|
| 採集 `0x1F` | **`MO031B`** ALF 20797，槽 `0x119` | **`MO031A`** ALF 19848，运行期槽 `510439[0x1F]` |
| 発掘 `0x20` | **`MO032B`** ALF 20798，槽 `0x11A` | **`MO032A`** ALF 19850，运行期槽 `510439[0x20]` |

- **选择位 = `50ff9f[槽]` 的 bit2**（④ 的 `instr#26`）：bit2=1 ⇒ 动效可见、静态门 ① 的 `(flags&6)==2` 不成立
  ⇒ 静态**不画**；bit2=0 ⇒ 动效 `alpha=0` ⇒ 静态门 ① 成立时画静态。两者互斥。
- **静态还要求**（缺一不画）：`511080[类型]==3`（`OBINIT` 把 31/32 写成 3）· `eb5c1[b222][槽]==1`
  （**这个点位触发过**：`FIELD` instr#6242-6245 在"打开交互菜单"那一拍写 `bit-set 50ff9f[槽] 2` + `eb5c1=1`）·
  `50ff9f` bit1=1（还没用完，`SETOBJ` 只在未耗尽支置它）· `25c00[b222][行]` 的第 `列` 位 = 1（该格有地块要素）。
- ⇒ **进图默认态 = 静态**（`SETOBJ` 对 `511080==3` 从不置 bit2）：`SETOBJ.BIN.txt:111-130`。
  触发点位 ⇒ 动效（`FIELD` #6242-6245）；交互结束 / 取消 ⇒ `bit-reset 50ff9f[槽] 2`（`FIELD` #6237 / #6383）
  ⇒ 回到静态。**刷新过的点**（`3e2a0[b222][槽] >= 5111ac[类型]`，採集/発掘 = 1）⇒ bit1=0 ⇒ 两态都不画（点消失）。
- `field-point-visible` 的 `static`（缺省 **1**）：
  - 两条 setmap 钩子仍 `flags |= 0x5`（bit0 画 + bit2 动效）；
  - **第三条钩子** `afterPosition("FIELD.BIN", FIELD_STATIC_LO, FIELD_STATIC_HI, {mode:"reenter"})`
    = `[0x00253C,0x002550)`（逐图派生表 `25c00`/`57dff`/`4a5f0` 重建之后的汇合点，`FIELD` #402）；
    在这里按**上面那套引擎静态门**（再叠 `14d8cc` 当层启用）决定 `flags` 的 bit2：
    门成立 ⇒ **清 bit2**（静态）；门不成立 ⇒ 保留 bit2（= 今天的动效，⛔ 不让点消失）。
    `25c00` / `14d73c` / `14d76e` / `14d8cc` / `eb5c1` / `511080` **只读**，写集只有 `50ff9f+槽`。
  - `static=0` ⇒ 一个 `25c00` / `14d7xx` / `14d8cc` / `eb5c1` 读都不做（日志一行 `field-points: static=0`），
    行为 = 2026-11-26 之前（只 `|=0x5` ⇒ 看到动效）。
- **复跑（窗口，人读档）**：
  ```bash
  cd apps/emulator-lift
  printf '{"version":1,"domains":{"field.point-visible":{"on":1,"diag":1}}}\n' > ../../.tmp/field-static/appdata/plugin-config.json
  ./out/emulator-lift-aot.exe --game-dir ../../links/install --appdata ../../.tmp/field-static/appdata \
    --present window --overlay --skip-logo --audio off --time-limit 300 --log-level info --plugin field-point-visible
  ```
  期望：进图后点位格是**植株 / 岩石本体**（无工具图标）+ 日志逐槽
  `field-points: static slot=… type=… row=… col=… cellBit=1 gate=1 enabled=1 class=3 eb5c1=1 0x7 → 0x3（静态）`
  与汇总 `field-points: static cleared=N/M`；走到点位交互 ⇒ 变成 7 帧动效（日志侧看 `50ff9f` 读数或画面）。
  若日志出现 `… gate=0 … eb5c1=0（保留动效：already-dynamic）` ⇒ 那个点从来没有触发过，
  引擎自己也不会画静态（此时**只有**写 `eb5c1` 才能让它变静态，而那是持久真值 ⇒ 需要用户裁决）。
- ⚠ **已知边界**：`0x001B10..0x002530` 的重建块只在 `b22a == 0` 时执行（`FIELD` #303/#304
  `eq` + `jcc 0x4CF0`）⇒ **地图续接**（`b22a != 0`）的进图不经过第三条钩子 ⇒ 那一次保持动效。

## 待核

1. **进图装载链**：`FIELD.BIN`(21095) 由谁 `call-script` / 由 `SETFIELD.BIN`(20745) 还是引擎的换图路径拉起来 —— 没有读数。
2. **`!`（`0x300 == 0` 那支）到底是什么**：它的两个位都来自 `25c00[b222]` 那张**每图一块 32×32 的格位图**
   （`25c00[b222*64 + 行]` 的第 `列` 位；行宽 64 dword、格网 32×32 见 ⑦ 的取证）；
   那一格位的语义没有定位（脚本侧 `--write` 只有 7 处写 `25c00`，都不是"点位发现"）。
3. **FIELD 的点击窗口**：没有任何读数（`## 输入点`）。要取证须按 `FELLOW.md` / `SAVE.md` 的形状做“门 + 门后 click”的 A/B。
   ⚠ 2026-11-24 记的“六次都停在选行后的确认框”**已归因，不是 FIELD 的门**：那几次把 `SAVE*.DAT`
   复制到了 `<--appdata>` **根目录**（引擎实际读 `<--appdata>/Local/Eushully/<标题>/SAVE/`）⇒ 读档列表为空
   ⇒ 点行不弹确认框（`load-save: gate timeout (load:sbunki)`）。2026-11-25 换成嵌套落点后，
   同一份存档（当时是 `SAVE10`）的 headless 与 window 各一次都走完 `page 1 → row → SBUNKI → FIELD.BIN`
   （用 ② 的读档链；★ 那条链当时被 `field-point-visible` 自己带着，现在只归 `scenario-save10-field`）。
   ⇒ 剩下的未知仍只是“FIELD 上哪一段吃 click”，与读档链无关。
4. **`bit 0x800` / `0x20000` / `0x1000000` / `0x40000`（`5b7e5`）/ `0x10`（`4a5f0` vs `56d69`）的语义**：
   置位条件已核（②表），**含义**没定位。`5b7e5` 只有 5 位（`FIELD.BIN` #2842 处 `lt … 5` 的循环 + #2827 清）⇒
   更像"当前这一组 5 个候选中同一 `14d722` 值的掩码"，不是点位状态。
5. **B 链上游的语义**：`13d7`（10000 项，`FIELD.BIN` #9129/#9304 用 `memcpy` 与 `a8b89` 互拷）、
   `14d8cc` 的第 `a9bb` 位、`3e2a0[b222][槽]`、`14d8fe`/`14da5c` 两组 id —— 谁写、写什么、是否落存档，全部未取证。
6. **`i086 5119`**（`FIELD.BIN` 清单行 3770）：只知它把 `0x5119`(=20761=`DRAWOBJ.BIN`) 当参数。
   同一形态在 `ALLMAP`/`CGVIEWER`/`REIGN` 也有（共 4 处）⇒ 像"登记绘制脚本"，但**语义未取证**。
7. **`MO031A`/`MO032A`**（ALF 19848/19850）与 B 用到的 `B` 版是什么关系（分辨率档？）——未取证。
8. **`field-point-visible` 在真机（人读档进 FIELD）上的 `on=1` 施加读数**：目前只有
   ①（`on=0` 常驻基线）与**纯档** `plugin-runtime` 的 `plugin/field-switch-apply`（假 guest 上
   "类型命中槽 `|0x5`（`0x3→0x7`）/ 不命中不动 / 已经可见不再写 / 换图实例号变 ⇒ 另一条 setmap 钩子
   重新施加 / `static` 的静态门成立 ⇒ `0x7→0x3`、门不成立（`eb5c1=0`）⇒ 保留 `0x7` / `static=0` ⇒ 不清"）。
   真机上还缺：首帧之后的 `field-points: setmap forced=N/M` 与 `field-points: static cleared=N/M` 读数、
   以及画面里出现 `MO031B` 绿草 / `MO032B` 岩石（**无工具图标**）。
   要取证须**人**（或带窗 + 手点）读一次档 —— ⛔ 不要为了这条把 `--input-plan` / 控制层塞进 `--plugin`
   （三者互斥）。复跑形状见 `.tmp/field-setmap/design.md` 的「验收」（N 帧按实际 FIELD 就绪再定，
   A/B 必须同一 N），`static` 的复跑命令见 ⑦。
9. **setmap 两个钩子区间的 run 读数**：`[0x18280,0x18284)` / `[0x35168,0x3517C)` 的位置来自**静态**
   （`FIELD.BIN` 的 `call-script 511a` 返回点，见 `design.md` 的 `dis-one.mjs` 读数）；还没有一次
   `fired=` / 控制层 `pause` 采样证明运行期真的落到这里（`[0x35168,0x3517C)` 尤其只在一类换图路径上）。
   要进 `## 位置区间` 表须补一条 run 读数。
10. **触发后 `50ff9f` bit2 是否真的被置回（真机读数）**：静态门 / 两态模型的**写点**目前只有静态证据
    （`FIELD` instr#6242-6245 在打开交互菜单那一拍 `bit-set … 2` + `eb5c1=1`；#6237/#6383 清）。缺一条
    运行期读数：走到点位、触发、再走开之后，`50ff9f+槽` 的 bit2 是否 0→1→0，以及 `eb5c1[b222][槽]`
    是否 0→1。取证形状（窗口 + 人）：`emu-ctl global-get --kind int --index <0x50ff9f+槽>` /
    `--index <0xeb5c1 + b222*32 + 槽>` 在"进图 / 站到点上 / 交互完 / 走开"四个时刻各读一次。
    ⚠ 这条是 `static` 的**唯一未证点**：若 bit2 在"走到点上"时**不**被置回，那 `static` 只是把动效关掉
    （不会自己回来），需要退回"只在进图那一刻清、由下一帧的 guest 写回"的 D 方案。
11. **`25c00` 的触发路径**：`SETLAND`（`0x2FD8`）/ `DELLAND` 是仅有的两个运行期写点，调用者是
    `STAGEREADY` / `STAGEROUND` / 剧情（`rg "call-script 2fd8|2fac"` 30+ 处）—— 没有一条是"走到采集点"。
    待核 = 采集点所在格的 `25c00` 位是否在任何触发路径上变化（若变，静态门 ② 会跟着变）。
