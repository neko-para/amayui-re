# 纹理影片 = 真 DirectShow 服务端（实现契约）

> 目标：把 `src/shims/quartz_video.cpp` 的「零长影片形状层」换成**真服务端** —— 宿主提供
> DirectShow 的**图管理器 + 源/解码侧的 pin**，把解码帧送到引擎**自己的**渲染器，让它写进纹理。
> **证据口径**：锚只锚 EA（`corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst`），引用一律 `EA + 助记符摘录`；
> `.c`（Hex-Rays 视图）只标「阅读位置」，**不当锚**。`g:` = guest（引擎自带代码），`h:` = 宿主实现。
> **置信度**：`★` = 语料逐字读过；`◇` = 推论（有间接证据）；`?` = 没拿到，进 §6/§7。
> 本文只写**宿主契约**，不写引擎语义（后者走 `AGENTS.md` §6 准入门）。

## §0 一行结论

最小协议 = 宿主拿出 **① 一个图管理器**（`IGraphBuilder`+`IMediaControl`/`IMediaPosition`/`IMediaEvent`/`IBasicAudio`）
和 **② 一个宿主侧"解码 filter"**（输出 pin 用标准 COM，输入侧自己读文件/解码 MPEG-1 PS）；
`g:` 的 `CStreamReader`（源）与 `CMovieTextureRenderer`（渲染器，含输入 pin）**不实现**，只被宿主**调用**：
`AddFilter` 记账 → `Render` 时把 `h:` 解码 filter 的输出 pin 接到 `g:` 渲染器输入 pin（`ReceiveConnection` + `SetMediaType`
+ `NotifyAllocator(h:alloc, FALSE)`）→ 两个 filter `Pause`/`Run` → 每拍 `Receive` 一帧 → 位置按引擎读法返回。
做到这一层就有画面；`IBasicAudio::put_Volume` 只需记账。**引擎不看 `Render` 的 HRESULT**，所以"接线失败"不会被引擎发现，
但也就没有帧 —— 判据只能是"纹理里出现像素"（§6-1）。

★ **三处复核修正**（与先前口述事实不符，均有语料证据）：

1. `sub_487270`（`WaitForCompletion(10ms)`）与 `sub_487530`（IVideoWindow）**不属于** `CMovieToTexture`：两者都是
   **`MovieIVideoWindow`** 的虚函数（取址证据 `data:??_7MovieIVideoWindow@@6B@` @`0x5281CC` / @`0x5281E4`，0 个直接 call）。
   它们属 **AMultiMediaStream 路径**（RTTI `Movie` / `MovieIVideoWindow`，`.data:0x53EB00` 一带）。本任务的 DShow 路径
   **不轮询 `IMediaEvent`**，每拍只读位置（§1-5）。
2. `0x52FB98` **不是 `IID_IMediaSeeking`**：字节 `B3 68 A8 56` ⇒ `56A868B3` = **`IID_IBasicAudio`**；它被写进 `[this+0x450]`
   （`0x48965E`），而 `0x487A2F` 对该指针调 `[vtbl+0x1Ch]`（IDispatch 派生 ⇒ `put_Volume`），入参是 `sub_4879E0`
   算出的 dB 值（0 ⇒ `-10000.0`，`dbl_527D20`；否则 `log` 换算，`0x4879F5`/`0x487A00`）。☆ 高置信。
3. 渲染器的 `SetMediaType` **不看** `biBitCount` / `biCompression` / `biSizeImage`：只比 **subtype** 是否等于
   `0x52FAD8`（`MEDIASUBTYPE_ARGB32`）来决定"4 字节/像素 + `w*4` 行距"还是"3 字节 + `(w*3+3)&~3`"（§3-2）。

## §1 引擎侧调用序列（每步带 EA）

| # | 步 | 证据（EA + 摘录） | 说明 |
|---|---|---|---|
| 1 | 建影片对象 | `0x489040` `sub_489040 proc near`（`operator new(0x480)`，`0x4237B0` @阅读位置 `.c:31355`） | `CMovieToTexture` 对象 **0x480 字节** |
| 2 | `Init` | `0x488DC0`；`0x488E4B call CoCreateInstance`（`rclsid`@`0x52FB18`，`riid`@`0x52FD08`，`dwClsContext=1`）→ `[+0x40C]`；`0x488E89` 起 `[+0x440]=1` | `0x52FB18` = `E436EBB3-…` = `CLSID_FilterGraph`；`0x52FD08` = `56A868A9`，失败串自证 `IID_IGraphBuilder`（`0x488E56 push offset aInitIidIgraphb_0`） |
| 3 | QI 三件套 | `0x488EA6 call edx`（`0x52FB88`→`[+0x410]`）· `0x488EC7 call eax`（`0x5284B4`→`[+0x414]`）· `0x488EE8 call eax`（`0x5284C4`→`[+0x418]`） | 三个 IID 的失败串分别自证 `IMediaControl`/`IMediaPosition`/`IMediaEvent`；模式 `[+0x43C]`：0=DShow、1=MF（`0x488EFD cmp eax,1`） |
| 4 | `Open`：建渲染器 | `0x489230`；`operator new(0x5C0)` + `0x488950`（ctor）→ `[+0x420]`；`0x4888D0` 把图指针写进渲染器、`0x488910` 把 `[+0x434]` 的方法返回值写进去（含义 ?） | `g:` `CMovieTextureRenderer`（`: CBaseVideoRenderer : CBaseRenderer`） |
| 5 | `Open`：`AddFilter(渲染器)` | 图 `[+0x40C]` vtable **+0x0C**，名字 `L"TextureRenderer"` | 图 vtbl+0x0C = `IFilterGraph::AddFilter`；**没取到精确 call EA**（与 #8 同一段） |
| 6 | `Open`：读文件前 12 字节 | `CreateFileA` 路径来自归档文件表（`sub_4559C0`：`80*id + this[261]` + `CreateFileA`）；`ReadFile(…,0x0C,…)` + `strstr("AVI")`（阅读位置 `.c:105653-105654`） | 选 subtype：AVI ⇒ `0x52FAF8`，否则 `0x52FAE8`（§3-1） |
| 7 | `Open`：建源 | `0x489503 operator new(0x28)` → `0x489527 call sub_486420`（`CStreamWapper`，包住 `g:` 文件句柄）→ `[+0x454]`；`0x489535 new(0x1E8)` → `0x48956A call sub_485F00`（`CStreamReader` ctor，参数 `stream, &mt, &hr`） | 源 filter = `CStreamReader : CAsyncReader : CBaseFilter`（RTTI `??_2CStreamReader@@8` @`.data:0x53EA88`） |
| 8 | `Open`：`AddFilter(源)` | `0x48961D call edx`（图 vtbl **+0x0C**，`pName=0`） | 之前 `0x489574` 对 `[edi+0xC]` 做 `AddRef`（`IBaseFilter` 子对象在 **+0xC**） |
| 9 | `Open`：**`Render(pin)`** | `0x48962B call edx` 取 pin（filter 主 vtable **+0x1C**，返回 `CBasePin*`，其 **+0xC** 即 `IPin*`）→ `0x489645 call eax`（图 vtbl **+0x30** = `IGraphBuilder::Render`） | **返回值被丢弃**：`0x489647` 直接 `mov ecx,[ebx]; …call [edx+8]`（`Release`） |
| 10 | `Open`：`QI(IBasicAudio)` | `0x489666 call eax`（`0x52FB98`→`[+0x450]`），失败走 `0x48967B push offset aOpenQueryinter_0` | 见 §0-2 |
| 11 | `Play` | `0x4883A0`；`0x4883C7 call edx`（`IMediaControl` vtbl **+0x1C** = `Run`）；`[+0x448]=arg`、`[+0x468]=1`、`[+0x444]=arg&1`（**循环标志**） | `IMediaControl : IDispatch` ⇒ Run/Pause/Stop/GetState = +0x1C/+0x20/+0x24/+0x28 |
| 12 | 每拍（主循环内） | `0x41266E call sub_408130`（`IMediaPosition` vtbl **+0x24**，`fstp [ebp-77Ch]`）· `0x412680 call sub_4080B0`（**+0x28**）· `0x41268D test ah,41h` / `0x412690 jp`（比较） | 见 §2 的 `+0x28` 形态告警 |
| 13 | 到点 ⇒ 循环 or 结束 | `0x41269D cmp dword ptr [ecx+444h],0`；`0x4126A6 call sub_488420`（Stop）→ `0x4126BA call sub_4081B0`（`SetPosition(0.0)`）→ `0x4126C8 call sub_4883A0`（再 `Run`） | 循环片：Stop→seek 0→Run；非循环：另一分支（`0x4126D3 call sub_488FB0`） |
| 14 | 收尾 `Stop` | `0x488420`：`[+0x468]!=0` ⇒ `0x48844C call sub_408130`（存 `[+0x460]`）→ `0x488463 call edx`（`IMediaControl` vtbl **+0x24** = `Stop`） | 每拍/关闭都会走 |
| 15 | 渲染（每帧） | `0x487C60`（`DoRenderSample`）：`0x487CCE call eax`（`IMediaSample` vtbl **+0x0C** = `GetPointer`）→ `0x487D59 call eax`（纹理 vtbl **+0x4C** = `LockRect`）→ 写像素 → `0x487FF9 call edx`（**+0x50** = `UnlockRect`） | 见 §3-3 |

**两条并行路径**（别混）：`CMovieToTexture` = 本任务（DirectShow）；`Movie`/`MovieIVideoWindow` =
`IAMMultiMediaStream`（`0x486715`/`0x487455` 的 `CoCreateInstance`，用 `IMediaSeeking` `+0x38 SetPositions` `0x487169`、
`+0x30 GetCurrentPosition` `0x487256`、`WaitForCompletion` `+0x24` `0x487292`、`IVideoWindow` `0x487530`）。

## §2 宿主必须实现的 COM 方法表

「能否同步完成」= 宿主方法可否**当场返回**（不等待）。全部必须是**同步**（见 §5）。

### §2-A 图管理器（`h:`）

| 接口 | 方法（vtbl 偏移） | 引擎调用点 EA | 期望行为 | 同步 |
|---|---|---|---|---|
| `ole32!CoCreateInstance` | `CLSID_FilterGraph` | `0x488E4B`（另有 `0x487455`、`0x486715`） | 返回图对象；`riid=56A868A9` | ✅ |
| `ole32!CoCreateInstance` | **`CLSID_MemoryAllocator`**（`0x52FB38` = `1E651CC0-B199-11D0-8212-00C04FC32C45`） | `0x4CFB95`（`sub_4CFB80` ← `0x4D1250` `CBaseInputPin::GetAllocator`） | 返回 `IMemAllocator`（§2-C） | ✅ |
| `IGraphBuilder` | `AddFilter`（+0x0C） | `0x48961D`（源）；渲染器那次**没取到 EA** | 记账；失败即错（引擎查 HRESULT） | ✅ |
| `IGraphBuilder` | `Render`（+0x30） | `0x489645` | 见 §2-D（宿主驱动接线） | ✅（不得阻塞） |
| `IGraphBuilder` | `QueryInterface`（+0x00） | `0x488E97`、`0x488EA6` 前的 `mov edx,[edx]` | 返回 §2-B 的视图 | ✅ |
| `IGraphBuilder` | `AddRef`/`Release`（+0x04/+0x08） | `0x488E9F` 前后、`0x488EC7` | 引用计数 | ✅ |
| — | `Connect`/`RenderFile`/`RemoveFilter` | **不在 DShow 路径**（无调用点） | 可 `E_NOTIMPL` | ✅ |
| `IMediaControl` | `Run`（+0x1C） | `0x4883C7` | 见 §2-E：驱动两个 filter 进 Run | ✅ |
| `IMediaControl` | `Pause`（+0x20） | 未取到调用点（`?`） | 同 Run（`Pause` 态） | ✅ |
| `IMediaControl` | `Stop`（+0x24） | `0x488463` | 停泵 + 两 filter `Stop` | ✅ |
| `IMediaControl` | `GetState`（+0x28，2 参） | `0x4870BE`/`0x4870E0`（**Movie 路径**，◇ 未自行复核） | 返回 `State_*` | ✅ |
| `IMediaPosition` | `get_CurrentPosition`（+0x24，出参 `double*`） | `0x40815B`（←`0x41266E`/`0x48844C`） | 写当前位置（秒，`REFTIME`） | ✅ |
| `IMediaPosition` | **+0x28（出参 `double*`）** | `0x4080D3`（←`0x412680`/`0x488FF9`） | 写"结束位置" | ✅ |
| `IMediaEvent` | 全部 | **DShow 路径 0 调用点**（`0x418` 只被存） | 可 `E_NOTIMPL` | ✅ |
| `IBasicAudio` | `put_Volume`（+0x1C） | `0x487A2F` | 记账（dB×100） | ✅ |

★ **`+0x28` 的 ABI 告警（◇）**：按 `control.idl`（`IMediaPosition : IDispatch`），`+0x24=get_CurrentPosition`、
`+0x28=put_StopTime`（要 8 字节 `REFTIME`，**不是**出参指针）；但引擎两处都按 `(this, double*)` 调用并读回
（`0x4080DD fld [ebp+var_8]`、`0x4080B0` 入口 `fldz`+`fst [ebp+var_8]`）。**宿主按引擎的读法填槽**：
`+0x24` 写当前、`+0x28` 写停止；并进 §6-4 用一次探针钉死。

### §2-B 引擎 filter 对象上的**子对象偏移**（宿主调 `g:` 代码时用）

| 对象 | 指针 | 依据 |
|---|---|---|
| `CStreamReader`（源 filter） | `IBaseFilter*` = filter **+0x0C**（`??_7CStreamReader@@6B@_0` @`.data:0x527C6C`，15 槽）；主 vtable @`0x527CAC` | RTTI `IBaseFilter` @偏移 12、`IAMovieSetup` @16（`.data:0x53EAA0`/`0x53EAB0`） |
| 源的输出 pin | `IPin*` = pin **+0x14**；`IAsyncReader*` = pin **+0x00**；`IQualityControl*` = pin **+0x18** | RTTI `??_R2CAsyncOutputPin@@8` @`.data:0x53C434`（`IAsyncReader`@0、`IPin`@20、`IQualityControl`@24） |
| `CMovieTextureRenderer` | `IBaseFilter*` = renderer **+0x0C**（`??_7…@@6B@_0` @`.data:0x52853C`） | 同上（`AddFilter` 用的是 `renderer+0xC`） |
| 渲染器输入 pin | `IPin*`；`IMemInputPin*` 在 pin **+0x98**（152，◇ 由 RTTI 偏移推） | RTTI `??_R1JI@?0A@EA@IMemInputPin@@8`（`.data:0x541650`）；`IMemInputPin` 的 9 槽 vtable @`.data:0x52F53C`（`GetAllocator`+0x0C/`NotifyAllocator`+0x10/`Receive`+0x18，◇ 槽序按 `strmif.h`） |

### §2-C 宿主 `IMemAllocator` / `IMediaSample`（`h:`，被 `g:` 调用）

| 方法 | 触发者 | 要求 |
|---|---|---|
| `SetProperties`/`GetProperties`/`Commit`/`Decommit` | `g:` `CBaseInputPin` / `CBaseRenderer::CompleteConnect` | 只用 1 个缓冲即可（`cBuffers=1`）；**`Commit` 不得等待** |
| `GetBuffer`/`ReleaseBuffer` | `g:` 上游 `Receive` 之后 | **绝不阻塞**（`CMemAllocator::GetBuffer` 会 `WaitForSingleObject` 信号量 ⇒ SCOPE §1 硬错误） |
| `IMediaSample::GetPointer`（+0x0C） | `0x487CCE` | 返回**自底向上 DIB** 像素（§3-3） |
| `GetActualDataLength`/`GetSize`/`SetActualDataLength` | `g:` `PrepareReceive`（主 vtable +0x98） | 长度 = `stride*高` |
| `GetTime`/`SetTime`/`GetMediaTime`（+0x14/+0x18/+0x1C ◇） | 同上 | 时间戳是否被读 **? **（§6-3）；给了就必须单调 |
| `IsSyncPoint`/`IsDiscontinuity`/`SetSyncPoint`/`SetDiscontinuity` | 同上 | `IsSyncPoint` 恒 `S_OK`（每帧都当同步点） |

### §2-D `Render(pin)` 的契约（本任务的核心决定）

`g:` 在 `0x489645` 调 `Render(源输出 pin)` 之后**只做**：`Release(filter)`（`0x489647`）→ `QI(IBasicAudio)`（`0x489666`）
→ `[+0x458]=时长参数`、`[+0x46C]=1`。**它不检查 `Render` 的返回值、不查 `ConnectedTo`**。
⇒ 宿主有两种实现：

* **(D1) 推荐**：`Render` 不碰 `g:` 的源 pin；改为把 **`h:` 解码 filter 的输出 pin** 接到 **`g:` 渲染器输入 pin**：
  `h:pin->ReceiveConnection` 由 `g:` 侧实现，宿主调 `g:` 的 `IPin::ReceiveConnection`（pin vtbl +0x10，需 guest 调用，
  见 §5-3），再把 `h:` 自建的 `AM_MEDIA_TYPE`（`MEDIATYPE_Video` + `VIDEOINFOHEADER`，§3-2）传进去；
  返回 `S_OK` 即可（引擎不看）。源 filter 变成"已加入图但未连接"，引擎不会发现。
* **(D2) 备选**：真调 `g:` 源 pin 的 `IPin::Connect`（pin vtbl +0x0C）。它内部会 `ReceiveConnection` 到渲染器并对
  `MEDIATYPE_Stream` 协商 —— 渲染器要 `VIDEOINFOHEADER`，协商必失败（除非引擎自带解码，**未见证据**）。
  ⇒ 只有在 §6-2 实测"引擎 pin 能自己完成 stream→video 协商"后才选它。

### §2-E Filter 状态机（Q7）

* `g:` 两个 filter 的 `IBaseFilter` 方法在**各自 +0x0C** 的 vtable 上：
  `GetClassID`+0x0C、`Stop`+0x10、`Pause`+0x14、`Run`+0x18、`GetState`+0x1C、`SetSyncSource`+0x20、`GetSyncSource`+0x24、
  `EnumPins`+0x28、`FindPin`+0x2C（源 @`0x527C78…0x527C98`；渲染器 @`0x52F448…0x52F468`）。
* `CBaseRenderer::Receive` = **`sub_4D4000`**（经 `CRendererInputPin::Receive` = `sub_4D4730` 的 `[rcx+0x9C]` 转发）。
  它按 `[this+0x14]`（`m_State`）分叉：`m_State==1`（Paused）分支当场渲染（`0x4D40C8 call [edx+0x34]`）并
  `0x4D40DE SetEvent([esi+0x5C])`；否则走另一条（定时器/`+0xA8`）分支。**整条 `Receive` 里没有 `WaitForSingleObject`** ★。
* ⇒ **宿主顺序**：`Pause`(渲染器) + `Pause`(源) → `Run`(两者，`tStart` 用同一时基) → 才开始 `Receive`。
  先 `Pause` 再 `Run` 是基类口径（`PrepareReceive` 在主 vtable **+0x98**，未单独取体 ⇒ ◇ 它按 `State_Stopped` 拒绝）。

## §3 数据布局

### §3-1 引擎建给源 filter 的 `AM_MEDIA_TYPE`（`Open`，阅读位置 `.c:105644-105667`）

`CMediaType` 就地构造（ctor `0x4D3760`，dtor `0x4D3750` @`0x4895E7`），只填两段 GUID、`cbFormat=0`：

| 段 | 值 | 数据地址 |
|---|---|---|
| `majortype` | `MEDIATYPE_Stream` = `E436EB83-524F-11CE-9F53-0020AF0BA770` | `0x52FAB8..0x52FAC4` |
| `subtype`（非 AVI） | `MEDIASUBTYPE_MPEG1System` = `E436EB84-…` | `0x52FAE8..0x52FAF4` |
| `subtype`（AVI） | `MEDIASUBTYPE_Avi` = `E436EB88-…` | `0x52FAF8..0x52FB04` |
| `formattype` / `pUnk` / `cbFormat` / `pbFormat` | 全 0（`memset`/清零，阅读位置 `.c:105648-105649`） | — |

★ **宿主给渲染器的类型是另一个**：`MEDIATYPE_Video`（`0x52FAC8` = `E436EB7D-…`）+ `VIDEOINFOHEADER`。

### §3-2 渲染器接受什么（`SetMediaType` = **`0x488A80`**，主 vtable **+0x90**）

| 检查 | 证据 | 结论 |
|---|---|---|
| `pbFormat` | `0x488AB3 mov eax,[ecx+44h]` | `AM_MEDIA_TYPE.pbFormat` 在 **+0x44**；为 0 ⇒ 失败 |
| `biWidth` | `0x488AB6 mov edx,[eax+34h]` → `[+0x5A4]` | `bmiHeader` 在 `VIDEOINFOHEADER` **+0x30**，`biWidth` 在 **+0x34** |
| `biHeight` | `0x488ABF mov eax,[eax+38h]`；`0x488AC5 xor/sbb` 取绝对值 → `[+0x5A8]` | **允许负高**（自底向上 DIB） |
| **subtype == `0x52FAD8`** | `0x488AD8 call sub_4028A0`（`IsEqualGUID(subtype, &0x52FAD8)`） | = `MEDIASUBTYPE_ARGB32`（`773C9AC0-3274-11D0-B724-00AA006C1A01`）⇒ `[+0x5B0]=4`、`[+0x5AC]=w*4`（`0x488AF1`/`0x488AF7`）；否则 `[+0x5B0]=3`、`[+0x5AC]=(w*3+3)&~3`（`0x488B07`/`0x488B10`） |
| `biBitCount` / `biCompression` / `biSizeImage` / `dwBitRate` | **无任何读取** | ⛔ 不要依赖它们；`BI_RGB` 不是判据 |
| **纹理格式** | `0x488BF1 call [ecx+48h]`（`IDirect3DTexture9::GetSurfaceLevel(0)`）→ `0x488C07 call [ecx+30h]`（`IDirect3DSurface9::GetDesc`）→ `desc.Format` → `[+0x570]`；`0x488C12 cmp eax,15h` / `0x488C17 cmp eax,1Ah` | 只接受 `D3DFMT_A8R8G8B8(21)` 或 `D3DFMT_A4R4G4B4(26)`；否则 `0x488C5B mov eax,8004022Ah` = **`VFW_E_TYPE_NOT_ACCEPTED`** |
| 纹理对象前置 | `0x488AB0`/`0x488AE0` 之前要求 `[+0x56C]`（`IDirect3DTexture9`）与 `[+0x568]` 非空 | 空 ⇒ `E_UNEXPECTED`（`0x487CAE mov eax,8000FFFFh`） |
| `EnumMediaTypes` | **? 没拿到**（未定位 pin 的 `EnumMediaTypes` 实现） | §6-2 |

### §3-3 样本 → 像素（`DoRenderSample` = `0x487C60`）

* `sample->GetPointer`（vtbl **+0x0C**，`0x487CCE`）→ `var_C`；行距 `[+0x5AC]`，每像素 `[+0x5B0]`（3 或 4）。
* **自底向上**：`0x487E3D mov edi,[esi+5A8h]` / `sub edi,[ebp+var_8]` / `dec edi` / `0x487E3D imul edi,[esi+5ACh]`
  / `add edi,[ebp+var_C]` ⇒ 源行 = `base + (h-1-y)*stride`（DIB 约定）。**宿主不要把行序翻正**。
* 像素序：`movzx edi,[ecx+2]`（B）/`[ecx+1]`（G）/`[ecx]`（R）→ `0x00RRGGBB`；第 4 字节（仅 4bpp）当 alpha。
* 透明：`[+0x588]!=0` ⇒ 用样本 alpha × `[+0x58C]`（`new[]` 的 alpha 面，`0x488B1D`）缩放；`==0` ⇒ 4bpp 用样本第 4 字节、
  3bpp 补 `0xFF000000`，且 `cmp edi,[esi+584h]`（**color key**）相等 ⇒ 写 0。
* 渲染器 busy 计数 `[+0x5B8]`：`InterlockedIncrement` 后 `>1` ⇒ **直接返回 0（跳过这一帧）**（`0x487C6A..0x487C7D`）。

### §3-4 时间单位

* `IMediaSample` 时间戳 = `REFERENCE_TIME`（**100 ns**，`LONGLONG`）。
* `IMediaPosition` = `REFTIME`（**`double` 秒**）：引擎 `fstp qword ptr [esi+460h]`（`0x488451`）、`fld qword`（`0x4884F4`）。
* 换算：`秒 = 100ns/1e7`；`ms = 秒*1000`；`SetPosition` 的入参是 `double`（`0x4126B0 fstp [esp+…]`）。

## §4 读源的三条候选路径与推荐

| # | 路径 | 可行性证据 | 风险 |
|---|---|---|---|
| **A** | 宿主 QI `IAsyncReader`（pin **+0x00** ★RTTI）→ `Length`/`SyncRead` | pin 确实实现 `IAsyncReader`（`.data:0x53C434`） | `CAsyncReader` 自带 `CAsyncIo` **工作线程**（真机 `CreateThread`）；`SyncRead` 可能等该线程的完成事件 ⇒ lift 里线程体不执行 ⇒ `WaitForSingleObject` **硬错误**（SCOPE §1）。**必须先实测**（§6-1） |
| **B** | 宿主**自己读文件**：路径从 VFS/kernel32 shim 记录里拿 | 引擎用 `sub_4559C0` 的 `CreateFileA(80*id + this[261], …)` 打开**真路径串**（不是 `DATA*.ALF`+偏移）★；`sub_455560` 只返回该表的句柄+`SetFilePointer` | 无（宿主已有 VFS；同一文件可重开）。★ 这是 **Q10 的答案**：可以按名字读 |
| **C** | 同 B，但形态是"宿主解码 filter **自己**读文件 + 解析 MPEG-1 PS"，`Render` 只做渲染器一侧接线 | 引擎**丢弃** `Render` 的 HRESULT（`0x489647` 紧接 `Release`）★；引擎不再查连接 | 必须自己解 PS/MPEG-1（含序列头取宽高、帧率） |

**推荐 C（= B 的实现形态）**；A 只作为"将来想复用引擎的 MPEG 解析"的备选，且**开通前必须先过 §6-1**。
落点提示：宿主侧解码不属本契约，但契约假设它存在（本仓已有 `src/runtime/video/movie_decoder.{h,cpp}` 这一落点在长——不以此为契约依据）。
判据：`--log-cat video` 下能看到"渲染器 `SetMediaType` 返回 S_OK" + "`DoRenderSample` 写过 `[+0x5B4]=1`"，且纹理里出现非背景像素。

## §5 等待与线程风险（对齐 SCOPE §1 / README §2.5）

1. **渲染时机靠多媒体定时器**：`CBaseRenderer` 用 `timeSetEvent` 排"到点渲染"。唯一调用站 `0x4CFB61 call timeSetEvent`
   （wrapper `sub_4CFB20`，带 `TIME_KILL_SYNCHRONOUS(0x100)` 位，`0x4CFB46 or [ebp+fuEvent],100h`）。本仓是**协作式**实现
   （`src/runtime/x86.h`：登记进 `ShimState::timers`，dispatcher 在**块边界**按虚拟时钟把 guest 栈铺成回调帧）
   ⇒ **可用**，但回调是 **guest 函数**：⛔ 不能从宿主线程直接进；宿主的 `Receive` 只负责把样本交出去。
2. **会踩 SCOPE §1 硬错误的宿主实现**（一律禁止）：`IMemAllocator::GetBuffer` 里等信号量（`CMemAllocator` 的形态）、
   在 `Render`/`Run`/`Receive` 里等"图完成"、`CAsyncIo` 的线程事件、任何 `CAMEvent::Wait`。
   `CBaseRenderer::Receive`（`sub_4D4000`）**自身不等** ★ ⇒ 只要宿主方法同步返回，就不会有 guest 等待。
3. **host→guest 调用**：`Render`（§2-D）与 `Pause`/`Run`（§2-E）都要求宿主**调 guest 的虚函数**。设施已有：
   `GuestCallStack::start(cpu, fn, args, cleanup, api, cont, err, callee_ecx)` 支持 **thiscall**（`callee_ecx` = this）
   + 续延（`kContinuationEa`）⇒ 可在 shim 里发起、在续延里接着做。⚠ 本契约的所有 guest 调用都必须是 **stdcall**
   （`ret n` 由 guest 弹参），且**不能**在续延里再等定时器。
4. **伪线程口径**：`CAsyncIo` / `CBaseRenderer` 的服务线程**不执行**（README §2.5）。⇒ 不要设计成"等那条线程喂数据"。
5. `WaitForCompletion(10ms)`（**仅 MovieIVideoWindow 路径**，`0x487292`）：引擎只在 `hr==0`（`0x487294 test eax,eax`）
   且 `evCode∈{1,2,3}`（`0x48729B/0x4872A0/0x4872A5`）时才认为完成；**超时必须返回 `S_FALSE(1)`**，否则影片被提前关掉。
   DShow 路径不使用它。

## §6 待实测项（每条都能写成一次性探针 / CTest）

| # | 问题 | 探针 |
|---|---|---|
| 1 | `g:` 源 pin 的 `IAsyncReader::SyncRead` 是否等 `CAsyncIo` 线程 | 建 `CStreamReader` 后 **不 Run**，QI `IAsyncReader`（pin+0）→ 读 1 字节；判据：走到 `WaitForSingleObject` 硬错误 = 不能走 A 路 |
| 2 | `Render` 时 `g:` pin 的协商能否走通（`EnumMediaTypes` 返回什么） | shim 里记录 pin 的 `EnumMediaTypes`/`QueryAccept`/`ReceiveConnection` 参数；判据：若只枚举 `MEDIATYPE_Stream` ⇒ 必须插宿主解码 filter（D1） |
| 3 | 渲染器是否读 `IMediaSample` 时间戳 | 给"NULL 时间戳"与"0 时间戳"两组样本，看 `Receive` 返回；判据：`VFW_E_SAMPLE_REJECTED`/挂起 = 需要时间戳 |
| 4 | `IMediaPosition` `+0x24`/`+0x28` 的真实语义 | 宿主让 `+0x24`=2.0s、`+0x28`=1.0s；判据：`0x41268D` 后引擎应在 1s 就结束（否则两槽反了） |
| 5 | `CBaseInputPin::GetAllocator` 是否被调到（要不要实现 `CLSID_MemoryAllocator`） | 在 `ole32!CoCreateInstance` 里记 `rclsid=0x52FB38` 是否出现；判据：宿主先 `NotifyAllocator(h:alloc,FALSE)` 成功则不该出现 |
| 6 | `NotifyAllocator(h:alloc, FALSE)` 是否被接受 | `CBaseInputPin::NotifyAllocator` = `sub_4D1310`：`EnterCriticalSection` → `AddRef(新)` → 换槽 `[edi+4]` → `Release(旧)` → 存 `bReadOnly` → `S_OK` ★；**不校验**类型 ⇒ 预期接受；探针断言返回值 0 |
| 7 | 收尾 CTest | 一条 `ctest` 用例：跑一段 `--movie`（若已有）直到 `DoRenderSample` 命中 ≥1 次，断言纹理像素变化 + 引擎完成回调 |

## §7 开放问题

1. **`IBasicAudio` 与 `IMediaSeeking` 的混淆**：`0x52FB98`=`IBasicAudio`（§0-2）已定；但 `IMediaSeeking` 的 IID
   在 `[+0x434]`（Movie 路径，`0x487169` `SetPositions` `+0x38`）—— 两条路径的接口集**不同**，别合并实现。
2. **`[+0x478]`**：`0x4885A0`/`0x487270` 都 `cmp [ecx+478h],0`（前者决定是否调 `put_Volume`，后者是 `WaitForCompletion`
   的宿主对象）。写入点 `0x486FD7` 在 AMultiMediaStream 的类里 ⇒ **DShow 路径下它可能是 0**，含义 ?。
   影响：引擎可能**根本不会**调 `put_Volume`（§2 的 `IBasicAudio` 仍要 QI 成功，否则 `Open` 直接报错 `0x48966B`）。
3. **`Render` 之后引擎是否还查连接**：目前证据是"不查"（`0x489647` 直接 Release）★；但 `CMovieTextureRenderer`
   的 `CompleteConnect`/`StartStreaming` 何时被调没定位 ⇒ 若宿主走 D1，`g:` 的源 pin 可能永远不进 `Connected` 态，
   会不会影响 `IMediaControl::Run` 的内部断言 = ?（§6-2 探针顺带看）。
4. **`[+0x43C]`=1 的 MF 路径**（`0x488EFD`）：`sub_485620` + `MFStartup`，本文不覆盖。
5. **`CStreamWapper`（`0x486420`）的 `Read`**：它走 `SetFilePointer`+`ReadFile`（`sub_455560`），
   宿主若走 A 路必须让这些 shim 在全同步下工作（本仓已有）。

## §8 实测修正（2026-11-07，真机 + `.lst` 复核）

> 本节是**对上面若干推断的更正**（真机跑 + 逐指令核对后得到）；与上文冲突时以本节为准。

1. **`IAsyncReader` 的 IID 不是 `56A868A0`**：本引擎 QI 键是 `.data:0x52FD18`
   = `56A868AA-0AD4-11CE-B03A-0020AF0BA770`（`sub_402A00` = `CAsyncOutputPin::NonDelegatingQueryInterface`，
   `0x402A13 push offset dword_52FD18`；命中 ⇒ `[this+0xA0]=1` + 返回对象基址）。用 56A868A0 只会 E_NOINTERFACE。
2. **`IAsyncReader` 的 vtable 槽序（`.data:0x51D95C`，按 retn/实现体逐个核）**：
   `+0x18 SyncReadAligned`（retn 8，参数 `IMediaSample*`）、**`+0x1C SyncRead`**（retn 14h =
   `(this, llPos_lo, llPos_hi, lLength, pBuffer)`）、**`+0x20 Length`**（retn 0Ch = `(this, total*, avail*)`）。
   ⛔ 不是记忆里 strmif.h 的 +0x18/+0x0C。真机实测：`Length ⇒ total=3532804`、一次 `SyncRead` 读满
   3,532,804 字节、first4=`0xBA010000` ⇒ **路径 A 可行，`SyncRead` 不等任何线程**（§6-1 的隐忧不成立）。
3. **`CMovieTextureRenderer::CheckMediaType`（`sub_4878B0`）比 §3-2 更严**：它要求
   `majortype == "vids"`（`.data:0x52FAA8` = `73646976-0000-0010-8000-00AA00389B71`）、
   `formattype == FORMAT_VideoInfo`（`.data:0x52FB48` = `05589F80-C356-11CE-BF01-00AA0055595A`）、
   `subtype ∈ {ARGB32(0x52FAD8), E436EB7D(0x52FAC8)}`。⛔ 旧稿把 majortype/subtype 都写成 E436EB7D、
   formattype 写成 F72A76A0（那是 VideoInfo2）⇒ `CheckMediaType` 返 E_FAIL，`CBasePin::ReceiveConnection`
   （`sub_4D0D00`）把它映射成 **`VFW_E_TYPE_NOT_ACCEPTED(0x8004022A)`**（实测）。改正后 `ReceiveConnection = S_OK`。
4. **`JoinFilterGraph` 不写 `[filter+0x6C]`**（更正 §2-E 的说法）：`CBaseFilter::JoinFilterGraph`
   = `sub_4D02B0` 实际写的是 `[this+0x34] = pGraph`（`0x4D02ED mov [edi+34h],eax`）与 `[this+0x38] = sink`
   （`[this+0x30]` = name、`[this+0x2C]` = lock）。`[filter+0x6C]` 是**另一个成员**：
   `0x4D38F3` 清零、`0x4D5DCB` 存一个接口并 AddRef（`sub_4D5C60` 内）。`CBaseRenderer::Pause`（`sub_4D5860`）
   只在 `state != 1` 时读 `[filter+0x6C]`（`0x4D5896` 一带），`Run`（`sub_4D5980`）在 `state != 2` 时读
   （`0x4D59C6`）。实测：调 `JoinFilterGraph` 返回 S_OK 后 `[filter+0x6C]` **仍为 0**，而 `Pause(渲染器)` 仍成功
   （走 state==1 分支）⇒ "JoinFilterGraph 修 +0x6C"这个前提不成立；+0x6C 的写者要另行定位（`sub_4D5C60` 是候选）。
5. **真图必须持有 filter 引用**：引擎在 `Render` 之后立刻 `Release(filter)`（`0x489647`），
   不 AddRef 的话源 filter 会被销毁 ⇒ 后面的 `Pause(源)`/`Run(源)` 的 vtable 槽为 0。宿主在 `AddFilter` 里
   `IBaseFilter::AddRef`（+0x04）后已实测走通 Pause/Run 链。
6. **样本必须由宿主自持一份引用**：引擎会 Release 它拿到的接口（COM 惯例）。`HostObjects::create` 里对
   sample/allocator/output pin 各多 AddRef 一次，否则对象会被 `ComRegistry` 销毁、之后 `[sample]` 读到野 vtable。
7. **样本头被改写的根因（已修）**：`IMediaPosition` 的 `get_CurrentPosition`/`get_StopTime` shim 里，
   出参指针 `call.u32(1)` 是在 `Graph::currentPosition()` **之后**读的 —— 而它会推进泵、发起一次
   guest `Receive`（`GuestCallStack::start` 把 `cpu.esp()` 压到回调帧上）⇒ 之后再 `call.u32(1)` 读到的是
   帧里的 `args[1]`（= 样本 EA），`writeDouble` 就把位置的 double 写进了样本对象头
   （`PrepareReceive` 的 `0x4D1423 mov eax,[edi]` 因此读到野 vtable，崩在 `0x4D1425`）。
   **修法**：进 `currentPosition()` 之前先取 `out_ea`。验证手段：`LIFT_WATCH_SAMPLE=1` 时
   `Machine::run` 每个 block 入口观察 `[sample]`（见 `runtime/watch.h`），修后**不再出现** `[watch]` 行。
   ★ 通用纪律：**任何可能发起 guest 调用的 Graph 方法之后都不要再读 `call.u32(i)`**。
8. **未决（修完上一条后的新状态）**：接线 + 第一次 `PrepareReceive` 已能走通（样本头不再被改写、
   无 `[watch]` 行），但随后仍以 `decode-error @ eip=0x10338A77`（guest 堆地址）停下 —— 即引擎在
   **渲染路径** 里跳进堆（`sub_4D13F0` 的 8 个 fallback 槽 + 返回地址被改写）。
9. **样本实现 `IMediaSample2`（已做）**：`PrepareReceive`（`sub_4D13F0`）优先 `QI(IID_IMediaSample2 =
   0x52FC88)`；成功时**只**调 `GetProperties(0x30, pin+0x10)` + `Release`，不进那 8 个 fallback 槽。
   我们的样本现在：QI 认 `36B73884-C2C8-11CF-8B46-00805F6CEF60`；`+0x4C GetProperties(DWORD cb, BYTE* p)`
   写 `AM_SAMPLE2_PROPERTIES`（严格 0x30 字节、`cb < 0x30` 或未提交 ⇒ `E_INVALIDARG`、`p==0` ⇒ `E_POINTER`）；
   `+0x50 SetProperties` 只记日志、⛔ 不写内存。实测：fallback 的栈破坏不再发生。
10. **未决（下一个真正的堵点）**：第一次 `Receive` 现在能进到渲染器内部，然后撞上
   `kernel32!WaitForMultipleObjects(nCount=2, dwMilliseconds=10000)` —— 这是 `CBaseRenderer`
   的**渲染时刻等待**（render event + thread event）。本仓 SCOPE §1 把"未 signaled 且 timeout != 0 的等待"
   定为**硬错误** ⇒ 机器停下。要么让这条等待协作化（例如：宿主把"样本已到期/立即渲染"表达成
   render event 已 signaled），要么由 SCOPE 裁决一个受控例外。**这是策略决定，不在本段自行放宽。**
11. **最终形态：渲染器服务线程事件由宿主合成（方案 A，2026-11-07 用户批准，**默认生效**）**
   `sub_4D5710` 对 `[this+0x58]`（#0）/`[this+0x54]`（#1）两个**匿名 Event** 做
   `WaitForMultipleObjects(2, …, 10000)` 的 10 s 重试等待，等的是"服务线程已就绪"；本模拟器是伪线程模型，
   线程体从不运行 ⇒ 没人 signal。宿主在 `WaitForMultipleObjects` shim 里**只对**这一形态
   （`nCount==2 && timeout==10000 && 调用点 ∈ 0x4D5710..0x4D57FF`）把 **#1 句柄**当作已 signaled
   ⇒ 返回 `WAIT_OBJECT_0+1` ⇒ 走 `sub_4D3E40(this)`（只 `this[26]=0`）的成功分支；`v7==0`（#0）
   反而是错误码 `0x80040223`。**默认开**，`LIFT_DSHOW_NO_SIGNAL=1` 关掉做 A/B；生效时只打一行
   `渲染器服务线程事件：宿主合成（sub_4D5710 的 #1 句柄…）`。
   实测：`Pause(渲染器)=S_FALSE(1)`、`Pause(源)=0`、`Run(源)=0`、首帧 pts=0、`samples` 跑到 259。
   （曾用 `LIFT_DSHOW_SIGNAL=1` 做探针；已改成默认开。`LIFT_DSHOW_NO_PAUSE_RENDERER` 那个
   "渲染器保持 Stopped"的对照探针**已删除**——它不送帧（引擎全程只问 1 次位置），留着只会误导。）
12. **guest 堆耗尽（已修）**：`LIFT_DSHOW_SIGNAL=1` 下的第 121 帧左右，`HeapAlloc(921600)` 返回 0
   （1280×720 一平面），引擎拿 NULL 后 `RaiseException`（SEH 未实现 ⇒ 硬停）。
   根因：`IMediaSample2::GetProperties` 把 **`AM_SAMPLE2_PROPERTIES.pMediaType` 给了非 0**
   ⇒ `PrepareReceive`（`sub_4D5C60` 的 `[pin+0xCC]!=0` 分支）**每帧**重新 ApplyMediaType
   （`0x4D5CF6 call [pin->vtbl+0x24]`）⇒ `CMovieTextureRenderer::SetMediaType`（`sub_488A80`）
   每帧 `*(this+1420) = operator new[](w*h)`（`0x488B23`）**覆盖旧指针、不释放** ⇒ 每帧泄 921600 B。
   **修法**：`pMediaType` 恒为 **NULL**（真样本只有被 `SetMediaType` 过才非空）。
   实测：`HeapAlloc` 大块从 138 次降到 **2 次**；`heap in_use` 稳定在 ~13.4 MB（peak 15.6 MB）；
   影片跑满 **259 帧 / 8.642 s** ⇒ `completed=true`，引擎清槽后脚本继续（`max-insns` 收尾，exit 0）。
   ★ 回归哨兵：`HeapAlloc ≥ 900000` 时 shim 会打一行"大块 #n … 调用点 / in_use"（见 `kernel32_mem.cpp`）。

13. **渲染器必须 `Run`（Running），样本时间必须是**流时间**（2026-11-07 实测，两条一起才出画面）**
   - **现象**：`Receive` 返回 S_OK、`samples` 正常推进、`completed=true`，但纹理**全黑**
     （`tex movie` 抽样 `nonzero=0`、hash 恒定）。
   - **根因 A**：渲染器保持 **Paused** 时，`CBaseRenderer::Receive` 只把样本挂到 `this[27]` 并
     `SetEvent(this[23])`，真正渲染由它的**服务线程**做；伪线程不跑 ⇒ 永远不渲染。
     **修法**：`IMediaControl::Run` 的链尾再对渲染器调一次 `IBaseFilter::Run(tStart)`（+0x18）。
   - **根因 B**：样本 `SetTime` 曾经写**绝对时钟**（`timeGetTime*10000`）。渲染器算"何时渲染"用的是
     `sub_4D5E50`：`*(QWORD*)(this+0x20) + *(QWORD*)(this+0xB8)`（= 样本 `m_rtStart` + Run 的
     `m_tStart`）⇒ 两个绝对时钟相加 = 2×时钟 ⇒ 永远"未到期"（还会用 `timeSetEvent` 排一个
     永远不响的定时器）。**修法**：`SetTime(pts, pts+帧长)`（**相对片头**）；`GetMediaTime` 同域。
   - **像素判据（可机械判）**：`d3d9::sampleTexture()` 按 `[renderer_base + 0x56C]`（`base` 是
     `IBaseFilter* - 0xC`，即 `0x487D42 mov eax,[esi+56Ch]` 的那张 `IDirect3DTexture9`）抽样：
     每 64 像素取一个，输出 `nonzero / sampled / FNV-1a hash`。泵在每 30 帧（+首帧）打一行
     `[dshow] tex movie #n w=… h=… nonzero=… sampled=… hash=…`。
     实测：`w=1280 h=720 nonzero=14400 sampled=14400`，帧间 hash 变化 ⇒ **纹理真的被引擎写了**。
   - **assets 守卫**：`tests/movie_logo.cpp` 断言 `tex movie` 的 1280×720 / `nonzero>0` / ≥2 个不同
     hash / 出现在 `首帧已送渲染器` 之后。★ `--real-time` 下渲染慢会**丢帧**（实测 43/259 帧，与真机
     一致）⇒ 测试只要求 `samples ≥ 20`，"播完"由 `completed=true` 守。
