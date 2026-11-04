# M2 P1 · DirectDraw 2D 核心

- id: REQ-01M6KJ4XAYFQTXA3YT765JQY8R
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/ddraw.cpp#ddraw/create+qi+release
- tags: [emulator-lift, m2]

## 一句话

在 `apps/emulator-lift` 上把 **DirectDraw 2D 核心**从"没有"做到"有会红的守卫"：
`LoadLibraryA("DDRAW.DLL")` + `GetProcAddress("DirectDrawCreate")` 的**通用动态导出转发**、
guest 可见的 IDirectDraw2 / IDirectDrawSurface3 对象（vtable + refcount）、surface 像素在 guest 内存、
Blt/BltFast/Lock/Unlock/GetDC/Flip/EnumDisplayModes 的语义。

## 做了什么（方法 → vtable 偏移 → 取证）

偏移**逐条**对过 Windows SDK `ddraw.h` 的接口声明顺序，并与 `.lst` 调用点核对（EA 见 `repo:apps/emulator-lift/src/shims/ddraw.cpp` 头注释）：

- IDirectDraw2：QI `+0`（0x43C413，成功后 0x43C45A Release 原对象）· AddRef `+4` · Release `+8`（0x43C44C）·
  CreateSurface `+24`（0x43C9FC）· EnumDisplayModes `+32`（0x43C482，回调 0x43BEA0）· GetDisplayMode `+48`（0x43AF9F）·
  SetCooperativeLevel `+80`（0x44050B）· SetDisplayMode `+84`（0x44057B，**5 参数**，SDK 同）·
  GetAvailableVidMem `+92`（0x43AF78）。
- IDirectDrawSurface3：Blt `+20`（0x43D7A7；0x43D768 push 0x8000 = DDBLT_KEYSRC）· BltFast `+28`（0x43DA58）·
  Flip `+44`（0x43E9A6）· GetCaps `+56`（0x43CB10，**1 参数**）· GetDC `+68`（0x43B478）· GetSurfaceDesc `+88`（0x43B304）·
  Lock `+100`（0x43CC6C）· ReleaseDC `+104`（0x43B4DA）· SetColorKey `+116`（0x43B0C3，flags=8）· Unlock `+128`（0x43C304）。
- 结论：`.lst` 与 SDK 无冲突；plan §3 F3 里"其余按方法序推测"的部分**已全部复核**（没有一条只靠推测）。
- DDSURFACEDESC = **108 字节**（DDSCAPS 只有 1 个 DWORD ⇒ ddsCaps.dwCaps 在 +104）：引擎读 +0x0C/+0x08/+0x54
  （0x440B39）、掩码 +0x58/0x5C/0x60（0x43B323）、写 +0x00/+0x04/+0x68（0x43C9B1）、读 +0x18（0x43AFE7）——逐字段对上。
- 语义：desc 往返（lpSurface = guest 指针 / lPitch / R·G·B·A 掩码）· SetColorKey(8) 后 Blt 键色**不拷贝** ·
  BltFast 双向夹紧且**不越界写**（后一块 surface 当 canary）· Lock 嵌套计数 + 真实 DDERR_* ·
  GetDC ⇄ 同一块 surface 像素（`ddraw::dcInfo`；`ddraw::setFontDcHooks` 是 P2 挂点）·
  Flip → HeadlessPresenter（frameCount/lastHash + 帧号日志）· EnumDisplayModes 用 `GuestCallStack` 真回调 guest ·
  GetAvailableVidMem / GetDisplayMode 返回确定值（虚拟 256/512 MiB；默认 1280×720×32，16bpp 切 565）。

## 未实现的 flag / 方法 / 格式（撞上就 loud trap，⛔ 不静默）

- 空槽（comUnsupported 打 iface+偏移+寄存器后停机）：DirectDraw2 的 Compact/CreateClipper/CreatePalette/DuplicateSurface/
  EnumSurfaces/FlipToGDISurface/GetCaps/GetFourCCCodes/GetGDISurface/GetMonitorFrequency/GetScanLine/
  GetVerticalBlankStatus/Initialize/RestoreDisplayMode/WaitForVerticalBlank；Surface3 的 AddAttachedSurface/
  AddOverlayDirtyRect/BltBatch/DeleteAttachedSurface/EnumAttachedSurfaces/EnumOverlayZOrders/GetAttachedSurface/
  GetBltStatus/GetClipper/GetColorKey/GetFlipStatus/GetOverlayPosition/GetPalette/GetPixelFormat/Initialize/IsLost/
  Restore/SetClipper/SetOverlayPosition/SetPalette/overlay 族。
- 参数侧：Blt 的 `lpDDBltFx` 非 NULL（dwRop / bkColor / 覆盖色键）· Blt/BltFast/Lock/Flip/SetColorKey/EnumDisplayModes
  的未知 dwFlags · 跨 bpp 的 Blt/BltFast · Lock 的 hEvent · Flip 的 back buffer · EnumDisplayModes 的 lpDesc 过滤 ·
  CreateSurface 的聚合（pUnkOuter）· SetDisplayMode 的 8/24bpp（返回 DDERR_UNSUPPORTED + 日志，plan D4 不做）。

## 会红的守卫

- `repo:apps/emulator-lift/tests/ddraw.cpp`（CTest `ddraw`，pure 顶层注册）：8 组 = 动态导出+QI+Release refcount ·
  CreateSurface+QI(Surface3)+GetCaps · SetColorKey+Blt 键色不变（不带 KEYSRC 时会拷）· BltFast 夹紧+canary ·
  Lock/Unlock（pitch/lpSurface/嵌套/DDERR_NOTLOCKED/DDERR_INVALIDRECT）· GetDC/ReleaseDC 配对+HDC⇄像素 ·
  Flip（frameCount+1、两台新机器 lastHash 相同）· EnumDisplayModes（guest 回调 30 条、含 1280×720×32/×16；CANCEL(0) 停）。
- 变异实测（已做、已还原）：键色改成"永远复制" ⇒ blt-colorkey 红 1 条；BltFast 去掉目标侧夹紧 ⇒ bltfast-clip 红 2 条。

## 与 plan 的偏差（以及为什么）

1. **EnumDisplayModes 回调方向**：实现为 SDK/DDraw 语义 **0 = DDENUMRET_CANCEL 停，非 0 = 继续**。取证：引擎回调
   0x43BEA0 → 0x43AFE0 的所有返回路径都是 `mov eax,1; retn 8`（0x43B024 / 0x43B061）⇒ 永远返回 1 = 继续。
   若按"非 0 提前终止"实现，引擎会在第一条模式后停下、把目标分辨率当"不可用"（任务书那句与平台 API 相反）。
2. **back buffer 未建模**：`DDSD_BACKBUFFERCOUNT` 只记日志；`Flip(NULL, WAIT)` 仍呈现 primary。真机全屏路径
   会建 complex primary + 1 back buffer 再 `GetAttachedSurface` —— 本切片不做（撞上会 loud trap，P5a 已知卡点）。
3. **显示模式是虚拟的**：EnumDisplayModes 列 30 条（640×480…1920×1200 × {16,32}），SetDisplayMode 接受任意 ≤16384 尺寸；
   没有真显示器 / 真独占全屏（plan D1 / §10）。Blt 拉伸用最近邻，不做硬件滤波（只做自洽）。
4. **SCOPE.md 未改**：文件头写明"不允许在用户没有明确要求的情况下编辑"；上述三条偏差登记在本单，等用户拍板再进 SCOPE。

## 剩余（超出本切片）

- back buffer / `GetAttachedSurface` / `RestoreDisplayMode` / `WaitForVerticalBlank`（P5a 跑全屏路径会撞）；
  Blt 的 DDBLTFX 族 / DESTCOLORKEY / 跨 bpp。
- P2 接缝（已接通）：`GetDC` 用 P2 的 `gdiCreateDc` 建**真 GDI DC** 并 `fontAttachDcSurface(hdc, arena EA, arena)`
  把它的目标绑到 surface 像素；`ReleaseDC` 调 `fontDetachDc` + 回收句柄。另有 `ddraw::dcInfo(HDC)` 供只读查询。
- **P0 机制债（本次撞到）**：`BlockCache` 的 SMC 清缓存会**连带丢掉 shim 块**（dropBlocks 清 blocks_，
  installShim 不重装）⇒ 写已解码代码页后再调 shim 会 decode-error。夹具用独立代码页避开；真机若把代码解压进
  可执行页再调导入会撞上。
