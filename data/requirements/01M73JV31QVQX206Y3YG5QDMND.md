# emulator-lift · TITLE 右侧条纹/散点：页内容 / alpha 语义 / 合成状态（mip 已排除）

- id: REQ-01M73JV31QVQX206Y3YG5QDMND
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: apps/emulator-lift/src/shims/d3dx9.cpp#uploadRgbaToTexture
- severity: S2
- tags: [emulator-lift d3d9 texture alpha artifact]

## 2026-11-17 三条前提已收口 ⇒ 正文两处旧判断作废
`REQ-01M7A74YG08R65NJBFXF7DEJDK`（退休 CPU 光栅）· `REQ-01M735AWXRDZKCDT4124BCFF27`（固定功能批次 α 恒 0）·
`REQ-01M7AZ65EAE7Q6STW2S0ZNXGM4`（alpha test 整批拒绝）**均已 done** ⇒ 这两句不再成立：
① "唯一差别只剩采样 ⇒ 归 CPU 软光栅"（CPU 光栅已不存在）；② "`gpu: fragments` 偏低 ⇒ GPU 侧 FF 覆盖率
本身还有问题"（少的碎片 = α=0 / alpha test 被整批拒的那批）。
⇒ 今天只剩**右侧条纹与左下散点出自哪几笔 `[draw]`**；⛔ 不拆"GPU 固定功能覆盖率"测量单（疑点已消，另开是空单）。

## 2026-11-14 取证工具已随 CPU 光栅退休（本单仍 open）

正文里的取证链有两处**已经不存在**，⛔ 不要再照抄：
- `LIFT_MIPDBG=1` 与 `rasterTriangle` 的取点诊断 —— CPU 光栅一族已删（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`），
  今天没有任何"在三角形那一刻取 p0/p1/p2"的钩子；
- `.tmp/d3d9/a5-artifact.md` / `art-*.log` 是本地临时件（`.tmp` 不入库，可能已丢）。

**现行可用的取证面**（都在 GPU 侧）：
1. `--present-screenshot <bmp> --present-screenshot-frame <N>` 在 TITLE 落一张 1280×720；
2. `--log-level info --log-cat d3d9`（`[draw]` 行：`tex= / stage0= / blend= / samp= / cw=` 与 `gpu=` 计数）；
3. `unsupported :` 行（bit11 = MIPFILTER 是已知并接受的近似）；
4. `--overlay`（画面上叠 `script` 身份与 worst 帧）用于定位"这一帧是不是 TITLE 稳态"。

⇒ 下一步取证必须是：**在 GPU-only 的今天重截一张图 + 抓那一帧的 `[draw]` 列表**，再判断条纹是"页内容 1:1 细点栅格"
还是"混合/alpha 的产物"。⛔ 不要把旧的 CPU 侧结论（`0.89–1.0 texel/px`、`LIFT_NO_MIP` A/B）当成今天仍然成立 ——
它们证明的是"与 mip 无关"，那个结论仍然有效，但**取证手段**已失效。
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

## 2026-11-11 再次收窄：mip 与该块**无关**（决定性 A/B）
- A14 的"page22 y 被压 2.84×"是**旧几何残影**（2.84 = 2048/720 来自状态块修复前那条泄漏的整页 `uv=0..1` 批）。现在 frame 2 **没有**整页 uv 批。
- `LIFT_MIPDBG=1` 在 `rasterTriangle` 那一刻取 `p0/p1/p2`：frame 1–2 去重 4786 个三角形里，覆盖右侧块 >25% 的 **23 个全是 level 0**，
  实测足迹 **0.89–1.0 texel/px（1:1）** ⇒ **level 0 正是 D3D9 语义要求的**。
- **A/B**：默认 vs `LIFT_NO_MIP=1` ⇒ 右侧块**逐像素差 0/128000**、`row_hf` 两边都 **4.4887** ⇒ mip 选级对该块贡献为 0。
- 公式与闸均无罪（A14 那条几何实测 `lod=1.506 ⇒ 2`，与真实三角形不符；闸对真实批正确地不响）。

⇒ 剩下的解释只有一条：**这层 1:1 的细点网格是页内容/alpha/混合的产物**，不是采样走样。
★ **GPU 参照也不可信**：该块 GPU `mean L 4.81` vs CPU `15.76`、56.9% 像素差、mean|Δch| 11.834 ⇒ 是**内容/低频**差；
且 `gpu: fragments=16662` 对 92 万像素明显偏低 ⇒ GPU 侧 FF 覆盖率本身还有问题（另查）。⛔ 不要拿 GPU 的 `row_hf` 当目标。
（**2026-11-17 作废**：偏低就是 α=0 / alpha test 被整批拒的那批，已由 735AWX / AZ65E 收口。）
`REQ-01M73P3GXW22Y467YGYPQSM571`（SdlWindow 漏转发 `rasterRequestDepthClear`）修复后：
- 角色区已回来（`TH6` 0 → 3.954%）；CPU vs GPU 整帧差 **21.9%**、角色区 **15.4%（mean|Δch| 0.714）**；
- **右侧 `(1060,40)200×640` 仍差 56.9%（mean 11.834）**，而 GPU 该块**近乎均匀**、CPU 是 **1px 高 / 2–6px 宽的暗短划**。
⇒ 两臂现在几何/深度/纹理都对齐了，唯一差别只剩**采样**：同一张纹理、同一批几何，硬件采样均匀、我们逐像素交替
⇒ **归 CPU 软光栅的采样/插值**（不再是"共享层"，也不是内容）。← **2026-11-17 作废**：CPU 光栅整族已删（`REQ-01M7A74Y…` done），这句没有对应物了。

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
