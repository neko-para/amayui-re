# M2 · D3DX9 Sprite 簇（D3DXCreateSprite → 第一帧）

- id: REQ-01M6MJRC587XWXR325K4N2JXMF
- type: req
- status: doing
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, m2, d3dx9]

`d3dx9_43!D3DXCreateSprite` 从 stub 变**真实现**（guest 可见 `ID3DXSprite`：vtable + refcount 在 guest 堆，
状态块 `'SPR1'` + 批次数组挂 `obj+8`），`Draw` 排批次、`End`/`Flush` **真的**合成像素到设备后缓冲。boot 越过它后
连撞 **`D3DXCreateTexture`（纹理簇）** 与 **`D3DXMatrix*`**（本单一并做掉），最终停在 **`IDirect3DDevice9::SetTransform(+0xB0)`**
（3D fixed-function 流水线 = 下一个大子系统）。
## 实测（2026-11-05 · `--max-insns 900000000`）
- 起点 `unimplemented @ 7FFE009C d3dx9_43!D3DXCreateSprite` / `insns=103,059,311`；现在
  `stopped @ eip=004A1D8B`（COM loud trap：`IDirect3DDevice9 +0xB0`）/ `insns=103,059,958` / 墙钟 ≈34–39 s；
  两次跑 `stop/eip/insns` 全同 ⇒ 确定性
- 已到点：Sprite S_OK → 两个 1280×720 渲染目标纹理 → PerspectiveFovLH / LookAtLH →
  `SetTransform(3/2/0x100)`（PROJECTION/VIEW/WORLD）第一处 trap；**未到 Present** ⇒ 无帧号/帧哈希/非背景像素数
## `ID3DXSprite` 槽表（★ = `.lst` 调用点实测；⚠ = 按 SDK 14 槽布局补的推断）
| 槽 | 偏移 | 方法 | 取证 EA |
|---|---|---|---|
| 2 | +0x08 | Release | ★ 0x4A2BB6 → 0x4A2BBA |
| 3/4 | +0x0C/+0x10 | GetDevice / GetTransform | ⚠ 推断（与下面同一份 SDK 布局） |
| 5 | +0x14 | SetTransform | ★ 0x4A30D3→0x4A30DB · 0x4A789A · 0x49FE1A |
| 6/7 | +0x18/+0x1C | SetWorldViewRH/LH | ⚠ 未取证 ⇒ **loud trap** |
| 8 | +0x20 | Begin | ★ 0x4A30E5→0x4A30EB（参数 **0x10 = ALPHABLEND**） |
| 9 | +0x24 | Draw | ★ 0x48AE6E→0x48AE84（`NormalTexture::DrawSprite` 的转发壳） |
| 10/11 | +0x28/+0x2C | Flush / End | Flush ⚠ 推断；End ★ 0x4A00EC→0x4A00F0 · 0x4A356A→0x4A356E |
| 12/13 | +0x30/+0x34 | OnLostDevice / OnResetDevice | ★ 0x499C80→0x499C84 · 0x4A8634→0x4A8638 |
⇒ 已取证的 7 槽顺序与 SDK 一致；未取证的 4 个按同一布局补（推断，注释里明说）。
## 合成语义
- `Begin(Flags)`：ALPHABLEND 生效；其余位（OBJECTSPACE/BILLBOARD/SORT/DONOT/未定义位）**记录并明确忽略**
  （`kSpIgnoredFlags` + 日志）。Begin 嵌套 / Draw 在 Begin 外 / Flush 在 Begin 外 ⇒ D3DERR_INVALIDCALL。
- `Draw`：**Draw 时**读齐输入、把 `SetTransform` 矩阵作用到四角（行向量 `v'=v*M`，只建模 2D 仿射，非仿射记日志）、
  对纹理 AddRef（除非 `DO_NOT_ADDREF_TEXTURE`）、压进 guest 批次表（64 B/条）。⛔ Draw 不写像素。
- `End`/`Flush`：源 texel（`pTexture=NULL` 时白色）×调制色（`mul255`）→ 目标；包围盒按后缓冲裁剪、
  **源矩形按纹理边界裁剪**；ALPHABLEND ⇒ `out=src*a+dst*(1-a)`（alpha 同因子），否则覆盖；像素中心采样；
  合成后批次清零 + 纹理对称 Release。`End` **幂等**（空批次 ⇒ S_OK，arena 哈希不变）。
- 像素判据（精确值）：`0x80FF0000` 叠 `0x004080C0` ⇒ `0x00A04060`；texel `0xFF804020`×`0x80808080` ⇒ `0x00402010`；
  (-2,-2) 的 4×4 只写 (0,0)..(1,1)；src(2,2,6,6) 在 4×4 纹理上只写 2×2；`SetTransform(2×3+平移)` ⇒ 4×4 变 8×12；
  第二次 End 前后整块 arena 哈希相等。
## 纹理簇（`D3DXCreateTexture` → `IDirect3DTexture9`）
- 为什么做：越过 sprite 后的**下一条** unimplemented 就是它（`.lst` 0x48AD07，`NormalTexture::Create`：
  Usage=1 RENDERTARGET / Pool=0 DEFAULT / MipLevels=1 / Format=0x1A|0x1E）；没有纹理像素，Draw 无从采样。
- 像素在 `SurfaceArena`（guest 空间），`LockRect` 给 guest 指针。槽表 22 槽**实测**：`.lst` 0x48AD18
  `mov edx,[eax+44h]`→0x48AD22 call ⇒ **+0x44 = GetLevelDesc**；0x48AD2A/0x48AD2D 读 desc+0x18/+0x1C 当宽/高
  ⇒ `D3DSURFACE_DESC` 32 B：Format+0/Type+4/Usage+8/Pool+0xC/MSType+0x10/MSQuality+0x14/**Width+0x18/Height+0x1C**
  （desc 基址 `[ebp-24h]`）。实现 #0/1/2、#3 +0x0C、#10 +0x28 GetType(=3)、#13 +0x34 GetLevelCount(=1)、
  #18 +0x44、#19 +0x4C LockRect、#20 +0x50 UnlockRect；其余 = loud trap。
- 只支持 MipLevels=1（⛔ 不假装有 mip 链）。顺手把 7 条 `D3DXMatrix*` + `Vec4Transform`/`Vec3Normalize` 从 gap
  变实现（Multiply/Scaling/Translation/RotationAxis/RotationYawPitchRoll/OrthoLH/PerspectiveFovLH/LookAtLH，
  按定义写、无记忆式展开）。其余 7 条 d3dx 导入仍是 gap（FromFileInMemory / LoadSurfaceFromSurface /
  CreateEffectFromResourceA / mesh / MatrixDecompose）。
## 夹具 + 变异（`tests/d3dx9.cpp` · CTest `d3dx9` · 顶层 ⇒ pure）
- **298 checks / 8 组**：`create+qi+getdevice+release`（QI 两 IID + 未知 IID + GetDevice 同设备 + Get/SetTransform +
  Release 到 0 且 guest 堆回基线）· `draw+end-pixels`（ALPHABLEND/覆盖/裁剪/变换/Flush 保持 Begin/End 幂等/
  空矩形/16bpp 565）· `texture+draw`（GetLevelDesc + LockRect + 纹理×调制色 + 源裁剪 + AddRef 对称）·
  `matrix` · `trap-slot`(+0x18) · `trap-texture-draw` · `trap-null-size` · `trap-bad-device`
- 变异（实测红，改回后 298/298 绿）：A 合成不乘调制色 ⇒ `draw+end-pixels` **24 红**；
  B End 只计数不写像素 ⇒ **34 红**；C（设计判据）ALPHABLEND 退化 ⇒ 半透明用例红；变换不应用 ⇒ 8×12 用例红
## 绿
`ctest` **37/37**（原 36 + d3dx9）· `lift --check` 绿（310 导入 = 实现 **239** / stub **71** / 裸缺口 **0**；
本单前 227/83/0，`sync --write` 重生派生文件）· `requirements --validate` 0 红。
## 当前卡点（保持 doing）
**没到第一帧。** 下一缺口 = `IDirect3DDevice9::SetTransform(+0xB0)`（`.lst` 0x4A1D79 `mov edx,[ecx+0B0h]`
→ 0x4A1D89 call，紧接 State=2/0x100），矩阵来自 `[edi+0A388h]`/`[edi+0A3C8h]`/`[edi+0A408h]` ⇒
**3D fixed-function 流水线**（世界/视图/投影 + 渲染状态 + 纹理采样 + DrawPrimitive）= 新的大子系统，不在本单；
到 Present 还差设备 #41/#42 BeginScene/EndScene 与整条 3D 绘制路径（未测）。
## 没做 / 原因
- `D3DXCreateTextureFromFileInMemory`：没做（需要图像解码；安装树无独立图片，纹素在 `.ALF/.AGF` 里，格式待取证）。
  本单纹理像素由 `D3DXCreateTexture`+`LockRect` 提供；引入 `SDL3_image`/PNG 解码前先问收益，且它不是第一帧前必经点。
- SetWorldViewRH/LH、纹理 GetSurfaceLevel 等：loud trap。3D 路径：没做（见卡点）。
- `docs/plan-m2-2d-text.md` / `SCOPE.md` 归上游，未动。
