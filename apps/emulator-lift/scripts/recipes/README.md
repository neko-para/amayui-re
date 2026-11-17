# `scripts/recipes/` —— 输入配方（`.plan`）索引

> ★ 这里每一份都是**顺序配方**：一套语法、一份解析器（`src/runtime/recipe.h` + `recipe.cpp`）。
> **唯一入口 = AOT 臂** `emulator-lift-aot --input-plan <path>`（`src/aot/aot_main.cpp`）。
> 解释器臂的 `--interaction-graph` 与仓根 `graphs/` 目录 **2026-11-16 退役**
> （裁决见 `data/requirements/01M7HQ57XV66FZQA5ECGHWD71N.md` 的"用户裁决"第 1/3 条；
> 收敛背景见 `01M7HP2Q1N0N004M8R4RPABWSV.md`）。
> 本目录的文件就是那次搬家：`graphs/<名>.graph` → `scripts/recipes/<名>.plan`，**内容原样**。
>
> ★ **行写法 / 顺序模型 / 完成度判据**的真源是 `src/runtime/recipe.h` 的文件头与
> `docs/03-emulator/input-plan.md`；⛔ 不要在这里复述 schema。
> ⛔ 本目录**不写帧号**（`docs/03-emulator/interaction-model.md` §4 的确定性口径）。

## ⚠ 这几行是需求树的 `repro:` 锚 —— 逐字不许改

需求树把 `repro:` 写成 `文件#子串`，校验方式是"文件存在 + 子串命中"。
下面这些**指令行**就是锚指向的子串（改一个空格都会让锚悬空）：

```text
click 1070 480
click 650 320
click 1186 373
click 1180 640
click 830 592
```

`click 650 320` 在 `sn0000-save.plan` 与 `sn0000-quit.plan` 里都出现（两张不同的单锚的是各自那条路径），
所以**两张都不许动这一行**。

## 行写法（语法真源 = `src/runtime/recipe.h`；这里只举例，⛔ 不复述 schema）

```text
await script TITLE timeout 30000        # 行尾 `timeout` = **这个门**的守卫（最多等 30 s）
await position TITLE 0x0460 0x0520 timeout 5000
wait 5000                               # 固定时长（宿主墙钟）⇒ ⛔ 不带 timeout
click 1186 373                          # ⛔ 不带 timeout（点击没有可等待的东西）
```

★ `timeout <ms>` **只能**写在 `await script` / `await position` 那一行的末尾。单独一行写 `timeout`
（旧的"守上一条指令"写法）/ 挂在 `wait` · `click` · `profile_*` · `quit` 上 ⇒ **解析期硬错**
（`exit 2` + 行号，文案会告诉你该写回哪一行）。`wait` **没有守卫**：它是固定时长、到点就走。

## 文件 → 干什么 → 谁在用

| 文件 | 干什么 | 谁在用（`repro:` 锚 / docs / 需求单） |
|---|---|---|
| `se-eof.plan` | TITLE → GAMESTART → 点「下一步」，SE 播完后留足 12 s 收尾（音频尾声重复的复现面） | **repro 锚** `01M75VZXV4SDMVVM1Z72G009XJ#click 830 592` · `docs/03-emulator/observations.md`（音频尖峰读数）· `apps/emulator-lift/docs/audio-implementation.md` · `01M75PTAC1JJ3G4QGQDRB1G4V8` / `01M75WWFGT8ZEBGEPENQHSYTB8` / `01M770JFDPZKKJR7D0ZZ09FJJ7` |
| `sn0000.plan` | TITLE → GAMESTART → ADV，窄窗口 profiler 三段（`idle` / `click1` / `click2`） | `docs/03-emulator/observations.md`（S3 ADV + 真音频的驱动面）· `01M78A0V0X1X1PCK7MP71Q1J81`（step3 丢点击的复现面） |
| `sn0000-save.plan` | TITLE → GAMESTART → ADV → 右键 FELLOW → SAVE → 选项存档（写 1 MB DAT + STH 缩略图） | **repro 锚** `01M77ZJYSPX5V21QEB89W51815#click 650 320` **与** `01M780VKKRFHJEX6VM74DRCYBM#click 650 320` · `docs/03-emulator/observations.md`（存档写 / 退出写摘要链）· `apps/emulator-lift/README.md` §4.2d 的跑法示例 |
| `sn0000-save-profile.plan` | 与 `sn0000-save.plan` 同路径，两处 `profile_begin/end` 窗（`fellow-enter` / `save-confirm`） | `docs/03-emulator/observations.md`（区间 profiler 读数）· `01M7A5VFKB7VYHEXVXB67QDNST`（`done_reason` 里的复跑命令） |
| `sn0000-quit.plan` | TITLE → GAMESTART → ADV → 右键 → FELLOW → SAVE → 回 TITLE → Quit | **repro 锚** `01M78A0V0X1X1PCK7MP71Q1J81#click 1186 373` **与** `01M77MC1374HXC2JR7E0ZYSHQJ#click 1180 640` · `docs/03-emulator/defects.md`（quit 不是正常退出那张单的复现面）· `01M7A5VFKB7VYHEXVXB67QDNST` |
| `title-loaddata.plan` | TITLE → 点 LoadData（1070,480）→ 留 20 s 看结局（进加载界面 / 干净退出） | **repro 锚** `01M75SDKZGY87B8SK7GBP92PXD#click 1070 480` · `docs/03-emulator/defects.md` · `01M7GA4B8T9JXFDAYK46YHNH7K`（AOT 侧手写 plan 的对照） |
| `sc0000-wipe.plan` | 加载第 4 槽（`click 300 269` = SAVE03）+ **1 次**推进点击 → blind wipe；`profile_begin/end pre` 与 `wipe` 两窗（三档 headless / window vsync0 / vsync1） | `docs/03-emulator/wipe-profile.md`（逐帧 timed 读数与 H1–H6）· `docs/03-emulator/scripts/SC0000.md`（wipe 转场静态定位）· `docs/03-emulator/scripts/SAVE.md`（槽位行 y 实测） |

★ 单号都是 `data/requirements/<ID>.md`。**改这里的路径 ⇒ 那 6 条 `repro:` 锚要一起改**（那是父级的活；
本轮只搬家，没改需求树）。

## 怎么跑（只有一条路：AOT 臂 `--input-plan`）

```bash
cd apps/emulator-lift
./out/emulator-lift-aot --game-dir ../../links/install --appdata <每次全新空目录> \
  --skip-logo --audio off --present window --present-hidden \
  --input-plan scripts/recipes/sn0000-save.plan \
  --max-frames 8000 --time-limit 400
```

判据读数（一次性输出，缺省档就有）：

- `input-plan: steps=N fired=M pending=K presents=… released=… held=…`
- **完成度是判据**：`pending != 0` 或某个**门**的 `timeout` 到期 ⇒ stderr 点名 `文件:行` + 门/动作，**exit 5**
  （`Done` ⇒ 0；解析失败 ⇒ exit 2 + 行号）。⛔ 不要把 exit 5 读成"解析失败"。
  ★ `wait` 是固定时长、**没有守卫** ⇒ 它不会被判 timeout（真卡住看 `--time-limit` / `--max-frames`）。

### ★ 这些配方是 **real-time 档**（`--present window`），不是 headless

理由：配方里的 `wait <ms>` 是**宿主墙钟**，而这些流程的 guest 侧长活（装载 / 转场 / 写档）在模拟器上要
几秒到几十秒。headless 下 guest 推进与墙钟不成比例（跑得"太快"，同样的墙钟等不到下一屏）⇒
配方先撞自己的 `timeout` 或跑完仍是 `pending`，**看起来像配方写错了**。
⇒ 要读"这张配方走完了没有"，用 `--present window --present-hidden`（隐藏窗口、real-time 时钟）。
只想知道"能不能解析"：`--max-frames 1` 就够（解析成功 ⇒ 停在 `pending != 0` 的 exit 5，解析失败 ⇒ exit 2）。

⚠ `--audio` 在 AOT 臂上**只实现 `off`**；要真音频（`se-eof.plan` 的尾声重复）今天只能在解释器臂上
用 `--audio sdl` 跑 —— 而那条路的输入入口已退役，得用同一份配方手改驱动或等 AOT 音频落地。

## 怎么查

```bash
# 谁引用了某份配方（repro 锚 / docs / 需求单）
rg 'scripts/recipes/<名>\.plan' data/ docs/ apps/ --glob '!apps/emulator-lift/out/**'

# 配方能不能解析（exit 2 = 解析失败；exit 5 = 解析成功但没走完）
cd apps/emulator-lift && ./out/emulator-lift-aot --game-dir ../../links/install \
  --appdata $(mktemp -d) --skip-logo --audio off --present headless \
  --input-plan scripts/recipes/<名>.plan --max-frames 1

# 语法 / 顺序模型 / 每步守卫宽度的唯一真源
sed -n '1,60p' apps/emulator-lift/src/runtime/recipe.h
```
