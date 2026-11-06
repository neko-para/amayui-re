# TITLE Live2D 纹理入口：D3DX header stub + 接口面实测

- id: REQ-01M6QXMAFJRK30B36Z78NQ1YVX
- type: req
- status: doing
- parent: REQ-01M6H6G7FN5BAAJPPQYQEB2M27
- tags: [emulator-lift, m3, live2d, d3dx]

## 一句话
把 TITLE 的 Live2D 纹理入口从导入缺口变成 **header 级 stub**，并实测出"整个模型还需要多少平台接口"：
新增面只有 `D3DXCreateTextureFromFileInMemory` + `IDirect3DStateBlock9` + `IDirect3DVertexDeclaration9`；
之后 45 s 内**不再有未实现导入 / 未实现 vtable 槽**。剩余是语义（PNG 像素解码 + 声明式 Draw），不是新接口。

## 起点 / 现状
- 起点：`unimplemented @ eip=7FFE0064 d3dx9_43!D3DXCreateTextureFromFileInMemory`（`.lst` 0x478398，调用点 `sub_478370`）。
- 现在：同一命令不再停在导入缺口；实跑 45 s 到 `--time-limit`，`called-unimplemented 0`。
- 纹理：3 张 **PNG 2048×2048**（SrcDataSize 2,378,956 / 2,125,096 / 1,811,179）——TITLE 不是"一张纹理"，是三张。
- `IDirect3DStateBlock9`：`sub_4783D0` 每帧 `CreateStateBlock(D3DSBT_ALL=1)`；45 s 内 115 次（每帧一次）。
- `IDirect3DVertexDeclaration9`：`sub_4C1BE5` 建 **1** 次，声明为
  `Stream=0 Offset=0 Type=FLOAT3 Usage=POSITION` + `Stream=1 Offset=0 Type=FLOAT2 Usage=TEXCOORD`；
  随后 `SetVertexDeclaration` 绑定、`SetStreamSource(0/1)` 两路顶点、`DrawIndexedPrimitive`。
- 模型 draw 仍失败（`DrawIndexedPrimitive` 6900 次，全部 `D3DERR_INVALIDCALL`），因为流水线 `drawPrecheck`
  只认 FVF；这不是接口缺失，是 `d3d9_pipeline.cpp` 的语义没接声明。

## 接口清单（本次实测）
| 面 | 需要 | 状态 |
|---|---|---|
| D3DX9 导出 | `D3DXCreateTextureFromFileInMemory` | **stub**：PNG/BMP/DDS 头 → 同尺寸纹理，像素清零 |
| D3D9 设备 | `CreateStateBlock(+0xEC)` | **stub 形状**：对象 + GetDevice/Capture/Apply/Release；不快照/恢复状态 |
| D3D9 设备 | `CreateVertexDeclaration(+0x158)` | **实现形状**：扫描 `D3DDECL_END`，复制到 guest 堆，GetDevice/GetDeclaration/Release |
| D3D9 设备 | `SetVertexDeclaration(+0x15C)` | 接受本模块声明；`SetFVF` 会清声明 |
| D3D9 设备 | `CreateVertexBuffer/CreateIndexBuffer/SetStreamSource/SetIndices/SetTexture/SetSamplerState/SetRenderState/SetTextureStageState/SetTransform/DrawIndexedPrimitive/GetDeviceCaps` | 已在 M2 实现，本次未新增 |
| 纹理对象 | `GetLevelDesc/GetLevelCount/LockRect/UnlockRect/Release` | 已在 M2 实现；本次未调用到新槽 |

## 判据
- `apps/emulator-lift/tests/d3dx9.cpp#texture-from-memory`：合成 7×5 PNG ⇒ S_OK、格式 21、宽高正确、Release 回收；
  未知容器 ⇒ E_FAIL + 出参 0（不 trap）。
- `apps/emulator-lift/tests/d3d9.cpp#vertex-declaration`：3 元素声明 ⇒ GetDeclaration 逐字节回读、GetDevice 同设备、
  Set/Get 往返、非本对象拒绝、SetFVF 清声明、Release 回堆基线。

## 没做 / 下一步
1. **PNG 像素解码**：当前只读 IHDR，纹素全 0；模型即使画出来也是透明/黑。需要引入一个解码器（或只做 zlib+PNG 最小解码）。
2. **声明式 Draw**：`drawPrecheck` 目前对 `kPlVertexDecl != 0` 直接失败；要按声明从 stream 0 取 FLOAT3 位置、
   从 stream 1 取 FLOAT2 UV，再走现有 TEXCOORD/光栅。`IDirect3DVertexDeclaration9` 的状态块已能回读，接上即可。
3. 状态块 `Capture/Apply` 仍为空；TITLE 单模型不依赖恢复，先记着。

## 复现
`apps/emulator-lift/out/emulator-lift --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE --game-dir ../../.tmp/game-logo --appdata ../../.tmp/appdata-video1 --raster gpu --present headless --real-time --time-limit 45 --native-overrides all --log-level info --log-cat d3dx9,d3d9`
- 文档冲突：`docs/03-emulator/platform-effort.md` §3.8 写 "`title` 0 次"（Live2D opcode 足迹），
  与本次实测（TITLE 3 张 PNG + 每帧 draw）冲突；不改该文档，另单复核。
