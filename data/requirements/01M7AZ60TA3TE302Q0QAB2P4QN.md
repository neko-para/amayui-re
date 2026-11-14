# D3D9 GPU 光栅的屏幕→NDC 用 target 尺寸换算 ⇒ 非全幅视口下几何被二次缩放/平移（视口施加两次）

- id: REQ-01M7AZ60TA3TE302Q0QAB2P4QN
- type: bug
- status: done
- parent: REQ-01M7A74YG08R65NJBFXF7DEJDK
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/clip-planes
- repro: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/clip-planes
- severity: S2
- done_reason: 已修（视口只施加一次）：src/runtime/d3d9_gpu.cpp 的屏幕→NDC 改用本批视口矩形换算（dst->x=(v.x-vp_x)/vp_w*2-1、dst->y=1-(v.y-vp_y)/vp_h*2，vp_* 取 d.state.viewport_x/y/w/h，宽或高为 0 时回落 width_/height_），它与 src/runtime/d3d9_pipeline.cpp 的 toScreen（x=(v.x*invw*0.5+0.5)*viewport_w+viewport_x、y=(-v.y*invw*0.5+0.5)*viewport_h+viewport_y）互为逆 ⇒ 视口只在 SetViewport 定义的那一次（加上后端 SDL_SetGPUViewport 的 NDC→window）被施加一次。诊断 [gpu-ff-tex] 的 v0_ndc= 同步改成同一份换算（否则读数与真正提交的 NDC 不一致，读数会骗人）。判据：tests/d3d9_pipeline.cpp#d3d9p/clip-planes 恢复非全幅视口的像素断言，且两个视图同时绿 —— 视图① vp(16,16,32,32)：(38,26)/(39,39)=in_px、(30,30)=清屏、(44,36)=out_px、(52,38)/(55,24)=清屏、(24,24)=out_px；视图② 全幅 vp(0,0,64,64)：(40,40)=in_px、(30,30)/(0,0)=清屏。反向条款实测：把两行改回 v.x/width_*2-1 与 1-v.y/height_*2 ⇒ CTest d3d9-pipeline-gpu 4 failures，判红像素 (38,26)/(39,39)/(44,36)/(24,24) 读到清屏色，与单里的偏差表逐条一致。为什么本局读数不变：130 帧真游戏 draws 7969 / tri 358551 / skipped 0 与修复前逐位相同，且实测 [draw] 7969 条的 vp= 与 tgt= 全部相等（nonfull_viewport = 0 —— 本局全是全幅视口，换算恒等）⇒ 画面不变是正确解释，不是"改了没效果"，也不是回归。
- tags: [emulator-lift, d3d9, gpu, viewport]

## 判据（机械可检验）

在 `apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/clip-planes` 恢复**非全幅视口**下的像素断言，且修前红、修后绿：

1. 设备 64×64、`SetViewport(16,16,32,32)`、清屏 `kClearColor`；
2. **完全在视口内**的 XYZ 三角形（`x,y ∈ [-0.5,0.5]` ⇒ 屏幕 `(24,40)-(40,40)-(40,24)`）：`pixelAt(38,26) == in_px`、`pixelAt(39,39) == in_px`、`pixelAt(30,30) == kClearPixel`；
3. **右平面切开**的三角形：`pixelAt(44,36) == out_px`（可见部分仍画）、`pixelAt(52,38) == kClearPixel`（被裁部分不画）；
4. `XYZRHW` 的视口外四边形：`pixelAt(55,24) == kClearPixel`、`pixelAt(24,24) == out_px`；
5. **反向条款**（防"把断言挪到全幅视口才绿"）：视口换成 `(0,0,64,64)`、同一批顶点重画 ⇒ `inside` 映射到 `(16,48)-(48,48)-(48,16)`（`x=(ndc.x*0.5+0.5)*64`、`y=(-ndc.y*0.5+0.5)*64`），断言 `(40,40) == in_px`（中心 81 > 斜边 64）、`(30,30) == kClearPixel`（60.5 < 64）、`(0,0) == kClearPixel`。⇒ 两条视口下的断言必须**同时**绿：证明修的是"屏幕→NDC 的换算"，而不是"挑一个视口让断言成立"。

⇒ 一条断言都不许留"只在全幅视口下成立"的例外。

## 现状证据（三条换算，逐字）

1. **屏幕映射用视口**（`src/runtime/d3d9_pipeline.cpp:1321-1322`，`toScreen`）：`out->x = (v.x*invw*0.5 + 0.5)*viewport_w + viewport_x`、`out->y = (-v.y*invw*0.5 + 0.5)*viewport_h + viewport_y`。
2. **后端把屏幕像素换回 NDC 时用 target 尺寸**（`src/runtime/d3d9_gpu.cpp:858-859`，注释自己写"尺寸取**这一帧**的 target"）：`dst->x = v.x/width_*2 - 1`、`dst->y = 1 - v.y/height_*2`。
3. **SDL_GPU 再按视口做 NDC→窗口**（`src/runtime/d3d9_gpu.cpp:964-971`，`SDL_SetGPUViewport`）；scissor 同段被重置成整幅 target（`:972-978`）⇒ **不会**被裁剪救回来。

**算一遍**（`sx,sy` = `toScreen` 的输出）：`x_win = sx*(vw/W) + vx`、`y_win = sy*(vh/H) + vy`。

- 全幅视口 ⇒ 恒等（所以全幅用例全绿、问题被藏住）；
- `d3d9p/clip-planes` 那组（`W=H=64`、`vp=16,16,32,32`）⇒ `x_win = 0.5*sx + 16`：`(24,40)→(28,36)`、`(38,26)→(35,29)`、`(40,24)→(36,28)`、`(44,36)→(38,34)`、`(52,38)→(42,35)` ⇒ 几何向视口中心**压缩一半并偏移**，越靠边偏得越多（整块图层错位，不是像素级容差）。

## 一行修法

`src/runtime/d3d9_gpu.cpp:855-869` 的循环里（`d.state` 带本批视口）改成用**视口矩形**（与第 3 步互逆）：`dst->x = (v.x - vx)/vw*2 - 1`、`dst->y = 1 - (v.y - vy)/vh*2`（`vw/vh` 为 0 时回落 `width_/height_`）。
自检：`x_win = ((dst->x+1)/2)*vw + vx = v.x` ⇒ 任意视口恒等。
⚠ 诊断 `d3d9_gpu.cpp:741-742` 的 `v0_ndc=` 是同一份换算，改实现时一并改；否则读数会与真实提交不一致（读数会骗人）。

## 为什么不是"设计"

1. **D3D9 的视口只施加一次**：`SetViewport` 定义的就是 clip→screen 那一次；仓库自己的 `toScreen` 就这么写，GPU 后端又有一次 NDC→window ⇒ 中间"屏幕像素→NDC"必须是它的**逆**。用 target 尺寸只是 `vw==W` 时恰好成立的近似 —— 把"两次施加"说成设计，等于把**条件成立**当成**定义**。
2. **仓库自己把它写成"现象"**：`src/runtime/d3d9_pipeline.cpp:4503-4504` 逐字"`vp` 与 `tgt` 不等 ⇒ `toScreen` 的视口映射会被 GPU 的 NDC→viewport 再乘一次"——语气是异常，不是"我们故意乘两次"。
3. **"设计"这个说法出现在删断言的同一次改动里**：`tests/d3d9_pipeline.cpp:1300-1302` 改写成"两者只有在 viewport == target 时才互逆 …… 那个几何会被位移，**不适合锁像素**"，并据此删掉 12 条像素断言（`13 → 1`）。同一事实在前者叫"会被再乘一次"，在后者叫"不适合锁像素"——后者是前者的后果，不是独立的设计理由。
4. **用户可见**：真 D3D9 里非全幅视口很常见（分屏 / RT 子矩形 / letterbox 内画）；错位 = 图层与角色位置整体偏，且**不会**被任何现存断言抓到。

## 复现

```powershell
cd apps/emulator-lift
cmake --preset windows-x64                        # RelWithDebInfo（preset 缺省）
cmake --build --preset windows-x64 --target lift-d3d9-pipeline
# ① 今天就能看到的读数（每批 Draw 打 [draw]）：预期 vp=16,16,32,32 tgt=64x64 ⇒ 这就是"两次施加"的输入
$env:LIFT_SLOW_DRAW=1; $env:LIFT_SLOW_DRAW_MS=0
ctest --test-dir out -R d3d9-pipeline-gpu -V
# ② 修好后必须绿的（判据 1-5 落进 d3d9p/clip-planes 之后）。⚠ ①② 都需真 GPU，否则该 CTest SKIP 77
# ③ 游戏侧：现网有没有真的踩到非全幅视口（人工比对 vp 与 tgt 是否不等）
./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install --present headless --native-overrides all --skip-logo --max-frames 600 --log-level info --log-cat d3d9 | tee ../../.tmp/vp/replay.log
```

## 收口锚点（`verify` 候选）

- `apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/clip-planes` —— CTest `d3d9-pipeline-gpu`（`CMakeLists.txt:1021-1023`，顶层无条件 ⇒ pure 档，`SKIP_RETURN_CODE 77`）。**就是本单要加严的那条**。
- 辅助：CTest `d3d9-pipeline`（`CMakeLists.txt:1017`，pure）—— 证明状态 / 视口存取本身没坏。
- ⛔ 不要引 `d3d9/present` / `present-window-gpu`（与视口换算无关）。

★ **`verify` 为什么留空**：该用例今天只留 1 条**全幅下也成立**的断言（绿，但抓不到本缺陷）⇒ 现在写进 `verify` 等于把"缺陷被藏住"当收口凭据。`repro` 挂同一条 = **重开条件**：谁把换算改回 target 尺寸、或把恢复后的断言再删掉，它就该红。修完（判据 1-5 同时绿）再把 `verify` 写成这一条。

## 未测

- 游戏里是否真的出现非全幅视口（`vp != tgt` 的行数）：**未测**，命令见上；`[draw]` 的 `vp=` / `tgt=` 就是为这件事准备的读数。
- 正文里的 `文件:行` 取自 `.tmp/cpu-migration/tickets.md`（2026-11-14 当前工作树），并发改动会让行号漂。
