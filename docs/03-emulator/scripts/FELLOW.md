# FELLOW.BIN

## 身份

- 清单：`dist/views/data/FELLOW.BIN.txt`（3991 行；头 `signature = SYS4450`、`local_vars = { e0e 1 2 f 1 3 }`，行 1–3）。
  该名字在 `dist/views/data/` 里**只有一份**（没有 `$1$` / `$2$` / `$3$` 副本）。
- `script` 身份名怎么来的：运行期名字匹配（ASCII 大小写不敏感、两侧剥 `.bin`），判定在
  `apps/emulator-lift/src/runtime/recipe.cpp` 的 `scriptNameMatches`。
- 被装载的机制：ADV 屏 `SN0000` 里右键（配方 `click 500 500 right`）之后被装载；实测中间会穿几个过场脚本
  （`.tmp/p2/trace1.log` 的采样序列：`SN0000 → SETARMSTATE → FELLOW`；`sn0000` 那一路另见 `trace-s2.log`：
  `SN0000 → SETTOOLLIST → FELLOW`）。⇒ **不要**把"右键之后立刻就是 FELLOW"当不变量。
- 结构（本文区间都靠它，行号=清单行，地址=`label_%08x`）：
  - **主轮询体** `0x1570`（行 296–300）：`get-input-type`@`0x1570` · `jcc`@`0x1574` · `sleep 1`@`0x1590` ·
    `jmp label_00001570`@`0x159C`。
  - **回调登记**：`mouse-callback 10 label_000015a8`（行 416）⇒ **回调体** `0x15A8`（行 302–310）：
    `call label_00001600`@`0x15A8` · `eq`@`0x15B4` · `jcc`@`0x15D0` · `call-frame 1c`@`0x15EC` · `i20c`@`0x15F8` · `ret`@`0x15FC`。
  - **读输入子过程** `0x1600`（行 312–321，唯一调用者 = 回调体）：`add`@`0x1600` · `i2fc`@`0x161C` ·
    `jcc`@`0x1648` · `read-mouse-pos`@`0x1684`（**label**）· `read-mouse-button`@`0x1698`。
  - **命中判定**：`i12e`@`0x174C`（行 330，读之后、只在鼠标位置变化时跑）；**左键动作** `0x2B80`（行 692）·
    **右键动作** `0x2B2C`（行 685，体内 `mov (local-int 3f4) 1` ⇒ 轮询体的 `jcc 3f4 → label_00012570` 跳出主循环）。

## 位置区间

读数口径：判据只有 `{script, pos}` 与配方的 `fired/pending/exit`。`pos` 与清单 `label_%08x` 同一坐标系。
★ **采样命中率既不能立门也不能否门**（`_recipe-map.md` 待核 7）：FELLOW 的 25 次采样只落在
`0xA4F4`×1（装载瞬间）/ `0x1574`×1 / `0x1590`×5 / `0x159C`×2 / `0x15F8`×16（`trace1.log` 45 行里的 FELLOW 部分）。
按区间数：落进 `[0x15A8,0x16A4)` 的是 **16/45**（全是 `0x15F8` 那个点）、落进**更窄的**
`[0x1684,0x16A4)` 是 **0/45** —— 而后者才是唯一点得进去的窗口（下面是门读数）。
⇒ 采样**分不开**"回调体前半"和"输入读段"这两条，能分开它们的只有 `--input-plan` 的门。

| 区间 `[lo,hi)` | 是什么 | 复现命令 | 读数日期 |
|---|---|---|---|
| `[0x1684,0x16A4)` | **输入读段**（`read-mouse-pos`@`0x1684` + `read-mouse-button`@`0x1698`，到 `label_000016a4` 之前）。★ **推荐门**：窄、点得进去 | 见下 ①（E1） | 2026-11-16：门 **4/4** 命中且点击被消费（`steps=10 fired=4 pending=0`、exit 0、四次 `presents` 4242/4260/4363/3765） |
| `[0x15A8,0x16A4)` | **回调体前半 + 输入读段**（回调入口 → `read-mouse-button` 末尾）。门可用、点击也生效 | 见下 ②（E2） | 2026-11-16：**1/1** 命中且点击被消费（`steps=10 fired=4 pending=0`、exit 0） |
| `[0x15A8,0x1600)` | **回调体前半**（`call 0x1600` / `eq,jcc` / `call-frame 1c` / `i20c`）。**也能吃 click**（反直觉，见 `## 待核` 2） | 见下 ③（E5） | 2026-11-16：**1/1** 命中且点击被消费（`steps=10 fired=4 pending=0`、exit 0） |
| `[0x1570,0x15A8)` | **主轮询体**（`get-input-type` / `sleep 1` / `jmp`）。门命中、click 也被施加，但**点击不生效** | 见下 ④（E0，负对照） | 2026-11-16：`steps=10 fired=4 pending=2`、**exit 5**，`input-plan: FAIL timeout` 点名 `await script SAVE`（20 s 到期）；停机原因是 `time-limit`，**不是**门没命中 |

- 执行源：`apps/emulator-lift/out/emulator-lift-aot`，`sha256 = 27fca94a09422ece80d9db64b9962911bbb2d65cea7d1ae3f6dc8893e13f35c9`
  （2026-11-16；macos-arm64）。全部读数都是 `--present headless --skip-logo --game-dir links/install`、
  `--appdata` = 从 `corpus/fixtures/` 组装的那份（见 `docs/03-emulator/save-format.md` §外部组装）。
- 原始日志（不入库）：`.tmp/p2/run-e1.log` · `run-e1-2/3/4.log` · `run-e2.log` · `run-e5.log` · `run-e0.log`。

```bash
cd /Users/nekosu/Documents/Projects/amayui-re
APP=.tmp/p2/appdata2     # 空 appdata 跑一次让引擎建目录，再按 save-format.md §外部组装 放 SAVE.DAT/RT.DAT + 槽

# ---- 共用前缀（= sn0000-save.plan L7–L18 的已取证前缀，逐字照抄；本轮没有改任何配方）----
cat > /tmp/fellow-prefix.plan <<'PLAN'
await position TITLE 0x0460 0x0520 timeout 30000
click 1186 373
await position GAMESTART 0x0664 0x0724 timeout 30000
click 830 592
await position SN0000 0x46B8 0x470C timeout 35000
click 500 500 right
PLAN

# ---- ① E1：输入读段当门（推荐形状）----
cat > /tmp/fellow-tail.plan <<'PLAN'
await position FELLOW 0x1684 0x16A4 timeout 60000
click 100 140
await script SAVE timeout 20000
quit
PLAN
cat /tmp/fellow-prefix.plan /tmp/fellow-tail.plan > /tmp/fellow-e1.plan
apps/emulator-lift/out/emulator-lift-aot --present headless --skip-logo --game-dir links/install \
  --appdata "$APP" --max-frames 0 --time-limit 120 --input-plan /tmp/fellow-e1.plan
# 读数：exit 0 · input-plan: steps=10 fired=4 pending=0 · stop: … — input-plan: quit
#       4 次 run 都是这两行（只有 presents 不同：3765 / 4242 / 4260 / 4363）

# ---- ② E2：整个回调体当门 ----
#   把 ① 的 tail 第一行换成 await position FELLOW 0x15A8 0x16A4 timeout 60000，其余不动
#   读数：exit 0 · steps=10 fired=4 pending=0

# ---- ③ E5：回调体前半当门 ----
#   把 ① 的 tail 第一行换成 await position FELLOW 0x15A8 0x1600 timeout 60000，其余不动
#   读数：exit 0 · steps=10 fired=4 pending=0

# ---- ④ E0 负对照：主轮询体当门 ----
#   把 ① 的 tail 第一行换成 await position FELLOW 0x1570 0x15A8 timeout 60000，其余不动
#   读数：exit 5 · steps=10 fired=4 pending=2 · input-plan: FAIL timeout
#         plan: /tmp/fellow-e0.plan:9 · waiting: await script SAVE（20 s 到期，点击已施加但没进 SAVE）
```

## 输入点

- **吃 click**：回调体 `[0x15A8,0x16A4)`；最窄可用的是输入读段 `[0x1684,0x16A4)`。
  ⇒ **门要落在这里，`wait` 不行** —— 同一屏、同一坐标，门落在轮询体 `[0x1570,0x15A8)` 时点击**不生效**（④）。
  这条读数是"什么时候才吃 `click 100 140`"的答案：**与进 FELLOW 多久无关，只与门落在哪一段有关**。
- **点不动 / 假阳性**：主轮询体 `[0x1570,0x15A8)`。它的形态与 TITLE / GAMESTART 同形
  （`get-input-type` + `sleep 1` + `jmp`），但**在 FELLOW 上它的点击不会生效**（TITLE 上"门命中但点击无效"那条
  结论在 FELLOW 上同样是负的，见 ④）。
- **控制层直投（旁证，⛔ 不作判据）**：在 FELLOW 稳态（暂停点 `pos = 0x15F8`）用 `emu-ctl click` 连投 **16 次**
  （`drive-f1`：1×`100 140`；`drive-f2`：10×`100 140` + 5×`100 270`，每次后面 ~1.2 s 采样）⇒ **0/16 换屏**
  （`.tmp/p2/drive-f1.log` / `drive-f2.log`）。
  ⚠ 这两份 run 用的是**旧的 `.tmp/appdata`**，且控制层点击落在**哪个读点不可控** ⇒ 只当"任意读点不行"的旁证。
- 坐标与它触发的转移：
  - `100 140` ⇒ **SAVE**（① 的读数；四次 run 都进 SAVE）。它点在 UI 的哪一项**未取证**（不写"第几项"）。
  - `100 270` = 另一项（`sn0000-quit.plan` 用），**本轮没取证**（5 次直投都落在轮询体上，不能作数）。
- **形态阅读（静态，⛔ 不是语义结论）**：左键在**抬起边沿**分派 —— 回调体把 `3f1`（按钮电平）与 `3f2`（按下边沿）
  比对，左键释放 → `0x2B80`，那里先判 `3f7`（命中项）`!= 0` 才继续；右键释放 → `0x2B2C`（`3f4 = 1` ⇒ 跳出轮询体）。
  `i12e` 的操作数语义未取证（同 `TITLE.md` 的"形状推断"）。

## 待核

1. **为什么轮询体上的 click 不生效**（机制未取证）。假设（**只是假设**）：`InputPlan::releaseDue`
   （`apps/emulator-lift/src/runtime/input_plan.cpp`）要 `inputSampleCount > press_samples + 1` 才抬起，而输入采样来自
   `GetCursorPos` / `GetAsyncKeyState`（`shims/user32_input.cpp` · `shims/user32_window.cpp` 的 `noteInputSample`）；
   轮询体的采样密度高 ⇒ 按下在下一次回调读 `read-mouse-button` 之前就被抬起了，脚本只看到"电平=0"。
   要钉死需要 shim 级 trace（`fired` 之外的按下/抬起时刻），本轮没做。
2. **`[0x15A8,0x1600)` 为什么也能吃 click**：按上一条假设，落在 `call-frame 1c` / `i20c` 上的按下应当能在**下一帧**
   的回调读按钮时被看到（它前面没有别的采样点把两次采样耗光）——**没有逐点取证**（E5 只证"门命中且点击生效"）。
3. **init 段**：装载早期采到 `pos = 0xA4F4`，≈0.7 s 后 `0x1590`、≈1.4 s 后 `0x15F8`（`trace1.log` 单次 run；
   采样间隔 300 guest-ms）⇒ "进屏后多久才稳"只有 1 次读数，没做 3×。
4. **`100 270` 是哪一项 / 是否也进 SAVE**：未取证（见 `## 输入点`）。
5. **命中区与坐标的对应**：`i12e` 的数组（`FELLOW.BIN.txt` 行 94 / 174 / 175 附近）**复现不出** `100,140` / `100,270`
   ⇒ 同 `_recipe-map.md` 待核 4，仍需采样或 shim 级 trace。
6. **`script_sha`**：`await position` 没有字节绑定参数（同 `TITLE.md` 待核 4 / `SN0000.md` 待核 7）。
