# 平台面清单与翻译路线可行性（接口闭包 / 复杂度分级）

- id: REQ-01M6GJMENJ8XEK4ZPTA768YBPW
- type: spike
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [platform, feasibility]

## 一句话
本单是**平台面清单 + 翻译路线可行性**的落点：登记 AGE.EXE / AGERC.DLL 用到的全部外部接口，按**调用者闭包**给每个簇分级（能整簇替换 / 必须实现），并回答"机械翻译 + 平台 shim 是否比 emulator 更可行"。
## 数据源（现查，不抄数）
- `pnpm tools platform census`：导入 / vtable / 动态解析三张表（工具 `repo:tools/platform.mjs`）。
- 逐簇六项 / 工程量 / 切分顺序：`repo:docs/03-emulator/platform-effort.md`（已由分析 subagent 产出，待独立复审）。
- 调用者闭包是一次性分析结果（`--by-caller` 尚未进工具，见「下一步」）。
## 接口总表（AGE.EXE；按 DLL，名字已去 `__imp_`）
- ADVAPI32.dll (5)：RegCreateKeyExA、RegEnumValueA、RegOpenKeyExA、RegQueryValueExA、RegSetValueExA
- EVR.dll (1)：MFCreateVideoSampleFromSurface
- GDI32.dll (23)：AddFontMemResourceEx、CombineRgn、CreateEllipticRgnIndirect、CreateFontIndirectA、CreateICA、CreatePolygonRgn、CreateRectRgn、DeleteDC、DeleteObject、EnumFontFamiliesExA、GetCurrentObject、GetGlyphOutlineA、GetRegionData、GetRgnBox、GetStockObject、GetTextExtentPoint32A、GetTextMetricsA、PtInRegion、RemoveFontMemResourceEx、SelectObject、SetBkMode、SetTextColor、TextOutA
- IMM32.dll (1)：ImmAssociateContext
- KERNEL32.dll (145, 1/3)：CloseHandle、CopyFileA、CreateDirectoryA、CreateEventA、CreateEventW、CreateFileA、CreateFileW、CreateProcessA、CreateSemaphoreA、CreateSemaphoreW、CreateThread、CreateTimerQueue、CreateTimerQueueTimer、DecodePointer、DeleteCriticalSection、DeleteFileA、DeleteTimerQueueTimer、DuplicateHandle、EncodePointer、EnterCriticalSection、EnumSystemLocalesA、ExitProcess、ExitThread、FindClose、FindFirstFileA、FindNextFileA、FlushFileBuffers、FreeEnvironmentStringsW、FreeLibrary、FreeLibraryAndExitThread、GetACP、GetCPInfo、GetCommandLineA、GetComputerNameA、GetConsoleCP、GetConsoleMode、GetCurrentDirectoryA、GetCurrentProcess、GetCurrentProcessId、GetCurrentThread、GetCurrentThreadId、GetDriveTypeA、GetEnvironmentStringsW、GetFileAttributesA、GetFileSize、GetFileTime、GetFileType、GetLastError、GetLocalTime
- KERNEL32.dll (145, 2/3)：GetLocaleInfoA、GetLocaleInfoW、GetLogicalDrives、GetModuleFileNameA、GetModuleFileNameW、GetModuleHandleA、GetModuleHandleW、GetOEMCP、GetProcAddress、GetProcessAffinityMask、GetStartupInfoA、GetStartupInfoW、GetStdHandle、GetStringTypeW、GetSystemDefaultLCID、GetSystemInfo、GetSystemTimeAsFileTime、GetTempFileNameA、GetTempPathA、GetThreadPriority、GetThreadTimes、GetTickCount、GetUserDefaultLCID、GetVersionExA、GetVersionExW、GetVolumeInformationA、GlobalAlloc、GlobalFree、HeapAlloc、HeapCreate、HeapFree、HeapReAlloc、HeapSetInformation、HeapSize、InitializeCriticalSection、InitializeCriticalSectionAndSpinCount、InitializeSListHead、InterlockedDecrement、InterlockedFlushSList、InterlockedIncrement、InterlockedPopEntrySList、InterlockedPushEntrySList、IsDebuggerPresent、IsProcessorFeaturePresent、IsValidCodePage、IsValidLocale、LCMapStringW、LeaveCriticalSection、LoadLibraryA
- KERNEL32.dll (145, 3/3)：LoadLibraryW、LocalAlloc、LocalFree、MoveFileA、MulDiv、MultiByteToWideChar、OutputDebugStringA、QueryDepthSList、QueryPerformanceCounter、RaiseException、ReadFile、ReleaseSemaphore、ResetEvent、ResumeThread、RtlUnwind、SetCurrentDirectoryA、SetEvent、SetFileAttributesA、SetFilePointer、SetFilePointerEx、SetFileTime、SetHandleCount、SetLastError、SetStdHandle、SetThreadAffinityMask、SetThreadPriority、SetUnhandledExceptionFilter、SignalObjectAndWait、Sleep、SleepEx、SwitchToThread、TerminateProcess、TlsAlloc、TlsFree、TlsGetValue、TlsSetValue、UnhandledExceptionFilter、VirtualAlloc、VirtualFree、VirtualLock、VirtualProtect、WaitForMultipleObjects、WaitForSingleObject、WideCharToMultiByte、WriteConsoleW、WriteFile、lstrlenA
- MF.dll (7)：MFCreateAudioRendererActivate、MFCreateMediaSession、MFCreateSourceResolver、MFCreateTopology、MFCreateTopologyNode、MFCreateVideoRendererActivate、MFGetService
- MFPlat.DLL (6)：MFCreateAsyncResult、MFCreateAttributes、MFCreateMediaType、MFInvokeCallback、MFShutdown、MFStartup
- OLEAUT32.dll (3)：SysStringLen、VariantClear、VariantInit
- SHELL32.dll (1)：SHGetSpecialFolderPathA
- SHLWAPI.dll (1)：PathRemoveFileSpecA
- USER32.dll (62, 1/3)：ClientToScreen、CreateWindowExA、DefWindowProcA、DestroyMenu、DestroyWindow、DispatchMessageA、DrawMenuBar、GetAsyncKeyState、GetClientRect、GetCursorPos、GetForegroundWindow、GetMenu、GetMessageA、GetMonitorInfoA、GetParent、GetQueueStatus、GetSubMenu、GetSystemMenu、GetSystemMetrics、GetUpdateRgn、GetWindowLongA
- USER32.dll (62, 2/3)：GetWindowPlacement、GetWindowRect、InvalidateRect、IsDialogMessageA、IsIconic、KillTimer、LoadCursorA、LoadCursorFromFileA、LoadIconA、LoadMenuA、MessageBoxA、MonitorFromWindow、MsgWaitForMultipleObjects、PeekMessageA、PostMessageA、PostQuitMessage、PostThreadMessageA、RegisterClassExA、RegisterWindowMessageA、RemoveMenu、ScreenToClient
- USER32.dll (62, 3/3)：SendMessageA、SetCursor、SetCursorPos、SetMenu、SetRect、SetRectEmpty、SetTimer、SetWindowLongA、SetWindowPos、ShowCursor、ShowWindow、SystemParametersInfoA、TrackPopupMenu、TranslateMessage、UnhookWindowsHookEx、UpdateWindow、ValidateRect、ValidateRgn、WaitMessage、wsprintfA
- VERSION.dll (3)：GetFileVersionInfoA、GetFileVersionInfoSizeA、VerQueryValueA
- WINMM.dll (20)：joyGetDevCapsA、joyGetNumDevs、joyGetPosEx、mciGetErrorStringA、mciSendCommandA、mciSendStringA、mixerClose、mixerOpen、mixerSetControlDetails、mmioAscend、mmioClose、mmioDescend、mmioOpenA、mmioRead、timeBeginPeriod、timeEndPeriod、timeGetDevCaps、timeGetTime、timeKillEvent、timeSetEvent
- d3d9.dll (2)：Direct3DCreate9、Direct3DCreate9Ex
- d3dx9_43.dll (19)：D3DXComputeNormals、D3DXCreateEffectFromResourceA、D3DXCreateSprite、D3DXCreateTexture、D3DXCreateTextureFromFileInMemory、D3DXFrameFind、D3DXLoadMeshHierarchyFromXInMemory、D3DXLoadSurfaceFromSurface、D3DXMatrixDecompose、D3DXMatrixLookAtLH、D3DXMatrixMultiply、D3DXMatrixOrthoLH、D3DXMatrixPerspectiveFovLH、D3DXMatrixRotationAxis、D3DXMatrixRotationYawPitchRoll、D3DXMatrixScaling、D3DXMatrixTranslation、D3DXVec3Normalize、D3DXVec4Transform
- dxva2.dll (1)：DXVA2CreateDirect3DDeviceManager9
- imagehlp.dll (1)：MakeSureDirectoryPathExists
- ole32.dll (9)：CoAddRefServerProcess、CoCreateInstance、CoFreeUnusedLibraries、CoInitialize、CoReleaseServerProcess、CoTaskMemAlloc、CoTaskMemFree、CoUninitialize、PropVariantClear
AGERC.DLL 另有一套（7 DLL / 150 导入，79 被调用）：`KERNEL32 97 · USER32 41 · SHELL32 4 · ADVAPI32 3 · VERSION 3 · WINMM 1 · d3d9 1`。21 个导出 = **18 个地图/碰撞 + 3 个 UI**（`_GetInstance@0`/`_ShowDialog@12`/`_OperateMenu@16`）；AGE 只用这 3 个、对 18 个地图名 **0 引用** ⇒ **原生 shim 顶掉整个 DLL**（`pnpm tools platform census --module agerc` 现查）。
## 动态解析（不在导入表里；EA 可回查）
`DDRAW.DLL`/`DirectDrawCreate` · `DSOUND.DLL`/`DirectSoundCreate` · `AGERC.DLL` · `dwmapi.dll` · `USER32`/`Shell32`/`KERNEL32`/`OleAut32` · 触摸/手势族（`RegisterTouchWindow`/`GetTouchInputInfo`/`SetGestureConfig`）· `IsDebuggerPresent` · `SetThreadExecutionState`。AGE 实测 40 个站点、6 个未解析。
## 簇表（调用者闭包；数字按语料，**父级初稿已被分析文档订正**，差异见 doc §1.3）
| 簇 | 规模 / 站点 | 档 | 判断 |
|---|---|---|---|
| 共享底座（≥5 caller，跨 7 DLL） | 56 导入 / **1487 站点（80.2%）** | T0 | 必须实现；临界区≈mutex 但**必须可重入**，风险在 Enter/Leave 配对 |
| 窗口/消息/region | 68 导入 / 276 站点 | T1 | 用 SDL 实现 Win32 子集；菜单/模态是 T2 口袋 |
| GDI 字体 | 14 导入 / 310 站点 / 45 caller | T1–T2 | FreeType + GDI 形状；度量一致、GGO 档位、DC 绑表面 |
| **DirectDraw 2D 表面**（初稿漏了） | 0 静态导入（DDRAW 动态 2 站点）+ 无类型证据 vtable | T1–T2 | **2D 表面主路径**（静态证据）：`sub_43AE30` 载 DDRAW、`sub_440A20`→`sub_4404B0`→`sub_43C8D0` CreateSurface、`sub_43B460` 表面 +68 GetDC；运行时命中待 doc §6-1；L（6–12 周） |
| 音频（MCI 音轨/MIDI/DSound） | 18 导入 / 38 站点 + DSOUND 动态 | T2 | 可替换：一个混音器 + 三个源；CD-DA/MIDI 的**内容来源**是待核 |
| 视频（MF/EVR/DXVA2） | 15 导入 / 20 站点 / 15 caller | T2 | 可整簇替换（解码+呈现+同步）；**仅占 AGE 站点 1.1%**，省下的平台工作有限 |
| D3D9 + D3DX | 21 导入 / 147 站点 + ≈218 vtable（两段之和；含窗口代码） | T2 | **XL（3–6 月）**，约占总量 30%；不自写则在此停手 |
| Live2D（静态 SDK） | 182 函数（0x4BC342–0x4CF10C）/ **0 导入站点** | T0 本体 | 原样 lift；4 个导入的归属见 doc §1.3 #2；draw 走游戏 renderer（**待核**） |
| AGERC.DLL | 79 导入 / 648 站点；21 导出，AGE 只用 3 | T1 | 原生 shim（3 个导出）；18 个地图导出游戏不用 |
## 结论（按 doc §5.1 的三行划分，互斥）
- AGE 1855 个导入调用点：共享底座 1487（80.2%）+ 可整簇替换 **45（2.4%）** + 其余固定成本 323（17.4%）；DDraw/D3D9 的 vtable 面**未计入**，真实固定成本更高。
- 工程量：**约 10–20 人月**（不含 lift 本体与调试）；D3D9+D3DX 占约 30%。⇒「视频样板最多、收益最大」只在**自包含**上成立，在"省下多少平台工作"上不成立。
## 偏差策略（用户 2026-11-03 口径；逐条见 doc §4.1）
可接受：菜单 no-op · 模态（关闭确认 / 存档命名 / 设置对话框）no-op · 音频 silent · 视频 skip · 3D 不做（若主线允许）。
必须保留：窗口 / 消息泵 / 输入 / region / 计时 · DDraw 2D · GDI 字体 · 存档 / 配置 · 影片 opcode 推进。
ADV 足迹（`sn0000`）：Live2D `0x341/0x345/0x34e/0x352` = 1/3/1/1 次；**`.x` 模型 `0x329` = 0 次**；简单 mesh `0x320` = 6 次 ⇒ Live2D 是 ADV 上唯一的复杂模型，但**若它走 D3D 后端（doc §6-2），D3D9 不能砍**。
每处 stub 必须返回合理值、不阻塞、打日志，并在实现单的 `SCOPE.md` 等价物里登记；偏差后总量级约 **7–15 人月**（再砍 D3D9 再减 3–6）。
## 复杂度分级
- **T0** 语义窄、无隐藏状态（临界区 / 文件 / 计时 / 错误码 / 内存 / Live2D 本体）。
- **T1** 语义清楚但要落到 macOS（窗口 / 音频 / 字体度量 / AGERC）。
- **T2** 状态机 / 时序 / 资源生命周期（D3D9 设备、MF 会话、GDI 度量、DirectDraw）。
## 判据（本单怎么算完）
1. 接口总表的名字集合与 `pnpm tools platform census --json` 一致（只比名字，不抄计数）。
2. 每个簇在 `repo:docs/03-emulator/platform-effort.md` 有六项：复杂度档 / 驱动因素 / 可观测副作用 / Wine 先例 / 工程量级 / 建议。
3. 给出**建议切分顺序**（先做哪个簇、理由、停手点），能被下一个实现单直接引用。
4. 分析文档由**非作者**的 subagent 独立复审（输入边界：只读文档 + 语料；不复用作者）。
## 下一步
- 把 `--by-caller` / `--exclusive` 进 `tools/platform.mjs`（可重算，替代一次性脚本）。
- 路径命中（哪些簇在 TITLE / 战斗真的被走到）另开单：需要原生 trace（doc §6 列了 11 条待核）。
- 本单**不产出实现**；实现单等复审结论出来后再开。
