# 🐞S2 emulator-lift · 保存缩略图 SAVE00.STH 全黑（GetRenderTargetData 的源纹理从未被渲染）

- id: REQ-01M780VKKRFHJEX6VM74DRCYBM
- type: bug
- status: blocked
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- blocked_by: [REQ-01M78BF1FNNH6K8N8GEY6GQYXX]
- repro: graphs/sn0000-save.graph#click 650 320
- tags: [emulator-lift, d3d9, save, thumbnail, black]

## 现象

保存时生成的缩略图 `SAVE00.STH` 是一张 320×180 24bpp BMP，**payload（偏移 54 之后 172,800 字节）全为 0**；
`SAVE00.DAT`（1,048,521 B）正常写出。玩家在保存 / 读取界面的存档列表里看到的就是这张黑图。

## 根因（已判 · 2026-11-13 · Windows x64）

`IDirect3DDevice9::SetRenderTarget` 在 shim 里按 **D3D8 的签名**取参，导致「渲染到纹理」的绑定被静默丢弃。

- 引擎调用点 `0x4A7788 push ecx`（纹理 surface）/ `0x4A7789 push esi`（=0）/ `0x4A778A push eax`（device）/
  `0x4A778B call [edx+94h]` ⇒ stdcall 右→左 ⇒ 实参 **`(RenderTargetIndex=0, pRenderTarget=surface)`**。
- `src/shims/d3d9.cpp:782` 把它们当 `(pRenderTarget, pNewZStencil)` 传给 `d3d9::setRenderTarget`；
  于是读到 `target=0`、`zstencil=surface` ⇒ 走「目标是后缓冲」分支并 `return kOk`，surface 被丢掉、**连告警都没有**
  （`src/runtime/d3d9_pipeline.cpp` 的早退在引用 surface 槽之前）。
- 后果：本线**任何**「渲染到纹理」的绑定都被静默忽略（不只是缩略图）。
- 引擎自己还带错误分支：`0x4A778D cmp eax,esi` / `jz`，失败则打
  "ReadFrameTex エラー：レンダリングターゲット設定に失敗しました"。我们返回 `S_OK` ⇒ 引擎也以为成功。

## 为什么守卫至今全绿

夹具把同一套错约定写死了（`{surface, 0}`）：`tests/d3d9.cpp:823/833/883`、
`tests/d3d9_pipeline.cpp:1342/1410/1423/1461/1464/1485`、`tests/gpu_batch.cpp:356`；
而 `save-graph` 只断言文件存在 + 尺寸（`tests/save_graph.cpp:212-224`）⇒ 断言与缺陷口径一致。

## 修法

1. `src/shims/d3d9.cpp:775-785`：取 `rt_index = call.u32(1)`、`target = call.u32(2)`；
   `rt_index != 0` ⇒ 出声 + `D3DERR_INVALIDCALL`；否则 `setRenderTarget(..., target, 0, ...)`。
   `d3d9_pipeline.cpp` **不用动**（它本来就是 `(target, zstencil)` 语义）。
2. 同案改夹具 9 处 `{surface, 0}` → `{0, surface}`，否则改完反而红。
3. 新增**红转绿**守卫：`save-graph` 补一条 —— `SAVE00.STH` 偏移 54 之后非零字节 > 0（修前必须红）。
4. 顺带：`SetRenderTarget` 日志带 caller EA（先例 `src/shims/d3d9.cpp:748-754`），三站压参约定相同，便于分辨是哪一站。

## 反证条件

- 修好参数后日志出现「目标 = 纹理表面」但 STH 仍全 0 ⇒ 改判 (c)（GPU→guest 读回路径错）。
- 修好后存档期间一次 `SetRenderTarget` 都没有 ⇒ 改判 (a)（引擎没调）。

## 判据

- `SAVE00.STH` 偏移 54 之后的非零字节 **> 0**，且能认出保存时画面；
- 该断言在**修前红、修后绿**；
- 夹具改完 `tests/d3d9.cpp` / `tests/d3d9_pipeline.cpp` / `tests/gpu_batch.cpp` 仍绿。

## 未取证

- 存档那段 `SetRenderTarget` 具体由哪一站发起（三站压参约定相同；日志目前不带 caller EA，修法第 4 条可补）。
- 修好后 GPU 回读是否真把帧写进纹理 arena（预期会，`rasterSyncToGuest` 覆盖 `GetRenderTargetData`）。

## 进展（2026-11-13 · 已落地与未达成的部分）

1. **shim 已按 D3D9 签名修好**：`src/shims/d3d9.cpp:787` 取 `(rt_index, target)`；`rt_index != 0` 出声 + `D3DERR_INVALIDCALL`。
   真跑证据：`目标 = 纹理表面 0x100AC9E0（1280x720）`、`RT0 ← 0x100AC9E0（ret=004A778D）` ⇒ **绑定真的发生了**。
2. **连带缺陷（修对后才暴露，已修）**：`0x4A521B` 那站绑 `GetBackBuffer` 返回的**后缓冲表面对象** ⇒ `color_surface != 0`
   ⇒ 每批 draw 退回 CPU 光栅（`gpu=0 fallback=125`、≈3 s/帧、30 帧 102.0 s）。已在
   `src/runtime/d3d9_pipeline.cpp:2236-2253` **规范化**（身份判据：非纹理表面 ∧ 像素块 == 设备后缓冲 arena ⇒ `target=0`，出声）。
   修后 `draws=6531 gpu=6529 fallback=2`、30 帧 8.6 s。守卫：`tests/gpu_batch.cpp` 的
   `gpu-batch/render-target-backbuffer-identity`；变异两条（D3D8 形读参 ⇒ `d3d9` 红；身份判据换尺寸 ⇒ 新守卫红）。
3. **缩略图仍全黑**（payload 非零字节 = 0）⇒ 缺口在「**纹理当渲染目标 + 读回**」，不是签名、不是绑定、不是文件 IO。
   那 2 批纹理 RT 现由 CPU 光栅回退接手，**而回退也没把像素写进纹理** ⇒ "回退是安全网"不成立（第二例）。
4. 判据"修前红、修后绿"本单**达不成**：`tests/save_graph.cpp` 那条断言已按裁决撤下（只留注释 + 单号 + 读数 printf，
   ⛔ 不 SKIP、不假绿）。落点移到 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 的「GPU 支持渲染到纹理」。
