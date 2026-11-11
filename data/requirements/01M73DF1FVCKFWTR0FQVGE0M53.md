# emulator-lift · TITLE 帧右侧条纹/左下散点（矩形块已由状态块单修好）

- id: REQ-01M73DF1FVCKFWTR0FQVGE0M53
- type: bug
- status: done
- parent: REQ-01M735AWXRDZKCDT4124BCFF27
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#buildDeclVertices
- severity: S2
- done_reason: 取证收口：矩形块 = 状态块 Apply stub 的残留绑定（已由 REQ-01M7372BPVGBD247KAGHB53YK7 修好并实测消失：x=780 run=254→42、#62 texserial 22→0）；剩下的右侧条纹/散点是缩小采样无 mip 链的走样（62 笔里 53 笔 >1.5×），不是某一笔的错 ⇒ 落地另开 REQ-01M73G2724479XAVS176WBZ65Q。排除项：sprite 零 Begin/Draw/End、修前后点密度逐 tile 差 ≤0.2、声明路 UV 逐字段不变。
- tags: [emulator-lift d3d9 raster artifact]

## 现象（2026-11-11 更新：**矩形块已修，条纹仍在**）
原现象 = frame 2/8 上有 ① 屏幕中部的矩形贴图块 ② 右侧与左下的逐行条纹/散点噪声 ③ 角色正确。

### ① 矩形块 —— **已修**，根因 = ④ 状态泄漏
`CreateStateBlock Type=1`（捕获时 stage0 = 0x00000000）→ Live2D 绑 page22 → `StateBlock::Apply`（当时是 stub）
→ `#62`（调用点 `0x49E376`，全屏 strip，`uv=0..1`，`LIFT_DRAW_LOCALS=004B4335` 读到纹理 id = -1）采到了**残留** page22。
`REQ-01M7372BPVGBD247KAGHB53YK7` 落地后实测：`#62` 的 `texserial 22→0`、`stage0 (2/3,2/3)→(2/1,2/1)`、
矩形块的竖直边 `x=780 run=254 → 最长 58`（消失）。
★ 顺带确认：那一笔的"整体变暗"**不是缺陷** —— 它本身就是 fade 黑幕（`d0=0,0,0,alpha`，逐帧 255→0），修前被泄漏纹理顶替。

### ② 右侧条纹 / 左下散点 —— **仍在**
- 与 #62 无关（#62 已改成 tex=0 的纯色 polygon），也不是 fade 那笔 ⇒ **另属别的批次**。
- 取证起点：`.tmp/d3d9/a5-artifact.md` + `art-draw.log` / `art-debug.log`（A5 已建立的日志口径可复用）。

## 现在要回答
哪几笔 `[draw]` 产生右侧条纹与左下散点？判据 = 批次目标矩形 / `uv=` 范围 / `px` 数与帧图位置**至少两项吻合**。
候选（按可能性，**未定案**）：
1. **顶点声明的 UV / 流交错**（`declLayoutOf` / `buildDeclVertices`）——2D 图层是 stream0=POSITION、stream1=TEXCOORD；
2. **UV 超出 [0,1] + `TEXF_WRAP`** ⇒ 整页重复；
3. **sprite 路径**的源矩形 / 变换（host `ID3DXSprite` 合成）；
4. **另一个残留状态**：状态块已修，但 stage/sampler 状态、或 `SetTextureStageState` 的某些位仍可能泄漏。

## 判据
- 同一帧 `--dump-frame` 上条纹与散点**消失**；或能证明"真机也这样"（那时改成结论，不叫修）。

## 未做
- 不靠调参掩盖（⛔ 不改 `TEXF_WRAP` / 过滤来"看起来对"）。
- 不把①的结论重复算一遍（它已由状态块单收口）。
