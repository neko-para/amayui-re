# emulator-lift · D3D9 GetDeviceCaps 的声明与实现对齐（清假声明位 + 行为见证守卫）

- id: REQ-01M79PP97ZD65BJG9GMAJ08MJ3
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/caps-claims
- tags: [emulator-lift, d3d9, caps]

## 缺口（索引 `REQ-01M7485DCS1RBQSDDH6F68HS1J` 第 2 行拆出）

`GetDeviceCaps` 的若干位声明了本线**只能响亮拒绝**的能力。最硬一条：`LineCaps` 声明 TEXTURE，
而 `tests/d3d9.cpp` 的 `d3d9/line-textured-loud-reject` 证明带纹理的线 = `D3DERR_INVALIDCALL`。

★ **纠正索引单**：`Caps`/`Caps2` 恒 0 **不是缺口**（`Caps` 只有 READ_SCANLINE 一位；`Caps2` 那几位本线都没有）⇒ 0 是真话，⛔ 不要"补"。
真正的假声明集中在 `src/runtime/d3d9.cpp` 的 `fillCaps`（LineCaps=0x1F / RasterCaps 注释写 ZTEST 实为 ZFOG / ShadeCaps.FOGGOURAUD /
TextureCaps.MIPCUBEMAP / StencilCaps=0x1FF / MaxUserClipPlanes=6 / MaxVertexBlendMatrices=4 / MaxPointSize=64 / MaxAnisotropy=4 /
MaxVolumeExtent=256 / CursorCaps / PresentationIntervals.ONE 等，逐位见 `.tmp/d3d9-left/brief.md` §1）。

## 判据（不抄数字）

- 每个置位必须有一条**行为见证**：声明 stencil ⇒ `STENCILENABLE=1 + STENCILFUNC=NEVER` 的 draw 必须 0 像素；
  声明 ZFOG/FOGGOURAUD ⇒ `FOGENABLE=1` 像素必须变；声明 LineCaps.TEXTURE ⇒ 带纹理线必须 S_OK；
  `MaxUserClipPlanes>0` ⇒ 半平面 clip 必须裁掉一半。
- 边界集合 `ZCmpCaps/SrcBlendCaps/DestBlendCaps` **机械生成**（对枚举值试探，S_OK 的集合就是该有的位），⛔ 不抄 0xFF/0x3FF。
- 守卫 `d3d9/caps-claims`（`tests/d3d9.cpp`）：修前红（LineCaps.TEXTURE 见证拿到 INVALIDCALL / StencilCaps 见证画出像素 / FOG 见证像素不变）。
- 变体：LineCaps 改回 0x1F、StencilCaps 改回 0x1FF、RasterCaps 改回 ZFOG ⇒ 该用例红。

## 已裁决

caps 没有"拒绝"动作 ⇒ **不能表达就清 0**；能力使用侧仍留 `recordUnsupported` 的拒绝。
CPU 点/线退休后 `LineCaps` 的 0x0E 必须归 0（写进 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`，本单不预支）。
同批删 `d3d9_pipeline.cpp` 的 `noteLinePointCapsOnce` 里"本单不许改 d3d9.cpp"的道歉注释。
