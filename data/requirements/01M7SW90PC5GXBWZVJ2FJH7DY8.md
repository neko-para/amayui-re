# SAVE06 → 回 SC0000 的全屏蓝遮罩（40% #A2FF）：疑似 SAVE.DAT/SAVE06 存档集合不一致；待 win32 真机确认

- id: REQ-01M7SW90PC5GXBWZVJ2FJH7DY8
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: docs/03-emulator/save06-blue-mask.md#现象
- severity: S2
- tags: [emulator-lift, sc0000, save, render]

## 一句话

从 `SAVE06` 加载 → 点击 → 等待 → 再点击两次 → 回到 `SC0000` 后，guest 后缓冲里持续有一层
**全屏 1280×720、RGB(0,162,255) @ α≈0.4** 的蓝色遮罩；**用户确认真机同一状态没有**。
取证结论：蓝由 `SC0000` 的 **scene 2** 块（`label_000197c0`）按脚本 `set-draw-color-alpha` 设置；
是否进 scene 2 由 `3f3c`/`13d9` 决定。**待 win32 真机确认存档集合与 `13d9` 读数后再继续**。

## 现状（2026-11-19 / macOS arm64，已暂停）

- **复现**：5 次独立 run + 1 次一键配方，全部复现；停在 `SC0000.BIN 0x19CE8`。
- **在 guest 后缓冲里**：`emu-ctl screenshot` 的 `source=guest-backbuffer`、`hash_still_matches=true`；
  不是窗口合成、不是 overlay（headless、无 `--overlay`）。
- **读数**：全屏均匀、等效 `RGB(0,162,255) @ α=0.4`；像素模型 `out = 0.6·参考 + 0.4·(0,162,255)`，残差 <0.5 LSB。
- **绘制者**：一条全屏 sprite（`serial=275`、1280×720 **纯白**纹理、`dst=0,0..1280,720`、
  `color=0x6600A2FF`、单位阵、α 混合）；颜色逐位来自 `SC0000.BIN.txt:6769`
  `set-draw-color-alpha (f802d) 0 66 a2ff`；点击后同一条 α 从 0x66 衰减到 0。
- **排除**：修订版（`links/raw` 与 `links/install` 都蓝，pos 差 ~0x130）；渲染层（白贴图是引擎
  `LockRect` 写的 `nonwhite=0, uniq16=1`；blend/stage/TFACTOR/alpha test/scissor/矩阵全标准 D3DX）；
  输入/流程少一步（`13d9` 唯一写者是 scene 2 自己；换点击位置/次数/等待的 A/B 全进 scene 2）。

## 分流链

- `SC0000` dispatcher `label_000047c4` 按 `3f3c` 选块：`2 → label_000197c0`（scene 2，块首
  `mov 13d9 1` + 设蓝）；`3 → label_00031bdc`（scene 3，无蓝）。
- `SCJUMP.BIN`：`13d8==1 && 13d9!=1 → 3f3c=2`；`13d9==1 && 13da!=1 → 3f3c=3`。
- 读档后本机 `13d7=1 13d8=1 13d9=0`、`3f3c=0x1327`（SAVE02 对照：`3f3c=1`）⇒ 读档按槽恢复；
  `13d9` 不是 `save-int/load-int` 键，走**槽内 global 快照**。

## 用户假设（本轮暂停的依据）

> “可能是因为 SAVE.DAT 和 SAVE06.DAT 不一致导致的……我当时只提交了 SAVE06，没有继续变更 SAVE.DAT。”

⇒ 如果 `saves/` 的 `SAVE.DAT`（脚本键池）/`RT.DAT` 与 `SAVE06` 不是同一次游戏状态，
真机读档可能恢复出不同的 `13d9/3f3c`（进 scene 3、无蓝），而本机用这份不一致集合恢复成 0（进 scene 2、有蓝）。
**在确认前不要改读档/存档层，也不要为消蓝改 sprite 合成。**

## 待确认（win32 真机）

1. 用**同一份完整存档集合**（`SAVE.DAT` + `RT.DAT` + `SAVE00..06.DAT/.STH`）加载 SAVE06，
   读 `13d9`/`3f3c`，确认是否进 scene 2。
2. 若真机集合一致、`13d9=0` 也进 scene 2 且仍无蓝 ⇒ 另找差异（渲染已排除，只剩读档/场景状态层）。
3. 若 `saves/` 集合本身不一致 ⇒ 修的是**样本集合**，不是 emulator；需要在 `saves/README.md` 的要求里写清
   “一套存档必须同时更新 `SAVE.DAT`/`RT.DAT`”。

## 判据（确认后）

- 若“样本集合不一致”成立：用一致集合复跑，蓝层消失 ⇒ 收口（无产品代码改动）。
- 若“恢复语义错”成立：给出 `SAVE06` 槽 payload 里 `13d9/3f3c` 的实际值与本机恢复值；修读档/global 快照
  恢复，并用一条能红的用例钉住（**不是**渲染用例）。

## 参考

- 详细取证：`docs/03-emulator/save06-blue-mask.md`（入库）；
  原始 426 行报告与诊断 patch：`.tmp/save06-blue-mask/report.md`、`shim-trace.patch`（不入库，patch 未提交）。
- 诊断开关（全部 opt-in、缺省零输出）：`LIFT_SPRITE_TRACE` / `LIFT_VCOLOR_TRACE` / `LIFT_TEXDUMP` / `LIFT_TEXFILL`。
