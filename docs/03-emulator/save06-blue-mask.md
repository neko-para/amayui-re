# SAVE06 → 回 SC0000 的全屏蓝遮罩（40% #00A2FF）：取证与待确认

> 2026-11-19 / macOS arm64 / `apps/emulator-lift/out/emulator-lift-aot`（RelWithDebInfo）
> 用户口径：**真机没有这层蓝**；本文是 emulator 侧的复现与取证，**不是**最终归因。
> 详细原始报告（426 行）在 `.tmp/save06-blue-mask/report.md`（不入库）；下文是**入库的结论面**。
> 状态：**暂停**——2026-11-19 用户要求在 win32 真机上确认后再继续；⛔ 不要继续改代码/继续调研。

## 现象

从 `saves/SAVE06.DAT` 加载 → 点击 → 等待 → 再点击两次 → 回到 `SC0000`（外部 ADV）后，
guest 后缓冲里出现一层**全屏 1280×720、均匀、RGB(0,162,255) @ α≈0.4（102/255）**的蓝色遮罩；
它逐帧恒定，点一下就消失（α 从 0x66 逐帧衰减）。用户确认**真机同一状态没有这层蓝**。

## 复现

```bash
# appdata 原样复制 saves/（SAVE.DAT/RT.DAT/SAVE00..06.DAT/.STH 全带；⛔ 不把槽复制成 SAVE.DAT）
cp saves/SAVE* saves/SAVE.DAT saves/RT.DAT "$APP/Local/Eushully/天結いキャッスルマイスター/SAVE/"
# TITLE → Load Data(1070,480) → LOAD 006（第 7 行，y≈450）→ 确认「是」
# 然后：click → wait → click ×2 → 回到 SC0000
./out/emulator-lift-aot --present headless --audio off --skip-logo \
  --game-dir ../../links/install --appdata ../../.tmp/save06-blue-mask/appdata \
  --input-plan ../../.tmp/save06-blue-mask/save06-blue.plan --frame-log
# 停住后：emu-ctl pause / sample / screenshot <bmp>
```

- 复现率：5 次独立 run + 1 次一键配方，全部复现。
- 配方：`.tmp/save06-blue-mask/save06-blue.plan`（加载 → SC4900 → STAGECLEAR → REWARD → SC0000）。

## 读数

| 项 | 值 |
|---|---|
| 在 guest 后缓冲里？ | **是**（`emu-ctl screenshot` 的 `source=guest-backbuffer`、`hash_still_matches=true`） |
| 几何 / 颜色 | 全屏 1280×720 均匀；等效 **RGB(0,162,255) @ α=0.4**；拟合 `out = 0.6·参考 + 0.4·(0,162,255)`，残差 <0.5 LSB |
| 绘制者 | **一条全屏 sprite**：`serial=275`、1280×720 **纯白**纹理、`dst=0,0..1280,720`、`color=0x6600A2FF`、单位阵、α 混合 |
| 颜色来源 | `SC0000.BIN` 脚本 `set-draw-color-alpha (global-int f802d) 0 66 a2ff`（`dist/views/data/SC0000.BIN.txt:6769`） |
| 点后消失 | 同一脚本 `wait-for-input`（`:6829`）之后 `:6832-6833` 重置为白/α0 |
| 场景分流 | `SC0000` dispatcher `label_000047c4` 按 `3f3c` 选块：`2 → label_000197c0`（scene 2，含设蓝）；`3 → label_00031bdc`（scene 3，无蓝） |
| 为什么进 scene 2 | `SCJUMP.BIN`：`13d8==1 && 13d9!=1 → 3f3c=2`；`13d9==1 && 13da!=1 → 3f3c=3`。读档后本机 `13d9=0` ⇒ scene 2 |
| `13d9` 写点 | **唯一写者**是 scene 2 块自己（`SC0000.BIN.txt:6755` `mov 13d9 1`）；`13d9` 不是 `save-int/load-int` 键，走**槽内 global 快照** |
| 存档对照 | SAVE02（`SC0000 0x6B54`）：`13d7=1 13d8=1 13d9=0 3f3c=1`；SAVE06（`SC4900 0x6130`）：`13d7=1 13d8=1 13d9=0 3f3c=0x1327` ⇒ 读档按槽恢复 |

## 排除

- **修订版/偏移**：`links/raw`（511,688 B）与 `links/install`（504,312 B）**都蓝**，只是 `SC0000` 的 pos 差 ~0x130。
- **渲染层**：白贴图是引擎 `D3DXCreateTexture + LockRect` 自己写的 `nonwhite=0, uniq16=1`；blend 5/6、
  stage `MODULATE(TEX,CURRENT)`、TFACTOR 白、alpha test 关、scissor 全屏关、单位阵——全标准 D3DX。
- **输入/流程少一步**：`13d9` 的唯一写者是 scene 2 自己；换点击位置/次数/等待的 A/B 全部进 scene 2
  （`3f3c` 恒 2），没有输入能跳到 scene 3。
- **HUD/overlay/窗口合成/DDraw**：headless、无 `--overlay`；蓝在 guest 后缓冲里。

## 假设与待确认（用户 2026-11-19 提示）

> “可能是因为 SAVE.DAT 和 SAVE06.DAT 不一致导致的……我当时只提交了 SAVE06，没有继续变更 SAVE.DAT。”

- 若 `saves/` 里的 `SAVE.DAT`（脚本键池）/`RT.DAT` 与 `SAVE06` 不是同一次游戏状态，真机上的读档
  可能恢复出不同的 `13d9/3f3c`（⇒ scene 3，无蓝），而本机用这份不一致的集合恢复成 0（⇒ scene 2，有蓝）。
- 待 win32 真机确认：用**同一份完整存档集合**（`SAVE.DAT` + `RT.DAT` + `SAVE00..06`）加载 SAVE06，
  读 `13d9/3f3c`，看是否进 scene 2。
- 若真机也不一致 ⇒ 这份 `saves/` 集合本身不是“可复现真机状态”的样本；修的是**存档集合/样本**，不是渲染。
- 若真机集合一致且 `13d9=0` 也进 scene 2，却仍无蓝 ⇒ 另找差异（本机渲染已排除，只剩读档/场景状态层）。

## 怎么查（确认前不要继续）

```bash
# 读当前脚本/位置/全局（暂停态）
node tools/emu-ctl.mjs sample --json
node tools/emu-ctl.mjs global-get --kind int --index 0x13d9 --json
node tools/emu-ctl.mjs global-get --kind int --index 0x3f3c --json
# 存档 payload 的 global 快照（下一步待做：解析 SAVE06 槽 payload 里的 13d9/3f3c 字段）
```

## 边界

- ⛔ 不要为消蓝改 sprite 合成（比如跳过 `a<1` 的全屏层）——会误伤所有半透明全屏效果。
- ⛔ 不要在确认前改读档/存档层代码；先确认是“样本集合不一致”还是“恢复语义错”。
- 诊断 patch（`LIFT_SPRITE_TRACE`/`LIFT_VCOLOR_TRACE`/`LIFT_TEXDUMP`/`LIFT_TEXFILL`，全 opt-in、缺省零输出）
  在 `.tmp/save06-blue-mask/shim-trace.patch`，**未提交**。
