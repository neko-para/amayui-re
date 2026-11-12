# emulator-lift —— 执行 `AGE.EXE` 原机器码的 guest-state 模拟器（`.lst` 作符号与对照）

> 本文件只写**不变的口径**（源 / 执行 / 宿主 / 覆盖 / 边界），**不写进度与计数**：
> 进度看 `pnpm tools requirements plan`（本线 `REQ-01M6GJMENJ8XEK4ZPTA768YBPW`）；
> 数据与方法看 `docs/03-emulator/platform-effort.md` §4.1、§5.5–§5.7。
> ★ 本文件只钉口径与落点；实现范围 / 进度看需求树。

## 1. 它是什么 / 不是什么

- **是**：把 `AGE.EXE`（**拆壳件**）的机器码放上"带 guest 状态的 x86 执行器" + Win32/GDI/DDraw 形状层，
  宿主用 **SDL3**（macOS Metal / Linux Vulkan / Windows D3D12）。
- **不是**：`apps/emulator-cxx` 的延续（那是按 spec 重写）；也**不是**把 `.c` 编成 64 位原生源码
  （32→64 是移植，不是机械翻译）。
- **边界**：`data/spec/` 只作调试对照，**不是本项目的真源**；本项目的真源是
  **拆壳件的字节 + `.lst`（符号与对照）+ 覆盖清单**；⛔ 不拿 emulator-cxx 的 core 当 shim。

### 1.1 与 Wine / 与 emulator 的分野（三条路线，本线是第二条）

`docs/03-emulator/platform-effort.md` §5.4 把"用 Wine 系组件跑 Windows 二进制"列为**第三条路线**。
本线**不是**它，但**平台层的形状与它同类**（都要自己装 PE、自己实现 Win32 语义）。差别在两处：

| | Wine（换 ISA 时 + Rosetta 2 / FEX / box86） | 本线 |
|---|---|---|
| PE 装载 / IAT | Wine 自己的 loader | **自写** |
| Win32 语义 | 自己实现（在宿主的原生执行之上） | **自写**，只做 `SCOPE.md` 允许的子集 |
| **指令执行** | **宿主直接执行 guest 的原机器码**；ISA 不同就外包 | **必须在自己手里**（Wine 没有翻译层，本线有；执行源 = 拆壳件字节，见 §2.1） |
| 图形 / 音频 | wined3d / vkd3d → Vulkan → MoltenVK | SDL3 + 自写 DDraw / GDI 形状；D3D9 按 `platform-effort.md` §3.7 外包**或停手** |
| 目标函数 | **兼容性面**（尽可能多的程序能跑） | **可观测 / 可复现**：虚拟时钟、确定性 trace、EA 级诊断、覆盖清单 |
| 出界行为 | 尽量兜住 | **响亮失败**（⛔ 不静默 no-op，见 `SCOPE.md` §3） |

★ 一句话：本线 ≈ **Wine 的平台层语义 + 一个 box86/FEX 那样的执行层**，两边都只覆盖"这一个 EXE
用到的子集"，再加一件 Wine 不做的事（**可观测 / 可复现**）。
★ 所以"这个 EXE 用不到的"一律不做，而"用得到但没实现"一律**响亮失败** —— 这与 Wine 的价值取向相反，
是**有意的**。

**★ 这条线"还是这条线"的唯一判据**：平台层是**自写、自洽的 Win32 形状**。
一旦某一块改成"嵌入 Wine 系组件"（典型：D3D9 交给 wined3d / DXVK），那一块就归**第三条路线**：
要单独立项、⛔ 不计入本线成本（这正是 `platform-effort.md` §3.7 把它记作"换路线"的原因）。

**★ 为什么不能把 Wine 的 DLL 直接当 shim**（最容易想到的捷径）：现代 Wine 的 **PE 侧基本是壳**，
实现在 **unix 侧的 `.so`** —— PE 模块经 `WINE_UNIX_CALL` / `__wine_unix_call` 转到 unix 侧的
`unixlib_entry_t` 函数表（两侧声明都在 Wine 的 `include/wine/unixlib.h` 里）。
那是**宿主原生代码，guest 执行层跑不了它** ⇒ 真要复用就等于把整个 Wine 进程搬进来 = 第三条路线。

## 2. 已定决策

### 2.1 源：**拆壳件的字节是执行源**；`.lst` 是符号 / 对照 / 覆盖真源；`.c` 是阅读器

★ **已定（2026-11-03 用户）**：执行源 = **拆壳件 `AGE.EXE__dumped.sectfix.EXE` 的机器码字节**，**已入库（LFS）**。
- 清单条目 **`binary/age-sectfix`**（`role: baseline` + `readOnly`）：
  `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` —— **默认就从这个仓库内路径读**
  （`git check-attr` 实测 `filter=lfs`；fresh clone 要 `git lfs pull`）。
  覆盖顺序：`--exe <path>` → 环境变量 → 仓库内路径；**都找不到就拒绝启动**，
  ⛔ 不回退到游戏目录里的原版 exe。
- **不是**另一份拆壳件 `binary/age-original`（`AGE.EXE__dumped.EXE`，仍是 `external-only`）：它的
  **节表未还原**（直接分析会丢数据段），且 sha256 与 `.lst` 头部的 `; Input SHA256 :` **不同**
  （两份只差节表 24 字节）—— 用错这一份，就意味着"执行的字节"与"用来看的语料"不是同一份。
  载入时**必须**校验这个 sha256（对上了就自证了这份对齐）。

**为什么不是"按 `.lst` 文本 lift"**（证据，不是偏好）：
- 语料**没有机器码字节列**（每行只有 `段:EA 助记符 操作数`）；
- 且一部分 CRT / C++ 运行时代码在 `.lst` 里被 `COLLAPSED`，**一行指令都没有** —— 而 `_memcpy` / `_rand`
  这类**被直呼**的函数就在其中 ⇒ 文本路拿不到它们的指令，执行必然断在半路。
- 现查方法与数字：`docs/plan-m1-cpu-base.md` §3 的 F1 / F2。

**`.lst` 因此仍然不可替代**：它给 `EA → 指令文本` 与**指令边界**，是解码器的**独立对照**
（判据见 `docs/plan-m1-cpu-base.md` §5 的 A3：逐条比 `EA` 差与助记符，不一致必须为 0）
以及所有诊断文本的来源。

- `.c` 的 32→64 面（`__thiscall` / `__stdcall` / 指针截断）是**系统性移植**，不是改几个类型；
  guest-state 不碰宿主指针。`.c` 的缺函数 / 位宽产物是**小补丁 + 逐处核对**，不是主路径。
- ⇒ 执行源 = 拆壳件字节；`.c` 只在"这段在干嘛"时读，结论回字节与 `.lst` 核。
- 判据与现查命令：`docs/03-emulator/platform-effort.md` §5.6 · `pnpm tools disasm-fn summary`。

### 2.2 执行：按块寻址，不按函数

- 存在**跨函数 `jmp sub_*`**、**function chunk**、**跳转表 / 间接 jmp** ⇒
  "一个 `sub_` 一个 C 函数 + 内部 `goto`"不成立。
- 执行单位 = **basic block**；`jmp` / `call` / `ret` 只改 `eip` 并回 dispatcher；C 函数不跨函数 `goto`。
- **函数** = 入口块 + 内部块 + 外部入口块（chunk）的**聚合**，只用于报告与覆盖；
  它**不是**执行单位。
- 数据见 doc §5.7；块表 / 调度表的接口见 `src/runtime/x86.h`。

### 2.3 宿主：SDL3，不直接调 AppKit

- SDL3 提供窗口 / 事件 / 音频 / SDL_GPU；macOS 走 Metal（MSL 源码），Linux Vulkan，Windows D3D12。
- ⚠ **"不依赖 AppKit" = 我们的代码不直接调**；SDL 的 macOS 后端内部仍用 Cocoa/AppKit
  （只要要原生窗口就绕不开）。
- SDL **只替换宿主层**；Win32 消息 / GDI DC / DirectDraw 表面仍由本项目自己写。
- 分层：`lifted guest blocks → Win32/GDI/DDraw 形状层 → SDL3 → Metal/Vulkan/D3D12`。

### 2.4 覆盖：清单 + 调度表

- 运行时表 `table[EA]` 合并三类条目：覆盖项 `ovr_<name>` · 桩 `stub_trap` · 其余走 guest 代码；
  函数级只是**聚合视图**（块级才是执行单位，见 §2.2）。
- **清单** = `data/lift/overrides.json`（格式见 doc §5.7）：
  `{ ea|range, kind: replace|stub|hook, impl, boundary, reason }`。
- 粒度：**平台 shim**（API 级）/ **函数级** / **子系统级**（视频推荐：`video::Session` + 其余 MF 包装 `stub`）。
- `stub` 必须 **trap**（打印 EA + 调用栈），不许静默 no-op。
- ★ **执行 = 纯内存解释（已定 2026-11-03 用户）**：运行时解码 + **块缓存**，`table[EA]` 的条目在首次执行时填入。
  **默认零生成代码** —— 没有构建期生成物，也就没有"生成树与字节不一致"这类守卫要养。
  `guest_<EA>` / `bb_<EA>` 因此在本线指**块缓存里的条目**，不是产物文件。
  将来若真要"把某段块离线导出"（对照用 / 把函数固化成夹具），那属于**调试工具**，⛔ 不是执行前提。

### 2.5 线程模型：业务单线程，只有服务线程

- 证据（`.lst`）：`CreateThread` ×2（DirectShow 异步读文件 → `StartAddress`/`sub_402330`；
  Sound → `sub_4B5AA0`）、`__beginthreadex` ×1（MF 回调 → `sub_47B200`）、`timeSetEvent` ×1（多媒体定时器）。
  `_WinMain` 的 `CreateSemaphoreA(..., 1, Name)` 是**单实例守卫**，不是线程；
  `CMemAllocator` 的信号量是 DirectShow 分配器；`Concurrency::...LoadLibraryAndCreateThread` 只有定义、无调用引用。
- **游戏逻辑（VM / 脚本 / 场景 / 存档）在主线程**；音频线程会改音频状态（临界区），
  视频线程只处理样本 / 缓冲。
- ★ **实现口径（2026-11-07 音频方案 C；用户裁决）**：guest VM 的游戏逻辑仍在**主线程**；
  音频喂料线程 `CreateThread(sub_4B5AA0)` 被 `AudioEngine` 接管成**真宿主线程**：
  - **feeder 线程**有自己的 `X86`（GPR/x87/XMM/EIP/ESP/FS/thread_id）、专用 guest 栈、
    私有 `BlockCache`/`Decoder`，直接在自己的线程里跑 `sub_4B6C30`；
  - **mixer 线程**独立按设备游标读 guest 环并推 SDL，主线程卡在 Present/长 shim 也不会断音频；
  - **令牌（`runtime/guest_token.h`）是 guest 可见共享容器的并发边界**：任一时刻最多一个宿主
    线程持有令牌；`GuestHeap` / `HandleTable` / `Vfs` / `ComRegistry` / `SurfaceArena` / guest
    内存账本只在令牌里被访问。**guest 指令执行可以并行**（主线程 / feeder 各跑各的），互斥点在
    **每次 shim 调用**（`ShimRegistry::invoke`）—— 容器访问全部发生在 shim 实现里。主线程另在
    每个**块边界**取/放。任何会阻塞的宿主等待（guest `CRITICAL_SECTION`、`GetMessageA` 的宿主
    事件泵、join feeder）先 `Yield` 再等 —— ⛔ 持令牌阻塞会与另一条执行流互等。
    ⚠ 为什么不是"整次 fill 持令牌"：feeder 一次 fill ~0.3–0.4 s CPU，与主线程 CPU 相加 ≈ 单核
    上界 ⇒ wall 不可能低于两者之和（实测 34.8–39.1 s；并行 15.1 s）。见需求单
    `REQ-01M6X64J338E6YDQXJB9RKAFSG`。
  - 同步边界还有引擎自己的**每槽 `CRITICAL_SECTION`**：guest 的 `Enter/LeaveCriticalSection`
    映射到宿主 per-EA 递归锁（`CriticalSectionTable`）。feeder 到点会等这个槽：实现是
    `tryEnter` + **放开令牌** 1 ms 重试（⛔ 不是持令牌阻塞）；主线程的 `Enter` 若撞上 feeder
    持有的槽，也先 `Yield` 令牌再等宿主锁。
  - `--audio=sdl` 且宿主时钟（窗口模式/`--real-time`）⇒ 方案 C；否则（`--audio=virtual`、
    纯 CTest）自动回退到方案 D 的主线程泵，保持确定性。
- 其余服务线程（DirectShow 读文件 / MF 回调）仍是**伪线程**：`CreateThread` 只发线程句柄，
  **不执行 start routine**；`ResumeThread` / `SetThreadPriority` / 亲和性只返回形态值。
  ★ 例外：**纹理影片**（DirectShow 模式）的渲染器服务线程等待由**宿主合成**——它等的是
  `sub_4D5710` 里 `[this+0x58]/[this+0x54]` 两个匿名 Event（10 s 重试），shim 把 #1 句柄当成已
  signaled（`WAIT_OBJECT_0+1`），默认生效、`LIFT_DSHOW_NO_SIGNAL=1` 可关（A/B 对照）。见 `SCOPE.md` §1 视频行。
- **视频（纹理影片）**：`CoCreateInstance(CLSID_FilterGraph)` 建真宿主图；帧从引擎**自己的源 pin**
  （`IAsyncReader`）分块读、宿主解 MPEG-1，`IMemInputPin::Receive` 逐帧送渲染器；`IMemAllocator` /
  `IMediaSample(2)` / `IReferenceClock` 都是宿主 COM 对象。`--log-cat video` 能看到 `[dshow]` 日志
  （接线 / 首帧 / `samples=` / `completed`）。⛔ 不做 MF/EVR/DXVA2、窗口影片族、影片音频。
- ⛔ **不是通用多线程 VM**：只有音频 feeder/mixer 两条宿主线程；游戏 VM/脚本/存档仍主线程。
  x86 TSO 没有做全局软模拟 —— 音频的并发只依赖引擎自己的每槽 CS 协议 + 环形缓冲"读当前圈 /
  写刚放完圈"的不重叠；`Interlocked` 的在飞计数在 CS 内。arm64 上这条窄路径由宿主锁的
  acquire/release 提供顺序；若将来把更多 guest 代码放线程，才需要完整 TSO 层。
  （`fs_base` 仍指向主线程 TEB；fill 内 `TlsSetValue` / `fs:[0]` SEH 的偏差见音频文档。）

### 2.6 内存模型：静态镜像 + guest 堆

- **静态数据不是"逐变量硬编码"**：把 PE 的 `.text` / `.rdata` / `.data` / `.idata` 按
  `imageBase + RVA` 映射进 guest 内存；`.c` 里的 `dword_XXXX` 只是 guest 地址，不是 C 变量。
  IAT 槽填成 guest thunk 地址（dispatcher 映射到 shim）。
  - 固定 `imageBase`（0x400000）⇒ 不重定位；若换基址，按 `.reloc` 重定位。
  - 若不能带 PE：把 section 字节抽成 blob 映射（仍是"段镜像"）；⛔ 不要逐变量 `constexpr`。
- **堆是 guest 地址空间里的分配器**，不是宿主 `malloc` 返回宿主指针：
  - `GuestMemory`：**一整块 4 GB guest 地址空间**（`mmap` / `VirtualAlloc` 预留、按需提交；
    不写不占物理内存）；`rd32/wr32` = `base + guestAddr`。
    ⛔ **不做稀疏页表**：64 位宿主上 4 GB 虚拟预留很便宜，平坦映射更快；只有 32 位宿主 /
    需要受控 fault 时才值得。
  - `GuestHeap`：在 `[heapBase, heapEnd)` 里做 free-list / size-class；返回 **guest 地址**。
  - Win32 形状：`GetProcessHeap` / `HeapCreate` / `HeapAlloc` / `HeapFree` / `HeapReAlloc` / `HeapSize` /
    `LocalAlloc` / `GlobalAlloc` / `CoTaskMemAlloc` → **同一个 guest 堆**（保证 `free` 跨 API 兼容）；
    `VirtualAlloc` = 在预留区内 commit（`mprotect` / `MEM_COMMIT`）；`VirtualFree` =
    `madvise(DONTNEED)` / decommit；`VirtualProtect` = `mprotect`（可 no-op，记偏差）。
  - CRT：lifted 的 `malloc/new` 仍走 guest CRT → 最终落到上面的 `HeapAlloc`；若替换 CRT，
    则 shim 的 `malloc/free/new/delete` 也必须用同一个堆。
- **布局（建议）**：image `0x00400000` 起；stack `0x70000000` 起（向下）；heap `0x10000000` 起；
  TEB/PEB `0x7FFD0000` 起；IAT thunk 页单独一段。**整块 4 GB 预留、按需提交**；物理只占 touched 页。
- **对齐 / 清零**：`HeapAlloc` 8 字节、`VirtualAlloc` 64 KB；`HEAP_ZERO_MEMORY` / `LMEM_ZEROINIT` /
  `VirtualAlloc` 清零；`realloc` 保内容。`fs` 段基址指向 guest TEB（SEH / TLS 用）。
- ⛔ guest 槽里不许出现宿主指针；⛔ 不把宿主 `malloc` 的返回值当 guest 地址。
- **怎么查**：fault 归因（`guest memory fault` / `host fault`，退出码 3 / 4）由本层的 VEH 决定；
  Windows 上 `LIFT_VEH_PASSTHROUGH=1` 让本层对**非 guest 地址**的异常返回 `EXCEPTION_CONTINUE_SEARCH`
  （把异常让给下一位 handler —— ASAN 的 shadow 惰性提交、调试器的第一机会异常都要靠它）。**默认关**：
  不设该环境变量时行为不变（host 诊断 + `_Exit(4)`，守卫 `tests/fault_report.cpp` 的
  `host-av-attributed-to-host`）。跑 ASAN 版二进制时必须开它，否则 ASAN 起不来 ——
  成因与该分支的边界写在 `src/runtime/memory.cpp` 的 `vectoredHandler` 注释里。

## 3. 目录（落点）

```text
apps/emulator-lift/
  CMakeLists.txt                      ← 目标 + CTest（**分级靠注册位置**，见 AGENTS.md §5）
  CMakePresets.json / vcpkg.json      ← windows-x64 / linux-x64 / macos-arm64；依赖 sdl3 + zydis + freetype + glm
  README.md / SCOPE.md / docs/        ← 口径 / 用户拍板的偏差 / 计划长文
  src/main.cpp                        ← CLI：--exe --lst --game-dir --appdata --agerc --skip-logo
                                        --accept-recovery-dialog
                                        --until shim|ea --trace-shims --trace-blocks
                                        --max-insns --max-frames --dump-frame --time-limit
                                        --safe-memory --report --bench --smoke
                                        --log-level --log-cat --verbose
                                        --profile --profile-top --native-overrides
                                        --present headless|window|auto --present-driver
                                        --present-vsync --present-hidden --present-screenshot
                                        --present-screenshot-frame --input-script
  src/runtime/log.h                   ← 日志分级 / 分类 / 模式（默认 warn；Error 永远显示）
  src/runtime/host_encoding.{h,cpp}   ← 进程入口编码契约：UTF-8 argv（GetCommandLineW）/ 控制台码页
  src/runtime/native_overrides.{h,cpp}← guest 函数级 native override（试点；默认关）
  src/runtime/present.{h,cpp}         ← 唯一记录点（hash / frameCount / BMP）+ 可选 PresentSink
  src/runtime/present_sdl.{h,cpp}     ← 真实 SDL 窗口 + GPU API 呈现（pimpl；头文件不暴露 SDL 类型）
  src/runtime/message_box.{h,cpp}     ← ★ MessageBox 宿主自绘模态框：uType→返回值表 / 布局 / 状态机 / 唯一模态循环 / CPU 合成
  src/runtime/input.{h,cpp}           ← ★ 输入应用层：SDL 真实事件与 `--input-script` 注入的**唯一**汇合点
  src/runtime/input_script.{h,cpp}    ← ★ 指令式输入时间线（at frame / at guest_ms / click / key ...）
  src/runtime/frame_blit.{h,cpp}      ← guest 后缓冲 masks → 宿主纹理格式（六种；纯函数）
  src/runtime/memory.{h,cpp}          ← 4 GiB 预留 + 按需提交 + 提交位图 + 区域表 + fault 诊断
  src/runtime/image.{h,cpp}           ← PE 节表/导入表/入口 + 映射 + 栈(1 MiB+guard) + TEB/PEB
  src/runtime/sha256.{h,cpp}          ← SHA-256（校验"执行的字节 = 语料描述的字节"）
  src/runtime/{regs.h,insn.h}         ← 寄存器标识 + 自有 Insn（Op / 操作数 / 块）
  src/runtime/decode.{h,cpp}          ← Zydis 包装 → Insn + 块解码 + 0x9B/`F3 C3` 前缀合并
  src/runtime/mnemonics.cpp           ← IDA↔规范名映射表（判据 A2 的唯一真源）+ .lst 行分类
  src/runtime/math.{h,cpp}             ← D3DX 行主序/行向量 ↔ GLM 的唯一适配点
  src/runtime/pixel_format.{h,cpp}     ← 掩码换算 / load/store / D3DCOLOR 打包的唯一实现
  src/runtime/x86.{h,cpp}             ← 寄存器/操作数/栈 helper（8/16 位合并）+ 块缓存
  src/runtime/machine.{h,cpp}         ← 装配 + dispatcher + fault hook + 最近指令环形缓冲
  src/runtime/exec.cpp                ← 语义分发 + 标志位公共部分 + 响亮失败
  src/runtime/exec_{integer,control,string,fpu}.cpp  ← 按族分文件的语义（S3 / S4）
  src/runtime/fpu_bridge.{h,cpp}      ← x87 ↔ vendor SoftFloat（extF80）
  src/runtime/{heap.cpp,handles.cpp,shims.cpp,clock.h}  ← guest 堆 / 句柄表 / shim ABI / 虚拟时钟
  src/corpus/lst_index.{h,cpp}        ← `.lst` 只读索引（EA→文本 + **下一行 EA**，A3 的边界来源）
  src/shims/kernel32_*.cpp            ← 七族（一文件一族）+ bootstrap（启动路径补的）+ register.cpp
  tests/*.cpp                         ← conformance(A1) / dispatch / shim-abi / mnemonics /
                                        loader / decode-crosscheck(A3) / corpus-mnemonics(A2) /
                                        boot-trace(A4) / base-coverage(A5) / native-override /
                                        present-sink / frame-blit / present-window
  third_party/berkeley-softfloat/     ← vendor 的 SoftFloat 3e（8086 特化）+ 清单哈希 README
```

## 4. 构建（vcpkg **跨目录复用**）

- toolchain 指向 `../emulator-cxx/vcpkg/scripts/buildsystems/vcpkg.cmake`；本目录的 `vcpkg.json`
  只声明自己需要的包（清单真源是 `vcpkg.json`：`sdl3` + `zydis` + `freetype` + `glm`）。
- ```bash
  cd apps/emulator-lift
  cmake --preset windows-x64        # 或 linux-x64 / macos-arm64（同一个相对 toolchain 路径）
  cmake --build --preset windows-x64
  ctest --test-dir out --output-on-failure
  ```
- ★ **CTest 的分级判据是"注册位置"**（`AGENTS.md` §5）：顶层无条件注册 ⇒ `pure`（不需要游戏/LFS）；
  落在 `if(...)` 里 ⇒ `assets`。默认 `ctest` 只跑前者；后者在资产在场时自动注册
  （`movie-logo` 例外，见 §4.0：它要真放影片，是 opt-in）。
  缺资产时这些用例**不注册**（`ctest` 的用例数会变少，不是"跑绿了"）；已注册但 env 缺失时测试返回 **77**
  （`SKIP_RETURN_CODE`），⛔ 不是"什么都不做然后 return 0"。
- ★ `LIFT_SAFE_MEMORY=1`：每次 guest 访存都查提交位图（慢）。判据 A1 要求**再跑一遍这一档**。
- 生成物（`out/`、`vcpkg_installed/`）由本目录 `.gitignore` 排除。

## 4.0 需要真机安装的用例：`LIFT_ASSETS` / `LIFT_STOP_AFTER_MOVIE`

`assets` 档里有一条**不跟默认门禁走**：`movie-logo`（`tests/movie_logo.cpp`）。它真启模拟器、真放
LOGO 影片（子进程按**墙钟秒**计），默认**不注册**（`add_executable` 也在同一个 `if` 里 ⇒ 纯构建环境
连这个 target 都不编）。注册条件是**两条同时成立**：安装根下存在 `SYS4INI.BIN`，且设了 `LIFT_ASSETS`。
安装根只有一处 —— 仓库根的 `links/install`（Windows / macOS 都指向真实安装树；见根 `.gitignore`），
换机器时用 `-DLIFT_ASSETS_GAME_DIR=<目录>` 覆盖（CMake 缓存变量，缺省 = `links/install`）。
判据见 `tests/movie_logo.cpp` 的文件头。

```powershell
# ① 显式注册 / 跑这条 assets 用例（macOS / Linux 把 preset 换成对应的那个）
$env:LIFT_ASSETS = "1"                # POSIX：export LIFT_ASSETS=1
cmake --preset windows-x64            # ★ DEFINED ENV{...} 是**配置期**求值 ⇒ 必须重新 configure
cmake --build --preset windows-x64
ctest --test-dir out -N               # 这时才会列出 movie-logo
ctest --test-dir out -R movie-logo --output-on-failure
Remove-Item Env:LIFT_ASSETS           # 想回到默认门禁：取消变量后再 configure 一次

# ② 手工跑（不经 CTest）时让"影片放完即停" —— CTest 已自动注入这一条
$env:LIFT_STOP_AFTER_MOVIE = "1"
```

- `LIFT_STOP_AFTER_MOVIE=1`：DirectShow 图的停机链看到"影片已放完"（`completed_`）就结束本次运行，
  不再空转到 `--time-limit`；`--time-limit` 仍保留作**兜底**。**缺省关** ⇒ 行为与不设它时一致。
  它是**模拟器级**开关（不是测试专属）：手工玩时 export 了它，会在第一个影片放完后退出。
- `movie-logo` 在 `ctest` 里**总是**带 `LIFT_STOP_AFTER_MOVIE=1`（`set_tests_properties(... ENVIRONMENT ...)`，
  分号分隔的列表），并由 `add_test` 传 `--expect-early-stop`。要验证"关掉时与旧行为逐字一致"，直接跑
  `out/tests/lift-movie-logo.exe`（参数见 `CMakeLists.txt` 的 `add_test`）而不设该变量、也不传那个开关。
- 测试据此断言钩子生效（两个子进程的 run summary 都得是 `stop      : stopped`）—— 钩子失效时它不会
  静默退回"变慢但仍绿"。判据挂在 CMake 传的那个参数上，**不挂在环境变量**上（env 被删掉时也要红）。
- 另一条 assets 用例是 `option-roundtrip`（`tests/option_roundtrip.cpp`）：注册条件与其他 opt-in
  资产用例相同（安装根 + `LIFT_ASSETS`）。它自己起两次模拟器（1 / 2 轮往返；`kCycles = 2`），判据与容差写在该
  文件头注释里。

## 4.1 渲染产物导出
D3D9 `Present` 可以把**后缓冲的 guest 像素**写成 BMP（`SurfaceArena::dumpBmp`，支持 16/32bpp）：

```bash
# 第一帧
./out/emulator-lift --game-dir <install> --appdata <tmp> \
  --max-frames 1 --dump-frame ../../.tmp/frame-{frame}.bmp

# 覆盖同一文件，拿第 N 帧（重跑时每次覆盖；最后留下的是第 N 帧）
./out/emulator-lift ... --max-frames 100 --dump-frame ../../.tmp/frame-last.bmp
```

- `{frame}` 会替换成 **4 位帧号**；不含占位符时同一路径被覆盖。
- `--max-frames N` 在 D3D9 `Present` 计数到 N 后主动停（日志里有 `已到 --max-frames`）。
- 这是**宿主诊断输出**，不参与 guest 语义；DDraw 的 `Flip` 目前仍走同一 presenter，但本开关只在
  D3D9 的 `Present` 上消费。
- BMP 只是当前实现的零依赖格式；⛔ 不声称它等于真机像素。

## 4.1b 跳过 LOGO（`--skip-logo`）

`--skip-logo` **不跳过 boot**，也不改 root script：它在 `T_Command` 构造完成、指针发布之前
（EA `0x004BAA37`，`EAX` = 新对象）把引擎自己的 show-logo 字段 `T_Command + 0x5EB5C` 写 0。
root script 的 `0x130` 因而读到 0、不执行 `call-script 5262`（LOGO），其余 boot 照旧到 TITLE。

- 字段初值由构造器 `sub_415640` 在 `0x00415AE0` 写成 1；复位例程 `sub_428A60` 在 `0x00428A75` 清 0
  （本线不依赖它）。
- 只写这一格；⛔ 不按帧号猜 LOGO、⛔ 不直接跳到 TITLE。
- `--skip-logo` 会进 trace 输入清单（同一个 exe + 不同开关 = 另一条执行路径）。
- **真实 boot 观测**（未自动化，需人看）：带 `--skip-logo` 的首帧 draw 签名是 TITLE
  （`sep_alpha=1 tex=1`）；不带是 LOGO（`sep_alpha=0 tex=0`）。
- CTest `skip-logo` 只守**字段写 / 邻居字节 / 关闭时不写 / 空对象不崩**；它不跑 boot、
  不看 draw 签名。⛔ 不要把“CTest 绿”当成“TITLE 首帧已验证”。

## 4.2 可切换呈现后端（`--present`）

**缺省 `--present headless`**：与今天逐字节一致（hash / frameCount / `--dump-frame` / `--max-frames`；
不初始化 SDL 视频）。**`--present window`** 打开真实 SDL 窗口，呈现路径按**实测**二选一
（见 §4.2b 的"直出"）：

```bash
# 看得见的窗口（真 SDL 窗口；自动 real-time）
./out/emulator-lift ... --max-frames 300 --present window
# 无人值守 / CTest：隐藏窗口 + 宿主回读截图
./out/emulator-lift ... --max-frames 1 --present window --present-hidden \
  --present-screenshot ../../.tmp/window-{frame}.bmp
```

- 驱动列表 `--present-driver`（逗号分隔，缺省 `gpu,direct3d12,direct3d11,vulkan,metal,opengl`）：
  只在 **SDL_Renderer 路径**上生效；GPU 直出成立时没有 renderer，这个开关不参与（日志会打
  `present=direct/<mode>`）。`gpu` 命中时日志 `present: driver=gpu gpu=direct3d12`，且
  `SDL_GetGPURendererDevice()!=NULL` —— 这是"确实走 SDL_GPU"的正证据。
  其它驱动是 GPU API 路径，但可能是 WARP/GDI，日志如实打。缺省列表**不含 software**；
  显式 `--present-driver software` 只用于测试，会打"不是 GPU 路径"警告。
- 像素格式按 **masks** 选（六种 D3D9 后缓冲格式）；SDL_Renderer 路径的纹理 blend=NONE
  （`A8R8G8B8` / DDraw 的 32bpp 带真实 alpha，alpha=0 的帧在默认混合下会整窗黑；
  `X8R8G8B8` 没有 alpha，不受影响）；逻辑呈现尺寸按当前帧，resize 用 letterbox。
  GPU 直出路径不做混合（swapchain 不透明），letterbox 是"清黑 + 按整数像素缩放置中"（NEAREST，
  与 renderer 路径的 `SDL_SetTextureScaleMode(NEAREST)` 同口径）。
- `--present-screenshot` 是**窗口回读**（present 前）：renderer 路径走 `SDL_RenderReadPixels`；
  直出路径走"target → 下载缓冲 → 同一份 letterbox"（swapchain 是 write-only，SDLR 明确不许读回）。
  它与 `--dump-frame` 的 **guest 后缓冲** 是两份独立产物：1:1 时可逐像素对照（实测 0 diff），
  缩放/letterbox 下只比内容矩形。
- 显式 `--present window` 打不开 ⇒ **exit 2**；运行期失败 ⇒ 记录 + 3 次后回退 headless（guest 的
  Present 结果不受影响）。`--present auto` 仅调试用：失败回 headless。
- DDraw **不在本次验收内**（用户口径：AGE 默认不走 DDraw）；`Flip` 复用同一 sink 只是副作用，
  `--dump-frame` 仍只在 D3D9 Present 消费。直出路径下 DDraw Flip 会先把 guest 上传到 target 再
  blit（见 §4.2b），⛔ 不会把上一帧当这一帧。
- 窗口模式把 SDL 事件接到 guest 消息队列：键盘 / 鼠标移动与按键 / 滚轮 → `WM_KEY*` / `WM_MOUSE*`；
  鼠标移动 / 按键 / 滚轮同时更新 guest 的**虚拟光标位置**（屏幕坐标 = 客户区 + 窗口原点）——
  `GetCursorPos` / `0x109` 读到的是当前位置，不是常量；⛔ 不建 OS 光标。
  调试：`LIFT_TRACE_INPUT=1` 打印每个 SDL 鼠标 motion 的窗口坐标与换算后的 guest 坐标。
  关窗（或 ESC）先发 `WM_CLOSE`，宽限 3 帧 / 500ms 后 `StopReason::Stopped`（`detail="present: 窗口关闭"`）。
  headless 没有事件源时，空队列 `GetMessageA` 仍按旧语义 abort。看画面用 `--real-time` / `--max-frames`；
  `--bench + window` 的 MIPS 含 SDL 上传/呈现开销，不是解释器绝对吞吐。
- `--present-screenshot-frame N`：只回读第 N 个 Present（0/缺省 = 每帧都写）。配合 `--max-frames N`
  就是"定帧截图"（只产出一张，不写 N 份 BMP）。见 §4.2c 的一行命令。

## 4.2c 脚本化输入（`--input-script`）

**为什么有它**：要"点一下 OPTION"曾经必须 `SetForegroundWindow` → 把真实光标挪进窗口 →
`SendInput` → 等几百毫秒。那套动作的前提全是**OS 侧条件**：SDL 只把键盘事件给**有焦点**的窗口、
鼠标事件给**光标下面**的窗口，而且 `WINDOW_FOCUS_LOST` 会把键电平清掉。下面这条通道把这些前提全去掉：
**注入不需要焦点、不移动真实光标、可一条命令复现**。

**机制（判据：注入路径不依赖任何只有 OS 能提供的条件）**

- `src/runtime/input.cpp` 是 SDL 真实事件与注入的**唯一汇合点**：两者都只改 `WindowManager` 的同一份
  状态（影子键电平 / 鼠标按钮位图 / 虚拟光标）并 post **同一组** `WM_*`。guest 的全部读点
  （`GetAsyncKeyState`、`GetCursorPos`、`PeekMessageA`/`GetMessageA` → `DispatchMessageA` → WndProc）
  因此读的是同一份状态；⛔ 不存在"注入只发消息、`GetAsyncKeyState` 仍读 OS"的第二份状态。
- 注入**不经过 SDL**、不读 OS 键鼠、不需要窗口焦点；`--present headless`（不初始化 SDL 视频）也能注入。
- ⚠ **模态对话框期间时间线只剩 `at wall_ms`**：`MessageBoxA` 的宿主自绘模态框会阻塞 guest
  （`runModalDialog` 每轮只推进 `at wall_ms`），`frame` / `insn` / `guest_ms` 全部冻住 ⇒ 给模态
  写的注入脚本必须用 `at wall_ms`（该 run 因此不可 diff）；key 要保持按住，后面得再有一条
  远期动作（否则脚本"结束即抬起"，模态可能看不到按下）。★ Enter / Esc 是**电平**判据：若该键
  在框打开**之前**就已按下，框打开的第一帧就会回答（既定口径，不是缺陷）。
- ⚠ **模态框的呈现有两条路径，直出有已知限制**：renderer 路径按逻辑坐标（LETTERBOX）自动缩放；
  **GPU 直出把模态图 1:1 贴到画面原点** —— 窗口被 resize / 出现黑边后对话框**不缩放、可能被裁**。
  窗口默认尺寸 == 后台缓冲时两者等价（正常路径）；这是有意的第一版限制（设计 §2.3），不是缺陷。
  `--present-screenshot` 回读的是**窗口像素**：模态帧也走同一条回读（文件名里的 `{frame}` 仍是
  最后一只 **guest** 帧的号 —— 模态帧不自增帧号）。
- 坐标是 **guest 客户区坐标**（引擎的 1280×720 虚拟屏）。`client → screen` 只经过
  `WindowManager::setCursorClientPos`（+ 窗口原点），与 SDL 路径**同一处**；⛔ 注入侧没有第二套映射。
- 时间线在**块边界**推进（`Machine::run`）；guest 阻塞在 `GetMessageA` 时由**窗口模式的宿主事件泵**
  推进（`--present headless` 没有事件源：guest 真阻塞在 `GetMessageA` 时仍按既有语义 abort）。
  `quit` 会同时投一条 `WM_QUIT` 把阻塞中的 guest 唤醒。
- **优先级（显式，互不污染）**：脚本按下的键 / 按钮归脚本，期间 OS 的按下 / 抬起 / **焦点丢失清键**
  都不生效（注入不因失焦而丢）；脚本 `key_up` / `mouse_up` 即交还 OS；反过来，脚本接管光标期间
  **真人松开按钮仍被接受**（否则 `VK_LBUTTON` 电平会卡在按下）。脚本结束（动作跑完 / `quit`）时，
  仍按着的键 / 按钮会**按同一组 WM_* 抬起**（WndProc 收得到 `WM_KEYUP` / `WM_LBUTTONUP`）再把所有权
  交还 —— 真人接着玩不会撞上"卡住的键"。
- ⚠ `--present window` 走 real-time 时钟 ⇒ **毫秒**触发不可复现，用**帧**；`--present headless`
  两档都确定性。headless 的帧是纯 CPU 光栅（比 GPU 路径慢一个量级以上）⇒ 要跑长脚本 / 看画面用
  `--present window --present-hidden --raster gpu`（仍然无焦点、无光标，仍可脚本化）。

**语法**（一行一条；`#` 起注释；换行或 `;` 分隔）

| 写法 | 含义 |
|---|---|
| `at frame N` | 触发条件 = 第 N 个 Present（1 起；headless 也计数） |
| `at guest_ms T` | 触发条件 = guest 虚拟时钟 T 毫秒 |
| `at insn N` | 触发条件 = guest 累计执行指令数 N（**帧内也能切**；确定性） |
| `at wall_ms T` | 触发条件 = 宿主墙钟 T 毫秒（自 run 起） |
| `at marker <名字>` | 触发条件 = 引擎**进入**某个脚本（脚本身份 `(cur, id)` 的**边沿**；采样点 = VM 循环头）。载荷 = 名字：读不到就降级成 `id:0xXXXXXXXX` **仍触发**并打一条警告；同一 `(cur,id)` 重复出现不重复发，首个样本只播种。等不到 ⇒ `InteractionFailed` + `marker-timeout:` detail + exit 5（超时 = `--input-marker-timeout-ms`，缺省 30000 **宿主墙钟** ms；0 = 不设限） |
| `after N frames` / `after N ms` / `after N insn` | 触发条件 = 上一个**同单位**锚点 + N |
| `wait N frames` / `wait N ms` / `wait N insn` | 推进锚点，并记一个 **no-op 动作**（脚本要等它到期才收口 ⇒ 尾部 `wait` 能"保持按住 / 保持等待"） |
| `... stall MS` | （只对 frame）帧停滞兜底阈值；见下 |
| `mouse_move X Y` | 客户区坐标；脚本由此**拥有光标**（`GetCursorPos` 立即是它） |
| `mouse_down [left\|right\|middle]` / `mouse_up [...]` | 用脚本上一次的坐标；`mouse_up` 若按下**还没被 guest 采样**过 ⇒ 自动推迟（见下） |
| `click X Y [button] [hold N [frames\|ms]]` | move + down 在触发点；up **自动**等到被采样（不带 `hold`），显式 `hold` 则按帧 / ms（见下） |
| `key_down VK` / `key_up VK` | Win32 虚拟键码（1..255，十进制或 `0x`）；`key_up` 与 `mouse_up` 走同一条"至少被采样一次"保证 |
| `profile_begin <label>` / `profile_end <label>` | 打开 / 关闭一个**区间 profiler** 窗口（§4.3.2）；边界 = 本动作的触发条件 |
| `quit` | 触发时结束本次运行（`StopReason::Stopped`） |

动作前必须先给时间（`at` / `after` / `wait`），否则解析失败（带行号，exit 2）。

**触发条件是一个"条件"，不是帧号。** 内部只有 `Trigger{kind, value, name}` 五种 kind（frame / guest_ms /
insn / wall_ms / marker），调度只问"到期了吗"，不关心是哪一种（`marker` 不按数字到期：只有边沿会应用它）。
★ **这条时间线现在只是过渡形态**：
要换的交互模型（任务 = 节点图，节点 = `when` → `action` → `next`；三种失败全部响亮；两种识别源
为什么都要）以 `docs/03-emulator/interaction-model.md` 为**唯一真源**。本文件**不复述**它的 schema，
也不写"未来的 marker / 状态机"长什么样；`script` 事件源与时间线怎么并入见那份文档的"分期"。

**`click` 会自动等到被采样（⛔ 不要手算 hold）**：一次 `click` 仍然是"按下 + 抬起"（不重试、不变两次、
不改 guest 采样率），但**抬起的最早时刻由注入层保证**，不是由写脚本的人猜：

- **判据**：按下之后 guest 的输入**采样计数**至少 +1（`WindowManager::inputSampleCount()`），
  并且从"看到按下"的那次采样起再等**一个采样周期**（= 最近一次采样间隔，夹在 1..200 ms）。
  第二条是安全下限：引擎可能先取位置、下一句才读按钮电平（避开采样边界的竞态）。
- **采样点只算电平读点**：`GetCursorPos` 与 `GetAsyncKeyState`（`src/shims/user32_input.cpp` /
  `src/shims/user32_window.cpp`）。⛔ **不算 `PeekMessageA`** —— 它在这条实现线里是每 ~1.8k 条指令一次
  （实测 profile，比 20 Hz 的采样点快三个数量级），算进去等于没等；而且消息在队列里不会丢。
  `GetKeyState` 本线没有实现。
- **上限**：`--input-max-hold <ms>`（guest 时间，缺省 **1000**；`0` = 不设限 = 显式选择"宁可挂死"）。
  guest 一直不采样（画面静止 / 在别的循环里）⇒ 到上限**仍然抬起 + stderr 告警**（⛔ 不静默、⛔ 不挂死），
  脚本继续往下跑。
- **显式覆盖**：`click X Y [button] hold N`（帧，旧写法）/ `hold N frames` / `hold N ms`（guest 时间）
  ⇒ **完全按用户说的做**，不看采样。手写的 `mouse_down` … `mouse_up` / `key_down` … `key_up`
  走同一条保证：对应的按下还没被采样过 ⇒ `mouse_up` / `key_up` 自动推迟（`deferred-ups`）。
- **可观测**：每次抬起打
  `click-release … auto=1 waited_samples=N hold_ms=M post_sample_ms=K`（`auto=0` = 显式 hold），
  run summary 的 `inputstat` 行下面再给一行
  `release : total … auto … explicit … deferred-ups … bounded-no-sample … waited-samples-max … hold-ms-max …`。
  ⇒ 判断"是不是又没点中"先看这两处，别先猜。
- **守卫**（顶层 pure CTest，`tests/input_injection.cpp`）：`input-injection/click-spans-sample`
  （伪造一个 50 ms 周期的消费者：down 与 up 之间必须跨过采样点，且 guest 侧 `kDownSeenPolls ≥ 1`）·
  `input-injection/click-honors-explicit-hold`（显式 `hold 2` 在 +2 帧落地，guest 一次没采样也照做）·
  `input-injection/click-bounded-without-sampling`（完全不采样 ⇒ 到 1000 ms 抬起 + `bounded-no-sample`
  + 脚本继续）。★ 变异实测会红：把自动判据改回"同一 tick 内 up" ⇒ 上面三条全红，
  `kDownSeenPolls = 0`（正是用户看到的现象）。

**帧停滞兜底（为什么必须有）**：`at frame` / `wait N frames` 的前提是"帧在推进"，而实测
**EU-ROOM 界面稳定后不再 Present**（帧号停住）—— 纯帧锚点会永远不触发。规则：

- 有未到期的 frame 条件、且帧号已 `stall` ms 没变 ⇒ 打一条**告警**，把剩下所有 frame 条件按
  "最近帧间隔均值（夹在 1..100 ms）"折算成 `wall_ms` 条件后**继续**（⛔ 不挂死）。
- `stall` 可逐条给（`wait 5 frames stall 8000`），缺省取 `--input-stall-ms`（缺省 10000，`0` = 关闭）。
- **还没出现第一帧（boot）不算停滞**：那一段本来就没有 Present，宽限取 `--input-stall-boot-ms`
  （缺省 30000）。⛔ 否则长 boot 会被误判成"卡住"并把整条时间线折算乱。
- 兜底只在"帧真的停住"时生效：帧正常推进时它一次也不触发（守卫 `tests/profile_window.cpp`）。

**一条命令**（实测：`--skip-logo` 到 TITLE → 点 Option → 停在第 155 帧 → 截图）

```bash
cd apps/emulator-lift
# .tmp/input/click-option.txt：
#   at frame 120
#   mouse_move 812 622
#   wait 5 frames
#   click 812 622
#   wait 30 frames
#   quit
./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install \
  --present window --present-hidden --raster gpu --native-overrides all --skip-logo \
  --input-script ../../.tmp/input/click-option.txt --max-frames 200 \
  --present-screenshot ../../.tmp/option-window.bmp --present-screenshot-frame 155
```

产物 `option-window.bmp` 就是 OPTION（系统设定）界面。`--max-frames 200` / `quit` 二者都只是兜底；
`--present-screenshot-frame` 让截图只写一张。把 `--present window --present-hidden --raster gpu` 换成
`--present headless` 就得到**不建任何窗口**的同一条时间线（CPU 光栅，慢，但确定性）。

**退出 EU-ROOM 的一行命令**（回归"点一次就能回 TITLE"；不需要任何 hold）：

```bash
cd apps/emulator-lift
# .tmp/clickfix/exit-click.txt：
#   at frame 90
#   mouse_move 944 562
#   wait 3 frames
#   click 944 562
#   at insn 300000000
#   mouse_move 1227 680
#   at insn 300020000
#   click 1227 680
#   at insn 600000000
#   quit
LIFT_FRAME_TRACE=1 ./out/emulator-lift.exe --appdata ../../.tmp/appdata --game-dir ../../links/install \
  --present window --present-hidden --raster gpu --native-overrides all --skip-logo \
  --input-script ../../.tmp/clickfix/exit-click.txt --time-limit 200
```

判据（三步都看 `[frame]` 行）：点击帧出现 `click-release … auto=1 waited_samples>0 hold_ms≈一个采样周期`
⇒ 其后有一帧是**单帧 ~1.5×10^8 条指令 / 8~9 s**（TITLE 重载）⇒ 再往后回到 ~2.8×10^5 条/帧（TITLE 动画）。
这就是"一次 click 就回 TITLE"的机械判据（不需要肉眼认同一个画面）。
运行日志与逐帧 BMP 是**本地证据**（`.tmp/` 不入库）：判据本身在上面这两行日志里。

**效果级判据（`--expect-change-after` / `--expect-change-window`，缺省关）**

时间线原本只管"动作发出去"，**不管 guest 有没有看见**（BUG-16 的房间「戻る」就是这么静默过的：点击无反应、
退出码 0）。这两个 flag 把"动作之后必须观测到变化"变成运行期判据 —— **旧脚本一个字都不用改**：

```bash
./out/emulator-lift ... --input-script <脚本> \
  --expect-change-after click[,key,mouse] --expect-change-window <采样数>
```

| 参数 | 语义 |
|---|---|
| `--expect-change-after click[,key,mouse]` | 只对这几类动作开观测窗。`click` = `click`；`key` = `key_down`/`key_up`；`mouse` = `mouse_down`/`mouse_up`。`mouse_move` **不**要求变化（hover 可能本来就不改像素） |
| `--expect-change-window <采样数>` | 观测窗长度，**单位 = 采样数**（一次 Present = 一个样本；缺省 30）。⛔ 不是毫秒：`--present window` 是 real-time 时钟，毫秒窗口不可复现；采样数才是"guest 又画了几帧"的确定性计数 |

- **判据**：动作被应用之后，运行期读 `FrameSampler::changeCount()`；窗口内增长过 ⇒ 满足（日志
  `[expect-change] ok "…"`）。三种失败**一律响亮**（stderr `[expect-change] FAIL …` + 退出码 **5**，
  与交互图的三种失败同一个停因 `InteractionFailed`，`detail` 里写明是哪种）：
  - `not-changed`：采样在增长、窗口也走满了，但画面**一次都没变**（动作没生效？）；
  - `no-sample`：动作之后**一次采样都没有**（识别源失明：没接采样器 / guest 一直没 Present）
    —— 与"没变"分开报（修法完全不同）；
  - `unclosed`：运行结束时窗口**还没判定**（⛔ "跑完就算过"正是要消灭的）。⇒ 开着这个策略时，
    给观测窗留时间（`wait` / 更长的窗口 / 别在 click 后立刻 `quit`）。
- **与 `--interaction-graph` 互斥**（exit 2）：图有自己的 `expect` 语义，两套判据同时开会出两套口径。
- **启动期拒绝**（exit 2，⛔ 不静默开局）：没给 `--input-script`；脚本里**没有匹配的动作**
  （否则一个观测窗都开不了 = 静默绿）；只给 `--expect-change-window` 而不给 `--expect-change-after`；
  类别 / 数字非法（窗口只收十进制正整数）。
- **其它消费者闸门**（`--gpu-strict` / `--assert-guest-token`）仍**先判**：它们的退出码优先于这里的 5
  （既有优先级，未因本策略改变）。
- **与自动 hold 的关系**：`click` 的抬起由自动 hold 决定（等 guest 的输入采样点，上限 `--input-max-hold`）。
  窗口在**按下**那一刻开，但在这次动作的抬起发出之前**不判失败** —— 否则一次合法点击会在 up 之前被窗口判死。
  ⇒ 窗口要**大于一次自动 hold 花掉的采样数**（缺省 30 个采样在 60 fps 下 ≈0.5 s，通常够；太小会误报）。
  运行结束时抬起还没发出的窗口归 `unclosed`（⛔ 不归成 `not-changed`：那是"没给动作留时间"，不是"动作没生效"）。
- **它只回答"动作之后 guest 有没有画出新帧"**，⛔ 不回答"是这次动作造成的"：TITLE 动画 / 淡入会掩盖漏点，
  一次变化也会同时满足重叠的窗口。要**因果**就用 `screen rect … avg`（交互图）或 BMP diff
  （`pnpm tools lift diff`，见 §4.1）。
- 失败时还会补写**最后呈现的那一帧**：`<脚本路径>.expect-fail.bmp`
  （像素仍等于最后一次 Present 的 hash 与否会如实打印；从未 Present 过则明说写不出来）。
- 守卫（顶层 pure CTest `input-expect`，`tests/input_expect.cpp`）：变 / 不变 / 一次采样都没有 / 策略关 /
  跑完未判定 / 解析拒绝 / 配置后换脚本导致零窗 七例；另断言"策略开时采样器真的挂上了、关着时没挂"
  （没接采样器会让 `sampleCount` 恒 0 ⇒ 全判 `no-sample`）。


## 4.2d 识别驱动的交互任务（`--interaction-graph`）

**要换掉的是什么**：`--input-script` 是**定时器时间线** —— `at frame N` 到点就发动作，发完不管有没有生效。
它已经骗过人：点击被静默漏掉时下游照样跑，于是基于假数据做了两轮优化。这里换的是**交互模型**：
识别 → 操作 → 再识别；失败**响亮**。★ schema / 三种失败 / 判定口径的**唯一真源**是
`docs/03-emulator/interaction-model.md`（本节不复述，两份 schema 必漂）。

```bash
./out/emulator-lift --game-dir <安装> --skip-logo --audio off \
  --interaction-graph scripts/interaction/room-enter.graph \
  --interaction-trace /tmp/room.trace \
  [--interaction-fail-dump /tmp/room-fail.bmp] [--time-limit 900]
```

| 退出码 | 含义 | 判据 |
|---|---|---|
| `0` | 任务**走完**（或用户任务里写了 `quit`） | trace 末尾有 `task done nodes=… transitions=…` |
| `5` | 三种失败之一（`timeout` / `no-sample` / `expect_change`），**或者任务没走完**（guest 侧硬停：`unimplemented-slot` / `resource-exhausted` / `ExitProcess` 等）；**时间线的效果级判据**失败（`--expect-change-after` 的 `not-changed` / `no-sample` / `unclosed`）也用 5；**`at marker <名字>` 等不到**（`marker-timeout:`）也是 5 | 三种失败 ⇒ stderr 有 `failureReport()`（节点 / 等的是什么 / 等了多久 / 每个受监视矩形的最后采样值）；"没走完" ⇒ 打 `stop=` / `detail=` 与最后状态；时间线 ⇒ stderr 前缀是 `[expect-change]`（不是 `[interaction]`）；marker 超时 ⇒ 前缀是 `[marker]`、detail 形如 `marker-timeout: waited=<名字> last=<名字或 id> waited_ms=<n>`（⛔ 不写 BMP：等的是**事件**，不是画面）。前三种都补写最后画面 BMP —— **能写就写**：从未 Present 过时会明说"写不出来"（那时没有画面可存） |
| `2` | 参数 / 解析失败（带 `文件:行`），或运行期**非 ok 的停因**（`unimplemented-slot` = 已注册但槽/形态没实现、`resource-exhausted` = 我方 arena / guest 堆不够、`unimplemented` / `decode-error` / `no-code` …） | 启动阶段就拒绝，⛔ 不跑一半；运行期停因看摘要 `stop` / `detail` 行 |

- ★ 退出码的**兜底口径**只有一条：`lift::exitCodeForRun(r)`（`RunResult::ok()` ⇒ 0，否则 2）。
  `--gpu-strict` 在它之前也判 2（并可能给 3），交互图的"任务没走完 / 三种失败"仍是 5 —— 那些**优先**。
  两个新停因 `resource-exhausted` / `unimplemented-slot` **不在 `ok()` 里** ⇒ 没有别的闸门时退出码是 2。

- **与 `--input-script` 并存**（真源 §7 第 3 期才把时间线并入节点图）。两条通道都只写
  `WindowManager` 的同一份状态、都走 `runtime/input.cpp` 那一层 ⇒ 真人 / 时间线 / 节点图三者同一语义。
- **⛔ 不写帧号**：房间装载要 ~5×10^8 条指令，而房间空闲时 Present 会稀疏到 ~0.5 s 一帧 ⇒
  帧锚点在这种路径上会**永远等不到**（这是时间线在 EU-ROOM 上点不到「戻る」的实证原因）。
- **画面采样点 = Present**（`HeadlessPresenter::present`，唯一呈现汇点）：`screen hash` 用已有的整帧 hash，
  `screen rect … avg` / `screen changed` 由 `runtime/screen_sample.h` 在同一个点取样。
  ⇒ 既没加载图、也没开 `--expect-change-after` 时一次也不采样（零成本；时间线的效果级判据
  会在策略开时把同一个采样器挂上，见 §4.2c）。
- **trace 可以逐行 diff**：同一路径两次运行逐字节相同（时间戳除外；`origin=` 只写文件名，
  ⛔ 不写宿主路径）；`--present window` 是 real-time ⇒ trace 头部（第 2 行）标 `# NON-DIFFABLE`，
  那一档的 trace 不参与 diff。
- ⚠ **两条通道并存时不要同时"按着"输入**：脚本的键 / 按钮所有权是**全局一份**（`WindowManager` 按 vk 记），
  任一份脚本收口都会把它按下的键抬起来。现在图的动作是"追加进它自己的 actuator"，所以两条脚本
  **同时各按一个键**不安全 —— 要么只用一条，要么把两条的动作时间错开。
- **线性写法**：人写用的短形态，解析期展开成同一份节点；schema 与判据的唯一真源是
  `docs/03-emulator/interaction-model.md`（§1 / §1.1），本文件**不复述**。示例：

  ```
  await script TITLE
  click 1174 377
  await script GAMESTART
  click 715 576
  await script SN0000
  click 100 100
  ```

  不知道 `script` 该写哪个名字时，用诊断 `LIFT_LOG_MARKERS`（见 §4.3.1 的诊断环境变量列表）先看身份。
- 现成的路径脚本在 `scripts/interaction/`（进 / 出 EU-ROOM；出房间的左键 / 右键变体也在那里）。
  `click` 可带按钮词 `left` / `right`（省略 = `left`）；需求单的判据与进度见需求树
  （`pnpm tools requirements show REQ-01M6XVYY5TCKSAS80WS7PADF8P`）。


## 4.2b GPU 光栅化（`--raster auto|cpu|gpu`）

缺省 `--raster auto`：**`--present headless` ⇒ CPU**（保持 headless 逐字节确定性 / CTest 口径），
**`--present window` ⇒ GPU**。显式 `--raster cpu` 是参考/测试后端；显式 `--raster gpu` 仍可用
（headless 时建一个隐藏的离屏 raster 窗口）。`--present window` 下 GPU 后端初始化失败 ⇒ **exit 2**
（⛔ 不静默退回 CPU）。`--present auto` 窗口打不开会回退 headless，此时 GPU 光栅化也随之不可用，
日志会明说"本轮是 CPU 光栅化"。

后端 = 独立 `SDL_GPUDevice`（Windows DXBC / macOS MSL），把 D3D9 的屏幕空间三角形与 D3DXSprite
纹理四边形画到 1280×720 BGRA8 + D32 离屏 target；T&L / 裁剪 / 状态机 / 失败码仍全在 CPU。

### 直出（`--present window` + `--raster gpu`）：target → swapchain，不走 CPU 上传

窗口创建**之后**把窗口 claim 给同一台 device（`SDL_ClaimWindowForGPUDevice`），每帧
`SDL_WaitAndAcquireGPUSwapchainTexture` + `SDL_BlitGPUTexture`（source = 同一份 target）+
`SDL_SubmitGPUCommandBuffer`。要点与边界：

- **同一个窗口不能既 claim 给 GPU device 又 `SDL_CreateRenderer`** ⇒ 直出成立时**不建 SDL_Renderer**；
  事件泵 / 截图 / capture / 关窗语义都在 `SdlWindow` 上，不依赖 renderer。CPU 光栅化
  （`--raster cpu`）与直出不可用时仍走 renderer 路径，行为不变。
- claim 失败（dummy 视频 / 没有 HWND / 驱动不支持）⇒ **打印 SDL 的实测错误并回退 SDL_Renderer
  上传路径**，⛔ 不假装直出。判据是 `describe()` 里的 `present=direct/<mode>`。
- `--present-vsync 1` ⇒ `SDL_GPU_PRESENTMODE_VSYNC`；`0` ⇒ 优先 `IMMEDIATE`，不支持退 `MAILBOX`
  再退 `VSYNC`（每次回退都打印实际生效的模式）。窗口隐藏 / 最小化导致的"没有 swapchain 纹理"帧
  只计跳过，⛔ 不当失败。
- **本帧像素在哪儿是要显式传的**：`d3d9::Present` 读过回 ⇒ target 就是最新内容（直接 blit）；
  没读过回（本帧没有 GPU 绘制 / 不是 D3D9 Present，例如 DDraw `Flip`）⇒ sink 在 blit 之前先把
  guest 后缓冲上传到 target（16bpp / 非 BGRA8 的 32bpp 按同一份掩码语义转成 BGRA8）。
  ⛔ 不靠"target 里大概是对的"来猜 —— 猜错的表现就是窗口停在上一帧。
- 直出只换**呈现**：`d3d9::Present` 的 guest 回读（hash / frameCount / `--dump-frame` / 后缓冲语义）
  照旧每帧做，`--present-screenshot` 从 target 回读（swapchain 是 write-only）。

### 能力范围（准入在 `d3d9::gpuEligible`；守卫 `tests/gpu_batch.cpp` 的 `gpu-batch/sampler-filter-eligibility`）

★ 准入的其余条件**没有**逐条纯档用例（原 `tests/gpu_mapping.cpp` 已随 `5e4fca3 refactor: clean tests` 删除）；
只有"采样器 mag≠min ⇒ 不接受"这一条被端到端钉住（回退 + 记账）。

明确支持（TITLE 的黑场渐变全屏四边形就在这一类）：
- 32bpp 默认后缓冲、GOURAUD、无 alpha test；color write mask 任意 4 位组合；
- 任意可映射的 blend，**含 separate alpha**：color/alpha 两段各自的 factor/op 映射到
  `SDL_GPUColorTargetBlendState`；不能表达（如 `D3DBLEND_BOTHSRCALPHA`）⇒ 回退并记账；
- stage 0 **单级** 32bpp 纹理；color/alpha op 限 `SELECTARG1` / `SELECTARG2` / `MODULATE` /
  `DISABLE`（`DISABLE` 按 CPU 的语义取 **arg1**；两个 op 都 `DISABLE` 时 CPU 直接 break ⇒ 顶点色），
  操作数限 `TEXTURE` / `DIFFUSE`（stage 0 的 `CURRENT` 等价于 `DIFFUSE`）/ `TFACTOR`。
  `TFACTOR` 是每批 Draw 的常量色，GPU 路径把它整批写进顶点色（⛔ 不逐像素读状态）；
- sampler：POINT/LINEAR × WRAP/CLAMP/MIRROR；
- 纹理坐标取 stage 0 的 `D3DTSS_TEXCOORDINDEX`（⛔ 不硬编码 TEXCOORD0）；
- 顶点已由 CPU 做完 T&L / 裁剪 / 屏幕映射，且 `invw==1`（屏幕空间线性插值与 CPU 的透视校正等价）。

仍回退 CPU（每次记账；首次出现打印 `[gpu] fallback ...`）：多 stage / 纹理非 32bpp / alpha test /
FLAT / 16bpp / BORDER 或 MIRROR_ONCE / `BOTH*` blend / 渲染目标是纹理 / `invw!=1` /
D3DXSprite 源矩形越界 / GPU 后端绘制失败。D3DXSprite 的纹理上传按
`(纹理 serial, 尺寸, alpha 语义, UnlockRect 版本)` 缓存：像素没变就不重传
（⛔ 不再每次 draw 完整 upload）。键用 `createTextureState` 分配的**进程级单调 serial**，
⛔ 不用 guest 像素指针 / 状态块 EA —— 二者都会被 arena / guest 堆复用，会把新纹理读成旧像素。
空批 / 全被裁掉的批（CPU 一个像素都没写）**不计 fallback**（`draw_calls` 照记）。

深度口径：GPU 的 D32 target 从 1.0 起，**不镜像 guest 深度缓冲**；D3DXSprite 的 GPU 路径直接关掉
深度测试/写（与 CPU sprite 一致）。因此非 1.0 的 `Clear(ZBUFFER)`、或 CPU/GPU 交替写同一块深度的
场景，像素结果可能与 CPU 参考不一致 —— 判据仍以 `--dump-frame` 的逐像素对照为准。

### 顶点声明（`SetVertexDeclaration` + 两条流）

固定功能路**支持一份受支持形状**：`POSITION(FLOAT3)` + `TEXCOORD0(FLOAT2)` [+ `COLOR0(D3DCOLOR)`]，
元素可以落在**两条流**上（引擎的 2D 图层就是 `stream0` = POSITION、`stream1` = TEXCOORD，
`DrawIndexedPrimitive` + 索引缓冲）。实现 = 按声明把各条流**去交错**成一份临时交错缓冲
（`XYZ`+`UV`[+`diffuse`]，步长 12/20/24），再走既有的 FVF 光栅路（见 `declLayoutOf` /
`buildDeclVertices`）。
- 其它元素（法线 / 混合权 / 第二套 UV / 非 32 位分量）、`stream≥2`、以及 `*UP` 变体 —— **响亮失败**并
  在 `[d3d9] DrawIndexedPrimitive: 顶点声明（0x…）不支持：…` 里给出**具体是哪一项**（⛔ 不猜布局）。
- 怎么查：`--log-cat d3d9 --log-level info` 的 `DrawIndexedPrimitive: PrimitiveType=… ⇒ 三角形 …（光栅化 …），
  写 N 个像素`；被拒时是同一行换成 `顶点声明（0x…）不支持：…`。⚠ 顶点声明的**对象**在创建那一刻
  把元素表复制进 guest 堆（`'VDCL'` 状态块，常量见 `runtime/d3d9.h`）—— 光栅侧读的就是这份副本。

### 怎么查（口径，不是进度）

`Machine::dumpRunSummary` 固定打印：

```text
gpu       : draws=N gpu=N fallback=N fallback_pixels=N
            sprite: draws=N gpu=N fallback=N fallback_pixels=N skipped=N
            fallback top: [N×] <原因> ; ...
```

`--raster cpu` 时这些计数全 0 —— CPU 是**显式参考后端**，不算 fallback。GPU 启用后：
- 默认（不设环境变量）第一次出现每种回退原因就打印一行
  `[gpu] fallback: <原因>（该原因只打印一次）` 到 stderr；
- `--gpu-strict`：运行结束后若 `fallback Draw + sprite 回退 > 0` ⇒ 打印错误 + 原因清单并 **exit 3**
  （summary 仍先打印）。它需要 GPU 光栅化模式（`--present window`，或 `--raster gpu`）；
  与 `--raster cpu` 或 `--raster-sprites 0` 矛盾 ⇒ exit 2；后端根本没启用也按 FAIL
  （⛔ 不能把 CPU 当覆盖）。sprite 的 `skipped=`（源矩形空 / 屏外 / 退化）是裁剪，不是回退，
  不计入 strict。
- `LIFT_SLOW_DRAW=1`（可选 `LIFT_SLOW_DRAW_MS`）的 `[draw]` 行扩了
  `fvf= / coord= / stage0= / samp= / texfmt=`：`why=` 是"这批为什么没走 GPU"，其余是当时的状态值。
- `LIFT_GPU_VERIFY=1`（诊断，默认关）：对每个 GPU 走纹理的 `Draw*` 批，在**同一批 / 同一状态**上
  先对后缓冲副本跑一次 CPU 参考光栅（不影响 guest 结果），再读回 GPU 结果逐像素对照，打印
  `[gpu-verify] textured batch tris=… diff_pixels=… max_channel_diff=…`。它不依赖两次运行的时间轴，
  是"同一帧"的 GPU ↔ CPU 对照；代价是这批 CPU 软光栅的墙钟（只在排障时开）。

```bash
# 离屏 GPU 光栅（不建可见窗口；A/B 用）
./out/emulator-lift ... --max-frames 4 --raster gpu
# 可见窗口 + GPU 光栅（auto 的缺省行为，显式写出来便于对照）
./out/emulator-lift ... --max-frames 300 --present window --raster auto
# 同一条命令的"直出"正证据：日志里 present=direct/<mode>（VSYNC/IMMEDIATE/MAILBOX）
./out/emulator-lift ... --max-frames 300 --present window --present-hidden --raster gpu
# 对照：同一帧的 CPU 输出
./out/emulator-lift ... --max-frames 4 --raster cpu --dump-frame ...
# 机械判据：GPU 覆盖必须 100%（TITLE 段也一样）
./out/emulator-lift ... --max-frames 80 --present window --present-hidden --gpu-strict
```

评估口径：GPU 与 CPU 的 blend / 采样舍入允许**每通道 ≤1 LSB**，不拿单一 hash 当唯一判据；
用 `--dump-frame`（guest 后缓冲）或 `--present-screenshot`（窗口像素）做逐像素 max-diff。
直出成立时这两者 1:1 下应当**逐像素相等**（窗口像素 = target = 读过回的 guest）；
CTest `gpu-direct-present` 把这条做成守卫（含 16bpp 转换 / 尺寸变化 / 连续两帧）。
CPU 缺省仍逐字节可复现；`--raster gpu` 的 `--present headless` 会初始化 SDL 视频
（否则 `SDL_CreateGPUDevice` 失败），这不影响 `--raster cpu` 的"headless 不碰 SDL 视频"承诺。

现状口径：TITLE 段**仍未达 33 ms**（也不把验收阈值改绿）。直出去掉了"GPU→guest 读回之外的
两次全帧搬运"之后，Present 的大头是 `d3d9::Present` 里那条 Info 日志的**全帧像素直方图**
（逐像素 `std::map`），其次才是 GPU 读回；guest 像素哈希（`SurfaceArena::hash`，按 8 字节字 +
末尾 finalizer）与直方图各自的耗时看 `LIFT_FRAME_TRACE=1` 的 `pres :` 行（§4.3.1）。再往下要动的是
sprite 合批与那条直方图的算法。⛔ 不把逐帧数字写进 README。

## 4.3 热点 profile 与 native override（函数级 + 段级）

```bash
# 找热点（guest 块级 + 按 .lst 函数入口聚合 + 宿主 shim 墙钟；不改变执行结果）
./out/emulator-lift ... --max-frames 1 --profile --profile-top 40

# 显式启用（默认关；函数级名字来自 .lst 的 `proc near` / `COLLAPSED FUNCTION` 行）
./out/emulator-lift ... --max-frames 1 --native-overrides _memcpy
# all = 稳定层（含 sub_429020 / sub_48AE90 / 段级 sub_414AC0；已知风险见下）
./out/emulator-lift ... --native-overrides all
# all 但排除某一项（`-名字`，排除项优先于启用项；函数级与段级在同一个命名空间里）
./out/emulator-lift ... --native-overrides all,-sub_48B090
./out/emulator-lift ... --native-overrides all,-sub_414AC0
# 也可显式叠加 debug 项（debug 项不会被 all 带进来）
./out/emulator-lift ... --native-overrides all,某个debug项
```

- 拦截点 = **dispatcher 的 block 入口**（不 patch guest 字节）；命中后按 ABI 设 `EAX / ESP / EIP`，
  失败（例如 safe mode 下参数越界）就**回退解释器**；摘要里能看每一项的 hits / fallbacks。
- 候选必须由 `--profile` 选出，⛔ 不先猜。`--profile` 的权重是"块命中 × 块内指令数"；
  函数归属是**近似**（chunk 会归到相邻函数，见 `src/corpus/lst_index.h`）。
- **选择策略**：`--native-overrides all` 开 `KnownNative::debug == false` 的**稳定**项；
  `debug == true` 的项必须显式点名（日志带 `[debug]` 与警告）。`-名字` 表示**排除**，
  排除项优先于启用项，且与出现顺序无关：`all,-sub_48B090` 与 `-sub_48B090,all` 等价。
  **完整清单以启动行 `native-ovr: ...` 为准**（⛔ 本节不复述全部名字，它随加项而变）。
  稳定层目前含 CRT 叶子（`_memcpy` / `_memcpy_0` / `_memset` / `_strlen` / `sub_401000`）、
  S4IC/LZSS `sub_436A80`（`__thiscall`，`info[0]=输出大小`、`info[2]=压缩大小`）、
  `NormalTexture` 的三项 `sub_48B090` / `sub_429020` / `sub_48AE90`、影片 `sub_487C60`、
  字形 `sub_46D9F0`、Live2D 仿射 `sub_4CCC60`（本线第一项 x87 override）、
  退出写存档的 LZSS 编码 `sub_4367B0`、语音起播的 FFT/MDCT `sub_4E0B40`（libvorbis
  `mdct_backward`；**本线第一项「数值容忍档」**，不是真字节等价，见下一段），以及**段级** `sub_414AC0`（SYS4INI 装载器的中段填池循环；
  不是整函数）。每一项都能用 `all,-<名字>` 单独排除。
  ★ GPU 逐像素黄金图守卫（`golden-image` / `golden-image-compare`）已按用户裁决**移除**：
  该判据依赖虚拟时钟相位与 GPU 驱动，**不能稳定复现**。画面回归改由各 override 自己的
  函数级两臂差分 + 既有 GPU 用例承担。
- **「数值容忍档」**（`sub_4E0B40` 起）：判据不是"逐字节等价"，而是**两臂（解释器臂当 oracle）逐值比
  `max|Δ| ≤ 1e-6 × peak`**，实测偏差与全部已知边界登记在实现处（`native_mdct.h` +
  `docs/03-emulator/design-notes.md` 的 `L3-mdct-01`）。⇒ 它**不满足**下面"准入五条"里的真字节等价档，
  是用户明确接受的例外（音频）。
- **函数级 override 只记 1 条指令 + 1 个 tick**（`machine.cpp` 的 dispatcher）⇒ 跳过 N 条 guest 指令
  就少走 `(N−1) × 100 ns` 虚拟时间，**相位偏移是登记的已知边界，不是缺陷**；因此"帧级 / 逐像素 A/B"
  **不是**准入门。准入的五条（契约可核 · 准入零写 · 纯核 + 分支覆盖 · 有一档函数级真字节等价 ·
  已知偏差登记且可隔离）见 `src/runtime/native_overrides.h` 的契约段。
- `sub_48B090` 的 8/24/32bpp 与 alpha 路径已按 `.lst` 核；4/1bpp 源、16bpp 目标与色键≠0
  仍**显式回退解释器**。锁走 `d3d9::lockRect`/`unlockRect`，失败不留半状态。
  **虚拟时钟相位**没有做补偿（明确不在当前范围）；Windows 侧曾观察到异常、macOS 未复现，
  可随时用 `all,-sub_48B090` 做隔离 A/B。⛔ 不要把它当作 A4 逐字节基线。
- `sub_429020`（`__thiscall retn 8`）：表指针在 `[this+0x18]`、容量 `[this+8]`、槽 28 B 的
  MSVC `std::string`（`len@+0x10` / `cap@+0x14`，cap<16 用槽内 SSO）；空槽 ⇒ `-1`；
  命中 = 先比 `min(len)` 字节、再比长度；不中线性探测。**只读** guest 内存。
  优化点（语义不变）：guest **每个探测**都重算 `strlen(key)`，宿主只算一次。
  编码非法（`this`/表/容量 0、`index>=capacity`、4 GiB 越界）⇒ 回退；safe mode 下未提交 ⇒ 回退。
  **整表没有空槽**时 guest 会死循环 ⇒ 本实现回退（⛔ 不编造 `-1`）。
- `sub_48AE90`（虚表 `??_7NormalTexture@@6B@` @0x528c00 第 6 项）：`Fill(rect, color888, alpha)` ⇒
  对 `[self+0x40C]` 的纹理 `LockRect(rect)`（vtable +0x4C）→ 16bpp（A4R4G4B4）或 32bpp
  （`alpha<0` 时**保留**原 alpha）→ `UnlockRect`（+0x50），返回 0。位深取
  `ds:dword_5518C0`（`.lst` 0x406B14 是唯一写点，值来自配置 `display:TextureBit`），
  ⛔ **不是** DDraw 的显示 bpp。像素核是纯函数（`fillRect16` / `fillRect32`），锁走
  `d3d9::lockRect`/`unlockRect`（与 guest 同一条路径，失败不留锁/半状态）。
  ⚠ 16bpp 的 `alpha<0` 分支行步进是 `2*((pitch*row)/4)`（= pitch/2，**不是** pitch）——
  这是 guest 的真实写法（`.lst` 0x48AF16），逐字照抄；⛔ 不要"顺手修正"。
- `sub_414AC0`（SYS4INI 装载器）**不做整函数 native**（文件 I/O + guest 容器/堆语义）；
  但它的 1.18 s 热段是函数**中段**的填池循环（`.lst` 0x415384，7.37M 次迭代），现在用**段级**
  override 覆盖：block 入口按固定 EA 拦一次，native 填 `count = [ebx+0x5D7E8]+edi+1` 个 dword，
  按 guest 语义设 `EAX=EDX=count / ECX=[ebx+0x5D7E8] / EFLAGS(=cmp eax,edx 相等) / EIP=0x41539F`。
  - 入口判据 `EAX==0`（`0x41537E xor eax,eax` 之后；回边进来时 EAX≥1）⇒ 一次调用只拦一次；
  - 前置条件（目标/范围/计数/提交状态）不匹配 ⇒ `Declined`，**本次运行永久停用**该项并回退解释器
    （段级项的前提是静态的，继续每迭代重试只会把成本加回热路径）；
  - `insns` 与虚拟时钟按**被替代的 guest 指令数**（`8*count`）补账 —— 不补的话 `timeGetTime`
    会少走一截虚拟时间，首帧可能与解释器路径不同；
  - 启用前用 `.lst` 校验锚（entry 必须属于 `sub_414AC0`、且首指令逐字匹配）⇒ 换镜像/改 EA 会当场拒绝。
- `_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节，返回约定未核）⇒ 暂不覆盖。
- ⚠ `--until ea` 指向被 override 的函数体内部时**不会生效**（函数体/代码段不再逐条执行）；
  `--bench` 的 insns / MIPS：函数级按"一次 override = 1 条"计（不要拿它做解释器吞吐的绝对比较），
  **段级**按被替代的 guest 指令数计（与解释器同口径）。`--profile` 里被判为"被替代"的块/函数
  不计入上面的表 —— 摘要会显式列出每一项的 hits / declines / fallbacks。
- 是否继续扩展、以及 A/B 数字见需求单（§5）。

**窗口调试叠层（`--overlay`，默认关；只作用于 `--present window`）**：在**呈现画面左上角**叠一块
`mouse <x>,<y>` / `fps <x.x>` / `frame <n>` / `raster <cpu|gpu>` / `present <direct|renderer>`
（`mouse` = **guest 客户区坐标**，与 `click x y` / `--input-script` 同一格；最近 1 s 的 Present 次数 = `fps`）。
另有 `script`（当前顶层脚本身份，名字来自引擎名字表；从未采到身份就整行省略）与 `worst`（最近 3 s 内
**每帧** wall / guest / shim 的最大值，每帧 = 相邻两次 Present 的间隔；`guest` 需要块计时，没开时写 `--`）。
拿不到的字段**整行省略**（⛔ 不编数字）。⛔ 只改**宿主送显的窗口像素**：不写 guest 内存。
叠层只在**窗口**与 `--present-screenshot`（窗口像素）里可见；`--interaction-fail-dump` /
`--dump-frame` 是 **guest 帧**（`arena.dumpBmp`），不含叠层 —— 与"guest 像素 / 整帧 hash 零改动"同口径。
两条呈现路径（GPU 直出 / SDL_Renderer 上传）都画；守卫 = `tests/overlay.cpp`（CTest `overlay` /
`overlay-gpu` / `overlay-window-smoke`）。

### 4.3.1 一帧的墙钟去哪了（逐帧诊断）

`--profile` 回答两类热点：**guest**（块级 + 按 `.lst` 函数入口聚合）与**宿主 shim 实现自身**的墙钟
（`=== profile: shims`）。shim 表只包 `ShimRegistry::invoke` 里的 `fn`，**不含** dispatcher 弹参 /
native override / 事件泵；它与 guest 块表**不可相加**（两表各自的口径见下）。要看"这一帧为什么慢"，
用逐帧墙钟拆分（默认关；开窗时必须加 `--real-time`，否则虚拟时钟会先卡住）：

```bash
LIFT_FRAME_TRACE=1 ./out/emulator-lift ... --max-frames 80 --real-time \
  --present window --raster auto --native-overrides all --profile
```

每次 `Present` 打五行：

```text
[frame] n=NNN wall=…ms insns=… guest=…ms shim=…ms other=…ms
        guest: sub_XXXXXX=…ms ...
        shim : com!IDirect3DDevice9::Present=…ms×… …
        gpu  : draws=… fallback=… fallback_pixels=… sprite_gpu=… sprite_fallback=…
        pres : readback=…ms hash=…ms stats=…ms sink=…ms direct=0/1
```

- `wall` = 两次 Present 之间的宿主墙钟；`guest` = `--profile` 的块执行时间之和；
  `shim` = shim 实现自身的时间（**含** Present / D3DXSprite / 固定功能流水线的软件光栅）；
  `other` = `wall - guest - shim`（dispatcher / native override / 事件泵等）。
- `shim :` 每项带 **`×调用次数`**：同样的墙钟可能是"一次很慢"或"很多次很快"，
  这两者的下一步完全不同（前者查实现，后者查 guest 调用点）。
- `pres :` 是 `com!IDirect3DDevice9::Present` 的拆解（四段之和 ≈ 上面那一项）：
  `readback` = GPU target ↔ guest（读过回才有；没读回 ≈ 0）、`hash` = guest 像素哈希、
  `stats` = 全帧像素直方图、`sink` = 呈现汇点（renderer 上传+`RenderPresent`，或直出 blit+submit）。
  `direct=1` ⇒ 这一帧呈现走 GPU 直出。**排序不是重点，数字才是** —— 别拿 shim 总时间猜瓶颈。
- `stats` 是 `d3d9!Present` 那条 **Info** 日志（"第 N 帧 hash=… 背景像素…"）的输入，逐像素
  `std::map` 很贵 ⇒ 按**日志可见性**门控：缺省 `warn` 级别下不计算（`stats=0.0ms`），
  `--log-level info`（或 `--log-cat d3d9 --log-level info`）时照旧计算并逐字打印同一行。
  ⛔ 这不是删功能：hash / frameCount / `--dump-frame` 都在别处，照旧每帧算。
- `--profile` 的 shim 表末尾还有一行 `font!TEXTMETRIC-cache hits=… misses=… hit=…%`
  （`FontEngine::metrics` 的**度量缓存**命中率）：`gdi32!GetTextMetricsA` 的花费是现象，
  这一行才是"缓存到底有没有用"；它不是墙钟、**不进**上面那张表（两表不可相加）。
  命中率低 ⇒ 调用方在字号 / 字体之间来回跳，先查 guest 的 `CreateFontIndirectA` + `SelectObject`
  分布，别先改缓存。
- 只有 shim 慢、guest 不慢 ⇒ 先看 `--profile` 的 `=== profile: shims` 表（哪一项、调用几次、平均多少），
  不要再到 guest 函数表里找热点；`shim :` 行仍用于"这一帧"的归因。
- `LIFT_SLOW_DRAW=1`（可选 `LIFT_SLOW_DRAW_MS=<ms>`，缺省 20）会为每次慢 `Draw*` 打一行
  `[draw] frame=… prim=… pixels=… gpu=… why=… sep_alpha=… tex=… texserial=… tfactor=… alpha=… d0=… uv=… blend=<0/1>/…/… op=… cw=… fvf=… coord=… stage0=… samp=… texfmt=… fa=<n>:<均值>/<min>/<max> submit=…ms`：
  `gpu=0` 与 `why=` 就是"这一批为什么回退 CPU"的**第一原因**（不是猜测）；`stage0=` 是
  color/alpha op 与 arg1/arg2（颜色与 alpha 各一对），`samp=` 是 address U/V 与 MAG/MIN，
  `texfmt=` 是 bpp/amask 与尺寸。
  ★ `frame=` = **清过后缓冲的次数**（`Clear(TARGET)` 是"新的一帧开始"的机械判据，见 `d3d9::frameSeq()`）——
  同一 `frame` 里两行 `[draw]` 的先后就是**真实的绘制顺序**（⛔ 不要拿时间戳或日志交错猜）。
  ★ **"这一批画的是不是同一个东西"要看的字段**：`texserial=` 是**纹理身份**（`TextureInfo::serial`，
  进程级单调、永不复用）—— 两批相同 = 同一张纹理；`alpha=` 是**输入顶点**的 diffuse alpha 区间、
  `d0=` 是第一个顶点的 `r,g,b,a`、`uv=` 是本批 UV 范围。★ 只看 `texfmt=`（w/h/format）**分不出**
  "同一张纹理"与"同尺寸的另一张"。
  ⚠ `tfactor=` 是 `D3DRS_TEXTUREFACTOR`（0xAARRGGBB）：**stage0 的 arg 是 3(TFACTOR) 时，顶点色
  根本不参与** ⇒ 顶点 alpha 推进也不会有任何像素变化。
  ★ `fa=` 是**真正参与混合的 α**（CPU 光栅逐像素统计：n / 均值 / min / max；GPU 批不打——CPU 那条
  不跑）。"这一笔到底有没有画上去"只能看它：`fa=…:0/0/0` = 整笔全透明（α 取自纹理 / 顶点色没参与 /
  alpha test 全灭，三者表现相同，要接着看 `stage0=` 与 `texserial=`）。
- `LIFT_DRAW_CALLSITE=1`（诊断，默认关）：每次 `IDirect3DDevice9::DrawPrimitive` 打一行
  `[draw-call] prim=… ret=<shim 返回地址> chain=<ebp 链上的返回地址…>` —— `ret=`/`chain=` 拿去 `.lst`
  就能回答"谁画了这一批"（最近的调用点 = `ret-5`），不用猜。
  加 `LIFT_DRAW_LOCALS=<hex EA>` 时，链上**返回地址等于该 EA** 的那一帧会多打 `locals=[ebp-0x40..ebp-0x08]`
  （读某个绘制调用点的实参与局部；例：`LIFT_DRAW_LOCALS=004B4335` 读 `sub_4AF1C0`（RenderPolygon）那一帧，
  `[ebp-0x24]`/`[ebp-0x20]` 是它传给 `sub_49E390` 的纹理 id 数组，`-1` = 这一批**没有纹理**）。
  同一个开关还会为 stage 0/1 的 op/arg 写打 `[sts] stage=… type=… value=… ret=…` ——
  **"这一批用的到底是 DIFFUSE 还是 TFACTOR"只能看这个**（`ret=` 定位写它的 guest EA）。
- 回退原因还按字符串聚合进 `dumpRunSummary` 的 `gpu       :` / `fallback top:` 行
  （见 §4.2b）；`--gpu-strict` 把"还有回退"变成 exit 3，而不是靠读日志判断。
- `LIFT_LOG_MARKERS=1`（诊断，默认关）：引擎帧身份（marker 源）**变化**时往 stdout 打一行
  `[marker] frame=… insn=… cur=… id=0x%08X name=…`（名字读不到时标 `（降级）`，稳态不刷屏）。
  它是"`script <名字>` 该写什么"的**发现路径**；只开它、不写任何脚本动作也会打开身份采样。
  ★ **存在即开**（`LIFT_LOG_MARKERS=0` 也算开 —— 与 `LIFT_FRAME_TRACE` 同口径）。
- 这些环境变量都只读、默认关；⛔ 不改变执行结果，也不进 A4 的逐字节基线。

### 4.3.2 可定界的区间 profiler（`profile_begin` / `profile_end`）

`--profile` 是**整轮**的、逐帧 trace 是**按 Present** 的。要回答"某一段里谁吃的"（例如"点进 EU-ROOM
的那几秒"），用脚本把一段墙钟**框起来**：窗口边界 = 脚本动作被触发的那一个 **block 边界**，
所以边界可以落在帧上（`at frame N`）、也可以落在**帧内**（`at insn N` / `at guest_ms T`）。

```bash
# .tmp/room/enter.txt：
#   at frame 88
#   profile_begin enter        # 窗口开始
#   at frame 90
#   mouse_move 944 562
#   wait 3 frames
#   click 944 562
#   at frame 106
#   profile_end enter          # 窗口结束 ⇒ 立刻打印一次表
#   at insn 500000000
#   quit
LIFT_FRAME_TRACE=1 ./out/emulator-lift.exe ... --input-script .tmp/room/enter.txt
```

- **只在该脚本真有 `profile_*` 动作时才计时**（没有这些动作 ⇒ 整条记账路径是 no-op）。
  解析期就在脚本里的与**运行期经 `appendText` 追加的**（`--interaction-graph` 走这条）都算：
  后者的 arm 发生在 `profile_begin` 被消费的那一刻（否则窗口照开照收、表全是 0）。
  `--profile` 的**表格内容**由 `tests/profile_window.cpp` 从 API 断言（窗口/块/边表）；**文本格式本身没有守卫**
  （纯诊断输出；原先解析它的 `tests/profile_shims.cpp` 已随 `5e4fca3 refactor: clean tests` 删除）。
- **闭式**：`wall = guest + shim + native + interp + host + unattributed`。各项互斥，量都来自
  dispatcher 的同一条时间轴：`guest` = 块执行；`shim` = `ShimRegistry::invoke` 整段；
  `native` = 函数级/段级 override；`host` = 音频泵 / 事件泵 / 脚本 tick / 定时器；
  `interp` = 一次迭代里**剩下的全部**（缓存查找 / 令牌 / 时钟 / 记账）；`unattributed = wall − 其余`。
  ⛔ 不吞掉差额：正 = 有没归因的时间，负 = 有重叠/重计。
- **`gpu :` 那一行是 Present 的内部拆解（readback / hash / stats / sink），⊂ `shim` 的 Present 行**，
  ⛔ 不再加一遍（否则闭式立刻破）。
- **guest 函数表区分 self / inclusive**：`self` = 该函数的块执行墙钟（互斥 ⇒ 求和 = `guest`）；
  `inclusive` = 该函数**在栈上**的墙钟（`call` 进入 → `ret` 离开，同一函数只算最外层一帧 ⇒ 递归不重复计）。
  inclusive 是调用树（父含子）⇒ ⛔ 不可相加、⛔ 不进闭式。影子栈的 `unbalanced` / `open_at_end`
  会打印出来：非 0 说明那几段 inclusive 不可全信（`self` 仍准）。
- 窗口**未闭合**（脚本没写 `profile_end` / run 提前停止）⇒ **自动收口 + 告警 + 照样打印**
  （⛔ 不静默丢数据）；`profile_end` 没有对应 begin 也会告警。
- ⚠ **测量本身有开销**，但**归因要按 `measurement.md` M-02**：每块那两次 `steady_clock` 是**无条件**的
  （基线也付），窗口 profiler 的边际是"每迭代两次时钟 + 每块边界的宿主段计时 + `addGuest`"。
  要做"真实墙钟多少"的判断，用同一脚本把 `profile_*` 两行去掉跑一遍做 **A/B**（只差一个开关、
  N=5 取中位数、两臂各带 `instr=` 标签）；⛔ 开/关两臂的数字**不许混进同一张表**。
- 窗口内的函数/块归属复用 `--profile` 那一套（块级计时 + `.lst` 函数入口聚合），⛔ 没有第二套计时。

**怎么查**：窗口表在 `profile_end` 时打印一次（`=== profile window: <label> ===`）；
逐帧的粗分仍是 §4.3.1 的 `LIFT_FRAME_TRACE=1`。守卫 = `tests/profile_window.cpp`（CTest `profile-window`）。

## 4.4 音频（方案 C：真 feeder 线程 + 独立 mixer）

```bash
# 默认 = SDL3 真设备但**静音**（--sound 才出声）；--audio=off 回到旧 silent；
# --audio=virtual 用虚拟设备时钟（确定性；dump 与 --sound 正交）
emulator-lift --game-dir <install> --skip-logo --native-overrides all --present window
emulator-lift --game-dir <install> --skip-logo --native-overrides all --present window --sound
emulator-lift --game-dir <install> --skip-logo --native-overrides all --present headless \
              --audio virtual --sound --audio-dump .tmp/title.wav --max-insns 400000000
```

口径（`docs/audio-implementation.md` 是唯一真源）：

- **谁产 PCM**：guest 解码器（WAV / Ogg / 逐流格式）。宿主只做"设备游标 + 位置通知 + 逐缓冲格式转换 +
  混音 + SDL3 输出"；⛔ 宿主不重写解码器、不伪造 `sub_4B6C30` 的语义。
- **谁执行喂料**：`CreateThread(0x4B5AA0)` 登记后，`ResumeThread` 启动**真宿主 feeder 线程**。
  它有自己的 `X86` / 专用 guest 栈 / 私有 `BlockCache` + `Decoder`，在自己的线程里调
  `sub_4B6C30`；`--native-overrides` 的 `_memcpy/_memset` 也在 feeder 私有一份。
- **谁混音**：独立 `mixer` 线程按设备游标读 guest 环并推 `SDL_PutAudioStreamData`。主线程卡在
  Present / 长 shim 不再导致音频断裂。
- **同步边界**：**令牌**（`runtime/guest_token.h`）—— 任一时刻最多一个宿主线程持有令牌；
  guest 可见容器只在令牌里访问。互斥点在**每次 shim 调用**（`ShimRegistry::invoke`）：
  feeder 的整次 fill **不**持令牌（guest 指令与主线程并行），只有它的 shim 调用持令牌；
  主线程按块取/放；guest `Enter/LeaveCriticalSection` 映射到宿主 per-EA 递归锁（阻塞前让出
  令牌）；设备模型另有 `AudioEngine::mutex_`。⛔ 不要绕开令牌直接碰 `GuestHeap` /
  `HandleTable` / `ComRegistry` / `Vfs`。可观测检查点覆盖 `GuestHeap::alloc/free/realloc`、
  `HandleTable::add`、`machine.block`、`shim.invoke`（未持令牌 ⇒ 违规计数，`--assert-guest-token`
  判红）；`Vfs` / `ComRegistry` 没有单独计数点，靠"访问都在 shim 里"这条结构约束。
- **时钟档**：`sdl` + 宿主时钟（窗口模式/`--real-time`）= **方案 C 真线程**；`virtual` 或没有
  `--real-time` 的 sdl = **方案 D 主线程泵**（确定性回退，CTest 用）。窗口模式会强制 `--real-time`。
- **输出格式固定 48k / 16 bit / 2 ch**；每条缓冲按自己的 `WAVEFORMATEX` 做采样率/声道转换
  （实测引擎会建 44.1k mono SE、48k stereo BGM、44.1k stereo 静音垫）。
- **静音开关**：缺省 `muted=1`（`--sound` 才出声）。它与 `--audio`（后端选择）**正交**：
  设备创建 / 混音器 / feeder 线程 / 格式协商 / guest 的 DirectSound 调用 / underrun·dropped
  统计**全部照跑**，只在最终交付点（`out` 生成之后、`pushMixed` 之前）把缓冲覆盖为 0。
  怎么查：运行摘要的 `audio     :` 行有 `device=ok|fallback|none muted=1|0 muted_bytes=
  fills= fill_bytes=`；`muted_bytes == mixed > 0` = "确实有内容被静音"，
  `muted_bytes=0` 且 `device=none` = "没有交付 / 没有设备"。`begin()` 的 Info 行也会写
  `muted=1（缺省；--sound 可听）`。⛔ 要看 dump 里的 PCM 必须带 `--sound`（缺省时 dump 也全 0，
  因为门在 `pushMixed` 之前的唯一交付点）。
- **通知/游标**：每条缓冲的 `played` 以它自己的 `nAvgBytesPerSec` 折算；通知按位置排序后入队
  （一次推进跨多个点时，⛔ 不能按 quarter 序号入队，否则回绕 0 会排在 3/4 前）。
- **lead**：真线程 **100ms**（mixer 线程独立，fill ≈400ms 不再需要覆盖它）；泵回退 **600ms**。
  `--audio=off` 时零成本。
- **join**：`WaitForSingleObject(喂料线程句柄)` = `SetEvent(quit)` + 真 `std::thread::join()`；
  join 前会 `releaseAll` 主线程持有的 guest CS（防止 feeder 永久阻塞）。返回 `WAIT_OBJECT_0` 后
  不再写 guest 环（防 `Release pDS` 后的 UAF）。
- **单次 run**：同一 `Machine` 第二次 `run()` 会拒绝音频并报错（guest DS 对象仍在；⛔ 不静默清空
  设备模型）。需要第二次 run 就新建 `Machine`。
- **CTest**：`audio-device`（`tests/audio.cpp`）覆盖逐缓冲格式 / 通知次序 / dump 逐字节 / 泵的整份
  上下文交换 / CS 与在飞计数成对收尾。⛔ 它不打开 SDL 设备（pure 档）；真线程路径由真实运行验收
  （`LIFT_AUDIO_TRACE=1` 打每次 feeder fill 的墙钟）。

### 怎么查（单令牌 / 并发）

- `--assert-guest-token`：每次运行的摘要里有 `token     : violations=0 max-concurrent=1`；
  违规 > 0 或同时持有者 > 1 ⇒ 退出码 4。没用令牌的纯单线程夹具不会 arm（不误报）。
- `ctest -R guest-token-stress`：两条宿主线程 × 200000 次真实 `GuestHeap` alloc/free 的压力守卫；
  把 `GuestToken::acquire/release` 短路成空操作 ⇒ 它红。
- `LIFT_AUDIO_TRACE=1`：每次 feeder fill 的墙钟 / override 命中；摘要里的
  `audio : ... underruns=N` 是设备欠载账（⛔ 弱指标：只在 SDL 队列 == 0 时计；跨版本不可比 ——
  并发修复的 A/B 用同一份代码的"令牌开/关"对照，数据见需求单
  `REQ-01M6X64J338E6YDQXJB9RKAFSG`）。
- 违规的**第一个**现场：摘要 `token` 行的 `first=<site>`（`GuestHeap::alloc` /
  `HandleTable::add` / `machine.block` …；这些 check 都在 shim 里执行 ⇒ 覆盖 feeder 的容器访问）。

## 4.5 运行摘要怎么读（`stop` / `detail` / `heap` / `arena`）

每次运行结束打一份摘要（`Machine::dumpRunSummary`）。诊断按这个顺序看：

- `instr     : profile=<on/off> frame-trace=<on/off> window-prof=<on/off> expect=<n|off>`：**仪器口径标签**
  （`measurement.md` M-02）。读的是**本轮真实生效**的位（`window-prof` = `WindowProfiler::armed()`），
  ⛔ 不是 CLI 回显；`expect=` 是时间线效果级判据这一轮真的开了几个观测窗（`off` = 没启用）。
  要给读数做 A/B 或写进 `observations.md`，**照抄这一行**，⛔ 不要凭参数回填。
- `stop      : <reason> @ eip=…` + `detail    : …`：**停因**。`detail` 优先用 shim 写的 `stop_why`
  （形如 `<原因>（shim <module>!<name>）`）⇒「未实现」「资源耗尽」「guest 主动退出」在摘要里分得开。
  ★ `reason` 本身也分开：`resource-exhausted`（我方 arena / guest 堆不够）· `unimplemented-slot`
  （已注册但方法槽 / 形态没实现）· `unimplemented`（注册表里根本没有 fn）· `stopped`（正常主动停：
  `ExitProcess` / `--max-frames` / 影片放完 / 窗口关闭）。前三个**非 ok** ⇒ 退出码 2（见 §4.2d）。
- `present-tail: last_frame=<N> insns_at_last_present=<N> tail_insns=<N> tail_wall_ms=<ms>`：口径 =
  **最后一次 Present 调用 → 本次 run 停**的 guest 指令数（= 上面 `insns :` − `insns_at_last_present`，
  同源 `cpu_.insns`）。★ 是"**调用**"不是"成功"：sink / dump 失败的那次**也算**（与 `frameCount` 同一契约）。
  ★ **可复现档（`--present headless` + 虚拟时钟）下 `tail_insns` 确定**；real-time 档（`--present window` /
  `--real-time` / `--audio sdl`）下 `last_frame` / `insns_at_last_present` / `tail_insns` **都只能当单样本**
  （同命令两轮读数就会不同），`tail_wall_ms` 在任何档位都只是宿主墙钟单样本。**本轮 0 次 Present ⇒ 不打这行**
  （⛔ 别读成"尾段为 0"）；`--max-frames` 下恒为 0（停点在 Present 内部；实测、无用例钉）、`--bench` 下停在
  bench 上限而非进程退出。
- `input-script: unfinished pending=<n> releases=<m> first="<原动作行>"`（BUG-20）：**只有**在脚本还有
  没跑完的东西、且**不是脚本自己 `quit`** 时打。`pending` = 没应用的动作条数；`releases` = 还没交付的抬起
  （`click` 的自动 hold 在等 guest 采样 / 显式 hold 没到期）；`first` = 第一条未应用动作的原文
  （`pending==0` 时取第一条未完成的抬起），含空格 ⇒ 双引号包住，内部 `"` 显示成 `'`。
  ★ 0 条未应用**且没有未交付的抬起** ⇒ 不打；`quit` 提前结束 ⇒ 不打（剩下的动作是脚本有意跳过的）。
  它**不替代** stdout 的 `[input-script] finished`：那条在 `finished()` **或 `quit`** 时打，而 `quit` 路径
  写的是**总**动作数（⛔ 别当"应用数"读）。
- `heap      : in_use … peak … allocs …`：**主 guest 堆**（Win32 / CRT 分配走它）。
- `arena     : surfaces N in_use U peak P largest-free F`：**surface arena**（D3D9 纹理 / 后缓冲 /
  深度的独立 GuestHeap）。★ 它不与主堆共享 ⇒"纹理没回收"在主堆那行**完全看不见**。
  `largest-free` 是当前最大**连续**空闲块：`in_use` 还没满但 `largest-free` 很小 = 碎片化
  （首次适配只认连续块，见 `runtime/heap.cpp`）。
- `shims     : imported … / called-unimplemented …`：后者只数**被调用过、且没有实现**的导入。

★ **三类停不许混**（否则会把我方容量问题记成"guest 用了没实现的导出"）：

| stderr 标题 | 含义 | 摘要 `stop` / 计数 |
|---|---|---|
| `=== unimplemented <模块> call ===` | 已注册的导出 / 槽走到 loud trap（方法体没实现） | `unimplemented-slot`（**非 ok**）；进 `called-unimplemented` |
| `=== unimplemented import (base gap) ===` | 导入表里有、但注册表里没有 fn（`had_impl=false`） | `unimplemented`（**非 ok**）；进 `called-unimplemented` |
| `=== D3DX 资源耗尽（不是 unimplemented）===` | **我方** arena / guest 堆不够（容量或碎片） | `resource-exhausted`（**非 ok**）；⛔ **不**进 `called-unimplemented`（不是缺口） |

D3DX 建纹理失败的 reason 里带 `w×h / D3DFORMAT / 像素字节`，与 `surface.cpp` / `heap.cpp` 的数字能直接对账。
★ COM 的两条**装配错**（`=== COM thunk 不在路由表里 ===` / `=== COM thunk 被调到，但 cpu.com 为空 ===`）
同归 `unimplemented-slot`（非 ok）：它们不是"缺口"而是"装配对不上"，但修法同样不是改容量。

## 5. 真源

| 想知道 | 看哪 |
|---|---|
| **执行源是哪个文件**（为什么是 `age-sectfix` 而不是 `age-original` / 游戏目录里的 exe） | 本文件 §2.1 + `docs/plan-m1-cpu-base.md` §4 D1 |
| 源选择 / 32→64 / 缺 C 的量化 | `docs/03-emulator/platform-effort.md` §5.6 |
| 块寻址 / chunk / 跳转表 | 同文件 §5.7 + `src/runtime/x86.h` |
| 宿主层 / 不依赖 AppKit 的口径 | 同文件 §5.5 |
| 可接受偏差（菜单 / 视频 / Live2D / 3D；音频已实现，见 §4.4） | `SCOPE.md` + 同文件 §4.1 |
| 可切换呈现后端（headless/CPU ↔ SDL/GPU） | 本文件 §4.2 + `src/runtime/present_sdl.h` |
| 覆盖清单格式 | 同文件 §5.7 |
| 实现计划（切片 / 判据 / 决策点） | M1 `docs/plan-m1-cpu-base.md` · M2 `docs/plan-m2-2d-text.md`（索引 `docs/README.md`） |
| 热点 profile / native override 的开关与 ABI | 本文件 §4.3 + `src/runtime/native_overrides.h` |
| 需求与进度 | `REQ-01M6GJMENJ8XEK4ZPTA768YBPW` · `pnpm tools requirements plan` |
