# 🐞S2 emulator-lift · 保存缩略图 SAVE00.STH 全黑（GetRenderTargetData 的源纹理从未被渲染）

- id: REQ-01M780VKKRFHJEX6VM74DRCYBM
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: graphs/sn0000-save.graph#click 650 320
- tags: [emulator-lift, d3d9, save, thumbnail, black]

## 现象
保存时生成的缩略图 `SAVE00.STH` 是一张 320×180 24bpp BMP，**172,800 字节像素全为 0**（纯黑）；
存档数据 `SAVE00.DAT`（1,048,521 B）正常写出来了，只有截图是黑的。玩家在保存 / 读取界面的
存档列表里看到的就是这张黑图。

## 复现
1. 用 `REQ-01M77ZJYSPX5V21QEB89W51815` 的命令跑 `graphs/sn0000-save.graph`（或 CTest `save-graph`）；
2. 检查 `<appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/SAVE00.STH`：
   - `xxd -l 64` 是 `BM` + 320×180×24 的 BMP 头（54 B 偏移）；
   - `python3` 统计 `data[54:]` 的非零字节 = **0**（实测 172,800 / 172,800 全 0）。

## 已测读数（2026-11-12，macOS arm64 / RelWithDebInfo）
- 文件：172,854 B = 54 B BMP 头 + 172,800 B 像素；payload 非零 = **0**。
- 同一次 run 的 D3D 链**都执行了**（`--verbose --log-cat general,file,d3d9`）：
  `CreateOffscreenPlainSurface 1280x720` → `GetRenderTargetData(0x10197510 → 0x1036BA48)`
  → `LockRect(1280×720)` → `CreateOffscreenPlainSurface 320x180` → `GetRenderTargetData(0x1036BAB0 → 0x1036BAC8)`
  → `LockRect(320×180)` → `UpdateSurface(0x1036BAC8 → 0x1036BAB0)`。
  ⇒ 复制/锁/回写的**形状是对的**；黑是因为**源像素本来就是 0**。
- 源 surface `0x10197510` 来自 `D3DXCreateTexture 1280x720 fmt=22`（对象 `0x10196130`）的 level 0
  （`--log-cat d3dx9` 的创建/取级日志）。该 run 的 `SetRenderTarget` 日志里**只有**
  `目标 = 后缓冲（0）`，**没有任何 `目标 = 纹理表面 …`** ⇒ 这块 1280×720 纹理在本线从未被绑成
  render target，所以内容一直是 0。
- 真机侧 `ReadFrameTex`（`sub_4A7210`）本应在 `0x4A778B` 调 `SetRenderTarget(纹理 surface, 0)`
  把当前帧画进去；本 run 没有这条绑定。**根因待查**：是 `ReadFrameTex` 没被调用，还是它的
  `SetRenderTarget` 被跳过 / 失败（日志里没有失败行，`pipeLog` 是 Info 且本 run 已开）。

## 影响
保存 / 读取界面的存档缩略图纯黑；存档数据本身可读（不是数据损坏）。

## 判据 / 重开条件
- **重开条件**：`SAVE00.STH` 的像素不再全 0，且是保存时画面（至少能认出画面内容）。
- 修复后补守卫：CTest `save-graph` 已拿到 `SAVE00.STH`，可断言 payload 非全 0；更严的
  "与保存时帧一致"另定（像素级比对需要先钉住缩放/编码）。
- 当前**故意不写红守卫**（缺陷未修，⛔ 不许把它写成 SKIP / 绿）。

## 待核
- `ReadFrameTex` 的调用点（`0x4A778B` 的 `SetRenderTarget`）是否被走到；若走到，为什么日志里没有。
- 若走到，GPU 路径下纹理 RT 的读回时序（`rasterSyncToGuest`）是否覆盖这条读点。
- 是否与 `GetRenderTargetData` 的源不是**当前** RT 有关（真机允许从任意 RT surface 读；本线
  只在源是当前镜像目标时做 GPU 读回）。
