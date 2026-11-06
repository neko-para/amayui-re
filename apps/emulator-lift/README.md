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
- ⇒ guest VM 保持**单线程**；服务线程要么宿主化（音频回调 / 视频解码），要么串行化；
  ⛔ 不把 guest 代码放多线程。音频 silent（`SCOPE.md`）时，声音线程可整条省掉。
- ★ **实现口径 = 伪线程**（用户 2026-11-05；详见 `SCOPE.md` §1）：`CreateThread` 只发线程句柄，
  **不执行 start routine**；`ResumeThread` / `SetThreadPriority` / 亲和性只返回形态值。
  因此"创建了服务线程"不等于"线程体在跑" —— 需要线程体副作用的路径会停在对应 trap，⛔ 不静默假装。

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
  src/main.cpp                        ← CLI：--exe --lst --game-dir --appdata --agerc
                                        --until shim|ea --trace-shims --trace-blocks
                                        --max-insns --max-frames --dump-frame --time-limit
                                        --safe-memory --report --bench --smoke
                                        --profile --profile-top --native-overrides
                                        --present headless|window|auto --present-driver
                                        --present-vsync --present-hidden --present-screenshot
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

## 4.2 可切换呈现后端（`--present`）

**缺省 `--present headless`**：与今天逐字节一致（hash / frameCount / `--dump-frame` / `--max-frames`；
不初始化 SDL 视频）。**`--present window`** 打开真实 SDL 窗口并走 **GPU API 路径**：

```bash
# 看得见的窗口（真 SDL 窗口；自动 real-time）
./out/emulator-lift ... --max-frames 300 --present window
# 无人值守 / CTest：隐藏窗口 + 宿主回读截图
./out/emulator-lift ... --max-frames 1 --present window --present-hidden \
  --present-screenshot ../../.tmp/window-{frame}.bmp
```

- 驱动列表 `--present-driver`（逗号分隔，缺省 `gpu,direct3d12,direct3d11,vulkan,metal,opengl`）：
  `gpu` 命中时日志 `present: driver=gpu gpu=direct3d12`，且 `SDL_GetGPURendererDevice()!=NULL`
  —— 这是"确实走 SDL_GPU"的正证据。其它驱动是 GPU API 路径，但可能是 WARP/GDI，日志如实打。
  缺省列表**不含 software**；显式 `--present-driver software` 只用于测试，会打"不是 GPU 路径"警告。
- 像素格式按 **masks** 选（六种 D3D9 后缓冲格式）；纹理 blend=NONE（`A8R8G8B8` / DDraw 的 32bpp
  带真实 alpha，alpha=0 的帧在默认混合下会整窗黑；`X8R8G8B8` 没有 alpha，不受影响）；
  逻辑呈现尺寸按当前帧，resize 用 letterbox。
- `--present-screenshot` 是**窗口回读**（present 前），与 `--dump-frame` 的 **guest 后缓冲** 是两份独立
  产物：1:1 时可逐像素对照，缩放/letterbox 下只比内容矩形。
- 显式 `--present window` 打不开 ⇒ **exit 2**；运行期失败 ⇒ 记录 + 3 次后回退 headless（guest 的
  Present 结果不受影响）。`--present auto` 仅调试用：失败回 headless。
- DDraw **不在本次验收内**（用户口径：AGE 默认不走 DDraw）；`Flip` 复用同一 sink 只是副作用，
  `--dump-frame` 仍只在 D3D9 Present 消费。
- ⚠ 本阶段**没有**做输入映射 / `GetMessageA` 宿主事件泵：如果 guest 走到空队列 `GetMessageA`，
  仍会按现有语义 abort（交互主循环另开单）。看画面用 `--real-time` / `--max-frames`；
  `--bench + window` 的 MIPS 含 SDL 上传/呈现开销，不是解释器绝对吞吐。

## 4.2b GPU 光栅化（`--raster gpu`）

缺省 `--raster cpu`。`--raster gpu` 用独立 `SDL_GPUDevice` 把 D3D9 的屏幕空间三角形与
D3DXSprite 的纹理四边形画到 1280×720 BGRA8 + D32 离屏 target，再在 Present 前读回 guest
后缓冲；T&L / 裁剪 / 状态机 / 失败码仍全在 CPU。headless 也可用
（`--raster gpu --present headless`，dummy 视频 + 离屏），窗口模式共用同一后端。

shader 后端按 **SDL_GPU 的原生格式**选平台：**Windows = D3DCompile → DXBC**，
**macOS = MSL 源码（Metal）**；其它平台明确失败（⛔ 不静默回退 CPU、不把 DXBC 带进 Metal）。

支持面：无纹理 XYZRHW+DIFFUSE、D3DXSprite 纹理四边形（NEAREST / 顶点色调制 /
ALPHABLEND，`pTexture=0` 用 1×1 白纹理）、GOURAUD、ONE/ZERO 或 SRCALPHA/INVSRCALPHA、
深度测试/写/比较、scissor/viewport；纹理 stage 链、alpha test、多 stage、FLAT、16bpp、
BORDER/MIRROR_ONCE、`BOTH*` 混合 ⇒ 回退 CPU。

```bash
# 离屏 GPU 光栅（不建可见窗口；A/B 用）
./out/emulator-lift ... --max-frames 4 --raster gpu
# 可见窗口 + GPU 光栅
./out/emulator-lift ... --max-frames 300 --present window --raster gpu
# 对照：同一帧的 CPU 输出
./out/emulator-lift ... --max-frames 4 --raster cpu --dump-frame ...
```

评估口径：GPU 与 CPU 的 blend 舍入允许 **每通道 ≤1 LSB**，不拿单一 hash 当唯一判据；
用 `--dump-frame` 做逐像素 max-diff。CPU 缺省仍逐字节可复现；`--raster gpu` 的
`--present headless` 会初始化 SDL 视频（否则 `SDL_CreateGPUDevice` 失败），这不影响
`--raster cpu` 的“headless 不碰 SDL 视频”承诺。

## 4.3 热点 profile 与 guest 函数级 native override（试点）

```bash
# 找热点（块级 + 按 .lst 函数入口聚合；不改变执行结果）
./out/emulator-lift ... --max-frames 1 --profile --profile-top 40

# 显式启用（默认关；名字来自 .lst 的 `proc near` / `COLLAPSED FUNCTION` 行）
./out/emulator-lift ... --max-frames 1 --native-overrides _memcpy
# all = 稳定层（包含 sub_48B090；其已知风险见下）
./out/emulator-lift ... --native-overrides all
# all 但排除某一项（`-名字`，排除项优先于启用项）
./out/emulator-lift ... --native-overrides all,-sub_48B090
# 也可显式叠加 debug 项（debug 项不会被 all 带进来）
./out/emulator-lift ... --native-overrides all,某个debug项
# boot 加速的已取证集合（CRT 叶子 + S4IC/LZSS）
./out/emulator-lift ... --max-frames 1 --native-overrides \
  _memcpy,_memcpy_0,_memset,_strlen,sub_401000,sub_436A80
```

- 拦截点 = **dispatcher 的 block 入口**（不 patch guest 字节）；命中后按 ABI 设 `EAX / ESP / EIP`，
  失败（例如 safe mode 下参数越界）就**回退解释器**；摘要里能看每一项的 hits / fallbacks。
- 候选必须由 `--profile` 选出，⛔ 不先猜。`--profile` 的权重是"块命中 × 块内指令数"；
  函数归属是**近似**（chunk 会归到相邻函数，见 `src/corpus/lst_index.h`）。
- **选择策略**：`--native-overrides all` 开 `KnownNative::debug == false` 的**稳定**项；
  `debug == true` 的项必须显式点名（日志带 `[debug]` 与警告）。`-名字` 表示**排除**，
  排除项优先于启用项，且与出现顺序无关：`all,-sub_48B090` 与 `-sub_48B090,all` 等价。
  稳定层当前是 CRT 叶子（`_memcpy` / `_memcpy_0` / `_memset` / `_strlen` / `sub_401000`）、
  S4IC/LZSS `sub_436A80`（`__thiscall`，`info[0]=输出大小`、`info[2]=压缩大小`）与
  `sub_48B090`（`NormalTexture::UploadImage`）。
- `sub_48B090` 的 8/24/32bpp 与 alpha 路径已按 `.lst` 核；4/1bpp 源、16bpp 目标与色键≠0
  仍**显式回退解释器**。锁走 `d3d9::lockRect`/`unlockRect`，失败不留半状态。
  **虚拟时钟相位**没有做补偿（明确不在当前范围）；Windows 侧曾观察到异常、macOS 未复现，
  可随时用 `all,-sub_48B090` 做隔离 A/B。⛔ 不要把它当作 A4 逐字节基线。
- `_memcmp` 的 CRT 实现是 COLLAPSED（没有可观察字节，返回约定未核）⇒ 暂不覆盖；
  `sub_414AC0`（SYS4INI 初始化）不做整函数 native（文件 I/O + guest 容器/堆语义）。
- ⚠ `--until ea` 指向被 override 的函数体内部时**不会生效**（函数体不再逐条执行）；
  `--bench` 的 insns / MIPS 也按"一次 override = 1 条"计 —— 不要拿它们做解释器吞吐的绝对比较。
- 是否继续扩展、以及 A/B 数字见需求单（§5）。

## 5. 真源

| 想知道 | 看哪 |
|---|---|
| **执行源是哪个文件**（为什么是 `age-sectfix` 而不是 `age-original` / 游戏目录里的 exe） | 本文件 §2.1 + `docs/plan-m1-cpu-base.md` §4 D1 |
| 源选择 / 32→64 / 缺 C 的量化 | `docs/03-emulator/platform-effort.md` §5.6 |
| 块寻址 / chunk / 跳转表 | 同文件 §5.7 + `src/runtime/x86.h` |
| 宿主层 / 不依赖 AppKit 的口径 | 同文件 §5.5 |
| 可接受偏差（菜单 / 音频 / 视频 / Live2D / 3D） | `SCOPE.md` + 同文件 §4.1 |
| 可切换呈现后端（headless/CPU ↔ SDL/GPU） | 本文件 §4.2 + `src/runtime/present_sdl.h` |
| 覆盖清单格式 | 同文件 §5.7 |
| 实现计划（切片 / 判据 / 决策点） | M1 `docs/plan-m1-cpu-base.md` · M2 `docs/plan-m2-2d-text.md`（索引 `docs/README.md`） |
| 热点 profile / native override 的开关与 ABI | 本文件 §4.3 + `src/runtime/native_overrides.h` |
| 需求与进度 | `REQ-01M6GJMENJ8XEK4ZPTA768YBPW` · `pnpm tools requirements plan` |
