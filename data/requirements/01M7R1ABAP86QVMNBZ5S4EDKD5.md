# 🐞S2 `ID3DXSprite` 批次丢掉 SRCBLEND/DESTBLEND：加性混合被当 SrcAlpha/InvSrcAlpha ⇒ SAVE05 影片黑底画成 120×120 黑方块

- id: REQ-01M7R1ABAP86QVMNBZ5S4EDKD5
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: apps/emulator-lift/src/runtime/d3dx9_sprite.cpp#gs_state.blend
- severity: S2
- tags: [emulator-lift, d3dx9, sprite, blend, render]

## 现象（用户口径 + 本轮读数）

SAVE05 是 SC0000 第一个地图中的一个场景：点击宝藏（参考坐标 **740,355**）会触发一段**闪光特效**。
实际看到的是：**特效所在位置是一个黑色矩形，而且整个特效出现期间一直是黑的**；特效播完后画面变成交互对话框。
（"没了"= 特效播完、渲染出对话框，不是黑框自己消失。）

## 已核事实（都可再跑；`.tmp/save05-black/report.md`）

- 黑框 **120×120**，跟着宝藏坐标走；它在 **guest 后缓冲**里（`screenshot.source=guest-backbuffer` +
  `hash_still_matches=true`，帧 2857 / 2867 每一张都是）⇒ 不是窗口合成 / letterbox / HUD。
- 同一帧**唯一一条加性批次**（帧 49）：`State=19⇒0x5`（`D3DRS_SRCBLEND=SRCALPHA`）+ `State=20⇒0x2`
  （`D3DRS_DESTBLEND=ONE`），随后 `[d3dx9] End: 合成 1 条 Draw / 14400 个覆盖格`（= 120×120）。
  帧 48（特效前）`DESTBLEND=ONE` 出现 **0** 次，帧 49/50 各 **1** 次。
- 同帧的影片侧读数：`[dshow] MovieDecoder open：80x80 fps=30.000 时长=0.800s`、
  `AddFilter(name="TextureRenderer")`、`[dshow] tex movie #1 w=120 h=120 nonzero=14 sampled=225`
  ⇒ 影片帧被铺进 120×120 纹理，**225 个采样点只有 14 个非零**（几乎全黑，星星形状）。
- 排除项：DDraw 三帧切片命中 0 次；本轮没有 `--overlay` / `--ui`；`b1b6` 置 bit4（关掉 SO033A 导航覆盖图）
  之后**黑框仍在**（帧 604，搜索区黑 11.19% vs A 组 8.91%）⇒ 与那张全屏覆盖图无关。

## 根因（读数 + 代码锚）

`ID3DXSprite` 的批次在本仓走一条**自己的**合成路，它**只从 `Begin` 的 flags 取一个 bool**：

- `d3dx9_sprite.cpp:693` `const bool blend = (s.flags & kSpriteAlphaBlend) != 0;`
- `d3dx9_sprite.cpp:940` `gs_state.blend = blend;` —— **只有这一句**，`src_blend` / `dst_blend` 一个都没赋；
  `rg "kRsSrcBlend|kRsDestBlend" src/runtime/d3dx9_sprite.cpp` = **0 命中**。
- `gpu_raster.h:217-218` 的缺省 = `SrcAlpha / InvSrcAlpha`；fixed-function 路会读那两个状态
  （`d3d9_pipeline.cpp` 的 `kRsSrcBlend` / `kRsDestBlend` 分支）⇒ **两条路的混合语义不一致**。

⇒ 引擎要的**加性**（`SRCALPHA / ONE`）被写成 `SRCALPHA / InvSrcAlpha`：影片黑底（alpha=1）不再"加 0"，
而是把背景**替换成黑**，于是 120×120 的黑方块；那 14 个非零像素就是画面里那颗白星。

## 要做什么

1. sprite 合成路按**引擎实际设的状态**取混合因子（`D3DRS_SRCBLEND` / `D3DRS_DESTBLEND`，
   以及对应的 `*_ALPHA` 与 `BLENDOP`），而不是从 flags 推一个 bool；判据与 fixed-function 路**同一个映射**
   （`gpuBlendStateFor()` 那一份，⛔ 不写第二套）。
2. `Begin` 只要 `ALPHABLEND != 0` 就应**允许**混合（现状），但**因子**必须来自状态；`ALPHABLEND=0` 仍必须关混合。

## 判据（守卫）

- pure CTest（挂 `tests/d3dx9_sprite*.cpp` 或既有的 d3dx9 用例）：自造 `Begin(ALPHABLEND)` + 先设
  `State=19⇒5` / `State=20⇒2`，断言合成出的像素 = 加性结果（`dst + src*srcA`）；把因子退回写死
  `InvSrcAlpha` ⇒ 该断言**当场红**（黑底会盖成不透明）。
- 变异一条：sprite 路改回"只用 flags 的 bool"（`gs_state.blend = blend;` 两句都删）⇒ 上面那条红。
- ⛔ 不要用"SAVE05 里黑框消失"当唯一判据（那是 e2e，不进默认门禁）。

## 复现（本轮真跑过）

```powershell
cd apps\emulator-lift
.\out\emulator-lift-aot.exe --game-dir ..\..\links\install --appdata ..\..\.tmp\save05-black\appdata `
  --control-root ..\..\.tmp\save05-black\ctl --control-orphan-timeout 0 --control-idle-timeout 0 `
  --present headless --skip-logo --audio off --frame-log `
  --input-plan ..\..\.tmp\alpha-flash\load05.plan --time-limit 1800
```

进入 SAVE05 后 `pause` → `move 640 322` → `resume` → `click 640 322` → 再 `resume`；
逐帧截图见 `.tmp/save05-black/shots/b011.png` / `e025.png` / `c010.png`。

## 未定论

- 没有一行日志直接打"这条 sprite 采样的是哪张 texture 的 EA"（120×120 是强相关，不是直读）。
  要钉死需要给 `[d3dx9] Draw` / `End` 加一行 opt-in 读数（坐标 + 纹理对象 + src rect）。
- 影片帧的黑底像素 alpha 是 1 还是 0 未直读（控制层没有"读纹理像素"的原语）；
  但两种取值下"因子被丢掉"都是缺陷（一个会盖成黑、一个会把该有的加性算错）。
