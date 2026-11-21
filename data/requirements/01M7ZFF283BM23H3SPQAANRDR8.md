# 🐞S2 ID3DXSprite 采样器按 MSDN 实现：sprite 路不再硬编码 Point+Clamp（与切立绘 2 帧跳无因果关系）

- id: REQ-01M7ZFF283BM23H3SPQAANRDR8
- type: bug
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/begin-sampler-state
- repro: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/sprite-sampler-pixels
- severity: S2
- done_reason: 判据 = MSDN 的 Begin/End/Flush/DONOTSAVESTATE 采样器契约，钉在两条守卫上：纯档 CTest d3dx9 的用例 d3dx9/begin-sampler-state（8 项文档值 + End 恢复含 guest 在 Begin 内改过的项 + Flush 不恢复 + DONOTSAVESTATE 不恢复）+ GPU 档 CTest d3dx9-gpu 的用例 d3dx9/sprite-sampler-pixels（同一 quad 在 LINEAR/POINT 下像素必须不同、设回 LINEAR 必须回到同一值）；变异 kSpriteBeginMagMin LINEAR→POINT 两条当场红（手工实测）。★ 归因边界：它不是用户报的「切立绘 2 帧跳」的根因（修前/修后那 2 帧的位移与 chg_px 都不变），根因另见 REQ-01M7ZFF3CXRB0Y6PJ119DFJZK4。
- tags: [emulator-lift, d3dx9, sampling, gpu]

## 一句话

`ID3DXSprite` 的两条路对同一张纹理用了不同的采样口径：sprite 路硬编码 `Point` + `Clamp` 并**忽略 device 的 `D3DSAMP`**，固定功能 / pipeline 路按 guest 的 `SetSamplerState` 走。按 MSDN 的 `Begin` / `End` / `Flush` / `DONOTSAVESTATE` 契约实现。

## 根因（MSDN 判据，不是"看起来像"）

- MSDN `ID3DXSprite::Begin`：它写 **stage-0 的整张采样器表** —— `ADDRESSU/V=CLAMP`、`MAG/MIN=LINEAR`（caps 支持各向异性时 `ANISOTROPIC`）、`MIPFILTER`、`MAXMIPLEVEL`、`LODBIAS`、`MAXANISOTROPY`；`End` 恢复整块，`Flush` **不**恢复，`D3DXSPRITE_DONOTSAVESTATE` **只写不存**。
- 引擎在每个 `DrawTexture` 的 `Begin` 之后又显式设 `MIN/MAG=LINEAR`、`MIPFILTER=NONE`（EA `0x4A30FB` / `0x4A310C` / `0x4A311D`）⇒ 真机有效采样 = `CLAMP` + `LINEAR/LINEAR` + `NONE`。
- 旧实现一个 sampler 都不写 ⇒ 落到 `GpuSampler{}` 的缺省 `Clamp/Clamp/Point/Point/mip-on`：① `MAG/MIN=Point` 把内容吸到 texel 网格（≤0.5 texel/轴）；② `MIPFILTER` 开 ⇒ 缩小绘制采更粗一级（更糊）。`ADDRESSU/V` 恰好一致，**不是**偏差来源。

## 修法（提交 `2f34f02` / 2026-11-21）

- `repo:apps/emulator-lift/src/runtime/d3dx9_sprite.cpp`：`applyBeginSamplerState` 按文档写 8 项进**设备**采样器表；`End` 整块恢复（含 guest 在 `Begin` 内改过的项）、`Flush` 不恢复、`DONOTSAVESTATE` 只写不存；状态块 `140 → 176 B`；`compositeBatch` **每批读一次** device stage 0（`readSamplerState` + `gpuSamplerFor`），替掉硬编码的 `Clamp/Point/Point/mip-on`；表达不了的采样器记账并跳过**带纹理**条目（1×1 白纹理不跳）。
- `repo:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp`：新增 `readSamplerState` / `writeSamplerState` / `gpuSamplerFor`（复用 `gpuAddressFor` / `gpuFilterFor`，`mipmap = MIPFILTER != NONE`）；D3DX 内部的写**不冒充** guest 写（不记账 unsupported）。约定只收在这一处。

## 判据（守卫）

- 纯档 CTest `d3dx9` 的用例 `d3dx9/begin-sampler-state`：`Begin` 后 `GetSamplerState` = 文档 8 项、`End` 恢复 pre-Begin（含 guest 在 `Begin` 内改过的项）、`Flush` 不恢复、`DONOTSAVESTATE` 不恢复。
- GPU 档 CTest `d3dx9-gpu` 的用例 `d3dx9/sprite-sampler-pixels`：同一 quad 在 LINEAR / POINT 下必须给出**不同**像素，显式设回 LINEAR 必须回到同一值。
- 变异实测（**手工**，未进 `repo:tools/mutate-check.mjs`）：`kSpriteBeginMagMin` LINEAR→POINT ⇒ pure 8 FAIL / GPU 5 FAIL。读数（win32 / 2026-11-21）：`ctest -R "d3dx9|d3d9|gpu-raster|present"` 17/17；`lift-d3dx9` pure 953 checks / GPU 232 checks，0 fail。

## 归因边界（★ 必读：⛔ 不要与本轴的下一条混成一条）

**本修复不是**"用户报的切立绘 2 帧跳"的根因。实测（SAVE08→SC0010，判据窗 `x[300,1100) y[80,500)`；`chg_px` = 与上一帧在窗内 `|ΔRGB|max>8` 的像素数；亚像素 = 双线性配准）：

- 修前 / 修后那 2 帧的位移 `(−0.48,−0.44)` → `(−0.48,−0.46)` px、进入/离开的 `chg_px` `49414/49055` → `49404/49045` **都没变**（异常帧只是整体挪了 1 帧，帧号随墙钟漂）。
- 锐度平稳帧 `17.05 → 15.94`、异常帧 `11.08 → 9.99`（sprite 路现在真的用 LINEAR，是预期变化），**双峰仍在** ⇒ 残余在 `ID3DXEffect` 的 `Fade` / RTT 路 = `repo:docs/03-emulator/defects.md` 的 **BUG-38**，其需求单 = `REQ-01M7ZFF3CXRB0Y6PJ119DFJZK4`（半像素，本批同批登记）。

## 复现（能照抄）

```bash
cd apps/emulator-lift
ctest --test-dir out -R '^d3dx9(-gpu)?$' --output-on-failure
```

场景对照（可选）：配方 `.tmp/jitter/save08-sc0010.plan`（⚠ gitignored、**不在仓内**，是当时取证件）；命令逐字见 `repo:docs/03-emulator/defects.md` 的 BUG-37 段（`--dump-frame` + `--frame-log` + env `LIFT_GPU_SUBMITS=1`）。
