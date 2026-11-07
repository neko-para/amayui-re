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
  - 同步边界就是引擎自己的**每槽 `CRITICAL_SECTION`**：guest 的 `Enter/LeaveCriticalSection`
    现在映射到宿主 per-EA 递归锁（`CriticalSectionTable`），feeder 会**真的阻塞**；
    `ComRegistry` / `ShimRegistry` 的宿主表加锁；设备模型用 `AudioEngine::mutex_`。
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

## 3. 目录（落点）

```text
apps/emulator-lift/
  CMakeLists.txt                      ← 目标 + CTest（**分级靠注册位置**，见 AGENTS.md §5）
  CMakePresets.json / vcpkg.json      ← windows-x64 / linux-x64 / macos-arm64；依赖 sdl3 + zydis + freetype + glm
  README.md / SCOPE.md / docs/        ← 口径 / 用户拍板的偏差 / 计划长文
  src/main.cpp                        ← CLI：--exe --lst --game-dir --appdata --agerc --skip-logo
                                        --until shim|ea --trace-shims --trace-blocks
                                        --max-insns --max-frames --dump-frame --time-limit
                                        --safe-memory --report --bench --smoke
                                        --log-level --log-cat --verbose
                                        --profile --profile-top --native-overrides
                                        --present headless|window|auto --present-driver
                                        --present-vsync --present-hidden --present-screenshot
  src/runtime/log.h                   ← 日志分级 / 分类 / 模式（默认 warn；Error 永远显示）
  src/runtime/host_encoding.{h,cpp}   ← 进程入口编码契约：UTF-8 argv（GetCommandLineW）/ 控制台码页
  src/runtime/native_overrides.{h,cpp}← guest 函数级 native override（试点；默认关）
  src/runtime/present.{h,cpp}         ← 唯一记录点（hash / frameCount / BMP）+ 可选 PresentSink
  src/runtime/present_sdl.{h,cpp}     ← 真实 SDL 窗口 + GPU API 呈现（pimpl；头文件不暴露 SDL 类型）
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
  落在 `if(...)` 里 ⇒ `assets`。默认 `ctest` 会跑前者，后者在有 `corpus/` 素材的机器上自动启用。
  缺资产时这 5 条**不注册**（`ctest` 的用例数会变少，不是"跑绿了"）；已注册但 env 缺失时测试返回 **77**
  （`SKIP_RETURN_CODE`），⛔ 不是"什么都不做然后 return 0"。
- ★ `LIFT_SAFE_MEMORY=1`：每次 guest 访存都查提交位图（慢）。判据 A1 要求**再跑一遍这一档**。
- 生成物（`out/`、`vcpkg_installed/`）由本目录 `.gitignore` 排除。

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

### 能力范围（准入在 `d3d9::gpuEligible`，映射是纯函数；守卫 `tests/gpu_mapping.cpp`）

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
（逐像素 `std::map`）与 guest 像素哈希，其次才是 GPU 读回；再往下要动的是 sprite 合批与
那条直方图的算法。⛔ 不把逐帧数字写进 README —— 看 `LIFT_FRAME_TRACE=1`（§4.3.1）。

## 4.3 热点 profile 与 native override（函数级 + 段级）

```bash
# 找热点（块级 + 按 .lst 函数入口聚合；不改变执行结果）
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
  稳定层当前是 CRT 叶子（`_memcpy` / `_memcpy_0` / `_memset` / `_strlen` / `sub_401000`）、
  S4IC/LZSS `sub_436A80`（`__thiscall`，`info[0]=输出大小`、`info[2]=压缩大小`）、
  `sub_48B090`（`NormalTexture::UploadImage`）、`sub_429020`（`Map::findIndex`）、
  `sub_48AE90`（`NormalTexture` 虚表第 6 项 = 纯色填充），以及**段级** `sub_414AC0`
  （SYS4INI 装载器的中段填池循环；不是整函数）。
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

### 4.3.1 一帧的墙钟去哪了（逐帧诊断）

`--profile` 只回答"guest 热点在哪"；它**看不见**宿主 shim / Present / 软件光栅的耗时。
要看"这一帧为什么慢"，用逐帧墙钟拆分（默认关；开窗时必须加 `--real-time`，否则虚拟时钟会先卡住）：

```bash
LIFT_FRAME_TRACE=1 ./out/emulator-lift ... --max-frames 80 --real-time \
  --present window --raster auto --native-overrides all --profile
```

每次 `Present` 打五行：

```text
[frame] n=NNN wall=…ms insns=… guest=…ms shim=…ms other=…ms
        guest: sub_XXXXXX=…ms ...
        shim : com!IDirect3DDevice9::Present=…ms ...
        gpu  : draws=… fallback=… fallback_pixels=… sprite_gpu=… sprite_fallback=…
        pres : readback=…ms hash=…ms stats=…ms sink=…ms direct=0/1
```

- `wall` = 两次 Present 之间的宿主墙钟；`guest` = `--profile` 的块执行时间之和；
  `shim` = shim 实现自身的时间（**含** Present / D3DXSprite / 固定功能流水线的软件光栅）；
  `other` = `wall - guest - shim`（dispatcher / native override / 事件泵等）。
- `pres :` 是 `com!IDirect3DDevice9::Present` 的拆解（四段之和 ≈ 上面那一项）：
  `readback` = GPU target ↔ guest（读过回才有；没读回 ≈ 0）、`hash` = guest 像素哈希、
  `stats` = 全帧像素直方图、`sink` = 呈现汇点（renderer 上传+`RenderPresent`，或直出 blit+submit）。
  `direct=1` ⇒ 这一帧呈现走 GPU 直出。**排序不是重点，数字才是** —— 别拿 shim 总时间猜瓶颈。
- `stats` 是 `d3d9!Present` 那条 **Info** 日志（"第 N 帧 hash=… 背景像素…"）的输入，逐像素
  `std::map` 很贵 ⇒ 按**日志可见性**门控：缺省 `warn` 级别下不计算（`stats=0.0ms`），
  `--log-level info`（或 `--log-cat d3d9 --log-level info`）时照旧计算并逐字打印同一行。
  ⛔ 这不是删功能：hash / frameCount / `--dump-frame` 都在别处，照旧每帧算。
- 只有 shim 慢、guest 不慢 ⇒ 不要再到 `--profile` 的 guest 函数表里找热点。
- `LIFT_SLOW_DRAW=1`（可选 `LIFT_SLOW_DRAW_MS=<ms>`，缺省 20）会为每次慢 `Draw*` 打一行
  `[draw] prim=… pixels=… gpu=… why=… sep_alpha=… tex=… blend=…/… op=… cw=… fvf=… coord=… stage0=… samp=… texfmt=… submit=…ms`：
  `gpu=0` 与 `why=` 就是"这一批为什么回退 CPU"的**第一原因**（不是猜测）；`stage0=` 是
  color/alpha op 与 arg1/arg1，`samp=` 是 address U/V 与 MAG/MIN，`texfmt=` 是 bpp/amask 与尺寸。
- 回退原因还按字符串聚合进 `dumpRunSummary` 的 `gpu       :` / `fallback top:` 行
  （见 §4.2b）；`--gpu-strict` 把"还有回退"变成 exit 3，而不是靠读日志判断。
- 两个环境变量都只读、默认关；⛔ 不改变执行结果，也不进 A4 的逐字节基线。

## 4.4 音频（方案 C：真 feeder 线程 + 独立 mixer）

```bash
# 默认 = SDL3 真设备（能听到）；--audio=off 回到旧 silent；--audio=virtual 用虚拟设备时钟
emulator-lift --game-dir <install> --skip-logo --native-overrides all --present window
emulator-lift --game-dir <install> --skip-logo --native-overrides all --present headless \
              --audio virtual --audio-dump .tmp/title.wav --max-insns 400000000
```

口径（`docs/audio-implementation.md` 是唯一真源）：

- **谁产 PCM**：guest 解码器（WAV / Ogg / 逐流格式）。宿主只做"设备游标 + 位置通知 + 逐缓冲格式转换 +
  混音 + SDL3 输出"；⛔ 宿主不重写解码器、不伪造 `sub_4B6C30` 的语义。
- **谁执行喂料**：`CreateThread(0x4B5AA0)` 登记后，`ResumeThread` 启动**真宿主 feeder 线程**。
  它有自己的 `X86` / 专用 guest 栈 / 私有 `BlockCache` + `Decoder`，在自己的线程里调
  `sub_4B6C30`；`--native-overrides` 的 `_memcpy/_memset` 也在 feeder 私有一份。
- **谁混音**：独立 `mixer` 线程按设备游标读 guest 环并推 `SDL_PutAudioStreamData`。主线程卡在
  Present / 长 shim 不再导致音频断裂。
- **同步边界**：引擎自己的**每槽 `CRITICAL_SECTION`** —— guest `Enter/LeaveCriticalSection`
  映射到宿主 per-EA 递归锁（`CriticalSectionTable`），feeder 会真的阻塞；`ComRegistry` /
  `ShimRegistry` 的宿主表加锁；设备模型用 `AudioEngine::mutex_`。⛔ 不是全局单令牌：fill 期间
  mixer / 主线程仍可运行。
- **时钟档**：`sdl` + 宿主时钟（窗口模式/`--real-time`）= **方案 C 真线程**；`virtual` 或没有
  `--real-time` 的 sdl = **方案 D 主线程泵**（确定性回退，CTest 用）。窗口模式会强制 `--real-time`。
- **输出格式固定 48k / 16 bit / 2 ch**；每条缓冲按自己的 `WAVEFORMATEX` 做采样率/声道转换
  （实测引擎会建 44.1k mono SE、48k stereo BGM、44.1k stereo 静音垫）。
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
