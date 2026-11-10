# emulator-lift · 固定功能批次的混合 α 恒为 0（纹理 α=0）⇒ 屏幕可见内容全靠宿主 sprite 合成

- id: REQ-01M735AWXRDZKCDT4124BCFF27
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#submitPrimitives
- severity: S2
- tags: [emulator-lift d3d9 alpha texture]

## 一句话
引擎的固定功能批次（60 笔 `DrawIndexedPrimitive` 的 2D 图层 + 2 笔脚本多边形）**混合用的 α 恒为 0**
—— 因为 α 段取的是**纹理**（`MODULATE(TEXTURE, TFACTOR)` 的 arg1=TEXTURE），而 arena 里这些纹理的
α 通道是 0。结果：这些批次**一个像素都落不到屏幕上**，TITLE 画面**全靠宿主的 `ID3DXSprite` 合成**
（`d3dx9_sprite.cpp`，不走固定功能流水线）⇒ 任何"引擎用固定功能画的覆盖层"（渐变到黑、闪白、图层
淡入淡出）在我们这里**全都不可见**。

## 复现（2026-11-11，win32 x64 / RelWithDebInfo，`apps/emulator-lift`）
```bash
LIFT_SLOW_DRAW=1 LIFT_SLOW_DRAW_MS=0 ./out/emulator-lift.exe --game-dir ../../links/install \
  --appdata <私有 tmp> --skip-logo --present headless --raster cpu --audio off \
  --native-overrides all --max-frames 136 --input-script <at frame 130 / click 1190 630>
```
`[draw]` 里的 `fa=<n>:<mean>/<min>/<max>` 是**这一批真正参与混合的 α**（CPU 光栅逐像素统计）：
- 2D 图层批（顶点色 `d0=255,255,255,255`，`texserial=21/22/23`）：`fa=921600:0/0/0`、`fa=41321:0/0/0` …
- 脚本多边形（`d0=0,0,0,α`）：`fa=919601:0/0/0`
- 同一次运行里 `texa=<n>:<min>..<max>`（绑定纹理自身的 α，4 点采样）恒为 `texa=4:0..0`。

## 已核（EA 锚）
1. α 段的 arg 是 **TEXTURE**：`sub_4C19E0`（`0x4C19E0`）逐帧写 `0x4C1A12`=`COLORARG1=TEXTURE`、
   `0x4C1A40`=`ALPHAARG1=TEXTURE`、`0x4C1A57`=`ALPHAARG2=TFACTOR`、`0x4C1A6E/0x4C1A9C`=`COLOROP/ARG2`；
   ⛔ 顶点色只在 `ALPHAARG2/COLORARG2=DIFFUSE` 时才参与（`sub_49E390` 的 `edi==1/2` 分支才那样设）。
2. 纹理 α 真是 0：`texa` 用 `pixel_format::expandChannel(raw, amask)` 在 1/4、3/4 四点采样，
   `amask=FF000000`、`32bpp`、尺寸 `2048x2048` —— 读到的是 arena 里的**真实像素**。
3. 2D 图层现在**能画**（本单之前被 `drawPrecheck` 以"只支持 SetFVF"整批拒绝，60 笔/帧全是
   `D3DERR_INVALIDCALL`）：`DrawIndexedPrimitive` 的顶点声明路已实现（POSITION FLOAT3 在 stream0、
   TEXCOORD0 FLOAT2 在 stream1，去交错成交错缓冲；见 `declLayoutOf` / `buildDeclVertices`），
   现在每帧 60 笔成功并写满全屏像素 —— 但因为 α=0，**屏幕上仍然没有它们**。

## 下一步（按证据强弱）
1. **查 α 是谁写成 0 的**：`native_image_upload`（`src/runtime/native_image_upload.cpp`）对 32bpp 源
   **原样复制第 4 字节**（注释里写明 "32bpp 源：alpha 参数被忽略"）⇒ 若源像素的第 4 字节是 0
   （AGF 里存的是 24bpp + 独立 alpha 掩码），我们就把 0 写进纹理。判据：同一张图在真机上
   `LockRect` 之后前 16 字节的 alpha 值。
2. 若源数据确实是 0 ⇒ 引擎必然在别处把 α 补成 255（例如 `sub_48B090` 的 `alpha<0` 保留分支 / 另一条
   上传路径），本线要跟到那一步。
3. 修好后本单的判据：**同一帧里 `fa` 的均值 ≠ 0**（≥ n/2 个像素的 α > 0），且 TITLE 画面在
   `--raster cpu` 与"只用宿主 sprite"两条路径下**不再逐像素相同**（= 固定功能真的落到了屏幕上）。

## 与其它单的关系
- `REQ-01M72GW002QVRSG5J5WJ0KB1KF`（TITLE→Quit 渐变）的 (B) 是**本单的一个表现**：那两笔脚本多边形的
  顶点 α 斜坡 0→255 之所以看不见，根因同一个（混合 α 取纹理且纹理 α=0）。修本单之前，(B) 不可能靠
  "改 stage arg"修好。
- ⛔ 不要把本单当"渐变单"收口：它影响**所有**固定功能覆盖层。
