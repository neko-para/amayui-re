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
| 延后实现 | **模态**（有限） | 延后实现：只做**关闭窗口的二次确认**（`MessageBoxA` 的 OK / Cancel 语义）；⛔ 不做 AGERC 提供的复杂设置对话框。**AGERC.DLL 按真实 guest 模块装载**（PE 节映射进 guest / 真实 HINSTANCE=ImageBase / 资源与版本从该镜像解析）；它的三个导出（`_GetInstance@0` / `_ShowDialog@12` / `_OperateMenu@16`）仍按本文 §1 的模态 no-op stub；镜像不存在 ⇒ `LoadLibrary` 返回 0 + 126（⛔ 不造假句柄），未实现的 AGERC 代码被执行 = **loud failure**（未实现导入 / fault 诊断，⛔ 不静默）。**镜像来源只有两段：默认 `--game-dir/AGERC.DLL`（安装树里那份=汉化免壳件，40/40 资源可映射），`--agerc` 可显式覆盖；两段都拿不到 ⇒ `LoadLibrary` 0 + 126**（⛔ 不造假句柄、⛔ 不回退 `DATA1.ALF` 的条目 —— 引擎本身也不回退，那条约只作取证对照）。**不需要拆壳件** —— 加壳只发生在正版根件（AsProtect，26/40 条资源在文件里没有字节）。伪 HICON 只作 `.rsrc` 映射真的失败时的**应急**（loud log），⛔ 不是预期路径、⛔ 不 fake 资源内容 | 当前仍是 `IDOK` no-op；关闭确认直接继续；设置对话框不生效；窗口图标可能是伪 HICON（不显示在客户区） | 配置读写的**文件**路径（INI / SYS4REG.INI）、存档本身；`_GetInstance` 的返回值 = **真实模块基址**（可当 HINSTANCE 传给 `LoadIconA`） |
| 永久 no-op | **注册表** | 整体 no-op：`RegOpenKeyExA` / `RegCreateKeyExA` 成功 + 非空假句柄；`RegQueryValueExA` 恒 `ERROR_FILE_NOT_FOUND(2)`（调用方回落默认值）；`RegSetValueExA` 成功但不落盘；`RegEnumValueA` 恒 `ERROR_NO_MORE_ITEMS(259)`；`RegCloseKey` 成功 | 注册表里的设置**不持久**；每次按默认值启动 | 句柄形态与错误码（成功 / 2 / 259）；配置读写的**文件**路径（INI / SYS4REG.INI）仍真写 |
| 永久 no-op | **摇杆** | 无设备：`joyGetNumDevs` 返回 0、`joyGetPosEx` 返回 `JOYERR_UNPLUGGED(167)`、`joyGetDevCapsA` 返回 `MMSYSERR_NODRIVER(6)` | 游戏若必须用摇杆则不可用（键盘 / 鼠标不受影响） | 键盘 / 鼠标输入（M2）；摇杆枚举的返回码 |
| **已实现（方案 D，2026-11-07）** | **音频** | **设备模型 + native 喂料泵**：`AudioEngine` 实现游标 / 4 个位置通知 / 状态机 / 逐缓冲 `WAVEFORMATEX` 转换 / 混音 / SDL3 输出；`CreateThread(sub_4B5AA0)` 不执行 guest 线程体，由 dispatcher 块边界的泵调 `sub_4B6C30`（PCM 仍由 guest 解码器产生）。**WAV 头 / 缓冲层是真的**（`REQ-01M6QSVR5NJDQVFHY6RX3934DN`）：`mmio*` 沿 guest 文件句柄真解析 RIFF；`IDirectSoundBuffer8` 的 `QI(IDirectSoundNotify)` / `SetNotificationPositions` 真存真投递。MCI / mixer 仍是成功 stub。默认 `--audio=sdl`；`--audio=off` 回到旧 silent。 | 能听到 BGM / SE / 语音；fill 期间主线程 ~370 ms / 0.5 s 音频（D 的吞吐代价）；SE 延迟 ≈ lead 600 ms | 设备游标 / 通知次序 / 逐缓冲格式 / 混音 PCM；`timeGetTime` / `timeSetEvent` / `timeKillEvent` / `timeGetDevCaps` 不变 |
| 延后实现（**M4**） | **视频** | skip（延后实现）：影片 opcode 必须**推进**，不许挂起。**DirectShow / AMultiMediaStream 形状层已落地**（`REQ-01M6QPYG431WK2M3C76F6H1XBZ`）：`ole32!CoCreateInstance` 对 `CLSID_FilterGraph` / `CLSID_AMMultiMediaStream` 返回 guest 可见的形状对象；`IGraphBuilder` / `IMediaControl` / `IMediaEvent` / `IMediaSeeking` / `IMediaPosition` / `IVideoWindow` 的取证槽返回成功；`Run` 后 `WaitForCompletion` **立即** `EC_COMPLETE`（零长影片）；时长 / 位置恒 0。MF 仍 no-op（`MFStartup` / `MFShutdown` 成功；`MFCreate*` / `MFGetService` / DXVA2 返回 `E_FAIL` + 空输出，⛔ 不返回假 COM 对象） | 无真实画面 / 声音；影片按零长立即结束（LOGO/OP/剧情影片被跳过） | 影片 opcode 的状态推进与结束通知；**不是**解码正确性 |
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

★ **线程 = 伪线程的依据与退路**（2026-11-04 用户先批准"整族不做"，2026-11-05 改为伪线程；2026-11-07 音频改方案 D）：`README.md` §2.5
已定 guest 单线程；`.lst` 里的线程全是**服务线程**（DirectShow 读文件 / 声音 / MF 回调 / 多媒体定时器），
视频仍 skip；**音频喂料线程**不再靠 silent 消化，而是由 `AudioEngine` 在块边界 native 化
（`sub_4B5AA0` 的 12 段 native + 唯一 guest 调用 `sub_4B6C30`）。其余服务线程仍只让创建 / 恢复 /
优先级 / 亲和性这组**控制面**成功返回，避免 boot 在"服务线程创建"上硬停；线程体本身仍绝不执行。
判据与退路见 `docs/thread-model-feasibility.md` §5.1 / `docs/audio-implementation.md`：**音频 fill 的
吞吐（实测 ~370 ms / 0.5 s）是 D 的已知代价**；⛔ 不在本线直接开并发（真并行要先有 TSO/同步层证据、
再改 §2.5 的单线程口径）。
白名单落点：`tools/lift.mjs` 的 `FAMILY_STUB_APPROVED` **只列 PE 导入表里真出现的键**，且只列**仍是 stub** 的键
（`CreateThread` / `ResumeThread` / `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask`
已改成实现 ⇒ 2026-11-05 从白名单移除）。
★ **档位**：`CreateThread` 等是**实现**（`add`），不是 stub；`ExitThread` / `FreeLibraryAndExitThread` /
`CreateRemoteThread` / `_beginthreadex` / 定时器队列仍是 `addUnimplemented`（**硬 trap**）。

★ **异常也按"发生即失败"登记**（2026-11-04 用户）：`RaiseException` / `RtlUnwind` 不做 SEH 展开，执行到就 trap
（plan §9 R1：M1 只做"未处理异常 = 响亮失败 + 完整诊断"）。**若成功路径也用异常做流程控制** ⇒ 单独开单再议，
⛔ 不先假装恢复（那会把"异常没实现"变成静默的数据路径偏差）。

## 2. 被跳过的 spec 条款（点名）

- 菜单 / 模态：Win32 菜单与对话框族（`WM_COMMAND` 的菜单命令、`DialogBoxParamA` 等）。
- 音频：MCI CD-DA / MIDI 仍是成功 stub；**DirectSound 设备语义已按方案 D 实现**（游标 / 通知 / 混音 /
  逐缓冲格式），PCM 仍由 guest 解码器产生。保留的是"请求 + 返回码"这一层不再用于 DS 的 silent 路径。
- 视频：MF 的 session / topology / EVR / DXVA2 内部；只保留影片 opcode 的状态推进。
- 线程 / 定时器队列：线程体执行 / 线程调度 / 定时器队列的线程语义
  （控制面的 `CreateThread` 等按 §1 的伪线程返回形态保留）；
  保留主线程的同步原语与等待、`timeSetEvent` 的计时。
- 异常：SEH 展开 / `.xdata` handler / C++ 异常展开的语义；保留"未处理异常 = 响亮失败 + 完整诊断"。
- Live2D：模型 / 动作 / 渲染接口（SDK 内部）；若走游戏 renderer，则不在平台层。
- 3D：D3D9 设备状态机 / D3DX effect / `.x` 层级；只保留 2D/ADV 路径。

## 3. stub 的硬要求

每处 stub 必须：

1. 返回**合理值**（`HMENU` 非空、`SetMenu` TRUE、`MessageBoxA` IDOK、`HRESULT` 成功）；
2. **不阻塞、不等待**（`TrackPopupMenu` 立即返回；影片 opcode 推进）；
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

★ **测试档位**：`tests/gdi.cpp` 顶层注册（pure，合成字形源）；`tests/gdi_font.cpp` 在
`if(EXISTS "${LIFT_HOST_FONT_PATH}")` 里（assets，`SKIP_RETURN_CODE 77`）。
