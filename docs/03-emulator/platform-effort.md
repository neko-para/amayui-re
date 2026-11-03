# 平台面复杂度与工程量（机械翻译 AGE.EXE + 平台 shim）

> 本文件是需求树「平台面清单与翻译路线可行性」（`REQ-01M6GJMENJ8XEK4ZPTA768YBPW`）的**长文分析落点**：
> 逐簇给复杂度档、驱动因素、可观测副作用、Wine 先例、工程量级与建议。进度 / 状态看
> `pnpm tools requirements plan`；接口原始清单由 `pnpm tools platform census` 派生，本文件**不抄全量清单**。
> ★ 本文件给**量化与判据**，不给 go / no-go：路线裁决写在需求单的收口理由里（见该单「判据」第 4 条）。
> ★ 所有计数都是**静态下界**：导入调用点只数「直呼导入」的 `call`；把导入存进函数指针再调用不计
> （实测反例 `mmioDescend`：0 个站点，但 `sub_4BBEF0` 里 `v8 = mmioDescend; v8(...)` 确实在用）。

## 1. 范围与方法

### 1.1 范围

`AGE.EXE` / `AGERC.DLL` 的**外部接口面**：静态导入、COM vtable 调用点、动态解析（`LoadLibrary*` /
`GetProcAddress`）。**不含** lift 本体（把 `.c` 的 3813 个函数复原成可编译源码、修 Hex-Rays 的类型与
内联痕迹）—— 那是另一笔成本，本文件只在 §5 提醒它存在。

### 1.2 簇 = 调用者闭包；证据 = caller / EA / 语料

- **簇不是按 DLL 名分的，也不是按接口名猜的**：先按「哪个函数调了它」求闭包，再看闭包里还有什么。
  ⛔ 本文件没有任何一句「`X` 大概是干 `Y` 的」——凡是语义判断，都绑了 EA（函数体）或错误串 / 配置键 / 字符串。
- **证据三件套**：
  1. `pnpm tools platform census --json`（导入 / vtable / 动态解析；字段真源是 `pnpm tools platform describe`）；
  2. 语料 `corpus/disasm/files/*.lst`（有 EA）+ `*.c`（有函数体，**无 EA**）；
  3. `.c` 里的**错误串 / 配置键 / 资源串**——它们是引擎作者自己写下的用途声明，比接口名可靠。
- **调用者闭包怎么算的**（本文件用的口径，可复算）：
  - 逐行扫 `.lst` 的 `call <import>`（census 的 `direct` / `thunk` / `iat` 三种形态），把站点 EA 经
    `tools/lib/catalog.mjs` 的 `functionOfEa` 映射成**函数起点 EA**；
  - 「调用者数」= 去重后的函数数；「站点数」= 调用次数（同一函数多次调用算多次）。
  - 11 处站点落不进 catalog 的函数区间 ⇒ 该次调用没有 caller；其中 `LeaveCriticalSection` 两处是
    `0x47A6A6` / `0x47A6B6`（`0x5130AD` 是 `CloseHandle`），所以 `LeaveCriticalSection` 是 90 caller / 156 站点。
- **vtable 站点**按 census 的形态判据（`(*(…)(… + off))(…)`），用 `.c` 的行区间把站点映射到函数起点 EA。
  ⛔ **offset 不是接口**：`+68` 既可能是 DirectDraw 表面的 `GetDC`，也可能是 D3D9 设备的 `Present`；
  本文件绝不按 offset 推断接口（见 §1.3 的诚实项）。

### 1.3 与一次性脚本数据的差异（已抽检；凡与语料不符处按语料写）

| # | 一次性脚本 | 语料实测 | 处置 |
|---|---|---|---|
| 1 | Live2D「~123 个 C 函数（0x4BC000–0x4C0000）」 | 该段**只有 41 个函数**引用 `live2d::*` vftable；引用 `live2d::*` 的函数一路延伸到 **0x4CF10C**（`IBaseContext`），共 **182 个**（4C0–4D0 段另 141 个） | 123 是**段内函数数**，不是 SDK 规模；本文件用 182（下界） |
| 2 | Live2D「4 个导入（wsprintfA/mmioRead/OutputDebugStringA/GetCurrentThreadId）」 | 这 4 个导入**合计 108 站点**（wsprintfA 61 / GetCurrentThreadId 18 / mmioRead 2 / OutputDebugStringA 27）；`0x4BBC00–0x4BC280` 区间内只有 14 个（7/5/2/0；其中 `sub_4BBEF0`/`sub_4BC120` 12 个，另 2 个在 `sub_4BBDF0`）；**引用 live2d vftable 的 182 个函数 0 个导入站点** | Live2D 平台面 ≈ 0；别把整簇 4 个导入都归给 WAVE |
| 3 | 「render 段 0x4B0000–0x4BC000 有 128 个 vtable 站点」 | 同一段用 catalog 的 `.c` 行区间映射是 **136** 站点 / 28 函数；且该段含**非 D3D 代码**：`sub_4B92F0`（窗口过程，14）、`sub_4BA310`（`CreateWindowExA`，13）、`_WinMain@16`（8） | 136 是段内站点，不是「D3D 站点」；两段之和 = 136+82 = **218**（含窗口代码）；「210」是**旧近似值**，不是 218 的上界；剔除窗口代码后的 D3D 站点未逐段枚举 |
| 4 | 簇表没有 **DirectDraw** 一行（DDraw 只出现在「动态解析」清单里） | DDraw 是**2D 表面主路径（静态证据；运行时命中待 §6-1）**：`sub_43AE30` 载入 `DDRAW.DLL`/`DirectDrawCreate`；`sub_440A20` → `sub_4404B0` → `sub_43C8D0` 走 `IDirectDraw::CreateSurface`（`DDSURFACEDESC` 108 字节；`DDSURFACEDESC2` 是 124；caps 三档 `2112` = `0x840` 系统内存 / `16448` = `0x4040` 显存 / `64` = `0x40` 离屏平面）；`sub_43B460` 用表面 vtable **+68** 取 HDC（错误串「GetDC エラー：サーフェスに対してのロックに失敗しました」） | **补一个簇**（§3.4）；它是「固定成本」里最大的一块之一 |
| 5 | AGERC「导出的 21 个（地图 / 碰撞族）」 | 21 个导出＝18 个地图 / 碰撞（`_FindClash@20` / `_GetLand@20` / `_SetMap*` …）+ **3 个 UI**（`_GetInstance@0` / `_ShowDialog@12` / `_OperateMenu@16`）；AGE **只**用这 3 个（`sub_48E730` 三处 `GetProcAddress`），对 18 个地图名 **0 引用** | 两个说法都对一半：对**游戏**它只是设置对话框；地图族是给编辑器工具用的 |
| 6 | 音频「~12 导入」 | 18 个 WINMM 导入 / 38 站点 / 24 caller（`mci*` 2+1、`mixer*` 3、`mmio*` 4（`mmioDescend` 走函数指针）、`joyGet*` 3、`timeBegin/EndPeriod`+`timeSetEvent/KillEvent` 4），**另加**动态 `DSOUND.DLL`（§3.5） | 用 18 + DSOUND 写 |

**诚实项**（同 census，不重复其全文）：vtable offset 无类型证据；调用点是静态计数、不代表路径命中；
`arg` 只是窗口内最近的候选串；`LoadLibraryA/W` 的站点同时计入导入与动态两张表。

## 2. 总表

> 「接口数」= 有调用的导入数 / 站点数（导入表里没被直呼的不算；见 §1.3 的函数指针反例）。
> ★ **各行会重叠**（一个导入可以既属某簇、又是共享底座）——本表是**两个切法**并列，不是 9 个互斥分区；
> 互斥的站点划分只在 §5.1 那张三行表里。
> 复杂度档：**T0** 语义窄、无隐藏状态 · **T1** 语义清楚但要落到 macOS · **T2** 状态机 / 时序 / 资源生命周期。
> 工程量级按**一个熟悉本仓的开发者**估，S ≤ 4 周、M ≈ 1–2 月、L ≈ 1.5–3 月、XL ≥ 3 月；逐簇区间含不确定性，是量级不是假精度。

| 簇 | 接口数（调用中 / 站点） | 复杂度档 | 工程量级 | 建议 | 主要风险 |
|---|---|---|---|---|---|
| 共享底座（≥5 caller 的 56 个导入，跨 7 个 DLL） | 56 / 1487（占 AGE 全站点的 80.2%） | **T0**（临界区、文件、计时、内存、错误码） | **S**（1–3 周） | **逐个实现**：C++ 薄封装，导出一套 Win32 形状的头 | 临界区**可重入**与生命周期；`GetLastError` 的线程局部 |
| 窗口 / 消息 / region | 68（USER32 60 + IMM32 1 + GDI region 7）/ 276 | **T1**（菜单与模态是 T2 口袋） | **M**（4–8 周） | **实现**：Win32 子集宿主（SDL3 底下） | 菜单栏 `HMENU`、模态对话框、IME、`WM_TIMER` 语义 |
| GDI 字体 | 14 / 310 / 45 caller（整个 GDI32 是 23 / 367 / 54 caller） | **T1–T2** | **M–L**（4–8 周） | **实现**：FreeType 后端 + GDI 形状 | 度量一致性；`GGO_GRAY4`/`GGO_GRAY8` 档位；DC 绑在表面上 |
| **DirectDraw 2D 表面**（新增） | 0 静态导入（`DDRAW.DLL` 动态 2 站点）+ 无类型证据的 vtable | **T1–T2** | **L**（6–12 周） | **实现**：软件或 GPU 表面 + 表面 DC | `Blt`/颜色键、`Lock` 的像素布局、显示模式 |
| 音频（MCI 音轨 / MIDI / DirectSound） | 18 / 38 / 24 caller + DSOUND 动态 | **T2** | **L**（6–12 周） | **替换**：一个混音器 + 三个「源」 | CD-DA 与 MIDI 的**内容**从哪来；淡入淡出与语音闪避 |
| 视频（MF / EVR / DXVA2） | 15 / 20 / 15 caller | **T2** | **L**（6–12 周） | **替换**：解码 + 呈现 + 同步；照 `MFSession` 的接口形状 | 会话状态机（lifted 代码是 MF 的**客户端**）、A/V 同步 |
| D3D9 + D3DX | 21 / 147 / 69 caller（其中 D3DX 数学 **128 站点**） | **T2** | **XL**（3–6 月） | **不自写**：嵌入成熟实现，或在此**停手** | 设备状态机 + 着色器翻译 + 网格层级；「有文档 ≠ 便宜」 |
| Live2D（静态 SDK） | 182 个 SDK 函数本体 **0 导入站点** | **T0** 本体 / **T2** 依赖 | **S**（1–2 周）+ D3D9 依赖 | **原样 lift**（C++），不 shim | 纹理由游戏侧建、draw 走游戏 renderer |
| AGERC.DLL | 79 / 648；21 导出，AGE 只用 3 | **T1** | **S**（2–4 周；若允许「无设置界面」可到 3 日） | **原生 shim**（3 个导出），不 lift | 设置对话框是一整套 Win32 对话框 UI |

合计（不含 lift 本体与调试）：**约 10–20 人月**；其中 D3D9+D3DX（XL）那一簇占约 **30%**（13–26 周 / 43–87 周）。

## 3. 逐簇分析

### 3.1 共享底座（≥5 caller；跨 KERNEL32 / USER32 / GDI32 / WINMM / ole32 / SHELL32 / d3dx9）

**证据.** 56 个导入有 ≥5 个调用者，合计 **1487 / 1855 站点（80.2%）**；按 DLL 分布：
KERNEL32 **27**、USER32 8、GDI32 7、d3dx9_43 6、WINMM 4、ole32 3、SHELL32 1。
★ **这个簇不是按 DLL 分的，是按「散布程度」分的**：`SelectObject`（143 站点 / 33 caller）既被字体簇用、
也被窗口簇用；`MessageBoxA`（28 / 18）散落在各处 —— 它们**不能随某个簇一起替换**，只能一次实现、全库共用。
前几名：
`EnterCriticalSection` 106 站点 / 92 caller、`LeaveCriticalSection` 156 / 90、`wsprintfA` 61 / 41、
`timeGetTime` 38 / 37、`SelectObject` 143 / 33、`ReadFile` 49 / 29、`GetLastError` 37 / 28、
`CloseHandle` 38 / 27。线程与同步另成一小片：45 个导入 / 236 站点（`SetEvent` 27、`Sleep` 36、
`Interlocked*` 47、`WaitForSingleObject` 17、`WaitForMultipleObjects` 5、`CreateThread` 2）。
★ 有 61 个导入**没有直呼站点**；其中 `SignalObjectAndWait` / `QueryPerformanceCounter` /
`CreateTimerQueueTimer` / `Interlocked*SList` 等是 0 站点（可能确实没用，也可能是走了函数指针——见 §1 的
`mmioDescend` 反例）。⇒ 所有「站点数」都是**下界**。
★ 零散底座不单列（调用者 < 5）：注册表 4 导入 / 11 站点（`RegOpenKeyExA` 等，`set:RegKey` 一族）、
`VERSION` 3 个 / 9 站点（版本资源）、`imagehlp` 1 个 / 3 站点（`MakeSureDirectoryPathExists`）、
`SHELL32` 1 个 / 5 站点（`SHGetSpecialFolderPathA`，已在共享底座里）；它们归入 §5.1 的「其余固定成本」。

**复杂度档：T0.** 语义窄、无隐藏状态；唯一的坑是并发原语本身。

**驱动因素.**
1. **临界区不是裸 mutex**（正面回答假设 ①）：Windows 的临界区**可重入**（同一线程可多次 `Enter`，
   必须等次数 `Leave`）[`CRITICAL_SECTION` 文档](https://learn.microsoft.com/en-us/windows/win32/sync/critical-section-objects)、
   有 spin count（`InitializeCriticalSectionAndSpinCount` 本作调了 6 次）、有 `TryEnter`（**本作没有导入**，
   零站点）、`DeleteCriticalSection` 释放系统资源。⇒ 实现用 `std::recursive_mutex`（或自写「线程 id + 计数」）
   即可；**不必**复刻 spin（只影响性能）、**不必**实现 `TryEnter`、**不必**处理 FIFO 公平性（文档明说不保证）。
   真正的风险不是语义，而是**配对**：`Leave` 站点比 `Enter` 多 50 处（循环/早退路径），一旦有一处配错就是**死锁**，
   表现为画面停帧、无输入响应（不是报错）。
2. `GetLastError` 的**线程局部**语义：错误码在 28 个 caller 里被读来拼日志 / 决定分支，跨线程共享就错。

**可观测副作用.** 死锁 ⇒ 画面冻结、`WM_TIMER` 不再触发、存档写一半；事件丢 ⇒ 影片不开始 / 线程不退出
（进程残留）；`timeGetTime` 基准错 ⇒ 帧步进漂移、ADV 自动消息时长偏移；文件封装错 ⇒ 存档 / 配置读写失败（有错误串可看）。

**Wine 先例.** Wine 的 ntdll 实现了同语义的原语：`RtlInitializeCriticalSectionEx`（分配 `DebugInfo`、写
`RecursionCount`、单核时把 spin count 归 0）、`RtlEnterCriticalSection`（`RecursionCount++` ⇒ 可重入）、
`RtlLeaveCriticalSection`（按次数递减）——
[`dlls/ntdll/sync.c`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ntdll/sync.c)。
但**本仓不需要它们的代码**：这层是纯本地原语封装，Wine 只作「语义有权威实现」的旁证。

**工程量级：S（1–3 周）.** 驱动工作量的 1–2 件事：① 临界区 / 事件 / 等待的**配对正确性**（含跨线程关闭顺序）；
② 文件 / 计时 / 错误码的 Windows 形状（`ReadFile` 的短读语义、`SetFilePointer` 的 32 位偏移、`GetLastError` 的 TLS）。

**建议：逐个实现.** 替换后要复现的行为边界：`ReadFile` 返回「读了多少」而不是「成功/失败」；`GetLastError`
只在同线程可读；`Sleep(0)` 让出时间片；`timeGetTime` 单调、毫秒、`timeBeginPeriod` 只影响精度提示。

### 3.2 窗口 / 消息 / region（USER32 + IMM32 + GDI region）

**证据.** USER32 62 个导入、**60 个有调用**（未调用：`LoadIconA` / `ShowCursor`）、261 站点、102 caller。
按「独占导入」（只被这一个函数用的导入）看，主窗口相关代码集中在四个函数：

| 函数 | 独占导入数 | 例子 |
|---|---|---|
| `sub_4B92F0`（**窗口过程**；`_WinMain@16` 在 `0x4BA973` 把它写进 `WNDCLASSEX.lpfnWndProc`） | 14 | `DefWindowProcA` / `DestroyWindow` / `TrackPopupMenu` / `ValidateRect` / `SetWindowLongA` |
| `_WinMain@16` | 7 | `RegisterClassExA` / `GetStartupInfoA` / `GetMenu` / `ImmAssociateContext` |
| `sub_412290`（消息泵） | 4 | `GetMessageA` / `TranslateMessage` / `WaitMessage` / `IsDialogMessageA` |
| `sub_4B9090`（region 命中测试） | 5 | `CreateRectRgn` / `GetRegionData` / `GetRgnBox` / `GetUpdateRgn` / `ValidateRgn` |

另有 `sub_4CF8C0`（第二条消息线程：`MsgWaitForMultipleObjects` / `PostThreadMessageA` / `RegisterWindowMessageA`）、
`sub_413DD0`（多显示器：`MonitorFromWindow` / `GetMonitorInfoA`）、`sub_4BA310`（`CreateWindowExA` / `UpdateWindow`）。
GDI32 的 region 族（`CombineRgn` 等 7 个导入 / 14 站点）也属这一簇。

**复杂度档：T1.** 单个接口的语义都清楚；难点在**窗口系统的整体形状**。

**驱动因素.** ① **菜单栏**：`LoadMenuA` / `DrawMenuBar` / `TrackPopupMenu` / `CheckMenuItem`（AGERC 里 80 站点）
/ `InsertMenuItemA` —— Win32 菜单是一套独立的对象系统（`HMENU` + 资源 + 命令 id），不是「画几个矩形」；
② **模态**：`DialogBoxParamA` / `CreateDialogParamA`（AGERC 各 14 站点）、`IsDialogMessageA`（消息泵；
键盘导航 / Tab / 默认按钮都在它里面）；③ **输入**：`GetAsyncKeyState`（轮询）+ `WM_*` 消息（推送）两套并存；
④ **IME**：`ImmAssociateContext`（1 站点）——日文输入要接宿主 IME；⑤ **计时器**：`SetTimer` / `KillTimer` +
`timeSetEvent`（多媒体定时器，另一条时间线）；⑥ **窗口样式 / 全屏 / 显示模式**：样式在
`SetWindowLongA(hWnd,-16,…)`（`sub_43AEB0`）里改，全屏走 DirectDraw 的 `SetDisplayMode`（不在 USER32）。

**可观测副作用.** 键盘 / 鼠标无响应 ⇒ 游戏停在标题；菜单不出 / 命令 id 错 ⇒ 设置、存档菜单点了没反应；
`WM_TIMER` 合并或丢 ⇒ 自动消息停留时间不对、光标闪烁异常；IME 死 ⇒ 存档命名 / 名字输入打不出假名；
region 命中错 ⇒ 点击落到错误的 UI 元素（尤其带掩码 / 非矩形按钮）；多显示器 / DPI 错 ⇒ 窗口位置与命中偏移。

**Wine 先例.** Wine 把这一整套放在 `win32u`（`window.c` / `message.c` / `menu.c` / `dce.c` / `imm.c` /
`input.c` / `painting.c` / `class.c`），macOS 上再接 AppKit（`UNIX_LIBS = CORETEXT_LIBS APPKIT_LIBS`）——
即「Win32 语义在 macOS 上落地」这件事有**完整先例**，不是无人区
（[`dlls/win32u/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/win32u/Makefile.in)）。
⚠ 但先例是「实现 Win32」，不是「把 SDL 包一层」——两者的差别就是上面那些状态机。

**工程量级：M（4–8 周）.** 驱动工作量的 1–2 件事：① 菜单 + 模态对话框（含资源 / 命令 id / 键盘导航）；
② 输入 + IME + 计时器的顺序与合并语义。

**建议：实现（Win32 子集宿主）.** 保留 Win32 形状（lifted 代码直接编译），底下用 SDL3 窗口 + 自绘菜单。
替换后要复现的行为边界：消息**顺序**（`WM_PAINT` 合并、`WM_TIMER` 不排队）、`IsDialogMessageA` 的键盘路由、
`WM_QUIT` 的传播、`GetClientRect` 与 DPI 的关系。明确**不做**：`LoadIconA` / `ShowCursor`（0 站点）。

### 3.3 GDI 字体

**证据.** GDI32 23 个导入 / 367 站点 / 54 caller。其中**字体 / 文本** 14 个导入、**310 站点**：

| 导入 | 站点 / caller | 用途（由调用点定） |
|---|---|---|
| `SelectObject` | 143 / 33 | 换字体 / 位图进 DC |
| `GetTextMetricsA` | 45 / 28 | 取 `tmAscent` / `tmHeight`（基线、行高） |
| `GetGlyphOutlineA` | 32 / 20 | **取字形位图 / 量度**（`fuFormat` 见下） |
| `GetTextExtentPoint32A` | 31 / 12 | 量一段文本的宽（排版用） |
| `CreateFontIndirectA` | 20 / 12 | 按 `LOGFONT` 建字体 |
| `GetCurrentObject` | 19 / 17 | 保存/恢复 DC 原对象 |
| `TextOutA` | 10 / 2 | 直接往 DC 上画串（旧路径） |
| `SetTextColor` / `SetBkMode` | 4 / 2；1 / 1 | 颜色 / 透明背景 |
| `AddFontMemResourceEx` / `RemoveFontMemResourceEx` / `EnumFontFamiliesExA` / `CreateICA` / `DeleteDC` | 各 1 / 1 | 内存字体、枚举、屏幕 DC |

★ **AA 档位是引擎自己选的，而且有据可查**：`sub_46E3E0`（EA 0x46E3E0）、`sub_46F2D0`（EA 0x46F2D0）、
`sub_471180`（EA 0x471180）三个入口同一模式——`fuFormat = 5`（`GGO_GRAY4_BITMAP`，17 级）、`fuFormat = 6`
（`GGO_GRAY8_BITMAP`，65 级）、否则 `fuFormat = 1`（`GGO_BITMAP`，1 位）；`sub_46DED0`（EA 0x46DED0）里是
字面 `5u`。这三个常量与微软文档的取值一一对应
（[`GetGlyphOutlineA`](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getglyphoutlinea)：
`GGO_GRAY4` = 17 级、`GGO_GRAY8` = 65 级）。⇒ **引擎是拿 GDI 的灰度位图自己合成**，不是让 GDI 直接画字
（`TextOutA` 只在旧路径 / 少数 UI 上出现）。
★ 字体名是写死的「ＭＳ ゴシック」（`asc_52686C`，引用点 `0x456D09`），`LOGFONT` 的
`lfCharSet = DEFAULT_CHARSET`、`lfQuality = DEFAULT_QUALITY`。

**复杂度档：T1–T2.** 「语义清楚」（文档全在）但要落到 macOS，且**度量与栅格化必须自洽**才能保住排版。

**驱动因素.**
1. **度量一致性**：排版读的是宿主度量（`60-ops-text.md:142`：「排版的度量（字形宽度 / 行高）来自宿主，
   本版不规定」；`62-adv-text.md:5` 也把字形位图 / 度量 / 混合划到平台边界）。所以 shim 只要**内部自洽**
   （同一字体、同一字号，`GetTextExtentPoint32A` 的宽 = `GetGlyphOutlineA` 的 advance 之和 = 实际画出来的宽度），
   引擎逻辑就对；但换成 Amayui 字体后 ascent 与 MS Gothic 不同，**行高 / 基线会整体偏**——emulator 为此加了
   「基线抬 2px」的宿主补偿（`apps/emulator-cxx/SCOPE.md` §1.1），说明这不是理论风险，是实测过的 1–2px 偏差。
2. **GGO 位图格式**：`DWORD` 对齐的行、值域 0..16 / 0..64、`gmBlackBoxX/Y`、`gmptGlyphOrigin`、`gmCellIncX`；
   FreeType 给的是 8 位覆盖度，必须**量化到 17 / 65 级**（乘 255 再除）。emulator 的
   `src/core/text_alpha.h` 已把这条做成实现（`apps/emulator-cxx/SCOPE.md` §1.2）。
3. **两个 DC**：字体对象自带一个 **`CreateICA("DISPLAY")` 的屏幕 DC**（`sub_464FD0`，EA 0x464FD0，
   存在 `this+1108`），`GetTextMetricsA` / `GetGlyphOutlineA` 都在**它**上面量；`TextOutA` 画在**表面 DC**
   （`this+1104`，由 `sub_43B460` 从 DDraw 表面取，见 §3.4）。⇒ 量度与栅格化分属**两个 DC**，
   GDI 的字体 realization 是设备相关的：两个 DC 的 DPI / 像素格式 / 字体映射必须一致，否则「量出来的宽」
   不等于「画出来的宽」。这是度量自洽之外的第二个坑。
4. **DC 绑在表面上**：本作**没有**导入 `GetDC`/`ReleaseDC`/`CreateCompatibleDC`/`CreateDIBSection`；
   表面 DC 来自 **DirectDraw**：`sub_455DB0` → `sub_43B460` → 表面 vtable **+68**（错误串
   「GetDC エラー：サーフェスに対してのロックに失敗しました」），用完 `sub_455E00` → `sub_43B4C0` 释放。
   ⇒ 字体簇**不能单独替换**：它依赖 DDraw 簇（§3.4）。

**可观测副作用.** 行宽算错 ⇒ 换行位置错（`62-adv-text.md` 的 `set:AutoLineFeed` 用「候选 x = 游标 x + 该字量度宽」）、
文本越界被裁；ascent 错 ⇒ 整段文字上下偏 1–3px、两行重叠；GGO 档位错 ⇒ 字发虚 / 台阶（1 位 vs 17 级 vs 65 级）；
`SelectObject` 恢复错 ⇒ 后续绘制用错字体（串色 / 尺寸跳变）；缺字形 ⇒ 豆腐块（尤其简中译文字形）。

**Wine 先例.** Wine 的 GDI 字体在 `win32u`：`font.c` + `freetype.c`，编译期接 FreeType + fontconfig，
macOS 上还接 CoreText（[`dlls/win32u/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/win32u/Makefile.in)）。
即「`CreateFontIndirect` / `GetTextMetrics` / `GetGlyphOutline`（含 GGO_GRAY*）/ `TextOut` 用 FreeType 在 macOS 上实现」
有**成熟先例**；Wine 的 `gdi32` 现在只剩对象/DC/元文件层
（[`dlls/gdi32/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/gdi32/Makefile.in)）。
⚠ 但 Wine 是「跑 Windows 程序」，它的度量选择服务于兼容性，不保证与本作引擎的**两次度量**顺序一致。

**工程量级：M–L（4–8 周）.** 驱动工作量的 1–2 件事：① 度量与栅格化的自洽（含 17/65 级量化、`gmBlackBox` 对齐）；
② 表面 DC 的生命周期（`GetDC`/`ReleaseDC` 与 Blt 的先后）。

**建议：实现（FreeType 后端 + GDI 形状）.** 复现边界：`GetTextMetricsA` 的 `tmAscent/tmHeight/tmAveCharWidth`、
`GetGlyphOutlineA` 的三种 `fuFormat` 与 `MAT2`（本作是单位矩阵）、`SelectObject` 的返回值语义、
`TextOutA` 的「串里含换行也不换行」等 GDI 怪癖（旧路径用到）。⛔ 不要指望「换个字体库就行」：本仓要的是
**GDI 的形状**，FreeType 只提供字形。

### 3.4 DirectDraw 2D 表面（新增簇；父级清单里只在「动态解析」出现）

**证据.**
- 动态解析：`sub_43AE30`（EA 0x43AE30）`LoadLibraryA("DDRAW.DLL")` + `GetProcAddress("DirectDrawCreate")`
  （census 的两个动态站点，`arg` 已解析）；
- 表面创建：`sub_440A20`（EA 0x440A20）先 `GetDisplayMode(+48)` / `GetAvailableVidMem(+92)`，再经
  `sub_4404B0` 调到 `sub_43C8D0`（EA 0x43C8D0）——由它填 `DDSURFACEDESC`（`dwSize=108`、`dwFlags=7`；
  `DDSURFACEDESC2` 是 124），按 `a6` 写三档 caps（`0x840` = 系统内存 / `0x4040` = 显存 / `0x40` = 离屏平面），
  再 `IDirectDraw::CreateSurface`（vtable **+24**），随后 `QueryInterface(IID_IDirectDrawSurface3)`（`unk_52E288`）；
  错误串写「ddCSpriteSurface / ddCSurface」；
- 表面取 DC / 还 DC：`sub_43B460`（EA 0x43B460）表面 vtable **+68**，错误串「GetDC エラー：サーフェスに対しての
  ロックに失敗しました」；`sub_43B4C0`（EA 0x43B4C0）表面 vtable **+104**，错误串「ReleaseDC エラー：
  サーフェスに対してのアンロックに失敗しました」——这是全表里**唯一**有字符串证据的两个 offset；
- 读像素：存档缩略图要「锁定表面 → 像素 + 行宽 + 通道掩码」（`data/spec/chapters/65-save.md:86`，`65-save.md:41`），
  对应的 `Lock`/`Unlock` 落在 vtable 直方图的 `+100`（11 站点）/ `+128`（16 站点）；
- 配置开关：`display:UseDirectDrawEmuration`（`0x4067C6` 读）经 `sub_43C380` 传给 `DirectDrawCreate`
  的 `DDCREATE_EMULATIONONLY(2)`；而 `sub_440A20` 的 `a6` 是**另一件事**——`sub_43C8D0` 在 `a6=0` 时的默认
  caps `2112`（系统内存表面）。两者不是同一个开关；`set:AlwaysBackupSurface` / `set:EnableMemFlip` 同理。

**复杂度档：T1–T2.** 接口少（`CreateSurface` / `Blt` / `BltFast` / `Flip` / `Lock` / `Unlock` / `GetDC` /
`ReleaseDC` / `SetDisplayMode` / `SetCooperativeLevel` / clipper），但**每个都带像素格式与生命周期**。
★ 下面这些 offset→方法名（`Blt` +20 / `BltFast` +28 / `Flip` +44 / `Lock` +100 / `Unlock` +128 /
`SetCooperativeLevel` +80 / `SetDisplayMode` +84）是**按 DDraw 的方法序推测**，census **不给类型证据**；
只有 `+68`/`+104` 有错误串直接作证（`GetDC` / `ReleaseDC`），`+100`/`+128` 由「缩略图要锁表面」间接支持。

**驱动因素.**
1. **Blt 语义**：`+20` 95 站点、`+28` 55 站点、`+44` 40 站点（`+44` 与 D3D9 的 `Present` 同 offset，**不可区分**）。
   颜色键（透明色）、源/目标矩形、`DDBLT_WAIT`、越界裁剪——每一条都会改变画面；
2. **表面 DC**：GDI 文本画进表面（§3.3），所以 DC 的像素格式（16/24/32bpp）、行宽、`GetDC` 期间的锁都必须对；
3. **`Lock` 的布局**：存档缩略图按「表面像素 + 行宽 + 通道掩码」组装 BMP（`65-save.md:86`）⇒ 像素格式 / pitch 错，
   缩略图就花；
4. **显示模式 / 全屏**：`SetDisplayMode`（vtable **+84**）与 `SetCooperativeLevel`（**+80**）——
   本作**没有**导入 `ChangeDisplaySettings*`，全屏只能走 DDraw。

**可观测副作用.** 整屏黑 / 花屏；文本整块不见（DC 取不到或格式不匹配）；透明处变黑框（颜色键错）；
全屏切换失败或分辨率不变（SetDisplayMode 错）；存档缩略图颜色错位 / 上下颠倒 / 花（pitch 或掩码错）；
动画闪烁（Flip 与 Blt 的次序）。

**Wine 先例.** Wine 的 `ddraw.dll` 是**完整实现**：`surface.c` / `device.c` / `palette.c` / `clipper.c` /
`vertexbuffer.c` / `viewport.c`，底下走 `wined3d`
（[`dlls/ddraw/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ddraw/Makefile.in)）。
即「DDraw 表面语义」有权威实现；但它是**Windows API 实现**，接到 macOS 仍需一个 3D 后端（wined3d → OpenGL/Vulkan）。

**工程量级：L（6–12 周）.** 驱动工作量的 1–2 件事：① Blt/BltFast 的全部标志与颜色键、加上表面 DC 的格式矩阵；
② 全屏 / 显示模式 / 协作级别（它们会反过来影响窗口簇）。

**建议：实现.** 静态证据下 DDraw 是 **2D 表面主路径**（Blt 95 / +28 55 / +44 40 vs D3DXSprite 1）；运行时命中待
§6-1，D3D9 侧还有 D3DXSprite 与 Live2D 两条 2D 路。复现边界：`Blt` 的裁剪与颜色键、`GetDC` 期间的
表面锁、`Lock` 返回的 pitch/掩码、`Flip` 在窗口模式下的行为。**可选简化**：利用引擎自带的「模拟」档
（系统内存表面）把 DDraw 实现成纯软件 blitter，省掉 GPU 后端——但必须在 `set:DrawMode` /
`display:UseDirectDrawEmuration` 的真实取值下验证（见 §6 待核 1）。

### 3.5 音频（MCI 音轨 / MCI MIDI / mixer / DirectSound）

**证据.** 18 个 WINMM 导入 / 38 站点 / 24 caller，按闭包分成**三套互不相干的后端**：

| 后端 | 证据 | 站点 |
|---|---|---|
| **CD-DA**（红皮书音轨） | `sub_4041C0`（EA 0x4041C0）用 `MCI_OPEN`(0x803) 打开 **CD 设备**（设备名由 `*(this+1344)+'A'` 拼驱动器号），错误串「CD::Open / Play / Stop / Repeat / Pause エラー」；`mciSendCommandA` 5 站点覆盖 open/play/stop/pause/resume | 5 + 6（错误串） |
| **MIDI** | `sub_485DB0`（EA 0x485DB0）`mciSendStringA("pause midi")`、`sub_485DD0`（`"close midi wait"`）、`sub_485DF0`（`"stop midi"`）；类名 `MIDI`（vftable `??_7MIDI@@6B@`） | 3 |
| **DirectSound**（SE / 语音） | `sub_4B5C50`（EA 0x4B5C50）动态 `LoadLibraryA("DSOUND.DLL")` + `GetProcAddress("DirectSoundCreate")`；`sub_4B5CF0` 调它、`SetCooperativeLevel`（+24，优先级 1/2/3）、`CreateSoundBuffer`（+12，`DSBUFFERDESC` 的 `dwSize=36`）；WAV 由 `sub_4BBEF0`（EA 0x4BBEF0）用 `mmio*` 读 RIFF（「wave/fmt/data チャンク」），`sub_4BC120` 按 0x2000 字节流式喂 | 动态 2 + mmio 5 |
| 音量 | `sub_404610` / `sub_404710` 用 `mixerOpen` / `mixerSetControlDetails` / `mixerClose` 扫 `this[850]` 个控件，按 `a2/dbl_51DD10*range` 换算 | 6 |
| 手柄 | `joyGetNumDevs` / `joyGetDevCapsA` / `joyGetPosEx`（`sub_476E40` 等 5 个 caller） | 7 |

**复杂度档：T2.** 不是「接口多」，是**三套状态机 + 内容来源 + 时序**。

**驱动因素.**
1. **CD-DA 是没有文件的**：MCI 的 `MCI_PLAY`（0x806）带「起始/结束磁道」，`MCI_STATUS` 读总磁道数
   （`this+1348`）——它放的是**光盘音轨**。要把这条复现出来，得先有音轨（提取成 wav/ogg？）或接受「无 BGM」。
   这是**内容问题**，不是 API 问题，且它决定「音频簇能不能整簇替换」；
2. **MIDI 需要合成器**：MCI sequencer 在 macOS 上不存在；要么接软音源（FluidSynth + SoundFont），要么预渲染；
3. **DirectSound 的缓冲/协作级别/主缓冲**：`CreateSoundBuffer` + `Play`/`Stop`/`SetVolume`/`SetFrequency` +
   通知事件；SE 并发数、语音闪避（`sound:MusicFadeOnVoicePlaying`）都靠它；
4. **计时**：`timeSetEvent`（1 站点）+ `timeGetTime` 是音频/影片的共同时钟（`timeGetTime` 归共享底座，不并进来）；
5. **手柄**：`joyGet*` 是输入，不是音频；产品上可以整簇「没有设备」（emulator 就是这么做的，`SCOPE.md` §0.4）。

**可观测副作用.** 无 BGM（静音）；音轨号错 / 循环点错 / 播完不回头；SE 叠音爆音或截断；音量滑杆反向 / 无效；
语音与 BGM 闪避不生效；MIDI 音色不对（用系统 GM 音源 vs 游戏预期）；CD 设备不存在时错误串进日志、游戏继续（有 fallback）。

**Wine 先例.** Wine 有 `mcicda.dll`（CD 音轨）、`mciseq.dll`（MIDI sequencer）、`dsound.dll`（完整
`buffer.c` / `mixer.c` / `primary.c` / `sound3d.c`）、以及 winmm 的 mixer：
[`dlls/mcicda/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mcicda/Makefile.in) ·
[`dlls/mciseq/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mciseq/Makefile.in) ·
[`dlls/dsound/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/dsound/Makefile.in)。
⇒ 「MCI CD / MCI MIDI / DSound 的形状」都有先例；**没有**先例的是「本作光盘上的音轨与 MIDI 从哪来」。

**工程量级：L（6–12 周）.** 驱动工作量的 1–2 件事：① 三个后端合一（一个混音器 + 三个源 + 淡入淡出/闪避的
时序）；② CD-DA / MIDI 的**内容管线**（提取音轨、选音源、决定是否降级为「无 BGM」）。

**建议：整簇替换，但先定内容策略.** 替换后要复现的行为边界：`play(track, loop)` / `stop` / `pause` / `resume`
的状态与错误码（错误串会进日志）、音量 0–100 的映射（`mixerSetControlDetails` 的线性换算）、
`sound:MusicFadeOnVoicePlaying` 的闪避曲线。手柄：**建议整簇不实现**（emulator 的既有口径），
或只保留 `joyGetPosEx` 的「无设备」分支。

### 3.6 视频（MF / EVR / DXVA2）

**证据.** 15 个导入 / 20 站点 / 15 个 caller，全部集中在 `0x47A000–0x486000` 的**一个播放子系统**里：

| caller（EA） | 用到的导入 | 做什么（由调用点定） |
|---|---|---|
| `sub_47AD40`(0x47AD40) / `sub_47B600` | `MFCreateAttributes` | 建属性袋 |
| `sub_47B130`(0x47B130) | `MFCreateAsyncResult` | 发起异步操作：`MFCreateAsyncResult(callback, state)`、把结果入队、`SetEvent` 唤醒 |
| `sub_47B200`(0x47B200) | `MFInvokeCallback` | 工作线程：取队列、读样本、`MFInvokeCallback`、`Release` |
| `sub_47C910`(0x47C910) | `Direct3DCreate9Ex` + `DXVA2CreateDirect3DDeviceManager9` | 建 D3D9Ex 设备（对 `Direct3DCreate9Ex` 的结果调 vtable **+80**，7 参形态、`BehaviorFlags` 0x44）+ DXVA 管理器 |
| `sub_47CA30` | `MFCreateVideoSampleFromSurface` | 把 D3D 表面包成 `IMFSample` |
| `sub_47CE00` / `sub_47D080` | `MFCreateMediaType` ×2 | 描述输入/输出类型 |
| `sub_4803B0` | `MFStartup` | 启动 MF |
| `sub_4807B0` | `MFCreateTopologyNode` | 建拓扑节点 |
| `sub_483A80`(0x483A80) | `MFCreateTopologyNode` ×2、`MFCreateAudioRendererActivate`、`MFCreateVideoRendererActivate`、`MFGetService` | 按媒体类型分岔：音频→SAR，视频→**EVR**（`MFCreateVideoRendererActivate(hWnd)`，`this+1432` 是窗口句柄），再用 `SetUnknown(GUID, 对象)` 挂一个属性对象（GUID 见 §6-3） |
| `sub_484760`(0x484760) | `MFCreateMediaSession`、`MFCreateSourceResolver`、`MFCreateTopology` | 建 session / resolver / topology；`MFSession` 的 vftable 里有它（`??_7MFSession@@6B@`） |
| `sub_4853D0` | `MFShutdown` | 关 |
| `sub_485880` / `sub_481780` | `MFGetService`、`PropVariantClear` | 取服务 / 清 `PROPVARIANT` |

★ 另有 `sub_485DB0/485DD0/485DF0` 是 **MIDI**（`??_7MIDI@@6B@`），不属于视频。

**复杂度档：T2.** 15 个导入看着像样板，**但它是 MF 的客户端**：session / topology / 事件 / 回调 / 呈现
全在 lifted 代码里，shim 必须扮演 MF，而不是「只解码」。

**驱动因素.**
1. **会话状态机**：`MFCreateMediaSession` → 把拓扑交给 session（`sub_484760` 对 `this+1456` 调 vtable **+16**）
   → `Start`/`Pause`/`Stop` 的异步事件（`MESession*`），以及 `IMFAsyncCallback`
   （本作自己实现了一个：`sub_47B130`/`sub_47B200` 里面有队列、临界区、`SetEvent`/`WaitForSingleObject`）。
   lifted 代码依赖这些**事件顺序与 HRESULT**；换成「解码器 API」就得重写这个类（这 15 个入口），
   而不是写 15 个函数桩；
2. **呈现**：EVR 走**独立子窗口**（`MFCreateVideoRendererActivate(hWnd)`），不是一个纹理；
   `display:UseIVideoWindow` 配置键（10 站点 / 9 函数，`0x405BE7` 等）——键名里的 `IVideoWindow` 是 DirectShow 的接口名，
   **是否真走 DirectShow 而不是 EVR 属待核**（§6-3）。要复现的是「一个能盖在游戏窗口上、按窗口缩放、
   全屏时跟着走」的视频窗口 + A/V 同步；
3. **DXVA2**：`sub_47C910` 建 D3D9Ex + `DXVA2CreateDirect3DDeviceManager9` 是**硬件解码**。macOS 上没有
   DXVA2；替换成 VideoToolbox/FFmpeg 即可，但**不要**把「DXVA2 的接口形状」当成必须复现的东西；
4. **Effect 不是 MF**：`D3DXCreateEffectFromResourceA`（4 站点 / 2 caller）是 `sub_418340`
   「Set3DEffectSnow」，属 D3D9 簇，⛔ 不要混进来。

**可观测副作用.** 影片窗口黑 / 不出（session 没 Start、拓扑没接上、EVR 窗口没建）；只有画面没声音
（SAR 没接或 audio stream 没选）；音画不同步（clock 用错：MF 的 presentation clock vs `timeGetTime`）；
不能暂停 / 不能跳（`MESessionRateChanged` / seek 没实现）；影片盖住 UI 或不随窗口缩放（EVR 窗口的 z-order /
parent）；全屏切走时崩溃（设备丢失 / 窗口销毁顺序）。

**Wine 先例.** Wine **有完整形状**：`mf.dll`（`session.c` / `topology.c` / `topology_loader.c` / `clock.c` /
`sar.c` / `sac.c` / `copier.c` / `samplegrabber.c` / `scheme_handler.c`）与 `evr.dll`（`presenter.c` / `mixer.c` /
`sample.c`，依赖 `d3d9` + `dxva2`）——
[`dlls/mf/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mf/Makefile.in) ·
[`dlls/evr/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/evr/Makefile.in)。
⚠ 但 Wine 的 EVR **依赖它自己的 d3d9/dxva2** ⇒ 在「lift + shim」路线里复用 Wine 的 MF/EVR 会绕回 D3D9
（循环依赖）；而在 macOS 上跑 Windows 二进制时它才有意义。⛔「Wine 有 MF」≠「本作这条路径等价」。

**工程量级：L（6–12 周）.** 驱动工作量的 1–2 件事：① session/事件/回调的状态机 fidelity（lifted 客户端会挑
HRESULT 与事件顺序）；② 呈现 + 同步（窗口、缩放、暂停/seek、音画对齐）。

**建议：整簇替换（不是逐个 shim），且允许改调用方.** 这 15 个入口属于**一个类**（`MFSession`），
把它重写成「打开/播放/暂停/停止/seek/取时长」的小 API，比让 shim 假装 MF 便宜得多——这是全线里**唯一**
「改调用方比 shim 便宜」的地方。要复现的行为边界：打开失败/解码失败的返回码与日志串、播放到结尾的
「结束」通知、暂停期间的时钟行为、窗口尺寸变化。若预算不足：**降级为「影片跳过 + 不崩溃」**是可接受的停手点。

**正面回答假设 ②（MF 是不是样板）**：**大部分是样板，但不是只有样板**。session/topology/属性的装配确实是
教科书写法；真正吃工作量的是三件非样板：① 自己实现的异步回调 + 队列 + 字节流
（`sub_47B130` 35 行 + `sub_47B200` 79 行 + 队列/取结果 `sub_47B0B0` 等，合计 200 余行 C）；
② EVR 的**独立窗口呈现**；③ 音画同步的时钟。
所以「样板最多、收益最大」这句只在「接口数量」意义上成立（20 个站点，占 AGE 全站点 1.1%）。

### 3.7 D3D9 + D3DX

**证据.** 21 个导入 / 147 站点 / 69 caller。**D3D9 本身只有 2 个导入**（`Direct3DCreate9` 1 站点、
`Direct3DCreate9Ex` 1 站点）；D3DX 19 个导入 / 145 站点，其中**数学 128 站点**（`D3DXMatrixMultiply` 55、
`D3DXMatrixTranslation` 24、`D3DXMatrixScaling` 21、`D3DXMatrixRotationAxis` 16、`D3DXMatrixDecompose` 5、
`D3DXMatrixRotationYawPitchRoll` 2、`LookAtLH`/`PerspectiveFovLH`/`OrthoLH`/`Vec3Normalize`/`Vec4Transform` 各 1），
**非数学只有 17 站点**：

| 导入 | 站点 / caller | 由调用点定的用途 |
|---|---|---|
| `Direct3DCreate9` | 1 / 1（`sub_498CC0`, EA 0x498CC0） | 建 `IDirect3D9`（参数 `0x20` = 32 = `D3D_SDK_VERSION`），`GetAdapterDisplayMode` → 建设备 |
| `Direct3DCreate9Ex` | 1 / 1（`sub_47C910`, EA 0x47C910） | 视频用 D3D9Ex（DXVA2） |
| `D3DXCreateEffectFromResourceA` | 4 / 2（`sub_418340`, EA 0x418340） | 「Set3DEffectSnow」：从资源 id 202 载入 **effect**（雪） |
| `D3DXCreateSprite` | 1 / 1（`sub_4A2BA0`, EA 0x4A2BA0） | `CSprite`：2D 精灵批处理 |
| `D3DXLoadMeshHierarchyFromXInMemory` | 1 / 1（`sub_47A140`, EA 0x47A140） | 载 `.x` 网格层级（自定义 `AllocateHierarchy`） |
| `D3DXFrameFind` / `D3DXComputeNormals` | 1 / 1；1 / 1 | 找骨骼 / 算法线 |
| `D3DXCreateTexture` / `D3DXCreateTextureFromFileInMemory` | 1 / 1；3 / 3 | 建纹理 / 从内存解码 |
| `D3DXLoadSurfaceFromSurface` | 5 / 5 | 表面拷贝（呈现 / 缩略图） |

★ **D3D9 的真实 API 面不在导入表里**——它在**设备 vtable**：AGE 的 vtable 站点 2200 / 83 offset，
其中 `0x49A000–0x4A0000`（scene3d）82 站点、`0x4B0000–0x4BC000` 136 站点（含窗口代码，见 §1.3）。
offset 直方图里 `+228`（84）、`+268`（60）、`+276`（27）与 `IDirect3DDevice9::SetRenderState` /
`SetTextureStageState` / `SetSamplerState` 的位置吻合，但**没有类型证据**，本文件不当结论用（census 的诚实项）。

★ **`.x` 复杂模型不在 ADV 主路径**：`0x329`（handler `sub_426EB0`，调 `D3DXLoadMeshHierarchyFromXInMemory`）
在 `title` / `option` / `sn0000` 三条足迹里**执行 0 次**（只有 1 条 static-op）⇒ 它属于战斗/独立 3D 场景；
ADV 上执行的是 `0x320`（6）/ `0x322`（7）/ `0x32a`（3）这类**简单网格/特效**，不是 `.x` 模型。

**复杂度档：T2.** 状态机 + 资源生命周期 + 着色器。

**驱动因素.**
1. **设备状态机**：渲染状态 / 纹理阶段 / 采样器 / 变换矩阵 / 顶点声明 —— 每个 draw 的正确性依赖「之前设过什么」；
   换成 Metal 后状态是**每管线**的，要重新建模；
2. **着色器**：只有 1 个 effect（雪），但它是**编译过的 FX 二进制**（`D3DXCreateEffectFromResourceA` 直接吃资源 id 202）；
   macOS 上要么反汇编/重写这个 effect，要么把 HLSL 翻成 MSL（emulator 的 `gpu_quad.cpp` / `gpu_scene.cpp`
   已经证明「HLSL + MSL 双源」是可行姿势，但那是**重写**的管线，不是**复用** D3DX 的 effect）；
3. **`.x` 网格层级**：`D3DXLoadMeshHierarchyFromXInMemory` + 自定义回调（`dword_55DA00/04` 是游戏的
   `ID3DXAllocateHierarchy` 实现）+ `D3DXFrameFind` + `D3DXComputeNormals`；**没有**看到 `D3DXLoadMeshHierarchy` 的
   skinning 调用点证据（`D3DXFrameFind` 之后做什么未查），但「载入 + 帧树 + 算法线」这条链要自己实现；
4. **D3DXSprite**：2D 批处理（`Begin`/`Draw`/`End`），在 3D 场景里贴 UI；
5. **两个设备**：D3D9 设备（3D + Live2D 纹理）与 DDraw 表面（2D）**并存**；资源不能互借。

**可观测副作用.** 3D 场景黑屏 / 花屏 / 缺模型；雪效果没有（少一个 effect）；Live2D 模型无纹理（纹理由
D3D9/D3DX 建，见 §3.8）；2D 精灵不显示（D3DXSprite）；透明/混合错（alpha blend state）；窗口缩放后崩
（device lost / swap chain resize）；帧率塌（每帧重编译着色器 / 状态切换过多）。

**Wine 先例.** 三段拼起来：`wined3d`（D3D9→OpenGL/Vulkan）、`DXVK`（D3D8/9/10/11→Vulkan，
[README](https://raw.githubusercontent.com/doitsujin/dxvk/master/README.md)）、`MoltenVK`（Vulkan→Metal，
[README](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/README.md)，另有 `dxvk-native`：
把 `HWND` 换成 `SDL_Window*`、用 `DXVK_WSI_DRIVER=SDL3` 直接跑原生程序）。D3DX 侧 Wine 有
`d3dx9_36`：`effect.c` / `mesh.c` / `sprite.c` / `skin.c` / `xfile.c` / `math.c` / `surface.c` / `texture.c`
（[`dlls/d3dx9_36/Makefile.in`](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/Makefile.in)）。
★ 组合起来**确实**有「D3D9 程序在 macOS 上跑」的完整先例——但那是 **Wine 路线**（跑 Windows 二进制），
不是「lift + shim」路线。在 lift 路线里嵌入 DXVK 意味着换掉「32 位 PE 的 d3d9.dll」、改用 DXVK 的 D3D9
实现（`dxvk-native` 用 `SDL_Window*` 代替 `HWND`）；但 `dxvk-native` 官方面向 **Linux（Steam Runtime）**，
macOS 还要再经 MoltenVK，这条组合**没有官方背书**——所以本文件把它记作「外部依赖」，不算进自写 shim 的账。

**工程量级：XL（3–6 月，自写）.** 驱动工作量的 1–2 件事：① 设备/状态/管线（含 shader 翻译）；
② `.x` 网格 + effect + sprite 三件 D3DX 工具的重建。若「嵌入成熟实现」：S–M（2–6 周）集成，但那是**换路线**
（等于承认「平台层用 Wine 系组件」，与「机械翻译 + 自写 shim」的假设不同）。

**建议：不自写.** 两条路：① 把 D3D9/D3DX 当成**外部依赖**（wined3d/DXVK + MoltenVK），接受它的兼容性与许可
（Wine: LGPL-2.1-or-later；DXVK: zlib；MoltenVK: Apache-2.0，以各自 LICENSE 为准）；② 在这里**停手**，
退回 emulator / 串流。⛔ 「D3D9 有文档」不等于「D3D9→Metal 便宜」：API 面小（2 个导入）是假象，
语义面是整台设备。

### 3.8 Live2D（静态 SDK）

**证据.**
- SDK 本体：第一个 `live2d::*` vftable 在 `0x4BC342`（`??_7LDObject@live2d@@6B@`），最后一个在 `0x4CF10C`
  （`IBaseContext`）；引用 `live2d::` 的函数共 **182 个**（`0x4BC000–0x4C0000` 41 个、`0x4C0000–0x4D0000` 141 个）。
- **平台面 ≈ 0**：这 182 个函数的**导入调用站点 = 0**；vtable 站点 12 个（`+8` 9 个、`+4` 2 个、`+12` 1 个），
  例：`sub_4C8240`（EA 0x4C8240，`DDTextureContext` 析构）对 `this[14]/this[15]` 各调一次 vtable **+8**（释放 D3D 纹理）。
- **在 ADV 主路径上**：`0x341`/`0x345`/`0x34e`/`0x352` 在 `sn0000` 与 `option` 足迹里分别执行
  **1 / 3 / 1 / 1** 次（`title` 0 次）⇒ Live2D 是 ADV 上唯一的**复杂模型**；但它的**画**走游戏 renderer，
  不是 Live2D 自己的平台调用（§6-2 的 D3D vs DDraw 后端仍待核）。
- 切片里的 4 个导入（`wsprintfA`/`GetCurrentThreadId`/`mmioRead`/`OutputDebugStringA`）**不在**这 182 个函数里；
  它们在 `0x4BBC00–0x4BC280` 区间里有 14 个站点（点名两支 12 + `sub_4BBDF0` 2；§1.3 #2），其余 94 个站点散布在别处。
- **纹理由游戏侧建**：`sub_478370`（EA 0x478370）先 `D3DXCreateTextureFromFileInMemory`，再 `sub_4BD070(*this, i, tex)`
  把纹理交给 Live2D；`sub_4783D0`（EA 0x4783D0）对 D3D 设备调 vtable **+236**、对模型调 **+8/+12**、
  对返回的状态块调 **+20/+8**。
- 渲染实现有两套名字：`live2d::Live2DModelD3D`/`DrawParam_D3D`（名字带 D3D）与 `live2d::DDTextureContext`/
  `DDTexture`（名字带 DD）——**同一份 SDK 的两个实现，哪一套在跑未定**（§6 待核 2）；本作的 182 个函数
  **没有**调用任何 DDraw/D3D 导入，所以「带 DD 的名字」不等于「走 DirectDraw」。

**复杂度档：T0（本体）/ T2（依赖）.** 本体没有平台语义；但它的「画」发生在**游戏 renderer**里。

**驱动因素.** ① C++ 虚表：lift 到 C++ 时**保留**虚函数即可（若 lift 成 C，要自己铺 vtable）；
② 纹理生命周期：`DDTextureContext` 析构里对纹理 `Release`，所以「谁建、谁放」必须与游戏侧一致；
③ 数学/动作：眨眼、口型、motion queue 全是纯逻辑（`0x4BC340` 之后那批函数），无平台调用。

**可观测副作用.** 模型不显示（draw 没被游戏 renderer 调用）；贴图错/白模（纹理没交进去）；
眨眼/口型不动（motion 状态机）；纹理泄漏（Release 没配对）；崩溃在 `DDTextureContext` 析构（重复释放）。

**Wine 先例.** 不需要：Live2D 是游戏**静态链接**的第三方 SDK（二进制在 EXE 内），没有 Wine 先例可说。
它的替代品是 Live2D Cubism SDK 本身（许可证与版本另议，不进本表）。

**工程量级：S（1–2 周）.** 驱动工作量的 1–2 件事：① 让 182 个函数的 C++ 虚表/`this` 布局在 lift 后仍然成立
（这是 lift 工具的问题，不是平台问题）；② 把「游戏侧建纹理 / 调 draw」的包装函数（`sub_478330` /
`sub_478370` / `sub_4783D0` / `sub_478640` 等）与 renderer 对齐。

**建议：原样 lift，不 shim.** 复现边界：不动 SDK 的虚表布局与构造/析构；纹理仍由游戏侧建；
不要求 Live2D 知道平台存在。若 D3D9 簇停手，则 Live2D 一起停（它的画依赖 D3D 设备）。

### 3.9 AGERC.DLL

**证据.** 7 DLL / 150 导入 / 79 有调用 / 648 站点；vtable 257 站点 / 56 offset；动态解析 17 站点。
**21 个导出**（`.lst` 的 `Exported entry`）：18 个地图/碰撞（`_FindClash@20`、`_GetEv@20`、`_GetLand@20`、
`_InitRect@20`、`_SetChParams@20`、`_SetMapEvPN/PX/PY@20`、`_SetMapLandIdx/PN/PX/PY@20`、`_SetMapParams@20`、
`_SetMapWallCr/PN/PX/PY@20`、`_SetNameLenMax@20`）+ **3 个 UI**（`_GetInstance@0`、`_ShowDialog@12`、`_OperateMenu@16`）。
AGE 只 LoadLibrary + `GetProcAddress` 这 3 个（`sub_48E730`，EA 0x48E730），对 18 个地图名 **0 引用**。
它自身的平台面是 **USER32 对话框族**：`DialogBoxParamA` 14、`CreateDialogParamA` 14、`SendDlgItemMessageA` 96、
`GetDlgItem` 55、`CheckDlgButton`/`IsDlgButtonChecked` 各 27、`SetDlgItemTextA` 26、`CheckMenuItem` 80、
`EnableMenuItem` 21、`ShowWindow` 23；另有 `SHBrowseForFolderA`、剪贴板 4 个、注册表 3 个、`Direct3DCreate9` 1 个
（设置对话框里枚举显示模式）。它编辑的键（AGE.EXE 的字符串里有全表，这里只给族与数量）：
`display:*` 18 个（`ScreenMode` / `UseDirectDrawEmuration` / `UseIVideoWindow` / `PixelShader` …）、
`sound:*` 15 个（`UseDirectSound` / `Music` / `SE` / `Volume0..4` …）、
`set:*` 60 个（`DrawMode` / `SavePath` / `UseAppDataFolder` / `EnableAntiFont` / `UseProportionalFont` …）、
`system:*` 7 个（`LimitJoy` / `LimitTouch` / `SaveBMPPath` …）。

**复杂度档：T1.** 无隐藏状态机，但 UI 是实打实的一套。

**驱动因素.** ① 设置对话框 = **AGERC 可见 44 个配置键**（`display:*` 11 / `sound:*` 10 / `set:*` 17 / `system:*` 6）；AGE 全表约 100 个，其余 56 个是否由 AGERC 编辑（动态拼键）**待核**；② 显示模式枚举要一个 D3D9 设备；③ 它与 AGE 共享「配置读写」（注册表 / INI）。

**可观测副作用.** 游戏内「设置」菜单点了没反应 / 崩溃；改的设置不生效或写错键（`SYS4REG.INI` 污染）；
显示模式列表空；`OperateMenu` 的菜单项失效。

**Wine 先例.** 无直接先例（这是本作私有 DLL）；但它的**实现材料**都是标准 Win32 对话框 + 注册表 + 枚举显示模式，
Wine 的 `comctl32`/`user32` 对话框实现可作语义参考。

**工程量级：S（2–4 周）.** 驱动工作量的 1–2 件事：① 3 个导出的原生实现（`GetInstance` 返回一个对象、
`ShowDialog` 开设置窗、`OperateMenu` 处理菜单命令）；② 设置项 ↔ 配置文件的映射（要防止写坏真机配置）。
若产品接受「没有设置界面」（改 ini / 命令行）：**3 日**。

**建议：原生 shim，不 lift.** ⛔ 不要为了 AGERC 去实现 Win32 对话框 + 剪贴板 + `SHBrowseForFolder`：
18 个地图/碰撞导出**游戏不用**，可以整块丢掉（要留证据：AGE 对它们的 0 引用）。复现边界：
`_ShowDialog@12` 的返回值语义（AGE 用它判断用户是否改了设置）、`_OperateMenu@16` 的命令 id、
`_GetInstance@0` 的单例指针。

## 4 建议切分顺序与停手点

依赖关系（→ = 依赖）：

```
共享底座 ──→ 窗口/消息 ──→ （能启动、能进标题）
                    └──→ DirectDraw ──→ GDI 字体 ──→ （画面 + 文本可读）
                                              └──→ 音频（可与画面并行，但闪避要看时钟）
                    └──→ 视频（依赖窗口 + 音频混音器；可与音频并行）
                    └──→ D3D9 ──→ Live2D（纹理/绘制都走 D3D）
AGERC（独立，随时可做）
```

1. **共享底座**（S）。所有东西的地基；先做它才能编译、才能谈「跑起来」。**不是停手点**。
2. **窗口/消息 + 文件/配置**（M）。目标是「能开窗、能进标题、能读配置」。这一步做完就知道 lift 本体
   （3813 个函数的编译/崩溃）能不能撑住 —— 若撑不住，**在这里停手**最便宜（沉没成本 ≈ 1–3 人月）。
3. **DirectDraw + GDI 字体**（L + M–L）。目标是「2D 画面 + 文本」。**这是最重要的验收点**，也是第一个
   「质量可见」的地方：字体度量偏 1–2px、颜色键错、缩略图花，都会在这里暴露。
   **停手点 A**：如果这两簇的实测工期超过 §2 的上限（≈ 3–5 人月），说明「复现 Windows 平台语义」这件事
   比 emulator 的「按 spec 重写」更贵，应退回 emulator / 串流。
   ★ **go/no-go 就在这里**：不要先做音频/视频（各 L）再决定——那会把沉没成本推到 7–14 人月。
4. **音频**（L）。放在画面之后，因为它可以先用「静音」跑通；但**内容问题**（CD-DA 音轨 / MIDI 音源）
   要**提前**确认（§6 待核 5），否则做到一半才发现没有音源。
5. **视频**（L）。自包含；预算不足时可降级为「跳过 + 不崩溃」。
6. **D3D9 + D3DX**（XL）。**最后做，且是主停手点**。理由：它是唯一「自写不划算」的簇；
   如果必须自写，整条 lift 路线的性价比就不如 emulator。到这一步之前的所有簇，都是「即使不划算也能独立交付」的。
7. **Live2D**（S）与 **AGERC**（S）。前者跟着 D3D9 走（D3D9 停则一起停）；后者完全独立，可最后补或不做。

**推荐的「最小可交付」组合**：1 + 2 + 3 + 4 + 5 + AGERC，把 6（D3D9）砍掉/替换；Live2D 跟着 D3D9 一起停，不进这个组合。
如果游戏的主线（ADV）不依赖 3D 场景，这个组合是**2D 可启动**；但 ADV 是否用 Live2D（§3.8 的 draw 走游戏
renderer）还未确认 ⇒ 需要 §6 待核 1（路径命中）+ 待核 12（ADV 是否用 Live2D）来判定"能玩"。

### 4.1 可接受偏差（用户口径 2026-11-03）

> 用户明确："这类部分复杂的内容可以接受偏差；菜单可以先全部视为 no-op（副作用内聚、不挡主路径），
> 只要 stub 插桩解决链接；模态对话框也只有极个别路径（关闭窗口、编辑存档名称）。"
> ★ **偏差不是遗忘**：逐条登记在下表；实现里做成**显式 stub**（返回值合理、不阻塞、打日志），
> ⛔ 不许静默 no-op；每条都要能在实现单的 `SCOPE.md` 等价物里回查。

| 偏差 | 覆盖的簇 | 省下的量级 | 用户可见代价 | 仍必须保留 |
|---|---|---|---|---|
| **菜单 no-op** | 窗口/消息 | ~1–2 周 | 无 Win32 菜单栏（游戏自己的标题菜单不是它；菜单族共 14 站点） | 窗口创建 / 消息泵 / 输入 / region / 计时器 |
| **模态 no-op** | 窗口/消息 + AGERC | ~2–4 周 | 无"设置"对话框；关闭确认 / 存档命名若走 `MessageBoxA` 则直接继续 | 配置读写（INI/注册表）、存档本身 |
| **音频 silent** | 音频 | L → S（~4–8 周） | 无 BGM / 语音 / SE | `timeGetTime` / `timeSetEvent`（计时底座）、音频状态机的返回码 |
| **视频 skip** | 视频 | L → S（~4–8 周） | 无影片（LOGO/OP/剧情影片） | 影片 opcode 必须推进（⛔ 不许挂起） |
| **Live2D 不在主线** | Live2D | 0（本来 ≈ 0） | 若主线用则模型不显示 | — |
| **3D 不做** | D3D9 + D3DX | XL → S（stub） | 无 3D 战斗/场景 | 2D/ADV 主路径（DDraw + GDI）；⚠ 若 Live2D 走 D3D 后端（§6-2），D3D9 不能砍 |

★ 偏差**不改变固定成本的排序**：DDraw + GDI + 共享底座仍在；D3D9 是否砍取决于"能不能没有 3D"（待用户裁决）。
★ 每个 stub 必须：① 返回**合理值**（`HMENU` 非空、`SetMenu` 返回 TRUE、`MessageBoxA` 返回 IDOK）；
② 不阻塞、不等待；③ 打一行日志（可回查）；④ 在实现单的 `SCOPE.md` 等价物里登记。
★ 偏差后的总量级（仍不含 lift 本体）：**约 7–15 人月**；若再砍 D3D9（接受无 3D）**再减 3–6 人月**。
这是量级，不是承诺；驱动项仍是 DDraw + GDI + 共享底座。

## 5 与 emulator 路线的对比

### 5.1 固定成本 vs 可替换节点

按 **AGE 的 1855 个导入调用点**分（三块**互不重叠**、按站点分；所以 §2 里 GDI 的 367 站点只有一部分
落在「其余」——`SelectObject` 等已被算进共享底座）：

| 类别 | 站点 | 占比 |
|---|---|---|
| 共享底座（56 个 ≥5 caller 的导入） | 1487 | **80.2%** |
| **可整簇替换**（视频 20 + 音频 22 + 路径辅助 3（`PathRemoveFileSpecA`）；另有 21 站点属这些簇但已计入共享底座） | 45 | **2.4%** |
| 其余固定成本（窗口 / region、GDI、D3DX 非数学、注册表 / 版本 / 其余 KERNEL32 中**不**在上两行的部分） | 323 | 17.4% |

★ 结论：**「能整簇替换」的部分只占导入调用点的约 2.4%**；而**固定成本（共享底座 + DDraw + GDI + 窗口 + D3D9）
占 97% 以上**，且 DDraw/D3D9 的 vtable 面**根本没被这张表计入**（无类型证据），所以真实的固定成本占比只会更高。
「视频样板最多、收益最大」在「自包含程度」上成立，在「省下多少平台工作」上**不成立**。

### 5.2 哪些知识 / 代码可复用

| 可复用 | 为什么 | 落点 |
|---|---|---|
| FreeType 的字形度量与 GGO 档位知识 | 引擎用 `GGO_BITMAP`/`GGO_GRAY4`/`GGO_GRAY8`（1 / 17 / 65 级），emulator 已把量化与两趟合成做成实现 | `apps/emulator-cxx/src/core/text_alpha.h`、`SCOPE.md` §1.2 |
| 基线 / 度量补偿的实测值 | 「基线抬 2px、20px 一致、28px 低 1px」是实测结论 | `apps/emulator-cxx/SCOPE.md` §1.1 |
| `pl_mpeg` 解码 | MPEG-1 视频解码可直接用 | `apps/emulator-cxx/third_party/pl_mpeg/` |
| SDL3 窗口 / 输入 | 窗口、键鼠接宿主（手柄按 `SCOPE.md` §0.4 不做） | `apps/emulator-cxx/src/window.cpp` |
| SDL_GPU 的「HLSL + MSL 双源」姿势 | 若要自己翻 shader，这是已被验证的管线写法（含 macOS 只发 MSL 源码） | `apps/emulator-cxx/src/gpu_quad.cpp`、`README.md`「着色器与平台路径」 |
| 存档 BMP 的容器口径 | 缩略图是引擎自己组装的 BMP，像素来自「表面 + 行宽 + 掩码」 | `data/spec/chapters/65-save.md:35-51`、`:86` |
| spec 里的引擎侧合同 | 文本层 / 渲染边界 / 存档，全部与平台无关 | `data/spec/chapters/60-ops-render.md`、`62-adv-text.md`、`65-save.md` |

### 5.3 哪些**不可**复用（架构不同）

- **Win32 消息循环 / 菜单 `HMENU` / 模态对话框**：emulator 没有 Win32，它直接读 SDL 事件。
- **GDI DC 与 `SelectObject` 的对象语义**：emulator 直接调 FreeType，没有 DC 这一层。
- **DirectDraw 表面 / `Blt` / `Lock` / 表面 DC**：emulator 用 SDL_GPU 纹理，没有「表面」概念。
- **D3D9 设备 / 状态机 / D3DXSprite / effect**：emulator 自己写管线（`gpu_scene.cpp`），不实现 D3D9 状态。
- **MF 的 session/topology/事件**：emulator 的影片后端是「解码 + 上传纹理」，不是 MF 客户端。
- ⇒ emulator 的**平台实现**不能拿来当 shim；能拿来的是**知识**（档位、补偿、容器口径）和**两个独立库**
  （FreeType、pl_mpeg）。这正是两条路线的结构差别：**emulator 替换平台，lift 必须复制平台**。

### 5.4 和「Wine 直接跑」的关系

如果把 D3D9 当成外部依赖（wined3d / DXVK + MoltenVK），本质上就是「用 Wine 系组件跑 Windows 程序」的
一个变体——那条路已经有成熟产品（CrossOver / Whisky 等）。**本文件的立场**：那是**第三条路线**，
不在「机械翻译 + 自写 shim」的假设内；把它混进来会让「工程量」与「许可」都不可比
（Wine LGPL-2.1-or-later / DXVK zlib / MoltenVK Apache-2.0）。

★ **结构对照**（帮助判断"这件事算不算在表内"）：
Wine = 平台层语义（其上是**原生执行**）+ 跨 ISA 时**外包**（Rosetta 2 / FEX / box86）；
`apps/emulator-lift`（`.lst` / guest-state 路线）= 平台层语义（自写）+ **执行层也自写**
（它没有"宿主直接执行 guest 原机器码"这个前提 ⇒ 执行语义必须在自己手里，见 §5.6）。
**两者重合的只有"Win32 形状"这一层**，其余都不重合。

★ **判据（哪些事算"还在自写 shim 路线内"）**：平台层是**自写、自洽**的 Win32 形状。
把某一簇换成 Wine 系组件（典型：D3D9 交给 wined3d / DXVK + MoltenVK，§3.7）就是**换路线**：
要单独立项，并且**不能把它的成本记进 §2 那张表**（那张表的量级前提是"自写"）。

★ **一个常被提出的捷径：把 Wine 的 DLL 当 shim。** 在「lift + 自写 shim」路线里这不成立：
现代 Wine 的 **PE 侧模块基本是壳**，实现落在 **unix 侧的 `.so`** —— PE 模块经
`WINE_UNIX_CALL` / `__wine_unix_call` 转到 unix 侧的 `unixlib_entry_t` 函数表
（两侧声明见 [`include/wine/unixlib.h`](https://raw.githubusercontent.com/wine-mirror/wine/master/include/wine/unixlib.h)）。
那是**宿主原生代码**，本项目的 guest 执行层跑不了它 ⇒ 复用 Wine 的 DLL 等于把整个 Wine 进程搬进来，
也就退化成本节的第三条路线。（⇒ Wine 在本文件里的用途只有一处：**"这个语义有权威实现"的旁证**，
见 §3.1–§3.9 的「Wine 先例」小节；⛔ 不是代码来源。）

### 5.5 宿主层：SDL3，不直接依赖 AppKit

**选择 SDL3**（与 `apps/emulator-cxx` 同一套宿主层）：

- **窗口 / 输入 / 音频 / GPU 一次到位**：macOS 走 SDL_GPU 的 Metal 后端（MSL 源码），Linux 走 Vulkan，
  Windows 走 D3D12；`apps/emulator-cxx` 的 `macos-arm64` preset 已经验证过这条路径；
- **源码不直接调 AppKit**：lift 出来的代码 + 我们的 Win32 形状层都不出现 Cocoa 调用；
  ⚠ 但 **SDL 的 macOS 后端内部仍然用 Cocoa/AppKit**——只要要一个原生窗口，就绕不开窗口服务器 API
  （GLFW / Qt 也一样）。所以"不依赖 AppKit"的准确含义是**我们的代码不依赖**，不是"进程不链接 AppKit"；
- **可移植**：同一份宿主层代码在 Linux/Windows 也能编；这是"翻译路线"相对 emulator 的额外收益；
- **许可**：SDL3 zlib；FreeType / pl_mpeg 也已经是仓里现成的依赖。

**分层**（重要）：SDL 只替换**宿主层**，不实现 Win32/GDI/DDraw 的语义。

```
lifted C（原逻辑）
  └─ Win32/GDI/DDraw 形状层（我们自己写：消息泵 / WndProc / DC / Surface / Blt / 菜单 stub）
       └─ SDL3（窗口 / 事件 / 音频 / SDL_GPU）
            └─ Metal / Vulkan / D3D12
```

⇒ 不要指望 SDL 直接顶掉 `DirectDraw` / `GDI` / `WM_*`；它顶掉的是"开窗、拿事件、出声、上屏"这四件事。
`apps/emulator-cxx` 的 `window.cpp` / `gpu_scene.cpp` / `text_alpha.h` / `pl_mpeg` 是**知识 + 库**的复用点，
⛔ 不是把它的 core 拿来当 shim。

### 5.6 源选择：`.lst` 还是 `.c`

**先给数据**（AGE；`pnpm tools disasm-fn summary` 现查）：

| 指标 | 值 | 含义 |
|---|---|---|
| `.lst` 函数 / `.c` 函数 | **3813 / 3798** | **15 个函数没有 C**；多为 0 字节尾跳/薄壳，有实体的只有 `sub_4531B0` 204 B、`sub_4B58C0` 313 B、`sub_4B87B0` 190 B、`sub_4B5660` 55 B（合计 ≈762 B） |
| `.c` 的显式反编译产物 | `HIDWORD/LODWORD/__PAIR__` **130** 函数（3.4%）；`__ROL/__ROR` 40；`_mm_` 25；`__asm` 1；`__try/__except` **0**、`JUMPOUT` **0**、`__decompiler` **0** | C 比预期干净，但这不是 32→64 风险的全部 |
| `.c` 的 ABI/宽度面 | `__thiscall` **7282** 处、`__stdcall` **3014** 处、`long` 100、指针截断 `(int)(…)` **631** + `(int)Name(…)` **71** | `this` 是 `char*` + 裸偏移，指针/句柄普遍存成 `int`/`_DWORD` ⇒ 编 64 位要过一遍指针宽度 / ABI / 结构布局 |
| `.lst` 的间接面 | 21016 调用点，其中 **3124 解不出目标**；函数指针表 7743 项；被取址 4488 | 两条路都要 dispatcher；`.lst` 的 guest 状态天然处理 `call [reg]` |

**两条路的本质**：

| | `.lst`（guest-state / 软 CPU） | `.c`（原生 64 位） |
|---|---|---|
| 完整性 | 3813/3813 | 3798/3813（15 个缺 C，需从 `.lst` 补） |
| 语义保真 | 机器码，最高 | Hex-Rays 视图，需逐处核对 |
| **32→64** | **不撞**（guest 寄存器/内存都是 `uint32`） | **撞**（指针宽度 / `__thiscall` / `long` / 布局） |
| 输出 | 基本块 + 运行时 = 软 CPU，难读 | 可读 C，好改 |
| 性能 | 解释 2–10× 慢；可 JIT / 热路径替换 | 原生 |
| 风险形状 | **集中**在 lifter 正确性 | **分散**在宽度/ABI 审计 |
| 与现有 emulator 的关系 | 执行**原代码**，不需要 spec（spec 只作调试对照） | 编译**原视图**，同样不需要 spec；但 spec 可帮对照 |

**结论（推荐）**：

1. **第一版可运行目标：执行原机器码**（guest-state 解释器），`.c` 只做阅读与调试对照。
   ★ **2026-11-03 用户确认**：**执行源 = 拆壳件（清单 `binary/age-sectfix`，现已入库 LFS 为
   `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`）的机器码字节**；执行方式是**纯内存解释 + 块缓存，
   默认零生成代码**。`.lst` 降为**符号 / 对照 / 覆盖**层（它提供指令边界与指令文本，用来验解码器）。
   ★ 本表仍然成立 —— 它比的是「**执行原机器码**」vs「**把 `.c` 编成 64 位**」，不是「字节 vs 文本」；
   语料**没有机器码字节列**、且一部分 CRT / C++ 运行时被 `COLLAPSED` 这两条事实，
   见 `apps/emulator-lift/docs/plan-m1-cpu-base.md` §3 的 F1 / F2。
   "软模拟器"不是降级——它和 binary translation（Wine + Box86/FEX）是同一类架构；对本作（ADV / 2D 为主、
   Live2D 可关）性能大概率够用。
2. **不要直接编 `.c` 成 64 位**：7282 处 `__thiscall` + 3014 处 `__stdcall` + 631/71 处指针截断
   说明它是**32→64 的移植**，不是机械翻译。真要原生性能，就按"逐函数核对 + 宽度审计"走，不是一次性开关。
3. **混合是现实解**：guest **字节**执行，`.c` 阅读；热路径逐个替换成"已核对的原生实现"（覆盖清单），
   保留 guest 兜底。15 个缺 C 的函数从 `.lst` 补；130 个位宽产物单独审。
4. **源选择不改变平台成本**：Win32/GDI/DDraw shim 两条路一样要写（§3）；它只决定"guest 代码怎么跑"。

### 5.7 覆盖清单（本线**不生成代码**）

**原则**（与仓里"派生视图不入库 / 只做指针"同构）：

- 真源 = **拆壳件的字节（可执行）+ `.lst`（符号与指令边界）**；覆盖是**显式清单**；**没有生成物**（§5.6 · D2）；
- ⛔ **不手改任何"生成代码"** —— 本线不生成；要改行为只有两个去处：**字节**（不行，只读）或**覆盖清单**；
- 清单的字节锚 = 拆壳件 sha256（= `.lst` 头部的 `Input SHA256`）；`pnpm tools lift check` 重算，手改即红。

**三层覆盖粒度**（从便宜到贵）：

| 粒度 | 做什么 | 例 | 代价 |
|---|---|---|---|
| **平台 shim（API 级）** | 替换某个 Win32/D3D/MF 导入的实现 | `MFCreateMediaSession` 用解码器实现 | 低；但 lifted 的 `MFSession` 包装仍在 |
| **函数级（EA → 原生）** | 整函数替换 | 用 `video::Session` 顶掉 `sub_484760` | 中；要保住 ABI / `this` / 返回码 |
| **子系统级（EA 范围 + 边界）** | 一组函数 + 数据一起换 | MF 会话 / 拓扑 / 回调整簇 | 高；要写边界契约 + 内部 trap |

**合并机制：调度表 + 清单**

- `table[EA]`：覆盖项 → `ovr_<name>`；桩 → `stub_trap`；其余 → **运行时解码得到的块**（首次执行时填入）。
  ★ **本线不做"生成"（2026-11-03 决定）**：执行 = **运行时解码 + 块缓存**，**默认零生成代码**
  （判据与理由见 `apps/emulator-lift/docs/plan-m1-cpu-base.md` §4 D2）⇒ 下文凡提到"生成物 / 生成树"，
  只对**将来**若引入离线导出时才适用（那是调试工具，不是执行前提）。
- 清单读 `data/lift/overrides.json`：`{ ea | range, kind: replace | stub | hook, impl, boundary, reason }`。
- `stub`：运行时 trap（打印 EA + 调用栈），⛔ 不静默 no-op。
- `replace`：清单必须声明"入口 / 退出 / 共享数据 / 必须保住的返回码"。
- `hook`：只对**平台调用点**用（如 `MFCreateMediaSession`）—— 钩子挂在 shim 上，不在 guest 代码里插桩。

**呈现（四个东西）**

1. **代码布局**：`apps/emulator-lift/{src/runtime, src/shims, tests/}`；⚠ 原设想的 `generated/` **取消**
   （D2 = 运行时解码，没有生成物要放）。
2. **清单**：`data/lift/overrides.json` + 同名 `.md`（谁拍板 / 为什么 / 可见代价 / 边界）。
3. **报告**：`pnpm tools lift report` → 每个函数的 `overridden / stubbed / guest / missing` + 覆盖率（可重算）；
   ⛔ 不手写进度表。
4. **运行时**：`--trace-overrides` 打一行"进入 override X（EA）"，用于确认"这条路径真的走了覆盖"。

**守卫**

- **解码对照**：清单里的每个 EA 必须落在镜像的可执行段、能解出指令，且与 `.lst` 在同一 EA 上的助记符一致
  （判据 A3）；不一致 ⇒ 红。
- 清单里的 EA 必须存在；`replace` 必须列边界；`stub` 必须列原因；同一 EA 同时 `replace` + `stub` ⇒ 红。
- 覆盖项的实现文件必须存在；`report` 里 `missing` 非 0 时，构建可以过，但发布前必须为 0（或显式 `deferred`）。

**视频的例子**（最可能的多函数替换）

- **入口**：`sub_484760`（session / resolver / topology）、`sub_483A80`（topology / EVR）、
  `sub_47B130` / `sub_47B200`（异步回调 / 队列）、`sub_47C910`（D3D9Ex / DXVA）。
- **覆盖**：`video::Session`（open / play / pause / seek / close + 帧→纹理）；其余 MF 包装函数 `stub`（trap），
  确保没有遗漏调用。
- **边界**：`0x20F` 影片指令的入口仍**走 guest 代码**，只把它调到的"对象操作"换成 `video::Session`；
  清单写清"哪些 EA 换、哪些 EA 留"。
- **呈现**：`report` 里这一簇显示为 `overridden`（5 个 EA）+ `stubbed`（其余 MF 包装），
  覆盖率报告里单列"影片通道"。

## 6 未定 / `<待核>`

每条都写「需要什么观察」——没有观察就不进结论。

1. **两条渲染路径到底走哪条**（`set:DrawMode` / `display:UseDirectDrawEmuration` 的真实取值）：
   需要原生 **trace**：逐帧统计 DDraw `Blt`/`BltFast`/`Flip`（vtable +20/+28/+44）与 D3D9
   `DrawPrimitive`/`Present`（+316/+68）的调用次数；或直接读发行配置（`SYS4REG.INI` / 默认配置）。
   ⚠ 这一条决定 §4 的「最小可交付组合」是否成立。
2. **Live2D 的 draw 走哪套**（`Live2DModelD3D` vs `DDTextureContext`）：需要运行时看
   `live2d::IDrawContext` 的实际实例与 12 个 vtable 站点的目标；并确认纹理仍由 `sub_478370` 建。
   静态证据：`sub_478370` 用 `D3DXCreateTextureFromFileInMemory` 建纹理、`sub_4783D0` 对 D3D 设备调 **+236**；
   若这条在跑，**D3D9 是 Live2D 的前置**，§4.1 的"3D 不做"就不能同时保留 Live2D（除非换掉它的渲染后端）。
3. **MF 的呈现配置**（EVR 的哪个属性 / 是否 DirectShow 窗口）：需要跑一次影片并记录
   `unk_527198`（GUID 字节 `8A 5E 7D 2A E1 8D 0A 4E B1 0F EC A8 DC E9 C8 6E`）与
   `unk_51D654`（`65 13 49 BA 50 BE 1E 45 95 AB 6D 4A CC C7 DA D8`）对应的属性/接口；
   以及 `display:UseIVideoWindow` 的真实取值（决定是 EVR 还是 DirectShow 窗口）。
4. **GDI 度量是否被字形缓存依赖**：需要确认 `GetGlyphOutlineA` 的三种 `fuFormat`（1/5/6）分别由哪条路径发、
   缓存键是否含 `tmAscent`/`tmHeight`；四个入口（`sub_46DED0` / `sub_46E3E0` / `sub_46F2D0` / `sub_471180`）
   是否都走同一缓存；以及**屏幕 DC 与表面 DC 的字体 realization 是否一致**（hook 两个 DC 的 `SelectObject`
   与 `GetTextMetricsA`，比较同一个 `LOGFONT` 在两边的量度）。
5. **CD-DA / MIDI 的内容从哪来**：需要看安装目录（是否有音轨文件 / `.mid`）、以及游戏内实际播的是哪套
   （`sound:Music` 配置 + 运行时 `mciSendCommand` 的 track 号）。没有内容，音频簇的「整簇替换」只能到「SE + 语音」。
6. **`sub_43B460` 的 `GetDC` 目标表面是哪种格式**（16/24/32bpp）：需要抓 `DDSURFACEDESC`（或 `DDSURFACEDESC2`，视 `dwSize`）的实际
   `ddpfPixelFormat`，它决定 GDI 文本与存档缩略图的像素布局（`65-save.md:70-72` 说本作缩略图恒 24 位，
   但那是「屏幕捕捉路径」，纹理路径 32 位零样本）。
7. **全屏 / 显示模式切换的调用者**：需要确认 `SetDisplayMode`（DDraw +84）与 `SetCooperativeLevel`（+80）
   的真实调用点（本作**没有** `ChangeDisplaySettings*` 导入）；以及 `display:VirtualFullScreen*` 是否绕开它。
8. **6 个未解析的动态站点**（`0x422A30` / `0x422AF0` / `0x42FF36` / `0x42FFB1` / `0x4EC71C` / `0x508107`）
   载入的是哪个 DLL：需要看窗口外更远的 push 或运行时 hook；它们可能是插件 / 外部工具。
9. **IME 是否真的需要**：`ImmAssociateContext` 只有 1 个站点，需要交互观察（存档命名 / 名字输入是否走 IME）；
   若不需要，窗口簇可以少一块。
10. **抬升后的 ABI**：AGE.EXE 是 **32 位 PE**（`.lst` 头 `Format: Portable executable for 80386`）。
    lift 成源码后可以编 64 位，但**指针宽度 / `this` 偏移 / 内联汇编 / `__thiscall`** 都要过一遍；
    这不是平台 shim 的成本，但会改变「哪一簇先做」——需要一次试点编译（挑 1 个纯逻辑簇，如 Live2D）验证。
11. **`sub_4B92F0` 的菜单与 `_WinMain` 的启动顺序**：需要一次交互观察（启动即闪退 / 菜单不显示）
    来确认菜单资源是从哪读的（`LoadMenuA` 的参数是资源 id 还是文件）。
12. **ADV 主线是否用 Live2D**：需要看 `0x478330`/`0x478370`/`0x4783D0` 的调用者是否在 ADV 路径上；
    若是，则「砍掉 D3D9 + Live2D」的最小组合不成立（模型不显示）。
13. **关闭窗口 / 存档命名是否走 Win32 模态**：AGE **没有** `DialogBoxParamA`/`CreateDialogParamA` 导入，
    只有 `MessageBoxA`（28 站点）；需要确认这两条是 `MessageBoxA` 还是游戏自绘 UI。若走 `MessageBoxA`，
    按 §4.1 的 stub 返回 `IDOK` 即可；若走自绘，则本来就不需要 Win32 对话框。
14. **Live2D 开关的落点**（用户 2026-11-03 给的观察法）：emulator headless 跑到 CONFIG 第一页，
    切换该开关后 TITLE 左侧会从 Live2D 变成固定立绘 ⇒ 可据此定位它写哪个配置键/格、gates 哪条路径。
    这决定「关掉 Live2D」是**脚本级跳过**（SDK 仍链接但不被调用）还是**平台级不需要**（D3D9 的 L2D 子集可省）。
    ★ 用户明说这条不重要；登记为**可选观察**，不阻塞翻译。

## 7 参考

> 用法：每条结论旁边标来源；外部资料只作**策略参考**，不是本作等价的证明。

**微软文档（语义真源）**
- Critical Section Objects（可重入 / spin count / `TryEnter` / `DeleteCriticalSection`）：
  <https://learn.microsoft.com/en-us/windows/win32/sync/critical-section-objects>（§3.1 假设 ①）
- `GetGlyphOutlineA`（`GGO_BITMAP` / `GGO_GRAY4`=17 级 / `GGO_GRAY8`=65 级 / `GGO_METRICS` / `MAT2`）：
  <https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getglyphoutlinea>（§3.3）
- Media Foundation Programming Guide（session / topology / EVR / DXVA2 / SAR-SAC）：
  <https://learn.microsoft.com/en-us/windows/win32/medfound/media-foundation-programming-guide>（§3.6）

**Wine 源码树（「有没有先例」的观察，不是等价证明）**
- `dlls/ntdll/sync.c`（临界区：`RtlInitializeCriticalSectionEx` / `RtlEnterCriticalSection` 递归 / spin）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ntdll/sync.c>（§3.1）
- `dlls/mf`（session / topology / clock / sar / sac / copier / samplegrabber）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mf/Makefile.in>（§3.6）
- `dlls/evr`（presenter / mixer / sample，依赖 d3d9+dxva2）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/evr/Makefile.in>（§3.6）
- `dlls/ddraw`（surface / device / palette / clipper，走 wined3d）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ddraw/Makefile.in>（§3.4）
- `dlls/dsound`（buffer / mixer / primary / sound3d）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/dsound/Makefile.in>（§3.5）
- `dlls/mcicda` / `dlls/mciseq`（CD 音轨 / MIDI sequencer）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mcicda/Makefile.in> ·
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mciseq/Makefile.in>（§3.5）
- `dlls/win32u`（font.c + freetype.c，macOS 接 CoreText/AppKit；window/menu/message/dce/imm/input）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/win32u/Makefile.in>（§3.2 / §3.3）
- `dlls/gdi32`（当前只剩 DC / 对象 / 元文件层）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/gdi32/Makefile.in>（§3.3）
- `dlls/d3dx9_36`（effect / mesh / sprite / skin / xfile / math / surface / texture）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/d3dx9_36/Makefile.in>（§3.7）
- **Wine 的 PE 侧 / unix 侧分工**（`__wine_unix_call` 与 `unixlib_entry_t` 函数表两侧的声明）：
  <https://raw.githubusercontent.com/wine-mirror/wine/master/include/wine/unixlib.h>（§5.4）


**D3D9 → Metal 的现成链（策略参考）**
- DXVK：D3D8/9/10/11 → Vulkan；`dxvk-native` 用 `SDL_Window*` 代替 `HWND`（`DXVK_WSI_DRIVER=SDL3`）：
  <https://raw.githubusercontent.com/doitsujin/dxvk/master/README.md>（§3.7 / §5.4）
- MoltenVK：Vulkan → Metal（SPIR-V → MSL），macOS/iOS/tvOS；Khronos 已标 deprecation，Apple Silicon 推荐
  KosmicKrisp（Mesa）：
  <https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/README.md>（§3.7）

**本仓**
- 接口普查：`pnpm tools platform census --json` / `pnpm tools platform describe`（字段与诚实项的唯一真源）。
- 引擎侧的文本 / 着色 / 存档合同：`data/spec/chapters/60-ops-render.md`、`60-ops-text.md`、
  `62-adv-text.md`、`65-save.md`。
- emulator 的宿主策略（只作知识参照，⛔ 不是 spec 真源）：`apps/emulator-cxx/README.md`、
  `apps/emulator-cxx/SCOPE.md`、`apps/emulator-cxx/src/core/text_alpha.h`。
- 图 `lib/catalog.mjs` 的调用者闭包口径：`tools/lib/catalog.mjs`（`callersOf` / `functionOfEa`）。
