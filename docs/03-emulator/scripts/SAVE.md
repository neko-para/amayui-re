# SAVE.BIN

## 身份

- 清单：`dist/views/data/SAVE.BIN.txt`（2418 行；头 `signature = SYS4450`）。该名字在 `dist/views/data/` 里**只有一份**。
- `script` 身份名怎么来的：运行期名字匹配（`apps/emulator-lift/src/runtime/recipe.cpp` 的 `scriptNameMatches`）。
- ★ **同一个脚本承载两屏**：**保存屏**（从 FELLOW 进来）与**加载存档界面**（从 TITLE 进来）。
  区分量是 `and (…)(global-int a9cb) …`（清单行 898 `& 2` · 951 `& 1` · 1165 · 1386）——
  `await position` 读的是 `script + pos`，读不到那个 global ⇒ **位置谓词区分不了这两屏**（下面 `## 位置区间` 有读数）。
- 被装载的机制（两条**都有读数**）：
  - **保存屏**：FELLOW 回调体门 + `click 100 140`（`FELLOW.md` ①②）。实测路上会穿过场脚本，**不是**直接点名 SAVE：
    `.tmp/p2/trace-s2.log` 的采样序列是 `SN0000 → SETTOOLLIST → FELLOW → SAVE`；同一次 run 的尾巴另见
    `SAVE → SBUNKIMOVE → SBUNKI`（那是**选槽之后**，见 `## 输入点`）。
  - **加载存档界面**：TITLE 回调体门 + `click 1070 480`（本文 ③）。
- 结构（本文区间都靠它，地址=`label_%08x`）：
  - **主轮询体** `0xFF4`（行 221–233）：`get-input-type` · `sleep 1`@`0x10EC` · `jmp label_00000ff4`@`0x10F8`。
  - **回调登记**：`mouse-callback 10 label_00001104`（行 219）⇒ **回调体** `0x1104`（行 235–238）：
    `call label_00001120`@`0x1104` · `call label_0000b39c`@`0x1108` · `ret`@`0x110C`。
  - **读输入子过程** `0x1120`（行 240–249，唯一调用者 = 回调体）：`add`@`0x1120` · `i2fc`@`0x113C` ·
    `read-mouse-pos`@`0x11A4`（**label**）· `read-mouse-button`@`0x11B8`；之后 `0x11C4` 起是按钮边沿/位置变化块。
  - **命中判定**：`i12e`@`0x131C`（行 270）。
  - **确认框**：`0x37D4`（`"ロードしますか？"`，行 901）· `0x3BB8`（`"セーブしますか？"`，行 954）——
    两处都在设完 `(global-string f17)` 之后 **`call-script 36`** ⇒ 确认框是**另一个脚本**（本轮实测到 `SBUNKI.BIN`）。

## 位置区间

读数口径：只有 `{script, pos}` 与配方的 `fired/pending/exit` 当判据。`pos` 与清单 `label_%08x` 同一坐标系。
执行源 `apps/emulator-lift/out/emulator-lift-aot`，`sha256 = 27fca94a09422ece80d9db64b9962911bbb2d65cea7d1ae3f6dc8893e13f35c9`（2026-11-16，macos-arm64）；
`--appdata` = 从 `corpus/fixtures/` 组装的那份（`docs/03-emulator/save-format.md` §外部组装）。
原始日志：`.tmp/p2/run-s2.log` · `run-s3.log` · `run-l1.log` · `trace-s2.log` · `trace-s3.log` · `trace-l1.log`。

| 区间 `[lo,hi)` | 是什么 | 复现命令 | 读数日期 |
|---|---|---|---|
| `[0x1104,0x11C4)` | **回调体**（入口 → `read-mouse-button` 末尾；`read-mouse-pos`@`0x11A4` + `read-mouse-button`@`0x11B8` 都在里面）。★ **保存屏与加载屏共用这一条** | 见下 ①（保存屏，不点）/ ②（保存屏，`click 300 100`）/ ③（加载屏） | 2026-11-16：门 **3/3** 命中 —— 保存屏 2/2（①②）、加载屏 1/1（③）；②里门后的 `click 300 100` 也被消费（`fired=5`，脚本随即离开 SAVE，见 `## 输入点`） |

- **`[0xFF4,0x1104)`（主轮询体）只在采样里出现过，没当过门** ⇒ 按本目录 `README.md` 的规矩进 `## 待核` 4。
- **稳态采样读数（不是门）**：`[0x1104,0x11C4)` 这一带**采样打不到**（三次 run 共 175 次采样，其中 `SAVE.BIN` 123 次，
  **0 次**落在 `[0x1104,0x11C4)`），
  稳态落点是两处：轮询体 `0xFF4 / 0x1008 / 0x100C / 0x1028 / 0x1044 / 0x1060 / 0x107C / 0x1098 / 0x10B4 / 0x10D0 /
  0x10EC / 0x10F8`（`trace-s3.log` 里 22/28 次 `SAVE.BIN` 采样；另有 5 次 `0xB39C`、1 次 `0xA4` 的过渡点）
  与 `0xB39C`（`call label_0000b39c`@`0x1108` 的**被调方体内**；`trace-s1.log` 38/40、`trace-l1.log` 25/55
  与轮询体交替出现）。
  复核命令：`node .tmp/p2/count-range.mjs .tmp/p2/trace-l1.log 1104 11c4`（打印样本数 / `SAVE.BIN` 样本数 / 区间内计数 + 直方图）。
  ⇒ 又一次印证"**采样命中率既不能立门也不能否门**"（`_recipe-map.md` 待核 7）。

```bash
cd /Users/nekosu/Documents/Projects/amayui-re
APP=.tmp/p2/appdata2     # 见 save-format.md §外部组装（本轮另有一份去掉 SAVE.DAT/SAVE.BAK 的 appdata4）

# ---- 共用前缀（保存屏一侧；= sn0000-save.plan L7–L18 的已取证前缀 + FELLOW 的输入读段门，逐字照抄，未改任何配方）----
cat > /tmp/save-prefix.plan <<'PLAN'
await position TITLE 0x0460 0x0520 timeout 30000
click 1186 373
await position GAMESTART 0x0664 0x0724 timeout 30000
click 830 592
await position SN0000 0x46B8 0x470C timeout 35000
click 500 500 right
await position FELLOW 0x1684 0x16A4 timeout 60000
click 100 140
PLAN

# ---- ① 保存屏当门、不给点击（证明 SAVE 一直停在这一屏）----
cat > /tmp/save-tail.plan <<'PLAN'
await position SAVE 0x1104 0x11C4 timeout 30000
profile_begin noselect
wait 600000
PLAN
cat /tmp/save-prefix.plan /tmp/save-tail.plan > /tmp/save-gate.plan
node .tmp/p2/runsample.mjs --tag s3 --plan /tmp/save-gate.plan --n 75 --resume 400
# 读数：input-plan: steps=11 fired=4 pending=1 · progress: applied=10（pending=1 只是末尾那条 wait 没轮到）
#       ⇒ **门命中**。收尾 exit 5 是"控制层 quit 截断"（stop: control: quit），不是门失败。
#       采样 75/75 全是 SAVE.BIN（pos 全在 0xFF4..0x10F8 的轮询体里）。

# ---- ② 保存屏当门 + click 300 100（选第 000 行）----
#   把 ① 的 tail 第二行换成 click 300 100，其余不动：
#   读数：input-plan: steps=11 **fired=5** pending=1（门后的 click 施加了）· stop: control: quit
#   控制层采样：之后脚本变成 SBUNKIMOVE.BIN → SBUNKI.BIN（trace-s2.log）

# ---- ③ 加载屏：TITLE → click 1070 480 → **同一道门** ----
cat > /tmp/load.plan <<'PLAN'
await position TITLE 0x0460 0x0520 timeout 30000
click 1070 480
await position SAVE 0x1104 0x11C4 timeout 30000
wait 600000
PLAN
node .tmp/p2/runsample.mjs --tag l1 --plan /tmp/load.plan --n 60 --resume 400
# 读数：input-plan: steps=4 fired=1 pending=1 · progress: applied=3（pending=1 只是末尾那条 wait）
#       ⇒ 加载屏也是 SAVE.BIN 且 pos 落在 [0x1104,0x11C4)；采样 60 次全是 SAVE.BIN
#       （轮询体与 0xB39C 交替，见 trace-l1.log）
```

★ **③-6 的裁决（读数钉死）**：加载存档界面 = **`SAVE.BIN`**，且**与保存屏同一个区间 `[0x1104,0x11C4)`**
（保存屏 2/2、加载屏 1/1 命中同一道门）。⇒ **位置谓词区分不了"保存屏 / 加载屏"**；
要区分只能靠 `(global-int a9cb)`（清单行 898 `& 2` vs 951 `& 1`）⇒ 见 `REQ-01M7J97QY66KMP5SBPCTR0S3TP`。

## 输入点

- **吃 click**：回调体 `[0x1104,0x11C4)`（与 FELLOW 同形：轮询体点不动、回调体吃得进 —— `FELLOW.md` 的 E0/E1 是
  FELLOW 上的同一条读法，SAVE 侧本轮只证到"门命中 + 门后的 click 被消费"，没有做"轮询体当门"的负对照）。
- **`click 300 100`（选第 000 行槽位）**：
  - 读数（② + 控制层采样）：`SAVE.BIN` → `SBUNKIMOVE.BIN` → `SBUNKI.BIN`（≈0.5 s 内）。
  - 负对照（①，同一条路但不点）：75 次采样**一直在 `SAVE.BIN`**（同一个稳态点集合）。
  ⇒ **"选槽"不是同一屏内的 pos 变化，而是换脚本**；`_recipe-map.md` ③-7 里"确认框的块在 SAVE 内部
  （`0x37D4` / `0x3BB8`）"这条**假设被读数否掉**：那两个块只负责 `set-string (global-string f17) "…しますか？"`
  然后 `call-script 36`，**真正的框是另一个脚本**（`SBUNKI.BIN`，691 行、无日文字符串、`call-script 5255`，
  形态像通用确认框；"它就是那个框"仍未裁决，见 `## 待核` 3）。
- **`click 650 320`（确认保存）**：**未取证**（本轮没跑；配方里那一步仍无读数支撑）。
- 加载屏上的点击（选一个非空槽 → 是否也是 `SBUNKI` 那条对话框）：**未取证**。

### 加载屏的槽位行（y 坐标，实测）

| 行 | 行中心 y | 内容 |
|---|---|---|
| LOAD 000 / 001 / 002 | ≈ 91 / 150 / 209 | 旧存档（01/02 19:54 · 01/02 20:03 · 01/03 21:22） |
| **LOAD 003** | **≈ 269** | **`NEW!` 01/06 21:23 = `SAVE03.DAT`（第 4 槽）** |
| LOAD 004..009 | ≈ 328 起 | `保存情報なし` |

- 读数（2026-11-18 / win32 / dev `@847ebb8`）：`emu-ctl screenshot`，frame=523，`script=SAVE.BIN pos=0xB39C`。
- **行距 ≈ 59.5 px，⛔ 不是 40**：`click 300 220` 落在 002 与 003 之间、偏 002（= `SAVE02`）⇒ 选第 4 槽要用 **`click 300 269`**。
- 复现 / 下一步：`apps/emulator-lift/scripts/recipes/sc0000-wipe.plan`（加载第 4 槽 + 1 击到条带转场）；
  那段的剖面（含 timed 读数与未定项）见 `docs/03-emulator/wipe-profile.md`。
- ⚠ 行距是**这份 appdata 的 10 行布局**；换档位数 / 分辨率要重测。

## 待核

1. **页基初值 ⇒ 第一屏是 110..119 还是 000..009**：本轮只在**画面**上看到过（用 fixtures 的 `SAVE.DAT`、或只留
   `SAVE.BAK` 时第一屏是 `110..119`（10 行都"保存情報なし"）；把 `SAVE.DAT` + `SAVE.BAK` + `RT.DAT` 全删掉后
   第一屏是 `000..009`（10 行都有项））。⚠ 那两次读数来自 **`emu-ctl screenshot`**，该路径会写出**全黑 BMP**
   （缺陷 `REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）⇒ **本条不作判据、已撤回**，只留现象与命令：
   `--appdata .tmp/p2/appdata2`（有 `SAVE.DAT`）vs `.tmp/p2/appdata4`（无 `SAVE.DAT`/`SAVE.BAK`）。
   要钉死需要一条非画面读数（或等 `screenshot` 修好）。⛔ 不要把它归因到"存档编码/头不合法"——那是未裁决的另一回事。
2. **`click 650 320` 之后有没有真的写盘**：没有读数（可从 `--appdata` 目录里 `SAVE00.DAT/.STH` 的 mtime 变化判，
   本轮没跑）。
3. **`SBUNKI.BIN` 就是确认框**：只有"点槽之后脚本变成它"这一条读数；它与 `call-script 36` /
   `(global-string f17)` 怎么对应**未裁决**。
4. **主轮询体 `[0xFF4,0x1104)` 没当过门**：只有采样（75/75 落在里面）⇒ 不知道它吃不吃 click（按 FELLOW 的同形结论
   应当**不**吃，但 ⛔ 这是推断不是读数）。
5. **两屏不可区分**：位置谓词做不到，只能靠 `global a9cb` ⇒ `REQ-01M7J97QY66KMP5SBPCTR0S3TP`。
   `a9cb` 的**哪一位**对应哪一屏只有静态阅读（`0x37D4` 由 `& 2` 守 / `0x3BB8` 由 `& 1` 守），**没有读数**。
6. **加载屏点**空槽**（第 110 行那种）有没有反应**：未取证。
7. **`script_sha`**：`await position` 没有字节绑定参数（同 `TITLE.md` 待核 4）。
