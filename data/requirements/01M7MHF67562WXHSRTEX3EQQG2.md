# 🐞S2 SN0000 最后一句 → SC0000 过渡段：渐变到黑整段缺失（直接黑屏）+ 背景黑白混合（预期蓝+白）

- id: REQ-01M7MHF67562WXHSRTEX3EQQG2
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: docs/03-emulator/scripts/SC0000.md#第 2/3 步评估
- tags: [emulator-lift, d3d9, sc0000]

## 一句话

用户从存档 **01**（SN0000 最后一句）点一下推进到 SC0000 章节开头，这一段过渡有三处不符。**复审已完成**：
**18 个图形接口逐个对文档过了一遍，没有一条能解释"直接黑屏"** —— 真因在脚本等待预算（已另开单），
"黑白混合"在本机不复现（需要用户那次的命令 / 截图）。

## 复审结论（2026-11-17 / mac arm64 / `links/raw` / headless / 二进制 sha `8332003f…` / 报告 `.tmp/sc0000-gfx/report.md`）

- 复现配方：`links/raw`（原版脚本，`SC0000.BIN` 511,688 B）+ appdata 副本（无 `SAVE.DAT/BAK/RT.DAT`）+ `.tmp/sc0000/black/prefix27.plan`；
  `emu-ctl sample` ⇒ `script=SC0000.BIN pos=0x6EAA8 frame=1996`。
  ⚠ `links/install`（汉化修订版 504,312 B）在这条配方上到不了 SC0000 —— 是**脚本修订版**问题，不是渲染问题。
- 窗口内图形 shim **1,654,266 次**、18 条接口结论：**没有一条会产生"直接黑"**。混合（引擎自设 `ALPHABLENDENABLE=1` +
  `SRCALPHA/INVSRCALPHA`）、顶点 α、无纹理取白（`untexturedResolvesToDiffuse`）、`ID3DXSprite` 批、
  全屏 quad 渐变**都在本窗口实测走通**（同一支 quad 在白闪段 α `0→255`，`fa=0→255`）。
- **老坑 OBS-20261110-07（残留纹理 α=0）今天不复现**：本窗口 quad `tex=0 / texserial=0`，无纹理按文档取白。
- 记录的偏离（都与本窗口无关）：`GetDeviceCaps` 报 shader 版本 0；`Present` 忽略 `pSourceRect/pDestRect`；
  `GetBackBuffer` 每次新建 surface；`UpdateSurface/GetRenderTargetData` 不校验 pool/locked；
  `SetViewport` 不校验 MinZ/MaxZ；`D3DRS_SRGBWRITEENABLE/DEPTHBIAS` 有效值被拒；`ID3DXSprite::Begin` 不改写设备状态。

## 三条观察的处置

- **a（渐变到黑失踪）**：**不是接口问题** ⇒ 已开 `REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ`（渐变塌成一帧：39.5e9 指令的
  一段计算把 `wait` 预算整段吃掉；要先补读数再分诊是不是空转）。
- **b（黑屏期间音效提前）**：只记时间点 —— 黑段 = 帧 1005–1612；`SetVolume`@1006、`Stop`@1010、`SetPan`@1610、
  `CreateSoundBuffer`/`Play`@1612。⛔ 本单不查（等 AOT 音频 D 泵那条 `REQ-01M7MHEZRQJSMAAWPESVW44DSK` 收口）。
- **c（黑白混合）**：**本机不复现** —— 章节卡淡入 62.89% 蓝向 / 1.36% 暗灰，完整卡 64.10% / 2.15%（预期"纯蓝 + 白"）。
  ⇒ 需要用户那一次的**机器 / 呈现档（`--raster`/`--present`）/ 截图**才能定位；若在 win32，优先怀疑呈现/读回面。

## 判据（收口时）

- a 的判据落在派生单（多级灰阶帧）。
- b 落在音频单收口之后。
- c 需要一次能复现的读数（机器 + 命令 + 截图）；在那之前本单保持 open。
