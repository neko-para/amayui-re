# emulator-lift · 固定功能批次的混合 α 恒为 0（纹理 α=0）⇒ 屏幕可见内容全靠宿主 sprite 合成

- id: REQ-01M735AWXRDZKCDT4124BCFF27
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/texture-from-memory-png
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#submitPrimitives
- severity: S2
- done_reason: 2026-11-13 收口（用户确认）。根因已于 2026-11-11 上移：三张 2048² 图集页是 Live2D 图集，由 D3DXCreateTextureFromFileInMemory 创建，而该导出当时是「只读文件头、不解码像素」的 stub ⇒ 纹理全 0（不是「α 通道为 0」，RGB 亦为 0）。修复落在子单 REQ-01M738VYG0DP3TNJKYDSA49J7H（PNG 解码 + 上传，done），本单 verify 沿用它的机械守卫（覆盖根因：解码后像素非全 0）。证据：用户 2026-11-13 确认 **Live2D 渲染正确** ⇒ 图集页确有像素、α 确实参与固定功能批次混合。两点保留：① 本单原拟守卫「stage0 纹理从未被写入 ⇒ 拒绝记成可见」未落地，机械守卫目前只覆盖根因（PNG 解码），不覆盖「未写入纹理仍被记成可见」这一面；② 本单判据依赖的 [draw] texa/fa 只在 CPU 光栅路径统计，按 REQ-01M78BF1FNNH6K8N8GEY6GQYXX 的裁决该诊断须迁到 GPU 路径，否则判据面会随 CPU 光栅退休一起消失。
- tags: [emulator-lift d3d9 alpha texture]

## 一句话
引擎的固定功能批次（60 笔 `DrawIndexedPrimitive` 的 2D 图层 + 2 笔脚本多边形）**混合用的 α 恒为 0** ⇒
一个像素都落不到屏幕上；TITLE 画面**全靠宿主的 `ID3DXSprite` 合成** ⇒ 任何"引擎用固定功能画的覆盖层"
（渐变到黑、闪白、图层淡入淡出）在我们这里都不可见。

## 2026-11-11 根因更新（**推翻**本单原来的 α 假设）
原假设 = "纹理被正确上传、只是 α 通道为 0"。实测**推翻**：那三张 `texserial=21/22/23` 的
`2048×2048 A8R8G8B8` 纹理**建了但一个像素都没写进去**，α=0 只是 arena 初值 ⇒ **RGB 也是 0**。
⇒ 把 α 补成 255 只能把"透明"变成"纯黑"；改 8/24/32bpp 的 convert 也不对（与本单正交，且 196 checks 正因它们而绿）。

读数（本机 `.tmp/q2/run23.log`，16.5 MB，`--verbose`；父级已复核）：
- `CreateTexture` = 23 行 · `LockRect` = 17 行；
- 三张 2048² 的像素块 `0x2BA747C0` / `0x2CA747C8` / `0x2DA747D0` 在整份日志里**各只出现 1 次**（= 它们自己的 CreateTexture 行）；
  落在 2048×2048 上的 `LockRect` = **0**；
- `GetSurfaceLevel` / surface 的 `LockRect` / `UpdateTexture` / `StretchRect` 调用数**全 0**；
- arena 初值：`surface.cpp` 的 `heap_.alloc(bytes, true)` 清零 ⇒ 没写过 = 全 0 字节；
- run28（4301 条 `[draw]`，帧 2..71）：`texa_mean>0` 的行数 = **0**；`fa_mean>0` 只有 1 行（`texserial=0` 的电影四边形）。

⇒ 缺陷从"α 通道"上移成"**只建不写**"：纹理创建之后没有任何填充步骤（既不是渲染进纹理，也不是从 surface 拷）。

## 已核（EA）
- 32bpp 源上传**不读** a4、α 取源像素第 4 字节：`0x48BEC0 / 0x48BECB / 0x48BEE3 / 0x48BEE7`；
  a4 只在 8bpp（`0x48B49F / 0x48B4E4`）与 24bpp（`0x48B6E7 / 0x48B735 / 0x48C088`）用。
- ACIF 平面链：`0x49E9D0` → `0x49EC38`（Alphadata ctor `0x4015E0`）→ `0x49EC58`（头校验 `0x401610`，magic `0x401655`）
  → `0x49EC7B`（`0x4016C0`）→ `0x49EC84`（`[ebx+1Ch]`=α 数组）→ `0x49ED2B`（`[eax+1Ch]`=slot `+0x1C` UploadImage）
  → `0x49ED33`（push key/alpha/img; call）——本单这三张纹理**没走到**这里。
- 我们的 native `sub_48B090` override 与 guest 在 8/24/32bpp 三条分支上**逐分支等价**（已知非 α 差异：
  4/1bpp、16bpp 目标、`key≠0` 回退、尺寸不等拒绝、`key==0` 少一次全 0 比较）⇒ 不是它写坏的。

## 下一步（2026-11-11 已定案，改为落地）
1. ~~定位创建者~~ **已定案**（子单 `REQ-01M737BDE3NXPPAF7TNTJ1C4DS`，done）：三张 2048² 是 **Live2D 图集页**，
   由 `D3DXCreateTextureFromFileInMemory` 建（`0x478398`），而该导出是"**只读文件头、像素不解码**"的 stub
   （`shims/d3dx9.cpp:884-890`）⇒ 建出来就是全 0。**RTT / `D3DXLoadSurfaceFromSurface` / DividedTexture 三条方向全部排除**。
2. 落地单：`REQ-01M738VYG0DP3TNJKYDSA49J7H`（补 PNG 解码 + 上传，像素走既有 lock/unlock 路径）。
3. 修好后本单判据：同一帧 `[draw]` 的 21/22/23 行 `texa` mean>0 **且** 覆盖区 RGB 不全黑；`fa` 均值 ≠ 0。

## 守卫（修前必须红）
- pure CTest（`tests/d3d9_pipeline.cpp`）：A8R8G8B8 纹理**不写**就绑 stage0 + 提交全屏四边形 ⇒
  批次诊断必须报出"stage0 纹理从未被写入"并拒绝把它记成可见。
  ⛔ 不要断言 `texa_mean==0`（那是当前取值，AGENTS.md §10 禁）。

## 与其它单的关系
- `REQ-01M72GW002QVRSG5J5WJ0KB1KF`（TITLE→Quit 渐变）的 (B) 是本单的一个表现；修本单之前 (B) 不可能靠"改 stage arg"修好。
- ⛔ 不要把本单当"渐变单"收口：它影响**所有**固定功能覆盖层。
