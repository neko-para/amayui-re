# emulator-lift · SCOPE（显式偏差）

> 本文件只记**用户拍板的偏差**与"必须保留"的边界；不写引擎语义（那在 `data/spec/`），
> 不写进度（那在 `pnpm tools requirements plan`）。
> **不允许在用户没有明确要求的情况下编辑这个文件。**
> 依据：用户 2026-11-03 口径 + `docs/03-emulator/platform-effort.md` §4.1。

## 0. 判序

**本节 > 本文件其余各节 > `data/spec/`**。与 spec 冲突时按本节取舍；
被跳过的 spec 条款在 §2 点名，⛔ 不写进 spec 正文冒充引擎语义。

## 1. 可接受偏差（用户 2026-11-03；临时文件名序号 / 执行源 exe / 平台偏差 / 注册表 / 音频·视频 no-op 2026-11-04；伪线程 / 影子写入 2026-11-05）

| 类别 | 偏差 | 做法 | 用户可见代价 | 仍必须保留 |
|---|---|---|---|---|
| **永久 no-op**（大概率一直不做） | **菜单** | 整体 no-op：`LoadMenuA` 返回非空假句柄、`SetMenu` 返回 TRUE、`TrackPopupMenu` 立即返回 0 | 无 Win32 菜单栏（游戏标题菜单不是它） | 窗口创建 / 消息泵 / 输入 / region / 计时器 |
| **已实现（系统对话框；headless 降级为默认返回值；2026-11-12 用户口径）** | **模态**（有限） | **系统对话框（`SDL_ShowMessageBox`）**：`MessageBoxA` 与 AGERC `_ShowDialog@12`（只 `id==3` 的关窗确认）都先生成 `ModalSpec`（正文 + 按钮），再交给窗口 sink 弹**系统对话框**；⛔ 不还原图标 / 位置 / 像素。返回值按 `uType` / 三臂给合法值（**`MB_YESNO` 的默认键与 X/Esc 一律 `IDNO(7)`** —— 答 YES 会弹启动设置并退出；AGERC `1=プログラムを終了する / 2=タイトルに戻る / 0=キャンセル`）；无窗口 / headless / feeder / 系统框失败 ⇒ 立即默认值 + 一行 warn（⛔ 不阻塞）。`_ShowDialog` 的其它选择子与 AGERC 设置对话框仍按 stub 口径。**AGERC.DLL 按真实 guest 模块装载**（PE 节映射进 guest / 真实 HINSTANCE=ImageBase / 资源与版本从该镜像解析）；它的三个导出里 `_GetInstance@0` / `_OperateMenu@16` 仍按本文 §3 的 stub 口径，`_ShowDialog@12` 只把 `id==3` 桥接成系统关窗确认（其它选择子返回 1 + warn）；镜像不存在 ⇒ `LoadLibrary` 返回 0 + 126（⛔ 不造假句柄），未实现的 AGERC 代码被执行 = **loud failure**（未实现导入 / fault 诊断，⛔ 不静默）。**镜像来源只有两段：默认 `--game-dir/AGERC.DLL`（安装树里那份=汉化免壳件，40/40 资源可映射），`--agerc` 可显式覆盖；两段都拿不到 ⇒ `LoadLibrary` 0 + 126**（⛔ 不造假句柄、⛔ 不回退 `DATA1.ALF` 的条目 —— 引擎本身也不回退，那条约只作取证对照）。**不需要拆壳件** —— 加壳只发生在正版根件（AsProtect，26/40 条资源在文件里没有字节）。伪 HICON 只作 `.rsrc` 映射真的失败时的**应急**（loud log），⛔ 不是预期路径、⛔ 不 fake 资源内容 | 窗口档会真的弹**系统对话框**并**等用户回答**（对话框期间 guest 冻结；单线程泵 / `--audio virtual` / `--audio off` 档音频停，真 feeder 档是否继续取决于后端）；headless 档看不到框、立即返回默认值；AGERC 设置对话框不生效；窗口图标可能是伪 HICON（不显示在客户区） | 配置读写的**文件**路径（INI / SYS4REG.INI）、存档本身；`_GetInstance` 的返回值 = **真实模块基址**（可当 HINSTANCE 传给 `LoadIconA`） |
| 永久 no-op | **注册表** | 整体 no-op：`RegOpenKeyExA` / `RegCreateKeyExA` 成功 + 非空假句柄；`RegQueryValueExA` 恒 `ERROR_FILE_NOT_FOUND(2)`（调用方回落默认值）；`RegSetValueExA` 成功但不落盘；`RegEnumValueA` 恒 `ERROR_NO_MORE_ITEMS(259)`；`RegCloseKey` 成功 | 注册表里的设置**不持久**；每次按默认值启动 | 句柄形态与错误码（成功 / 2 / 259）；配置读写的**文件**路径（INI / SYS4REG.INI）仍真写 |
| 永久 no-op | **摇杆** | 无设备：`joyGetNumDevs` 返回 0、`joyGetPosEx` 返回 `JOYERR_UNPLUGGED(167)`、`joyGetDevCapsA` 返回 `MMSYSERR_NODRIVER(6)` | 游戏若必须用摇杆则不可用（键盘 / 鼠标不受影响） | 键盘 / 鼠标输入（M2）；摇杆枚举的返回码 |
| **已实现（方案 C，2026-11-07）** | **音频** | **真 feeder 线程 + 独立 mixer 线程**：`AudioEngine` 实现游标 / 4 个位置通知 / 状态机 / 逐缓冲 `WAVEFORMATEX` 转换 / 混音 / SDL3 输出；`CreateThread(sub_4B5AA0)` 由 `ResumeThread` 启动真宿主线程，feeder 有自己的 `X86` / guest 栈 / 私有块缓存并执行 `sub_4B6C30`（PCM 仍由 guest 解码器产生）；guest 的每槽 `CRITICAL_SECTION` 映射到宿主递归锁。`--audio=virtual`/无 `--real-time` 时回退到方案 D 的主线程泵。**WAV 头 / 缓冲层是真的**（`REQ-01M6QSVR5NJDQVFHY6RX3934DN`）：`mmio*` 沿 guest 文件句柄真解析 RIFF；`IDirectSoundBuffer8` 的 `QI(IDirectSoundNotify)` / `SetNotificationPositions` 真存真投递。MCI / mixer 仍是成功 stub。默认 `--audio=sdl` + **静音**（`--sound` 才出声：设备 / 混音 / 喂料 / guest DS 调用照跑，只在交付前覆盖缓冲）；`--audio=off` 回到旧 silent。 | `--sound` 时能听到 BGM / SE / 语音；SE 延迟 ≈ 100 ms；主线程不再被 fill 卡住 | 设备游标 / 通知次序 / 逐缓冲格式 / 混音 PCM；`timeGetTime` / `timeSetEvent` / `timeKillEvent` / `timeGetDevCaps` 不变 |
| **已实现（方案 A，2026-11-07 用户批准）** | **视频**（纹理影片 / DirectShow 模式） | **真画面**：`CoCreateInstance(CLSID_FilterGraph)` 建**真宿主图** —— `IGraphBuilder::AddFilter` 接管引擎的 `CMovieTextureRenderer` 与源 filter（`AddRef` + `JoinFilterGraph`），`Render` 走 D1 接线（`EnumPins` → `QueryDirection` → `ReceiveConnection` → `QI(IMemInputPin)` → `NotifyAllocator(宿主分配器)`），**字节从引擎自己的源 pin 取**：`QI(IAsyncReader)`（本引擎 IID = `56A868AA-…`，槽 `Length +0x20` / `SyncRead +0x1C`）分块 `SyncRead`，宿主用 `video::MovieDecoder` 解 MPEG-1（1280×720 RGB24，自底向上 DIB），每帧把 one-shot 样本 `Receive` 给渲染器；`IMemAllocator` / `IMediaSample` / `IMediaSample2` / `IReferenceClock` 都是宿主 COM 对象（`pMediaType` 必须 NULL，否则渲染器每帧泄露一平面）。`Run` 前对两个 filter `SetSyncSource(宿主时钟)`。**② 渲染器服务线程事件由宿主合成**（显式平台偏差）：`sub_4D5710` 对 `[this+0x58]/[this+0x54]` 两个匿名 Event 做 `WaitForMultipleObjects(2,…,10000)` 重试；伪线程不跑线程体 ⇒ shim 把 **#1 句柄**当成已 signaled（`WAIT_OBJECT_0+1` ⇒ `sub_4D3E40` 的成功分支），默认生效，`LIFT_DSHOW_NO_SIGNAL=1` 可关。**影片音频已接**（2026-11-07）：`dshow::AudioRenderer` 就是 quartz 那条音频支路的输出端 —— 同步机制在它里面（时间 = `cpu.clock`，与视频同一份；`pts <= 流now + 200 ms` 交给设备、迟到 300 ms 丢弃、队列空记 underrun），`pl_mpeg` 解 MP2 / 44.1k→48k 重采样后经 `AudioEngine::pushHostPcm` 混进同一路 SDL 输出，音量走 `IBasicAudio::put_Volume`（−100 dB = 静音）。**仍然不做**：MF（`MFStartup` / `MFCreate*` / `MFGetService` / EVR / DXVA2 仍 no-op 或 `E_FAIL`）、**窗口影片族**（`IMediaEvent` + `IVideoWindow` 的窗口播放仍走形状层）。**保留**：影片 opcode 的状态推进与结束通知（`Run`/`WaitForCompletion`/`EC_COMPLETE` 语义不变） | 真实 LOGO/OP/剧情影片能播（实测 LOGO 8.642 s / 259 帧跑完、音轨 fed=383616 帧 / pushed=417582 帧、dropped=0 / underruns=0）；窗口级影片（非纹理）仍是形状层 | 帧的**来路**（引擎源 pin 的 `IAsyncReader`，⛔ 不解析 ALF、不猜文件名）、`ReceiveConnection` 的 media type 约定、样本时间戳的时钟域、影片 opcode 的状态推进与结束通知；**不是** MF/EVR/DXVA2 等价性  ★ 渲染器必须 **Running**（`Run`）且样本时间是**流时间**（相对片头）—— 否则引擎只挂样本不渲染、纹理全黑（doc §8.13）；像素由 `[dshow] tex movie` 抽样守（`tests/movie_logo.cpp`）。 |
| 延后实现 | **Live2D** | 关闭（游戏内开关）作为第一可交付状态 | 模型不显示（固定立绘） | 如果开关只是脚本级跳过，SDK 仍链接但不被调用 |
| 延后实现 | **3D** | **D3D9 最小设备簇 + D3DX Sprite/纹理 + fixed-function 流水线已落地**（`REQ-01M6MH5M975Q63AH85F03RVQFD` / `REQ-01M6MJRC587XWXR325K4N2JXMF` / `REQ-01M6MNR9M7S5M5711J79F9K32A`：`Direct3DCreate9` / `CreateDevice` / `Clear` / `Present` / `GetBackBuffer` / `SetRenderTarget`，`D3DXCreateSprite` 的 Begin/Draw/End、`D3DXCreateTexture`、纹理 surface、`D3DXMatrix*`、软件光栅）；**已到 Present 第一帧**；D3DX effect / `.x` 层级 / 完整 3D 战斗场景仍不做，未取证槽 = loud trap | 无 3D 战斗/场景；未取证的 D3DX effect / `.x` / 3D 状态仍会在命中时响亮失败 | 2D/ADV 主路径（D3D9 + GDI；DDraw 只作备用后端） |
| **延后实现（伪线程，2026-11-05 用户批准；音频例外 2026-11-07）** | **线程 / 定时器队列** | **guest 单线程 + 伪线程**：`CreateThread` 一般只发线程句柄、不执行 start routine；`ResumeThread` 按挂起计数返回旧值；`SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask` 按单核单线程返回成功值；`GetThreadPriority` 对当前线程与伪线程返回 `THREAD_PRIORITY_NORMAL`。**音频喂料线程 `sub_4B5AA0` 是唯一例外**：被 `AudioEngine` 接管，线程体不执行，控制流由块边界的泵 native 化（见音频行）。`ExitThread` / `FreeLibraryAndExitThread` / `CreateRemoteThread` / `_beginthreadex` / `CreateTimerQueue*` / `DeleteTimerQueueTimer` 仍**响亮失败** | 无真正后台线程；DirectShow / MF 的服务线程仍被伪线程 / 视频 skip 消化；音频由主线程泵承担（不并行） | 主线程的同步原语 / 等待 / 临界区、单实例信号量、`timeSetEvent` 的计时语义；伪线程句柄的 AddRef / Release / 优先级 / 亲和性**返回形态**；音频泵的 CS defer / 在飞计数 / 线程句柄 join |
| 硬错误 | **SEH / C++ 异常** | **不展开**：`RaiseException` / `RtlUnwind` 一律**响亮失败**（`addUnimplemented`，执行到就 trap）；`_CxxThrowException` 走同一条"未处理异常"路 | 异常路径**不恢复**（进程终止并留下诊断） | 失败诊断（EA / 访问类型 / 区域）——异常发生在成功路径上会**当场暴露**，不静默 |
| 硬错误 | **等待（未 signaled）** | `WaitForSingleObject(Ex)` / `WaitForMultipleObjects(Ex)` / `SignalObjectAndWait` 命中"需要等待"（对象未 signaled 且 `dwMilliseconds != 0`，含 `INFINITE`）⇒ **硬错误**：`shimAbort` 打印 API / handle / timeout 后终止，⛔ 不返回 `WAIT_TIMEOUT` | 游戏若真依赖等待会在这里停下；需要人工检查该调用场景并设计等待方案 | `dwMilliseconds == 0` 的轮询仍返回 `WAIT_TIMEOUT`；signaled 路径不变；`bWaitAll=TRUE` 且全部 signaled ⇒ `WAIT_OBJECT_0` |
| **平台偏差（2026-11-05）** | **游戏目录写入 = 影子覆盖层** | `--appdata` 非空时，落点原本在 `--game-dir` 的**写**改到 `<--appdata>/Game/<guest 相对路径>`；读先影子、再安装树；已有文件在写打开前 copy-up；`MakeSureDirectoryPathExists` 建影子目录。**没有 whiteout**：只在安装树里存在的文件 / 目录，删除 / 改名 / 改属性一律响亮失败（不改安装树）。无 `--appdata` 时保持旧行为（写安装树）并每次打警告 | 存档 / 配置 / 临时文件写进 appdata 影子而不是安装树；首次覆盖已有文件会 copy-up（不保时间戳）；安装树里的删除 / 改名不生效 | 游戏目录只读；读先影子；copy-up；安装树字节不变；无 appdata 的旧行为仍可用 |
| **平台偏差** | **临时文件名序号** | `GetTempFileNameA(uUnique=0)` 从 **0** 起递增找空号（真机从系统时间起）；`uUnique!=0` 按文档用调用方的值、**不创建**文件 | 具体序号与真机不同（可复现优先于逐值一致） | 名字形态 `<pre><uuuu>.TMP`（前缀只取 3 字符）、`uUnique!=0` 返回原值且不创建 |
| 平台偏差 | **文件时间（POSIX）** | creation 用 **birthtime**（macOS `stat` 的 `st_birthtimespec`）；文件系统不提供 birthtime 时**回退 ctime**；`SetFileTime` 的 creation 在能写的平台写（Windows `::SetFileTime`、macOS `setattrlist(ATTR_CMN_CRTIME)`），写不了就跳过并留日志 | 非 Apple POSIX 上 creation 可能是 ctime / 改不了；不影响 access / write | access / write 始终真写；`GetFileTime` / `WIN32_FIND_DATAA` 三个时间各写各的 |
| 平台偏差 | **超越函数** | `fsin` / `fcos` / `fsincos` / `fptan` / `fpatan` / `fyl2x` / `fyl2xp1` / `f2xm1` / `fscale` 走宿主 `libm`，允许 **1 ulp** 偏差 | 极端值可能与真机差 1 ulp（跨平台也可能不同） | 其余 x87 走 Berkeley SoftFloat（逐位一致） |
| 平台偏差 | **宿主页保护粒度** | 16 KiB 宿主页上，提交 / 退提交按**宿主页对齐**；非 safe 模式下同宿主页的邻页也可读写 | 越界访问可能不触发 fault（业务不应依赖越界 fault） | `LIFT_SAFE_MEMORY=1` 仍按 4 KiB guest 页逐页检查；guest 自己的越界由诊断路径暴露 |
| 平台偏差 | **GPU 光栅化 blend 舍入** | `--raster gpu` 允许 GPU 与 CPU 每通道 **≤1 LSB** 的差异（GPU 浮点 blend vs CPU 8-bit 舍入）；不把 `frameStats` 的单一 hash 当唯一判据，用逐像素 max-diff。CPU 缺省不受影响 | GPU 模式下与 CPU 输出可能差 1 LSB；不影响 CPU 逐字节判据 | CPU 路径逐字节不变；GPU 第 1 帧与 CPU 一致；差异超过 1 LSB 仍是 bug |

★ **四类不要混**：**永久 no-op**（大概率一直不做：菜单 / 注册表 / 摇杆）· **延后实现**（最终要做：模态〔仅关闭窗口二次确认〕/ 音频 / 视频 / Live2D / 3D）· **硬错误**（⛔ 不静默失败：线程 / SEH / 等待）· **平台偏差**（实现细节的显式取舍）。新加偏差时必须先归到其中一类。
★ **音频 / 视频的里程碑 = M4**（`REQ-01M6K84KHPA1XF3B1HSHXME6W0`）；用户 2026-11-04 口径：音频大概率更靠前，具体顺序在做 M2 时再调。

★ **3D 的前提**：若 Live2D 走 D3D 后端（`doc §6-2`），D3D9 不能砍；"3D 不做"与"保留 Live2D"互斥。

★ **线程 = 伪线程的依据与退路**（2026-11-04 用户先批准"整族不做"，2026-11-05 改为伪线程；2026-11-07 音频改方案 C）：`README.md` §2.5
已定 guest 单线程；`.lst` 里的线程全是**服务线程**（DirectShow 读文件 / 声音 / MF 回调 / 多媒体定时器），
**纹理影片**已按方案 A 落地（`Run` 链由宿主驱动、渲染器的服务线程等待事件由宿主合成，见 §1 视频行）；
**音频喂料线程**不再靠 silent 消化，而是由 `AudioEngine` 启动**真宿主 feeder 线程**
（自己的 `X86` / guest 栈 / 私有块缓存，执行 `sub_4B6C30`）+ 独立 mixer 线程。其余服务线程仍只让
创建 / 恢复 / 优先级 / 亲和性这组**控制面**成功返回，避免 boot 在"服务线程创建"上硬停；线程体本身仍绝不执行。
判据与退路见 `docs/thread-model-feasibility.md` §5.2 / `docs/audio-implementation.md`：音频只依赖
引擎自己的每槽 CS 协议 + 环形缓冲不重叠；⛔ 不把游戏 VM/脚本放多线程，也不做通用 TSO。
白名单落点：`tools/lift.mjs` 的 `FAMILY_STUB_APPROVED` **只列 PE 导入表里真出现的键**，且只列**仍是 stub** 的键
（`CreateThread` / `ResumeThread` / `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask`
已改成实现 ⇒ 2026-11-05 从白名单移除）。
★ **档位**：`CreateThread` 等是**实现**（`add`），不是 stub；`ExitThread` / `FreeLibraryAndExitThread` /
`CreateRemoteThread` / `_beginthreadex` / 定时器队列仍是 `addUnimplemented`（**硬 trap**）。

★ **异常也按"发生即失败"登记**（2026-11-04 用户）：`RaiseException` / `RtlUnwind` 不做 SEH 展开，执行到就 trap
（plan §9 R1：M1 只做"未处理异常 = 响亮失败 + 完整诊断"）。**若成功路径也用异常做流程控制** ⇒ 单独开单再议，
⛔ 不先假装恢复（那会把"异常没实现"变成静默的数据路径偏差）。

## 2. 被跳过的 spec 条款（点名）

- 菜单 / AGERC 设置对话框：Win32 菜单与对话框族（`WM_COMMAND` 的菜单命令、`DialogBoxParamA` 等）。
  ★ 例外：`MessageBoxA` 与 `_ShowDialog@12` 的 `id==3` 已实现为**系统对话框**（见 §1 的模态行），不在本条被跳过之列。
- 音频：MCI CD-DA / MIDI 仍是成功 stub；**DirectSound 设备语义已按方案 C 实现**（游标 / 通知 / 混音 /
  逐缓冲格式 / 真 feeder 线程），PCM 仍由 guest 解码器产生。
- 视频：MF 的 session / topology / EVR / DXVA2 内部；只保留影片 opcode 的状态推进。
- 线程 / 定时器队列：线程体执行 / 线程调度 / 定时器队列的线程语义
  （控制面的 `CreateThread` 等按 §1 的伪线程返回形态保留）；
  保留主线程的同步原语与等待、`timeSetEvent` 的计时。
- 异常：SEH 展开 / `.xdata` handler / C++ 异常展开的语义；保留"未处理异常 = 响亮失败 + 完整诊断"。
- Live2D：模型 / 动作 / 渲染接口（SDK 内部）；若走游戏 renderer，则不在平台层。
- 3D：D3D9 设备状态机 / D3DX effect / `.x` 层级；只保留 2D/ADV 路径。

## 3. stub 的硬要求

每处 stub 必须：

1. 返回**合理值**（`HMENU` 非空、`SetMenu` TRUE、`HRESULT` 成功）；
2. **仍为 stub 的菜单族不阻塞、不等待**（`TrackPopupMenu` 立即返回；影片 opcode 推进）。
   ★ 例外：`MessageBoxA` / `_ShowDialog@12` 已不是 stub（`add` 登记）：窗口档按设计**故意阻塞等人**
   （系统对话框），headless / feeder / 无窗口 ⇒ 立即返回默认值（`runtime/message_box.h`）；
3. **留一条可回查的记录**（`runtime/log.h` 分级）：默认 `warn` 下 `Info` / `Debug` 可能不打印，
   但调用点必须存在，`--log-level info|debug` / `--log-cat` 可查；⛔ 不是"没有记录"。
4. 在覆盖清单 `data/lift/overrides.json` 里登记 `kind: stub` + `reason`。

⛔ 不许静默 no-op；⛔ 不许用 stub 掩盖"该实现的路径"。

### 3.1 日志分级（2026-11-06）

`src/runtime/log.h` 是唯一口径：`quiet < error < warn < info < debug < trace`；默认 **`warn`**，
`Error` 永远显示（`quiet` 也不压），`shimAbort` / 未实现导入 / fault 不经过滤。分类由 `--log-cat`
（`file` / `vfs` / `shim` / `com` / `d3d9` / `d3dx9` / `dsound` / `winmm` / `video` / `loader` /
`native` / `report`）过滤。**引擎的只读探测失败**（先试外围文件、再走自己的归档）是 `Debug`，
默认看不到；真正的失败（写 / 权限 / 共享冲突 / 所有回退都失败 / 未实现）仍是 `Warn` / `Error`。
`--trace-shims` / `--trace-blocks` 的 trace 字节流不经过本闸门（A4 可复现性不变）。

## 4. 实现策略（不是偏差）

- 执行源 = **拆壳件 exe**（D1：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`，执行它的字节）；`.lst` 只作符号 / 对照 / 覆盖（`doc §5.6`）。
- 执行单位 = basic block；`jmp/call/ret` 只改 `eip` 回 dispatcher（`doc §5.7`）。
- 宿主 = SDL3；Win32/GDI/DDraw 形状层自己写（`doc §5.5`）。
- 宿主 fault 归因**分平台实现**（POSIX `sigaction` / Windows VEH）；这不是 guest 可见语义，不做抽象层。
- 覆盖 = 清单 + 块级调度表；生成物不入库（`doc §5.7`）。

## 5. 字体 / 文本的宿主策略与登记偏差（P2 · 2026-11-04）

> ★ 这一节是**用户 2026-11-04 的 D3 裁决在 lift 里的落地**（真源：`apps/emulator-cxx/SCOPE.md` §0 第 1 条 / §1.1 / §1.2）。
> 它登记的是**宿主视觉策略**，⛔ 不是引擎语义，⛔ 不写进 `data/spec/`。
> 实现：`src/runtime/font.{h,cpp}`（FreeType 桥 + GDI 对象状态）+ `src/shims/gdi32_object.cpp` /
> `src/shims/gdi32_font.cpp`（Win32 形状）；判据：`tests/gdi.cpp`（pure）/ `tests/gdi_font.cpp`（assets）。

| 类别 | 偏差 | 做法 | 用户可见代价 | 仍必须保留 |
|---|---|---|---|---|
| **平台偏差（用户裁决）** | **字体来源 = 宿主 Amayui CN** | 一律取 `corpus/assets/fonts/Amayui-CN_cnjp.ttf`（粗体同目录 `-Bold.ttf`，CMake 注入 `LIFT_HOST_FONT_PATH`；`--font` 只是调试覆盖）。`CreateFontIndirectA` 的**字面名忽略**；`AddFontMemResourceEx` 送进来的**内存忽略**（仍返回非 0 句柄 + `*pNumFonts = 1`：宿主字体确实可用，但**不是**这段内存加进去的，日志会明说）。`--font-engine-name` 是显式 A/B 对照档 | 与真机（= 引擎字面名那份 MS Gothic）字形不同；简体字形由 Amayui 覆盖 | 字体对象的 `lfHeight` / `lfWeight` / 斜体等**请求形态**；`GetTextMetricsA` 的各格由 FT face 的 `size` 算；`SelectObject` / `DeleteObject` / `GetCurrentObject` 的对象语义 |
| 平台偏差 | **A/B 对照档只到"上报族名"为止** | `--font-engine-name` 打开时，`EnumFontFamiliesExA` 上报引擎字面名；字形仍来自宿主文件（lift 没有"按系统族名查字"的平台入口；⛔ 不造第二条取字路径） | 对照档不能真的画出 MS Gothic | 默认档完全不受影响 |
| **平台偏差（2026-11-10 新增）** | **枚举时把引擎硬编码的族名全部报出（字形仍一律 Amayui CN）** | `EnumFontFamiliesExA` 按**固定名单逐条回调**（第一条 = 宿主 `Amayui CN`）：`ＭＳ ゴシック`（EA `0x52686C`）、引擎配置里的 `ＭＳ 明朝`、`AGE Extend`（EA `0x5208D8` = `aAgeExtend`；`@AGE Extend` @`0x5268F8` 是竖排变体、不报。引擎经 `AddFontMemResourceEx` 送进来的字体）、`IPA Pゴシック`（EA `0x5268CC`）/ `IPA P明朝`（EA `0x5268C0`）、以及 `.data` 那 10 条已知族名（EA `0x52985C` 起：MS PMincho / MS Mincho / MS UI Gothic / MS PGothic MC / MS PGothic / MS Gothic / Meiryo UI Bold / Meiryo UI / Meiryo Bold / Meiryo，`sub_491060` 用它过滤 `FontLink\SystemLink`）。**字形一律仍来自宿主 Amayui CN** —— 名字只是**标签**（`lfCharSet` 回显请求值；`@` 开头的竖排族名不报，见下）。名单在 `src/shims/gdi32_font.cpp` 的 `kEnumFamilyNames`，⛔ 不解析 TTF 的 name 表、⛔ 不枚举宿主系统字体（名字不对且不确定） | "按字面名查系统字体"被替换成"名单里的名字都落到同一份宿主字形" ⇒ 真机上会出现的字体差异在这里不出现 | **名字表的填充本身**（引擎 boot 时用它建字体名表 `this+0x313C0..0x313C4`，运行期 `sub_4581F0` 按自己配置里的字面名查 `sub_428990`）；缺一条 ⇒ 查表返回 `-1` ⇒ `0x4586AA` 抛 `std::out_of_range`（`TITLE→OPTION→ADV设定` 与 `TITLE→GameStart` 都崩，2026-11-10 实测）。**回调极性**：返回 **0** 才停、非 0 **继续**（MSDN `EnumFontFamExProc`；引擎的 `Proc` EA `0x45EAC0` 所有路径都返回 1） |
| **平台偏差（宿主视觉策略）** | **基线补偿 +2px** | 画字前把基线抬 `--baseline-lift`（缺省 **2**；`0` = 关）。理由：Amayui 的 ascent 比引擎的参考字体「ＭＳ ゴシック」大（emulator-cxx 实测 20px 20 / 28px 28 vs 17 / 24），墨迹整体偏低 | 行高 / 基线与真机可能差 1–2px | `tmAscent` 仍是 FT 的真值（补偿**不**写进度量）；`GetTextExtentPoint32A` / GGO 的 advance 与补偿无关 |
| 平台偏差 | **GGO 位图布局按 Wine 的实测口径** | `GGO_BITMAP`：1bpp、行 = `((w+31)>>5)<<2`（DWORD 对齐）、覆盖率 ≥ 128 置位；`GGO_GRAY4/8`：**每像素 1 字节**、行 = `(w+3)&~3`、值域 0..16 / 0..64（17 / 65 级，满覆盖 = 16 / 64）；`cbBuffer == 0` ⇒ 返回所需字节数 | 灰度档的字节布局与"4bpp"的常见误解不同（按 Wine `win32u/freetype.c` 的 `max_level` 口径，见 ops-forensics §35） | 行对齐 / 值域 / 返回字节数 / `gmBlackBoxX·Y` / `gmptGlyphOrigin` / `gmCellIncX` 的自洽（`tests/gdi.cpp` 逐条钉住） |
| 平台偏差 | **`TextOutA` 用 FT 连续覆盖率混合** | 屏幕 DC 只记账（不写宿主窗口）；表面 DC 写 guest surface 的像素（通道掩码解码 + 裁剪 + 文字色 + bk mode） | GDI 自己的文字默认是 1 位（`font_smoothing = GGO_BITMAP`）；1px 级栅格差按平台 discard | 文字色 / 背景模式 / 裁剪 / 推进（`advance` = `Σ gmCellIncX`） |
| **未做 / loud 失败** | **未实现的字形形态** | `GGO_NATIVE` / `GGO_GRAY2` / `GGO_LCD` ⇒ `GDI_ERROR` + 一行日志；**非单位 MAT2** ⇒ 拒绝（`GDI_ERROR` + 日志）；`GGO_UNHINTED` 位剥掉后按正常字形（hinting 差属平台 discard） | 引擎若真发这些形态会看到"字形取不到"（不会画错） | `GGO_METRICS`(0) / `GGO_BITMAP`(1) / `GGO_GRAY4`(5) / `GGO_GRAY8`(6) |
| 未做 | **DC 位图 / Blt / 设备能力** | `CreateCompatibleDC` / `CreateDIBSection` / `BitBlt` / `GetDeviceCaps` 仍是 `addUnimplemented`（P1 / P4 的落点） | 依调用方 | 表面 DC 的 `GetDC`/`ReleaseDC` 由 P1 接（见下） |
| 未做 | **调色板 / 8bpp 表面** | `TextOutA` 只按通道掩码写 16 / 24 / 32bpp；8bpp 索引色不建模（遇到会写不进去，像素数照实记） | 8bpp 下的文字不出现 | 32 / 16bpp 的像素写入 |
| 平台偏差 | **缺宿主字体 ⇒ 明确失败** | `GetTextMetricsA` / `GetGlyphOutlineA` / `GetTextExtentPoint32A` / `TextOutA` 返回 0 或 `GDI_ERROR` + 一行日志（⛔ 不画空白冒充成功）；assets 夹具缺字体时 `LIFT_SKIP`（77） | 没字体就没有文本 | 失败值本身（调用方可以走自己的降级） |

★ **与 P1 的接缝**（DDraw 表面 DC）：
`bool lift::fontAttachDcSurface(std::uint32_t hdc, std::uint32_t surface_ea, SurfaceArena* arena)`
（`src/runtime/font.h`）。P1 在 **`IDirectDrawSurface::GetDC`（vtable +68，十进制）返回 HDC 之前**调用它，
在 `ReleaseDC`（vtable +104）里调 `fontDetachDc(hdc)`；`TextOutA` 于是直接写进那块 guest surface 的像素。
绑定后 arena 里找不到表面 ⇒ `TextOutA` **loud trap**（P1 的拼装错误，不静默跳过）。

★ **测试档位**：`tests/gdi.cpp` 顶层注册（pure，合成字形源；里面的 `enum-font-families*`
两条用例钉住"枚举条数 > 1 / 引擎的查表目标逐字节命中 / 每条 lfCharSet 回显 / 回调返回 0 才停"）；
`tests/gdi_font.cpp` 在 `if(EXISTS "${LIFT_HOST_FONT_PATH}")` 里（assets，`SKIP_RETURN_CODE 77`）；
`tests/title_entry_paths.cpp` 在 `if(EXISTS "…/SYS4INI.BIN" AND DEFINED ENV{LIFT_ASSETS})` 里
（assets，端到端跑 `TITLE→OPTION→ADV设定` 与 `TITLE→GameStart` 两条路径）。

## 5.1 修订（2026-11-08）：基线补偿的**位移点**从 TextOutA 搬到 `gmptGlyphOrigin.y`

> ★ 本节**追加**，不改上面任何一行原文；与 §1 / §5 冲突时以本节为准（口径不变，改的是落点）。
> 起因与证据：`.tmp/task3-report.md` §4 + 父级 2026-11-08 裁决。

**为什么要改**

- §5 的补偿（`--baseline-lift`，缺省 2）原先**只在 `TextOutA` 的绘制基线**上生效
  （`gdiDrawText` 的 `baseline = y + tm.ascent - lift`）。但引擎正文**不走 `TextOutA`**：它调
  `GetGlyphOutlineA` 拿灰度位图，再用 `tmAscent` 自己算基线合成
  （`0x46F893`：字形顶 = 基线 − `gmptGlyphOrigin.y`）⇒ 那个 2px 对正文**完全没生效**，
  guest 可见的 ascent 仍是 FT 真值 20，而 cxx 的有效值 = 20 − 2 = 18 ⇒ lift 的字比 cxx **低 2px**。
- cxx 的 `lift`（`apps/emulator-cxx/src/core/text_raster.cpp:647`，唯一应用点 `:675`）减的是"每次绘制的
  最终基线"，等价于**只移墨迹、不改行高**。要同一个效果，就只能移"引擎自己也会用的那个量"。

**移在哪（唯一落点）**

- `src/runtime/font.cpp` 的 `FontEngine::glyph`：`origin_y = slot->bitmap_top + fontConfig().baseline_lift`。
  常量只有一处：`src/runtime/font.h` 的 `kBaselineLift`（注释指向 cxx 的 `text_raster.cpp:647`），
  运行时仍由 `--baseline-lift`（`FontConfig::baseline_lift`）决定，`0` = 关。
- 同时把 `gdiDrawText` 里的 `- lift` **删掉**（否则 `TextOutA` 会抬 2N px）。两处的合成结果逐像素等于旧行为：
  旧 `baseline = y + ascent − lift` + 原 origin；新 `baseline = y + ascent` + (origin + lift) —— 墨迹位置相同。
- 由此 §5 表格里"`tmAscent` 仍是 FT 真值（补偿**不**写进度量）"**依然成立且是硬约束**：
  这次只动 `gmptGlyphOrigin.y`，`TEXTMETRICA` 的 56 B 逐字节不变（守卫 `lift/metrics-unchanged`）。
- §5 表格里"`gmptGlyphOrigin` 自洽"这条**含义收窄为**：`gmptGlyphOrigin.y` 与**实际落墨位置**自洽
  （字形顶 = 基线 − `origin.y`），而**不再**等于 FT 的裸 `bitmap_top`。这是有意的：宿主字体替换后，
  引擎那份"参考字体的 ascent"与 Amayui 的 FT ascent 有差（实测差 2–3px，见 `ops-forensics` §73），
  `lift = 2` 是**按 2px 经验值补偿**，不是"把差还到 0"的精确值。
  ★ 谁能扛这条：合成档的 `origin_y == black_box_y` 由 `tests/gdi.cpp` 钉住（合成档不吃补偿）；
  **真字体档**只有 `tests/text_baseline_lift.cpp` 的 ②（钉"补偿 = +2 且方向正确"）+ ⑤（钉"整块墨迹
  平移、无裁剪"）—— 它钉的是**差值**，不是绝对像素值（绝对 `bitmap_top` 随字体文件变）。

**可观测 / 防裁剪证据（守卫，都会红）**

- `tests/text_baseline_lift.cpp`（assets，缺字体 ⇒ SKIP 77）：
  ① 缺省 `baseline_lift` == 2；② `--baseline-lift 0` 与缺省下 guest 读到的 `gmptGlyphOrigin.y` 差**正好 2**
  且方向正确（origin.y 更大 ⇒ 墨迹更高）；③ 两次 `TEXTMETRICA`（写**两块不同**的缓冲）56 B 逐字节相同、
  `GetTextExtentPoint32A` 的宽/高不变；④ 开关设 5 ⇒ origin 差正好 5（位移量不是硬编码常数）；⑤ 同一段文本
  画两次 ⇒ **墨迹像素总数不变**（容差 0，证明没有被字形格 / 表面裁掉）+ **垂直质心正好移动 2px**；
  ⑥ `gdi32!TextOutA`（**经 shim 跑 guest 代码**）也仍然只随开关移动 2px，且 DC 的 `text_calls` /
  `pixels_written` 真的涨了。★ ⑤ 走的是宿主 helper `gdiDrawText`、⑥ 走 Win32 形状层，两者**都**跑同一条
  绘制路径（`TextOutA` 的宿主实现就是 `gdiDrawText`），差别只在"经过 shim"。
- 端到端（**OPTION** 帧 155，`--present-screenshot` + `--dump-frame`，`.tmp/task3-impl/`）：
  `--baseline-lift 0` 与缺省两档的差异**只落在文本行的墨迹上**（其余 483/720 行逐字节相同）；
  在变化像素集上做垂直平移匹配 ⇒ **`k=+2` 均差 3.57/255、`k=0` 均差 108.16**（墨迹整体上移 2px）。
  残差 3.57 的来源是"字形半透明混在**静止**纹理背景上"（背景那 2 行不同），**不是**裁剪。
  ⚠ 这是**一次性证据、不可在 ctest 里回归**（要游戏安装 + CPU 光栅化 5 分钟/次）；能回归的判据是上面那条
  assets CTest。另：**TITLE 帧三张两两逐字节相同** —— 该画面没有经 GGO 合成的可见文本（标题 / 按钮是素材
  sprite），所以整机对照只能在 OPTION 这类画面上做，⛔ 别拿 TITLE 当"文本基线对齐"的判据。

**怎么回退**

- 只想关掉补偿：`--baseline-lift 0`（= 改动前二进制在 `--baseline-lift 0` 下的逐字节行为）。
- 想连代码一起回退：把 `kBaselineLift` 改 0 **并**把 `glyph()` 里的 `+ fontConfig().baseline_lift` 去掉、
  把 `gdiDrawText` 的 `- lift` 加回来 —— 这三处是同一个改动。**哪些守卫会红（按实际测过的）**：
  ①（缺省值 ≠ 2）、**②**（origin 差没了）、**④**（开关差没了）会红；⚠ ⑤ / ⑥ **仍然绿** —— 它们走的就是
  `gdiDrawText`，把 lift 挪回去那条路的墨迹照样只随开关动 2px ⇒ "回退不完整"不会被 ⑤/⑥ 发现，
  只有 ② 会（这就是为什么 ② 是这条改动的**唯一**方向判据）。

★ **测试档位不变**：`tests/gdi_font_metrics_cache.cpp` 顶层注册（pure）；`tests/text_baseline_lift.cpp`
在 `if(EXISTS "${LIFT_HOST_FONT_PATH}")` 里（assets，`SKIP_RETURN_CODE 77`）。