# emulator-lift · CPU 兜底路径的命中普查只覆盖 boot→TITLE：其余场景的 invw/mip 命中未测（现会被拒绝）

- id: REQ-01M7A5VFKB7VYHEXVXB67QDNST
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, gpu, census]

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
