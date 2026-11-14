# emulator-lift · CPU 兜底路径的命中普查只覆盖 boot→TITLE：其余场景的 invw/mip 命中未测（现会被拒绝）

- id: REQ-01M7A5VFKB7VYHEXVXB67QDNST
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 判据落地为读数（OBS-20261114-13）：两个场景逐次跑现行命令（不再是已删的 --raster gpu）—— graphs/sn0000-save-profile.graph（TITLE→GAMESTART→ADV→FELLOW→SAVE）与 .tmp/profile/load.graph（TITLE→LoadData→读档）——两次的 unsupported : 行都只有 bit11（MIPFILTER，已知并接受的近似）⇒ bit21（透视 invw!=1）/ bit22（mip 缩小）/ bit23（GPU 后端失败）在两段里一次都没置位，即'ADV/存档路径会撞 CPU 兜底拒绝'这条风险在这两段不成立。⚠ 未测：OPTION 界面与战斗场景（仓库里没有对应交互图）；fp 那次 exit 5 之前只走到 SAVE 之后（step18 no-sample，与本单无关）。按单里'读数进 observations、⛔ 不写成 test 里的取值断言'的口径，没有新增 CTest。判据重基（--raster 已删 / 图名更正）写进正文。
- tags: [emulator-lift, gpu, census]

## 2026-11-14 判据重基（本单仍 open；原来那条命令跑不起来）

- **`--raster gpu` 已删除**（2026-11-13，GPU 是唯一光栅实现）⇒ 原文的"逐场景跑 `--raster gpu`"照抄会 `未知参数` 退出 2。
  现行命令（`apps/emulator-lift` 下）：
  ```powershell
  ./out/emulator-lift.exe --present window --present-hidden --native-overrides all --skip-logo --audio off `
    --game-dir ../../links/install --appdata <全新副本> `
    --interaction-graph ../../graphs/<图> --max-frames 8000 --time-limit 400 --log-level info
  ```
  只看运行摘要的 `unsupported : 0x…` 行（bit21 透视 / bit22 mip 缩小 / bit23 后端失败）。
- **四条兜底路径的现状变了**：点/线已补 GPU 实现（`gpu-batch/lines-points`）；另三条仍是**显式拒绝 + 记账**（bit21/22/23）
  ⇒ 本单要回答的问题没变，但"命中即被拒绝"的后果要按现状描述。
- **图的选择**：原文点名的 `room-enter` / `option` 图**不存在**；可用的是 `graphs/sn0000.graph`（ADV）、`graphs/sn0000-save.graph`、
  `.tmp/profile/load.graph`、`graphs/sn0000-quit.graph`。⛔ 不要再写不存在的图名。
- 判据本身（bit21/22/23 命中必须 0；命中 >0 ⇒ 在 GPU 补或显式拒绝并写 SCOPE；读数进 observations）**不变**。
## 缺口

「点/线、`invw != 1`、`MIPFILTER=NONE` 缩小、GPU 批失败重放」四条 CPU 兜底路径的命中普查只做了 **boot→TITLE** 两个窗口
（skip-logo 60 帧 / 不 skip 240 帧），四条的命中都是 0。于是：
- 点/线已补 GPU 实现（守卫 `gpu-batch/lines-points`）；
- 另三条改成了**显式拒绝 + 记账**（`UnsupportedBit` bit21 透视 / bit22 mip 缩小 / bit23 后端失败）。

**风险**：ADV / 存档 / OPTION / 战斗等场景未普查。若那里命中 `invw != 1` 或 mip 缩小，现在会**被拒绝**而不是画出来 ——
表现是"某些几何/贴图不显示"，痕迹只在 `unsupported : 0x…`（bit21/22）里。

## 判据

- 用现有交互图（`room-enter` / `sn0000-save` / `option` / `quit` 等）逐场景跑 `--raster gpu`，断言 `unsupported` 的 **bit21/22/23 均为 0**
  （真命中 ⇒ 它们会置位，用例红）。
- 若某场景确实命中：按 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 的规则处置（命中>0 ⇒ 在 GPU 补；否则显式拒绝并写进 SCOPE）。
- 读数（每场景的 `unsupported=` 行）进 `docs/03-emulator/observations.md`，⛔ 不写成 test 里的取值断言。

## 非目标

- 不要求像素级线规则（不做 diamond-exit）；不扩带纹理的点/线（仍 bit16 拒绝）。
