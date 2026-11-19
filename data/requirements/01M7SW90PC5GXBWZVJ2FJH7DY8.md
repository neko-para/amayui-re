# SAVE06 → 回 SC0000 的全屏蓝遮罩（40% #A2FF）：真机复测为预期表现，不是缺陷

- id: REQ-01M7SW90PC5GXBWZVJ2FJH7DY8
- type: bug
- status: dropped
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: docs/03-emulator/save06-blue-mask.md#现象
- severity: S2
- dropped_reason: 不是缺陷：2026-11-19 用户用存档在 win32 真机复测，SAVE06 同一状态也有这层蓝遮罩；且从开头直接推进到对应流程也是蓝 ⇒ 蓝色是预期表现，此前"真机没有"的判断不成立。保留 repro 作重开条件（若预期改为无蓝 / 换到无蓝版本）。
- tags: [emulator-lift, sc0000, save, render]

## 一句话

从 `SAVE06` 加载 → 点击 → 等待 → 再点击两次 → 回到 `SC0000` 后，guest 后缓冲里有一层
**全屏 1280×720、RGB(0,162,255) @ α≈0.4** 的蓝色遮罩。**2026-11-19 用户用存档在 win32 真机复测：
同一状态也有这层蓝；并且从开头直接推进到对应流程也是蓝** ⇒ 蓝色是**预期表现**，此前"真机没有"的判断
不成立 ⇒ 本单**不是缺陷**（`dropped`）。

## 裁决（2026-11-19，用户；本单唯一裁决来源）

- 真机 + 同一份完整存档集合（`SAVE.DAT` + `RT.DAT` + `SAVE00..06`）加载 `SAVE06`：蓝遮罩**存在**，与模拟器一致。
- 真机**从开头直接推进**到同一流程：蓝遮罩**也存在** ⇒ 与"读档恢复出的 `13d9`/`3f3c` 不同"无关。
- ⇒ 蓝色是 `SC0000` scene 2 的**预期演出**（`set-draw-color-alpha`）；⛔ 不要为消蓝改读档 / 存档层，
  ⛔ 不要改 sprite 合成。
- **重开条件**：若预期改为"此处无蓝"（或换到无蓝的版本 / 存档），按 `repro` 重跑；`repro` 保留。

## 原取证（2026-11-19 / macOS arm64，保留供重开参考）

- 复现：5 次独立 run + 1 次一键配方全部复现；停在 `SC0000.BIN 0x19CE8`。
- 在 guest 后缓冲里：`emu-ctl screenshot` 的 `source=guest-backbuffer`、`hash_still_matches=true`；
  不是窗口合成、不是 overlay（headless、无 `--overlay`）。
- 读数：全屏均匀、等效 `RGB(0,162,255) @ α=0.4`；像素模型 `out = 0.6·参考 + 0.4·(0,162,255)`，残差 <0.5 LSB。
- 绘制者：一条全屏 sprite（`serial=275`、1280×720 **纯白**纹理、`dst=0,0..1280,720`、`color=0x6600A2FF`、
  单位阵、α 混合）；颜色逐位来自 `SC0000.BIN.txt:6769` 的 `set-draw-color-alpha (f802d) 0 66 a2ff`；
  点击后同一条 α 从 `0x66` 衰减到 0。
- 分流：`SC0000` dispatcher `label_000047c4` 按 `3f3c` 选块：`2 → label_000197c0`（scene 2，块首
  `mov 13d9 1` + 设蓝）；`3 → label_00031bdc`（scene 3，无蓝）。`SCJUMP.BIN`：`13d8==1 && 13d9!=1 → 3f3c=2`；
  `13d9==1 && 13da!=1 → 3f3c=3`。读档后本机 `13d7=1 13d8=1 13d9=0`、`3f3c=0x1327`（SAVE02 对照：`3f3c=1`）。
- 排除：修订版（`links/raw` 与 `links/install` 都蓝，pos 差 ~0x130）；渲染层（白贴图是引擎 `LockRect` 写的
  `nonwhite=0, uniq16=1`；blend/stage/TFACTOR/alpha test/scissor/矩阵全标准 D3DX）；输入 / 流程少一步
  （`13d9` 唯一写者是 scene 2 自己；换点击位置 / 次数 / 等待的 A/B 全进 scene 2）。

## 参考

- 详细取证：`docs/03-emulator/save06-blue-mask.md`（入库，含"怎么查"与边界）。
- 原始 426 行报告与诊断 patch：`.tmp/save06-blue-mask/report.md`、`shim-trace.patch`（不入库、未提交）。
