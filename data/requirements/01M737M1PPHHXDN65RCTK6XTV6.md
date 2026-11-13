# 读数：CPU vs GPU 光栅的每帧墙钟与占比（裁决 oracle 保留成本）

- id: REQ-01M737M1PPHHXDN65RCTK6XTV6
- type: spike
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- dropped_reason: 裁决目标已达成：2026-11-13 用户裁决 GPU 唯一光栅、CPU 光栅整体退休（REQ-01M78BF1FNNH6K8N8GEY6GQYXX），CPU 臂将被删除 ⇒ 两臂读数不再有决策价值。
- tags: [emulator-lift raster measurement]

## 一句话
给"CPU 光栅有多慢"一个**读数**（现在只有印象）：同一 build、同一输入、**只差 `--raster cpu|gpu`** 的两臂，量
① 同一段帧数的总墙钟 ② 光栅在帧时间里的份额 ③ 长流程（到 TITLE / 到 Quit）在 `--time-limit` 内能不能跑完。
用途：裁决"CPU oracle 值不值得保留"，把 `REQ-01M737D3BHNMRY97T9A02DARCC` 的三层契约钉在读数上。

## 口径（`docs/03-emulator/measurement.md` 的 A/B 形状）
- 两臂**同一二进制**、同一 `--present`、同一 `clock`、同一 `--input-script`、同一 `--max-frames`，只差 `--raster`。
- 读数落进 `docs/03-emulator/observations.md` 的新 OBS 条目（命令 + 口径四件事 + 日期）。

## 已有读数（先落一半，别重复量）
`docs/03-emulator/observations.md` 的 **OBS-20261111-02**：`--raster cpu` 在本机 `--time-limit 300` **只到 frame 69**、
`--profile` 的 `shim` 桶 **236.5 s** 几乎全在 CPU 光栅化；同脚本 `--raster gpu` 能跑到 Quit。
⇒ 本单**还剩**：① 同一段帧数的两臂墙钟（CPU vs GPU，同一 `--max-frames`）② 光栅占帧时间的**百分比**
（`[frame]` 的 `rast :` 桶或 profile 桶占比）③ 结论句（份额 X% ⇒ oracle 保留成本可接受/不可接受）。

## 判据（三样都要有读数）
1. 同一段帧数的**总墙钟**两臂对比；
2. `[frame]` 的 `rast :` 桶或 `--profile` 里 raster 的**占比**；
3. 长流程（`--max-frames 136` 到 TITLE、以及 TITLE→Quit）在 `--time-limit` 内**是否跑得完**。
- 结论写成一句可裁决的话（例："CPU 光栅占帧时间 X% ⇒ oracle 保留成本可接受 / 不可接受"），**不写"感觉慢"**。

## 不做
- 不改任何代码；不做像素比较（那是 `REQ-01M737D3BHNMRY97T9A02DARCC` 的夹具层）。
- 不测 `--present window` 的显示开销（那是呈现轴，不是光栅轴）。

## 裁决（2026-11-13 · 关单理由）

本 spike 的唯一用途是"裁决 CPU oracle 值不值得保留"。该裁决已由用户给出并落成
`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`：**GPU 是唯一光栅实现，CPU 光栅整体退休，无 GPU ⇒ 拒绝启动**。
⇒ 两臂读数不再有消费者（CPU 臂将被删除），本 spike 的剩余三样读数**没有决策价值**，不再跑。
