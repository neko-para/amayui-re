# 我们依赖哪些外部文档（以及我们偏离它的地方）

> 这一域的知识锚**从 EA 换成了文档**：一条 `L1-*` 合同的最终依据是**微软的页面**，不是我们的代码。
> 三条纪律：
> 1. **只写取证过的 URL**。没核过的一律写 `待补（未取证）`，并在"本仓哪里引了它"一栏给出代码注释 / 文档落点 ——
>    ⛔ 不许凭记忆补 slug（`.../api/<header>/nf-<header>-<name>` 这个形状看着像就填，正是最容易编错的地方）。
> 2. **每条记日期**：`✅` = 本仓已存链接且我核过（`Last updated` 也一并记）；`待补` = 只有名字与引用点。
> 3. **页面只作"语义真源"，不作"等价证明"**（`AGENTS.md` §11 的措辞纪律同样管这里）。

## 1. 语义真源（Microsoft Learn）

| id | 主题 | 页面 | 用它定了哪条合同 | 日期 / 状态 |
|---|---|---|---|---|
| MS-virtualalloc | Win32 · 内存 | <https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc> | `L1-MEM-02`（新提交页读 0、已提交页再 COMMIT 不失败）、`L1-MEM-04`（COMMIT 未先 RESERVE ⇒ 失败 + `ERROR_INVALID_ADDRESS`） | ✅ 2026-11-09 抓取（HTTP 200，页面 `Last updated 2024-02-05`） |
| MS-heapalloc | Win32 · 堆 | `HeapAlloc` / `HeapReAlloc` / `HeapSize` / `LocalAlloc` | `L1-MEM-01`（失败返回 0 + `GetLastError`）、`L1-MEM-03`（realloc 保内容）、`L1-MEM-06`（尺寸查询失败码） | 待补（本仓引用：`shims/kernel32_mem.cpp:88` 注释"分配失败的统一出口"） |
| MS-createfile | Win32 · 文件 | `CreateFileA`（`dwDesiredAccess` / disposition / `dwShareMode`） | `L1-FILE-02`（`access=0` 什么都不授予）、`L1-FILE-03`（`CREATE_ALWAYS`/`OPEN_ALWAYS` 命中已存在 ⇒ 183）、`L1-FILE-04`（共享冲突 ⇒ 32） | 待补（本仓引用：`shims/kernel32_file.cpp:559-560`、`:726`） |
| MS-fullpath | Win32 · 路径 | `GetFullPathNameA` | `L1-FILE-01`（`..` 归一；**但"不许逃出根 / 多候选即失败"是我们自定**） | 待补（本仓引用：`runtime/vfs.cpp:236-243`；口径本身是自定） |
| MS-mbtowc | Win32 · 码页 | `MultiByteToWideChar` | `L1-CP-01`（非法字节 ⇒ `U+FFFD` 且前进 1 字节；`MB_ERR_INVALID_CHARS` ⇒ 0 + 1113）、`L1-CP-02`（返回值是字符数） | 待补（本仓引用：`runtime/cp932.cpp:93`、`shims/kernel32_bootstrap.cpp:250-304`） |
| MS-getstringtypew | Win32 · 字符分类 | `GetStringTypeW` 的 `C1_*` 位表 | 未写成 `L1-*`（只作实现依据）：`CT_CTYPE1` 位值 | 待补（本仓引用：`shims/kernel32_bootstrap.cpp:403` 注释明写"MSDN「GetStringTypeW」的 C1_* 表"） |
| MS-getglyphoutline | GDI · 字体 | <https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getglyphoutlinea> | `L1-GDI-02`（`GGO_BITMAP` / `GGO_GRAY4`=17 级 / `GGO_GRAY8`=65 级 / `GGO_METRICS` / `MAT2`）；也是"引擎拿灰度位图自己合成字"的依据 | ✅ 链接已存于 `platform-effort.md` §3.3/§7（**抓取日期未记**，本次未重新抓取） |
| MS-gettextmetrics | GDI · 度量 | `GetTextMetricsA` / `GetTextExtentPoint32A` / `CreateFontIndirectA` / `DeleteObject` | `L1-GDI-01`（两次调用逐字节相同、`tmHeight = ascent+descent`）、`L1-GDI-03`（宽 = Σ advance）、`L1-GDI-04`（stock 对象 / 仍被选中的对象不许删） | 待补（本仓引用：`shims/gdi32_object.cpp:139` 注释"stock object 不是调用方拥有的"） |
| MS-peekmessage | user32 · 消息 | `PeekMessageA` / `PostQuitMessage` | `L1-MSG-01`（`PM_REMOVE` 取走后不再投递、`PM_NOREMOVE` 只窥视）、`L1-MSG-02`（`WM_QUIT` ⇒ `GetMessage` 返 0） | 待补（本仓引用：`shims/user32_window.cpp:545-574`） |
| MS-getasynckeystate | user32 · 输入 | `GetAsyncKeyState` | `L1-MSG-03`（最高位 `0x8000` = 当前按下；**低字节我们未建模**） | 待补（本仓引用：`shims/user32_window.cpp:480`） |
| MS-msgwait | user32 · 等待 | `MsgWaitForMultipleObjects` | `L1-MSG-04`（有消息 ⇒ `WAIT_OBJECT_0+nCount` 且不取走；`INFINITE` ⇒ **我们硬错**） | 待补（本仓引用：`shims/user32_input.cpp:279-343`） |
| MS-criticalsection | Win32 · 同步 | <https://learn.microsoft.com/en-us/windows/win32/sync/critical-section-objects> | `L1-SYNC-03`（可重入、必须等次数 `Leave`、spin count、`TryEnter` 不在本作） | ✅ 链接已存于 `platform-effort.md` §3.1/§7（**抓取日期未记**，本次未重新抓取） |
| MS-syncobjects | Win32 · 同步 | `OpenMutexA` / `OpenEventA` / `OpenSemaphoreA` / `WaitForSingleObject` | `L1-SYNC-01`（`Open*` 找不到 ⇒ `NULL` + 2、名字比较大小写敏感、`Create*` 同名 ⇒ 183）、`L1-SYNC-02`（消费一个信号） | 待补（本仓引用：`shims/kernel32_sync.cpp:11-13`、`:407-409`） |
| MS-present | D3D9 | `IDirect3DDevice9::Present` / `Clear` | `L1-D3D9-02`（整帧送出、`pSourceRect`/`pDirtyRegion` 的语义）、`L1-D3D9-03`（`Clear` 掩码） | 待补（本仓引用：`runtime/d3d9.cpp:525-532`、`:688`） |
| MS-lockrect | D3D9 | `IDirect3DSurface9::LockRect` / `UnlockRect` | `L1-D3D9-01`（`D3DLOCKED_RECT` 的 `Pitch`/`pBits`、二次锁 ⇒ `D3DERR_INVALIDCALL`） | 待补（本仓引用：`runtime/d3d9.cpp:1073` 注释"D3DLOCKED_RECT = { INT Pitch; void* pBits; }"） |
| MS-ddraw-surface | DDraw | `IDirectDrawSurface7::Blt` / `BltFast` / `Lock` / `Flip` | `L1-DDRAW-01..04`（键色、裁剪、嵌套锁错误码、`Flip` 只翻 primary） | 待补（本仓引用：`shims/ddraw.cpp:8` 注释"方法表偏移**逐条**对过 Windows SDK `ddraw.h`"） |
| MS-dshow-graph | DirectShow | `IGraphBuilder::Render` / `IMediaControl::Run` / `IMemInputPin::Receive` | `L1-DSHOW-02`（接线顺序、`Run = Pause(渲染器)+Pause(源)+Run(源)`）、`L1-DSHOW-03`（逐帧交付、忙则丢帧） | 待补（本仓引用：`runtime/dshow/graph.cpp:1230-1241` 注释） |
| MS-dsound-buffer | DirectSound | `IDirectSoundBuffer::Lock` / `Unlock` / `Play` / `GetCurrentPosition` | `L1-AUDIO-01`（窗口指针、错误码分家、游标） | 待补（本仓引用：`shims/dsound.cpp:50` 注释"来自 **SDK dsound.h**，⛔ 不是从 `.lst` 取证"） |
| MS-findresource | Win32 · PE 资源 | `FindResource` | `L1-PE-01`（资源名比较**大小写不敏感**；失败原因要分开） | 待补（本仓引用：`runtime/pe_resource.cpp:54`） |
| MS-handles | Win32 · 句柄 | `CloseHandle` / 失效句柄的失败码 | `L1-HANDLE-01`（Win32 = 失败返回 + `ERROR_INVALID_HANDLE`；**本线改为诊断 + abort**，见 §3 #2） | 待补（本仓引用：`runtime/handles.cpp:138`） |
| MS-wsprintf | Win32 · 字符串 | `wsprintfA`（返回值 = 写入的字符/字节数） | `L1-CP-03`（按**字节**计数、`%s` 的 CP932 字节透传） | 待补（本仓引用：`shims/kernel32_string.cpp:300-316`） |
| MS-mf-guide | Media Foundation | <https://learn.microsoft.com/en-us/windows/win32/medfound/media-foundation-programming-guide> | **不定本线的合同**（只作"若走 MF 会是什么形状"的策略参考）；`platform-effort.md` §3.6 用它论证路线成本，本线最终走 DirectShow + SDL | ✅ 链接已存于 `platform-effort.md` §7（**抓取日期未记**） |

## 2. 非微软、但我们要用的两种参考（**不是语义真源**）

| 来源 | 用来做什么 | 边界 |
|---|---|---|
| Wine 源码树（`dlls/ntdll/sync.c`、`dlls/mf`、`dlls/evr`） | "**有没有先例**"的观察：临界区递归 / MF 图的模块划分 | ⛔ 不作等价证明；Wine 的 PE 侧基本是壳、实现在 unix 侧 ⇒ 不能当 shim 复用（`apps/emulator-lift/README.md` §1.1） |
| 平台 SDK 头（`ddraw.h` / `dsound.h` / `d3d9.h`） | 方法表**偏移顺序**、常量值（`kBltKeySrc`、`DSBLOCK_ENTIREBUFFER` 等） | 只给常量与 vtable 顺序；行为仍要按页面 + 我们的观测 |

> **怎么补 URL（下一轮的判据，不是记忆）**：
> `curl -sI <候选 URL> | head -1` 要 `200`，并且页面里能搜到我们引用的那段原话（例如 `ERROR_INVALID_ADDRESS`）；
> 两条都过才把 `待补` 改成 `✅ <日期>`，否则继续 `待补`。

## 3. 我们偏离文档的地方（偏在哪 / 为什么 / 谁受益）

| # | 偏在哪 | 为什么 | 谁受益 | 落点 |
|---|---|---|---|---|
| 1 | D3DX 资源耗尽**不返回 `E_OUTOFMEMORY` 让 guest 继续**，而是打出独立标签后硬停 | 实测：返回失败码后 guest 确实走失败分支（`0x48AD0C`），但随后在 `0x4E8AB6` `throw`（`0xE06D7363`）⇒ 撞上未实现的 `kernel32!RaiseException` ⇒ 停在一个**更假**的 gap 上 | 排障的人：停因是"我们没内存"而不是"没实现 `RaiseException`" | `shims/d3dx9.cpp:104-127` |
| 2 | 失效句柄 ⇒ 打印诊断 + `abort`（Win32 是失败返回 + `ERROR_INVALID_HANDLE`） | 返回错误码会被引擎当正常路径吞掉，症状转移到很远的地方 | 玩家：不会带着错状态继续跑坏存档 | `runtime/handles.cpp:138` |
| 3 | `WaitForSingleObject` 未 signaled 且 `dwMilliseconds != 0` ⇒ **进程级硬错**（Win32 是阻塞） | 单线程 guest 里真阻塞会死锁；返回 `WAIT_TIMEOUT` 又会给出 `INFINITE` 下不可能的值 | 排障的人：卡住的调用点被点名，而不是永远挂着 | `shims/kernel32_sync.cpp:706-712`（用户 2026-11-04 裁决） |
| 4 | `MsgWaitForMultipleObjects` 的 `INFINITE` / 需等对象 ⇒ 硬错 | 同 #3 | 同上 | `shims/user32_input.cpp:279-343` |
| 5 | `DefWindowProcA(WM_CLOSE)` **不销毁窗口**（真机会 `DestroyWindow`） | 本线只有主窗口，销毁与退出由宿主/present 侧统一管（`WM_CLOSE` 后宽限 3 帧再停） | 开发：关窗时序只有一处 | `shims/user32_window.cpp:90`、`README.md` §4.2 |
| 6 | `PeekMessageA` 的 `hwnd` / `min`·`max` 过滤**不匹配也取走** | 单窗口简化：留在队列里会死循环 | 开发：不会因为一条过滤不掉的消息挂死 | `shims/user32_window.cpp:558-568` |
| 7 | `GetAsyncKeyState` 的**低字节**（"自上次查询后按过"）未建模，只给 `0x8000`/0 | 引擎只读最高位；建低字节就得维护"每次查询后清"的状态机 | 维护的人：少一份易错状态 | `shims/user32_window.cpp:480` |
| 8 | `CreateThread` 是**伪线程**：只发线程句柄，**不执行 start routine**（音频 feeder 例外，走真宿主线程） | 游戏 VM 必须确定性地单线程；只有音频是被明确拆出去的两条宿主线程 | 玩家：可复现；开发：没有 TSO 全局软模拟 | `apps/emulator-lift/README.md` §2.5 |
| 9 | **没有真 swap chain**：`Present` / `Flip` 只把当前后缓冲（/ primary）送给呈现汇点，**不交换**前后台 | guest 从来没有写过第二块后缓冲；建真交换链只会多一份要同步的状态 | 开发：`--dump-frame` 与窗口像素 1:1 可对照 | `runtime/d3d9.cpp:632-691`、`runtime/ddraw_surface.cpp:757-793` |
| 10 | surface arena 容量**自定**：`0x28000000` 起 128 MiB（`SurfaceArena` 不模拟显存上限） | 真机的上限由驱动/显存决定，不可知；按"这个 EXE 用得到"给固定区 | 开发：分配失败是**确定性**的（容量不够 ⇒ `E_OUTOFMEMORY` + 见 #1） | `runtime/machine.cpp:107`、`runtime/surface.h:39` |
| 11 | GPU 后端的深度的口径：D32 target 从 1.0 起，**不镜像 guest 深度缓冲**；sprite 路径直接关深度 | CPU 参考后端才是像素真源；GPU 只加速"能等价表达"的那批 | 开发：GPU/CPU 的差异面被限定在"非 1.0 的 `Clear(ZBUFFER)` / 交替写深度" | `apps/emulator-lift/README.md` §4.2b |
| 12 | `VirtualProtect` **不真的改保护**：所有已提交页恒 `PAGE_READWRITE`，旧保护恒写回 4；请求别的保护时打一行日志 | 本线只有 RW 页；真的 `mprotect` 会让"读已释放页"这类 bug 变成 SIGSEGV（而我们要的是可诊断的 guest fault） | 排障的人：越界读有可读诊断 | `shims/kernel32_mem.cpp:587-600` |
| 13 | DirectSound 静音档的 `GetCurrentPosition` **两个出参同值**（`play == write`），游标 = 累计 `Unlock` 字节 | 没有真设备消费；分成两个游标只会让"引擎认为还差多少"失真 | 玩家：音频不推进时不会误判为"播完了" | `shims/dsound.cpp:884-893` |
