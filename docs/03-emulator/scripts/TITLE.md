# TITLE.BIN

## 身份

- 清单：`dist/views/data/TITLE.BIN.txt`（839 行；头 `signature = SYS4450`、`local_vars = { 412 1 3 3 1 2 }`，行 1–3）。
  该名字在 `dist/views/data/` 里**只有一份**（没有 `$1$` / `$2$` / `$3$` 副本）。
- `script` 身份名怎么来的：`await script` 是**运行期**名字匹配（ASCII 大小写不敏感、两侧剥 `.bin`），
  判定在 `apps/emulator-lift/src/runtime/recipe.cpp` 的 `scriptNameMatches`；名字表反解不属于本文。
- 被装载的机制：配方里用 `await script TITLE` 当第一个门（`scripts/recipes/*.plan` 的每个文件）。
  引擎侧"谁装载 TITLE"仓里没有记录 ⇒ **待核**（本文不写剧情）。
- 结构（本文三条都靠它）：主**轮询体** `0x039C`（行 55–64：`get-input-type` + 3×`eq/jcc` + `sleep 1` + `jmp 0x39C`）；
  鼠标回调登记 `mouse-callback 10 label_00000460`（行 53 与行 451 各一次）→ **回调体** `0x0460`（行 66–69：
  `call 0x3844` / `call 0x47C` / `ret`）→ **读输入段** `0x047C`（行 71–80：`i2fc`@`0x498`、`read-mouse-pos`@`0x500`、
  `read-mouse-button`@`0x514`）；命中判定 `i12e`@`0x65C`（行 100）在**读之后**；菜单分派 `menu-dispatch`@`0x1164`（行 314）。

## 位置区间

读数出处：`docs/03-emulator/observations.md` 的 `OBS-20261116-05`（2026-11-16，macos-arm64 RelWithDebInfo；
每格 3–4 次 run 逐位相同）与 `docs/03-emulator/input-plan.md` §2.6 / §2.9 的同名表。
判据读数 = 收尾 `region` 三数（`in_use` / `surfaces` / `heap_blocks`）：点击**生效** = `117490928 / 25 / 6821`，
**没生效（基线）** = `108595152 / 21 / 6674`（`heap_blocks` ±1 容差）。

| 区间 `[lo,hi)` | 是什么 | 复现命令 | 读数日期 |
|---|---|---|---|
| `[0x0460,0x0520)` | **回调体的输入段** = 吃 click 的窗口（点得进去） | 见下 ① | 2026-11-16（3/3 生效，父级复跑 +1） |
| `[0x039C,0x0460)` | 轮询体（`get-input-type` + `sleep 1` + `jmp`）：**门命中但点击无效** | 见下 ② | 2026-11-16（5/5 = 基线） |
| `[0x388C,0x38A4)` | 出口段（行 835–839），轮询期**永不执行** ⇒ 负对照 | 见下 ③ | 2026-11-16（3/3 `fired=0` + 基线） |

★ **配方侧命中率**（2026-11-16，本轮把 `scripts/recipes/*.plan` 的前缀改成位置门时取的读数）：
`[0x0460,0x0520)` 在 `se-eof` / `sn0000` / `sn0000-save` / `sn0000-save-profile` / `sn0000-quit`
**各 3 次**（同一台机器、`--present headless --skip-logo --game-dir ../../links/install`、
每份一个新 `--appdata`）⇒ **15/15 命中**：这 15 次 run 的第一道门都通过，紧跟的 `click 1186 373`
都被施加（`fired` 计的是 click 数）。⚠ 其中 3 次 run 的 `pending` 不是 0 —— 那 3 次卡在**后面的**
FELLOW/SAVE 段（与本门无关，见 `_recipe-map.md` ③-4/③-5）。
⇒ 它当"TITLE 的第一道门"是可用的；读数表在 `.tmp/reql/REQ-01M7HTXBADJC7R5CHKRJ1MMADG/report.md`。
负对照（同一轮）：把这一行换成 `0x388C 0x38A4` ⇒ `fired=0` + `pending=5` + exit 5，
`input-plan: FAIL timeout` 点名那一行（见 report）。

```bash
cd apps/emulator-lift   # ① 正例
printf 'await position TITLE 0x0460 0x0520\nclick 1070 480 left\n' > /tmp/pos-cb.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/pos-cb.plan
# ② 轮询体（fired=1 但 region = 基线）  ③ 负对照（fired=0 + exit 5） 分别把区间换成 0x039C 0x0460 / 0x388C 0x38A4
```

## 输入点

- **吃 click**：`[0x0460,0x0520)` —— 只到 `read-mouse-button` 的末尾（`0x514` + 3 字 = `0x520`）。
  区间取到回调后端（`[0x05D0,0x06C8)` / `[0x0778,0x083C)`）或轮询体端点都**不成立**（§2.6 读数 3）。
- **点不动**：`[0x039C,0x0460)`（轮询体）。这是"假阳性"来源：`fired=1` 但什么都没发生。
- 回调**登记点**有两处（行 53 在 `0x184` 块里、行 451 在 `0x153C` 调试菜单块里）⇒ 第 451 行那次登记之后回调仍在，
  与 `[0x0460,0x0520)` 不冲突（同一个 label）。
- 配方用的三个坐标（1280×720）与它触发的转移：
  - `1186,373` = 「はじめから」⇒ GAMESTART（`scripts/recipes/se-eof.plan:2`，用户给的坐标）。
  - `1070,480` = LoadData ⇒ 加载存档界面（`scripts/recipes/title-loaddata.plan:1`；承载脚本见 `_recipe-map.md` ③-6）。
  - `1180,640` = Quit（`scripts/recipes/sn0000-quit.plan:36`；该文件第 1 行的路径描述是"…→ 回 TITLE → Quit"）。
- 命中区候选（**形状推断，⛔ 不是语义结论**）：行 11–17 的 `cd=[0,9C,0,9C]` / `5=[44E,3E0,365,2D9,453]` /
  `69=[126,192,1E5,21F,22A]` + 行 100 的 `i12e`，若读作"5 个 156×156 的矩形，左上角 = (5[i],69[i])"，
  则上面三个坐标分别落在第 0 / 1 / 4 个矩形（**3/3 相合**）。这只是自洽，**i12e 的操作数语义未取证**。

## 待核

1. ~~**回调体的执行频次**~~ ⇒ **已由采样回答（2026-11-16）**：
   - **回调体是周期性执行的**。机制证据：`0x3844`（`i20c` + `ret`）在整份清单里**只有一处** `call`
     （行 67 = 回调体 `0x460` 的第一句）⇒ 采样落在 `0x3844` 就等于"回调体被进入过"。读数：
     `pause`+`sample` 共 40 次（run A：8×200 ms + 20×50 ms；run B：12×200 ms）里 **23 次落在 `0x3844`**，
     相邻两次采样的帧数增量 +3（50 ms 间隔）～ +12–13（200 ms 间隔，TITLE 稳态 ≈60 fps）
     ⇒ 回调体**每帧至少一次**（⛔ 不由此推 `i20c` 的语义）。
   - 但 **`[0x0460,0x0520)` 在采样里 0/40**：另外 17 次全落在轮询体 `[0x039C,0x0460)`
     （`0x39C/0x3A0/0x3BC/0x3F4/0x42C/0x448/0x454`）。⇒ **"隔 200 ms 采两次 pos 仍在区间内"这条老判据不可达**：
     `sample` 取的是**暂停点**（块边界 / Present），而 `await position` 在**读点**上判；
     TITLE 的墙钟主要花在 `0x3844`（每帧阻塞）与轮询体上，读输入段只占一帧里的极短一段。
   - 这个区间的可当门性另有读数：`await position TITLE 0x0460 0x0520` 在 2026-11-16 的 8 次 run 里
     **8/8 命中且点击被消费**（`GAMESTART.md` ① 的读数链就是从这里起步的）。
   - ⚠ 由此得到的通用口径：**判"区间能不能当门"只能跑 `--input-plan`，⛔ 不能用采样命中率否门**
     （反方向的反例在 `SN0000.md`：粗区间采样 59/95、却点不动）。
2. **5 个命中区 ↔ 菜单语义**：第 0/1/4 个矩形 ↔ はじめから / LoadData / Quit 是坐标相合推出来的；
   而 `menu-bind` 的 5 个 handler（行 308–314 绑 `0x1184`/`0x1240`/`0x12C8`/`0x14F0`/`0x153C`）里，
   `0x153C`（行 383–450）在造「デバッグメニュー」串 —— 与"第 4 个矩形 = Quit"**互相矛盾**，
   要么矩形顺序 ≠ 菜单 id 顺序，要么 `1180,640` 的意图不是 Quit ⇒ 需一次 `sample` + 截图判据。
3. **换屏后门何时变红**：离开 TITLE 后 `script` 身份切换的确切时刻没取过证（配方今天靠 `wait` 跨过去）。
4. **`script_sha`**：`REQ-01M7HTXBADJC7R5CHKRJ1MMADG` 判据 4 要求位置谓词带脚本字节哈希
   （汉化 `insert-after` 会移偏移），而 `await position` 今天**没有**这个参数 ⇒ 属另一单，本文不假设它存在。
