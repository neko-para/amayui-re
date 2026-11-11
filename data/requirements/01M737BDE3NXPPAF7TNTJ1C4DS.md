# emulator-lift · 定案 2048² 纹理（serial 21/22/23）的创建者与填充路径

- id: REQ-01M737BDE3NXPPAF7TNTJ1C4DS
- type: spike
- status: done
- parent: REQ-01M735AWXRDZKCDT4124BCFF27
- done_reason: 定案：三张 2048² 是 Live2D 图集页，由 D3DXCreateTextureFromFileInMemory 建立，而该导出在本线是'只读文件头、像素不解码'的 stub ⇒ 全 0。（EA 0x478398 / 0x4A1970 / 0x427CF0 / 0x48AD07；机械判据 run8.log 的 20 条 CreateTexture 无 2048²、紧随 3 条 FromFileInMemory。）instrument 不需要（ret_ea 方案无效：20/20 都是 0048AD0C）。落地另开单。
- tags: [emulator-lift d3d9 texture spike]

## 结论（2026-11-11 定案）
三张 `2048×2048 A8R8G8B8 / Usage=0 / Pool=1 / Fmt=21` 是 **Live2D 图集页**，由 **`D3DXCreateTextureFromFileInMemory`** 建立；
它们的填充**本应在 D3DX 内部完成**，而我们的那条导出是"**只读文件头、像素不解码**"的 stub
（`shims/d3dx9.cpp:884-890` 自注"纹理内容解码是下一层"）⇒ 建出来就是一块全 0 的 arena，**RGB 与 α 都是 0**。

## 证据（EA）
- `0x478398`（∈ `sub_478370`）：`D3DXCreateTextureFromFileInMemory` 的**唯一** Live2D 调用点（返回地址 `0x47839D`）。
- `0x4A1970`：SetFilePointer → GlobalAlloc → ReadFile → 交 D3DX → GlobalFree。
- `0x427CF0`：脚本 **opcode 0x345**，失败串「L2Dテクスチャファイル %s の読み込みに失敗」。
- `0x48AD07`：全引擎**唯一** `j_D3DXCreateTexture` 调用点（在 `NormalTexture::Create` 里）⇒ 这三张**不**走它。
- 机械判据：`run8.log`（`--trace-shims`）里 `D3DXCreateTexture` 20 条、尺寸 `00000800,00000800`（2048²）**0 命中**；
  紧随其后 3 条 `FromFileInMemory`（缓冲 `0x244CCC / 0x206D28 / 0x1BA2EB` = PNG 量级，出参是同一对象连排的 3 个槽）
  ⇒ 同一 Live2D 模型的第 0/1/2 页。
- **2048 不是常量**：该导出签名里没有尺寸参数，宽高由 shim 从 **PNG IHDR** 读（`d3dx9.cpp:921-922`）——这是数据值。

## 被否掉的候选（别再走）
- `0x4A2C10`（`0x1F8`）· `0x49FB60`（CFrameTexture，1 调用方 `0x4A548E`）：尺寸/调用形状对不上；
- `0x439200`：全函数仅 3 出口，bpp=32 必到 `0x439612` 的 `LockRect`；
- `0x48B090`（UploadImage）：内部创建后必然 lock；
- **RTT / `D3DXLoadSurfaceFromSurface` / DividedTexture 三条修法方向全部不成立**（`Pool=1 MANAGED` 也不能当 RT）；
- **instrument 不需要**：`a3` 的 `call.ret_ea` 方案无效（20/20 条 `CreateTexture` 的 ret 都是 `0048AD0C`，全引擎只有一个调用点）。
  零改动复核 = 加 `--trace-shims` 跑一次：`FromFileInMemory` 恰好 3 条、`ret=0047839D`、紧跟在最后一条 `D3DXCreateTexture` 之后。

## 下一步（已另开单）
补 `shimD3DXCreateTextureFromFileInMemory` 的 **PNG 解码 + 上传** ⇒ 见父单正文指向的落地单。

## 未做 / 待核
- 三张 PNG 在 ALF 里叫什么（只有成员偏移与大小）。
- 真机 `MipLevels=D3DX_DEFAULT`（我们传 1）——保真差异，与本单成因无关。
- Live2D 绘制侧（`0x4AF459 → 0x49E37B`）未查，归父单。
