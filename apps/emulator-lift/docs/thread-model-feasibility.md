# emulator-lift 真线程可行性评估（只读）

- 范围：`apps/emulator-lift`——**把 guest 的 `CreateThread` 改成真线程**（start routine 真的在宿主线程上跑）需要什么、会漏什么、值不值得做。
- 方式：只读代码 + 只读语料（`.lst`）+ 只读测试。⛔ 未改任何源码、未 `git add`、未 `git commit`、未运行游戏。
- 证据标签：**【实测】** = 本次机械读出（代码行 / `.lst` 行）；**【静态推断】** = 由代码与 x86/ARM 语义推出、可再核；**【待核】** = 明确没核。
- **行号 = 快照，不是锚**：`machine.cpp` / `machine.h` / `main.cpp` / `native_overrides.*` 在 2026-11-07 评估期间**被并发编辑**（`git diff --stat` 已达 +1300 行）⇒ 行号会漂。**稳定锚是符号名**（`fireDueTimer` / `startNestedGuestCall` / `traceInputsHash` / `opt.real_time` / `Machine::cpu_`）；文中行号只作跳转提示，遇到漂移请按符号找。（与本仓"锚 EA、不锚行号"同一条纪律。）
- 本文不写引擎语义结论（那走 `AGENTS.md` §6 与 `data/spec/`），只写"线程模型这一层的缺口与代价"。
- 承接：`SCOPE.md` §1（伪线程）· `README.md` §2.5（业务单线程）· `docs/audio-feasibility.md`（音频侧卡点）。
- **实施记录（2026-11-07）**：用户先裁决 **V1 = D** 并落地；随后实测 D 的 SE 延迟 ~600 ms /
  主线程 Present 期间断裂，用户澄清"完全 override"是要**真线程入口** ⇒ 已升级为 **C（真 feeder
  线程 + 独立 mixer）**：feeder 有自己的 `X86` / guest 栈 / 私有块缓存，引擎每槽 CS 映射到宿主
  递归锁；实测 fill 381–462 ms，lead 100 ms，30 s `underruns=0`。实现口径与偏差见
  **`docs/audio-implementation.md`**；D 仍作为 `--audio=virtual` / 无 `--real-time` 的回退泵。
  ⛔ 本文 §2/§3 的"通用真线程 / TSO"仍未实施 —— C 只开了音频两条线程，游戏 VM 仍单线程。

---

## 0. 结论（先看这个）

**"直接简单实现成真线程"= 不可行；"实现成真线程"= 可行，但"简单"的那一半必须先换掉。**
**而"根本不实现线程"（方案 D：把喂料线程 native override 成主循环里的一个确定性泵）= 可行且更省** —— 见 §5.1。

一句话：**缺的不是并发能力，是"同步对象"这一层不存在** —— 现在的临界区/事件/等待是"单线程里的布尔记账"，它们既不会阻塞、也不携带任何跨线程可见性；而宿主侧那十几个容器也没有并发契约。把 `start routine` 直接丢给 `std::thread` 会在这三处同时出错（而且大多**静默**）。**方案 D 绕开的正是这一整层**：不阻塞、不并发、不需要跨线程可见性，只需要"在正确的时机、用正确的 guest 上下文，调一次 `sub_4B6C30`"。

| 判定 | 结论 |
|---|---|
| 音频喂料线程 `sub_4B5AA0` 能不能真跑 | **能**。它在 0x4B5B3E 是 `retn 4`（**返回**，不是 `ExitThread`）【实测】，所以"线程结束"= start routine 返回 ⇒ 我们只需处理"返回即退出"，不必先实现 `ExitThread` |
| **能不能干脆不要这个线程（方案 D，§5.1）** | **能，而且更省**：线程体里**只有一处** guest 调用（`0x4B5B03 call sub_4B6C30`），其余 12 段（等待 / 槽分派 / 临界区 / 在飞计数 / 退出）全是 native。而 `sub_4B6C30` **本来就是主线程在调**（`0x4B74B7` / `0x4B7502` / `0x418549`）【实测】⇒ 用"泵 + 现有 `startNestedGuestCall`"替掉那一个调用点即可 |
| 内存序要不要"软模拟" | **取决于调度方式**：单令牌（任意时刻只有一个宿主线程执行 guest 代码）⇒ **零内存序模拟**且 arm64 与 x86 同结果；真并行 ⇒ 必须软模拟 x86 TSO，**这是本评估里唯一真正昂贵的部分**；**方案 D 根本没有第二线程 ⇒ 整章不适用** |
| 推荐 | **D（native override 喂料线程）优先**（11–17 人日，§5.1）；**吞吐不够再升级 C**（真线程 + 单令牌，§5.2）；D 的 P0/P1 在 C 里一字不改复用 |
| 卡点（真线程路线的唯一模型缺口） | **"可阻塞、可跨线程可见的同步对象"不存在**：`waitOne` 未 signaled 且 timeout≠0 直接 `shimAbort`；临界区跨线程持有直接 `shimAbort`；`TryEnterCriticalSection` 恒成功；线程句柄不是可等待对象 |
| 方案 D 的卡点 | **不是语义缺口，是吞吐**：一次 `sub_4B6C30` 要解 0.5 s 音频；超过一帧预算就变成"每 0.5 s 掉一帧"。**可零代码先量**（引擎已在主线程连调 3 次，§5.1 D.4-④） |
| ⛔ 不是理由 | "复杂"不是理由；上面每一条都能落到具体文件行与具体判据 |

---

## 1. 真线程需要共享 / 隔离什么

### 1.1 必须 **per-thread**（每线程一份）

| 东西 | 现状落点 | 真线程要什么 | 判据 |
|---|---|---|---|
| 通用寄存器组 / eip / eflags / 段选择子 | `X86::r[8]` `eip` `eflags` `seg[6]`（`src/runtime/x86.h:169-172`）；`Machine` 只有**一个** `cpu_`（`machine.h` 的 `X86 cpu_;` 成员） | 每线程一个 `X86` | 【实测】 |
| `fs_base` / `gs_base`（guest TEB） | `x86.h:173-174`；`prepare` 里 `cpu_.fs_base = setup_.teb`（`machine.cpp:248`） | 每线程一个 `fs_base` → 每线程一个 TEB | 【实测】 |
| x87 / MMX / XMM | `X87State fpu`、`xmm[8][4]`（`x86.h:175-181`） | 每线程（x87 是**线程私有**上下文，`fxsave` 也按线程） | 【实测】 |
| guest 栈 | 只有一份：`layout.stack_top = 0x70000000`，1 MiB（`image.h:90-92`），`machine.cpp:250-251` | 每线程 1 MiB 栈 + guard 页（`setupProcessStructures` 只建一份，`image.cpp:412-417`） | 【实测】 |
| TEB（SEH 链 / StackBase / StackLimit / Self / UniqueThread / TLS 数组指针） | 只有一份：`layout.teb_ea = 0x7FFD0000`（`image.h:95`），字段写在 `image.cpp:433-440` | 每线程一份；`UniqueThread` = 本线程 id；`ThreadLocalStoragePointer` 指向**本线程**的 TLS 数组 | 【实测】 |
| TLS **值**数组 | 进程一份：`layout.tls_array_ea`（`image.h:97`），`TlsGetValue` 直接读它（`kernel32_error_tls.cpp:119-131`） | 每线程一份数组；**`TlsAlloc` 的槽位表仍是进程级**（`kernel32_error_tls.cpp:84-100`） | 【实测】 |
| `GetLastError` / `SetLastError` | `ShimState::last_error`（`x86.h:123`）——**现在跟着机器走** | 每线程（Win32 语义就是线程局部） | 【实测】 |
| 当前线程 id / 句柄 | `ShimState::thread_id` `thread_handle`（`x86.h:124-125,130`）；`GetCurrentThreadId` 恒返回它（`kernel32_error_tls.cpp:164`） | 每线程 | 【实测】 |
| `GuestCallStack`（嵌套 guest 回调帧） | `Machine::guest_calls_`（`machine.h:211`，`guest_call.h:79-80`） | 每线程（回调帧压在**某个线程的栈**上） | 【实测】 |
| `GuestCallbackState`（`EnumSystemLocalesA` 续延） | `ShimState::callback`（`x86.h:156`） | 每线程（它是"某个线程正在枚举"的状态） | 【实测】 |
| 定时器"正在跑"的现场 | `timer_firing` / `timer_resume_ea` / `timer_saved_esp` / `timer_saved_regs`（`x86.h:160-164`） | 每线程（若允许多线程各自 `timeSetEvent`），且**必须**跟着线程走 | 【实测】 |
| 指令诊断环 / 停因 | `last_eip/last_mnemonic/last_flags/last_count`（`x86.h:200-213`）、`running/stop_why`（`x86.h:216-218`） | 每线程（否则 fault 报告指向别的线程） | 【实测】 |

### 1.2 必须 **全局（一台机器一份）**

| 东西 | 落点 | 为什么必须共享 |
|---|---|---|
| guest 4 GiB 平坦地址空间 | `GuestMemory`（`memory.h:166-276`） | guest 指针是**跨线程**的（`Sound+0x654` 槽指针就是一个线程写、另一个线程读） |
| `GuestHeap` | `heap.h:57-65`（`std::map blocks_`） | Win32 堆是进程级的；`HeapFree` 跨线程必须成立 |
| `HandleTable` | `handles.h:99-104`（`std::vector<Slot> entries_`） | 句柄表进程级 |
| `BlockCache` | `x86.h:283-328` | 代码页 / SMC 失效是**全进程**的；`installShim` 是全进程唯一地址 |
| `ShimRegistry` | `shims.h:144`（`std::vector<ShimDef> defs_`） | 导入桩地址全进程唯一 |
| `Vfs`（打开的文件 / 影子层 / 临时文件名序号） | `vfs.h` / `vfs.cpp` | 文件表进程级 |
| `ComRegistry` | `com.h:128-129` | guest 侧 COM 对象全进程可见 |
| `SurfaceArena` | `surface.h:66-69` | DDraw 表面被任何线程引用 |
| `VirtualClock` | `clock.h:14-49` | Win32 的 `GetTickCount` / `timeGetTime` 是全局时间 |
| `FontEngine` | `font.h`（FreeType face） | 字体对象进程级（且 FreeType face **不是**线程安全的）【静态推断】 |
| `WindowManager`（窗口 / 消息队列 / 计时器） | `host_window.h:159-175` | 真 Win32 里消息队列是**每线程**的；本线只有主线程有窗口 ⇒ 建议**主线程独占**（⛔ 不要做成共享） |
| D3D9 设备 / GPU 光栅 / SDL 呈现 | `present.h:7`（明写"单线程契约"）、`d3d9.h`、`gpu_raster.h` | 宿主 GPU 句柄与 SDL 设备只能一个线程碰【静态推断；SDL 线程亲和性待核】 |

### 1.3 现在**不是线程安全**的宿主结构（逐字段 / 逐容器）

| # | 结构 | 危险点（具体到字段） | 后果 |
|---|---|---|---|
| R1 | `HandleTable` | `entries_` 是 `std::vector<Slot>`，`get()` 返回 `&s.entry`（`handles.cpp:46-52` 的指针**指向 vector 元素**），`add()` 是 `push_back`（`handles.cpp:42`） | 另一线程 `add` 触发 realloc ⇒ 已返回的 `HandleEntry*` 悬垂；`kernel32_sync.cpp:385-400` 就是先取 `e` 再 `add` 的写法。而且**句柄值 = `0x100+4*size`**（`handles.cpp:36`）⇒ 创建顺序变 ⇒ **guest 可见的句柄值变** |
| R2 | `BlockCache` | `blocks_` / `shim_blocks_`（`std::unordered_map`）、`stale_`、`code_page_bits_`、`hits_/misses_` | A 线程 `cache_.get()` 拿到的 `Block*`（`machine.cpp:580`）会被 B 线程的解码插入 rehash 失效 ⇒ **use-after-free**；`stale_` 的"下一个 get 先清"会**清掉别人正在执行的块** |
| R3 | `ShimRegistry` | `defs_` 是 vector；`invoke` 里 `def.fn(...)` 可能**运行期** `addRuntime`（COM 建对象）⇒ `defs_` 重新分配；`def.called/call_count/ns` 是无保护写（`shims.cpp:313-314,339`） | `ShimDef*` 悬垂（代码里已经为单线程防过一次，见 `shims.cpp:305-310` 的注释）；计数丢更新 |
| R4 | `GuestHeap` | `blocks_`（`std::map`）、`in_use_/peak_/alloc_count_`（`heap.h:61-64`） | 两个线程同时 `HeapAlloc` ⇒ map 结构崩坏 / 双重分配同一块 |
| R5 | `GuestMemory` 的区域账本 | `commit_bits_`、`regions_`、`va_ranges_`（`memory.h:270-274`） | `VirtualAlloc`/`VirtualFree` 与别的线程 `committed()` 检查并发 ⇒ 位图撕裂 |
| R6 | `VirtualClock` | `now_100ns_`（`clock.h:46`）是普通 `uint64_t`，`tick()` 每条指令 `+=`（`clock.h:21`，`machine.cpp:674`） | 两个线程各自 tick ⇒ 丢更新（时间倒退）且**结果不确定** |
| R7 | `ShimState` | 见 §1.1——它把**进程级**（`heap_handle`/`module_handle`/`va_cursor`/`va_limit`/`exit_code`/`timers`）与**线程级**（`last_error`/`thread_id`）混在一个结构里（`x86.h:122-165`） | 复制一份 ⇒ 进程级字段分叉（两个 `va_cursor` ⇒ 两次 `VirtualAlloc` 拿同一段）；共享一份 ⇒ 线程级字段互相踩 |
| R8 | `ComRegistry` | `objects_` / `methods_` 两张 map，`com.h:32` 自己写明"没有锁" | 对象 refcount 丢更新 ⇒ 提前释放 guest 块 ⇒ 另一个线程写到已回收内存 |
| R9 | `SurfaceArena` | `surfaces_`（`std::map`）、`lock_count`（`surface.h:34,68`） | Lock/Unlock 计数错 ⇒ `texture`/`surface` 被当没锁而改写 |
| R10 | 进程级单例 | `g_active`（`machine.cpp:25`）、`g_memory` + `g_hook`（`memory.cpp:37-38`）、`presentBreakdown()`（`present.cpp:19-22`）、`g_gpu_coverage`（`d3d9_pipeline.cpp:57`）、`next_texture_serial`（`d3d9.cpp:777`，函数内 `static`）、`logConfig()`（`log.h:52-55`） | fault 报告读**主线程**的寄存器（`machine.cpp:57-70` ⇒ 线程 2 崩了会打印线程 1 的现场）；`next_texture_serial` 是 `++` 无保护 ⇒ 纹理身份撞号；`logV` 是 6 次 `fputs`+`vfprintf`（`log.h:140-154`）⇒ **一行日志被另一线程插进来** |
| R11 | `Vfs` / 文件句柄 | `vfs.cpp` 的打开表 + 影子复制 + 临时文件名序号 | 两个线程同时 `CreateFileA` ⇒ 序号/表项竞争；影子 copy-up 半写 |
| R12 | 呈现链路 | `present.h:7` 的单线程契约、`HeadlessPresenter::frame_count_`/`last_hash_`（`present.h:81-88`） | 任何线程 Present ⇒ 帧号/hash 账本竞争；SDL 事件泵必须主线程 |

> ⚠ **最容易被低估的一条是 R2 + R3**：它们不是"数据偶尔错"，而是**指针生死**问题（`Block*` / `ShimDef*`）。这类错误在 x86 上通常表现为随机崩溃，在 arm64 上可能表现为"跑通一次、下次不通"——正是"没有稳定可重现"的来源。

### 1.4 结论：共享/隔离的最小切法

```
per-thread : X86{ regs/eip/eflags/seg/fs_base/gs_base/fpu/xmm, 本线程 TEB, 
                  last_error, thread_id/handle, GuestCallStack, GuestCallbackState,
                  timer 现场, 指令环/停因 }
shared     : GuestMemory, GuestHeap, HandleTable, BlockCache, ShimRegistry, Vfs,
             ComRegistry, SurfaceArena, VirtualClock            ← 全部要一把锁（或单令牌）
main-only  : WindowManager, D3D9 设备/GPU 光栅/SDL 呈现, FontEngine
```

**关键判断**：`shared` 那一行有 9 个结构、几十个容器。"给每个都配一把锁"不是不可以，但锁序（R2 里 `cache_` 与 `heap_` 会互相嵌套：解码要提交页、分配要 guest 堆）会成为**新的、更难查的 bug 源** ⇒ 这是 §5 里推荐"单令牌"的核心动机：**一把锁，就是令牌本身**。

---

## 2. `CreateThread` 语义

### 2.1 现状（逐条，代码锚）

| 行为 | 落点 | 事实 |
|---|---|---|
| `CreateThread` 只发句柄 | `kernel32_sync.cpp:1081-1117`（日志自己写明"**不执行 start routine**"，`:1112-1116`）；`PseudoThread` 只存 `id/start/param/suspend_count`（`:146-151`） | 【实测】 |
| thread id | `0x1008` 起、+4（`x86.h:128`、`kernel32_sync.cpp:1101-1102`） | 【实测】 |
| `ResumeThread` | 递减挂起计数并返回旧值（`kernel32_sync.cpp:1119-1137`） | 【实测】 |
| `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask` / `GetThreadPriority` | 只记账（`:1139-1196`、`:1061-1076`） | 【实测】 |
| `ExitThread` / `FreeLibraryAndExitThread` / `CreateRemoteThread` / `_beginthreadex` / 定时器队列 | `addUnimplemented`（硬 trap，`:1276-1289`） | 【实测】 |
| 栈 / TEB / 参数 | **不存在**——`CreateThread` 一个字节的 guest 栈都不碰 | 【实测】 |

**引擎实际怎么用它**【实测，`.lst`】：

```
.text:004B5AA0 sub_4B5AA0 proc near          ; DWORD __stdcall sub_4B5AA0(LONG *lpThreadParameter)
.text:004B5AA5 mov  ebx, [ebp+lpAddend]      ; ← 参数 = Sound 对象
.text:004B5AB6 call j_WaitForMultipleObjects_0   ; 61 个句柄 / bWaitAll=0 / INFINITE
.text:004B5ABE cmp  esi, 3Ch                 ; index 60 = 退出事件
.text:004B5B3E retn 4                        ; ← 线程体是 **返回**，不是 ExitThread
```

⇒ 这是本评估里最有利的一条事实：**音频线程的正常结束路径不需要 `ExitThread`**，只需要"start routine 返回 ⇒ 宿主把线程标记为已退出 + 把线程句柄置 signaled"。

### 2.2 真线程需要的 guest ABI 初始化（缺一不可）

| 项 | 要做什么 | 锚 / 依据 |
|---|---|---|
| **栈** | 给每个线程提交 1 MiB + 下面 1 页 guard（照 `setupProcessStructures` 的栈做法：`image.cpp:412-417`），`esp = top - 4`，`[esp] = 线程返回哨兵` | `machine.cpp:250-251` 的主线程先例（哨兵 `0xFFFFFFFF`）；已有两个专用哨兵 EA 的先例：`kTimerCallbackReturnEa = 0x7FFEF004`（`x86.h:91`）、`kContinuationEa = 0x7FFEF008`（`guest_call.h:37`）⇒ 建议 `kThreadExitEa = 0x7FFEF00C` |
| **初始帧** | `[esp+4] = lpParameter`，`eip = lpStartAddress`，`ecx = 0` | Win32 的线程入口约定：`DWORD WINAPI ThreadProc(LPVOID)` |
| **返回** | 执行到 `kThreadExitEa` ⇒ 置 `ExitCode = eax`、线程句柄 signaled、本线程 `running = false`、令牌交还 | 【静态推断】Win32 语义：线程函数返回 ⇒ 系统调 `ExitThread(返回值)` |
| **TEB** | 每线程一份：`+0x00` SEH 链 = `0xFFFFFFFF`、`+0x04` StackBase、`+0x08` StackLimit、`+0x18` Self、`+0x24` UniqueThread、`+0x2C` 本线程 TLS 数组、`+0x30` PEB | `image.cpp:433-440` 的字段表（直接照抄即可） |
| **fs_base** | `cpu.fs_base = 本线程 TEB`（`x86.h:173`，`fs:` 前缀走它：`x86.cpp:6-7`） | 【实测】 |
| **TLS** | `TlsAlloc` 的槽位图进程级，值数组每线程；`TlsGetValue`/`TlsSetValue` 只能读**当前线程**的数组 | `kernel32_error_tls.cpp:84-146`（现在是"进程一份"，`image.h:97`） |
| **`GetLastError`** | 每线程；`SetLastError(0)` 在成功路径也要照做 | `kernel32_error_tls.cpp:119-131` 的注释（`TlsGetValue` 清 last error 的口径） |
| **`GetCurrentThreadId`** | 返回**当前**线程 id（现在恒返回 `thread_id`，`kernel32_error_tls.cpp:164`） | 【实测】 |
| **`ResumeThread` / 挂起计数** | `CREATE_SUSPENDED(0x4)` ⇒ 计数 1 且**不运行**；计数降到 0 才可被调度。现在只减数字（`kernel32_sync.cpp:1132-1136`） | 【实测】 |
| **`ExitThread`** | 仍可不做（音频线程不用），但**一旦真线程跑起来，"主线程调 ExitThread" 必须至少是响亮失败而不是"停机器"** | `kernel32_sync.cpp:1280-1281` |
| **句柄语义** | `HandleKind::Thread` 要成为**可等待对象**：退出即 signaled | `handles.h:16` 已有 `Thread` 种类；等待侧不认识它（见 §2.3） |

### 2.3 `WaitForMultipleObjects` / 事件 / 临界区 怎么跟宿主线程互操作

这是**整个评估的核心**。现状三条硬约束：

1. **等待 = 硬错误**：`waitOne` 在"未 signaled 且 `dwMilliseconds != 0`（含 INFINITE）"时 `shimAbort`（`kernel32_sync.cpp:670-677`）；`waitMultiple` 同（`:747-751`）。SCOPE §1 把这条登记为"硬错误"。
2. **临界区 = 记账，不是锁**：`enterCriticalSection` 只改 guest 里的 `RecursionCount`/`OwningThread`（`kernel32_sync.cpp:292-306`）；**拥有者不是本线程 ⇒ `shimAbort`**（`:300-304`）；`TryEnterCriticalSection` **恒成功**（`:350-356`）。
3. **线程句柄不是同步对象**：`syncOf` 只认 Event/Semaphore/Mutex（`kernel32_sync.cpp:200-220`）⇒ `WaitForSingleObject(thread, 3000)`（引擎关机路径，`.lst 0x4B6522`）现在返回 `WAIT_FAILED`。

真线程下的正确互操作（**用宿主原语，不自己造**）：

| guest 原语 | 宿主实现（真线程） | 注意 |
|---|---|---|
| `WaitForSingleObject` / `WaitForMultipleObjects` | `SyncObj` 加 `std::mutex` + `std::condition_variable` + `signaled/count`；等待 = `cv.wait(pred, timeout)`；**等待期间必须交还令牌** | 自动复位事件的"消费"必须与"判定 signaled"在**同一把锁**里完成，否则两个等待者会同时被唤醒 |
| `SetEvent` / `ResetEvent` / `PulseEvent` | 持锁改状态 + `cv.notify_all()`；`PulseEvent` 才第一次有真语义（现在按"丢失"处理，`:468-490`） | 跨线程可见性由 mutex 提供（arm64 也成立） |
| `EnterCriticalSection` / `LeaveCriticalSection` | 宿主 `std::recursive_mutex`（15 个槽 × 每槽一把，`.lst 0x4B5B50` 的 ctor 建 15 个 CS） | guest 里的 `RecursionCount/OwningThread` 仍要写（guest 自己会读），但**判据变成宿主锁**；`owner != 本线程` 不再是 `shimAbort` 而是**等待** |
| `TryEnterCriticalSection` | `try_lock()`，失败返回 FALSE（现在是恒 TRUE，`:355`） | 这条不改 = 互斥被破坏 |
| `Interlocked*` / `lock` 前缀 | 必须真原子（见 §3） | 现在是 `rd + wr`（`kernel32_error_tls.cpp:185-222`；`exec_integer.cpp:406-428`） |
| 线程句柄等待 | 线程退出时置 signaled + `notify`；`syncOf` 扩一个 `kSyncThread` | 否则关机路径（`.lst 0x4B6522`）永远返回 `WAIT_FAILED` |
| `Sleep` | 真线程下可以真睡（或推进虚拟时钟 + 让令牌） | 现在"不睡只推钟"（`kernel32_sync.cpp:1199-1209`）在单线程下是对的 |

### 2.4 只让音频喂料线程真跑：最小改动清单

前提：**DS 设备模型（位置通知）必须先有**，否则线程等的事件永远没人投递（`SetNotificationPositions` 现在是空实现，`dsound.cpp:339-342`）——见 `docs/audio-feasibility.md` §3.6 的 P1。

按依赖排序（每项都必要）：

1. `X86` 拆成"每线程实例"，`ShimState` 拆成 `ThreadState`（`last_error`/`thread_id`/TEB/`fs_base`/TLS 值数组/回调帧）+ `ProcessState`（`va_cursor`/`heap_handle`/…）。
2. `CreateThread`：分配 guest 栈 + TEB（照 §2.2），登记 `HandleKind::Thread` payload，`CREATE_SUSPENDED` 期不启动。
3. 调度：**单令牌**（任意时刻只有一个宿主线程执行 guest 代码）+ 固定量子（每 N 条指令或每 M 个块强制换手）+ **所有阻塞 shim 换手**。
4. 同步：CS/Event/Semaphore/Mutex/Wait 换成宿主 `mutex`/`cv`（§2.3 表），**线程句柄可等待**。
5. 原子：`Interlocked*` 与 `lock` 前缀真原子（§3.4）。
6. 只读主线程：`WindowManager` / D3D9 / GPU / SDL / `FontEngine` 断言"当前令牌持有者是主线程"，否则响亮失败。
7. 记账：`data/lift/overrides.json` 的 `kernel32!ExitThread` 等条目要按新口径改（`data/lift/overrides.json:209-227`），`tools/lift.mjs` 的白名单口径跟着改。

---

## 3. 内存序

### 3.1 x86 guest 期待什么，本模拟器现在给了什么

**x86（TSO）的合同**【静态推断，Intel SDM】：
- 访存顺序：**Store→Load 可重排**；Load→Load、Load→Store、Store→Store **不重排**。
- 原子性：对齐的 1/2/4 字节访问是原子的；8 字节在 32 位模式下**不保证**（`fild qword` / SSE `movq`）。
- `LOCK` 前缀：把该 RMW 变原子 **并**当**全屏障**（≈ seq_cst）。
- `Interlocked*` API：原子 + 全屏障。
- `EnterCriticalSection` / `LeaveCriticalSection` / `SetEvent` / `WaitFor*`：MSVC 文档口径是**全屏障**（在 x86 上由 `lock`/`xchg` 实现）。
- ⚠ 还有一条**很容易漏**：MSVC 的 `volatile` 在 x86 上默认 `/volatile:ms`（acquire/release + 不重排）⇒ guest 代码可能用**光秃秃的 `mov`** 做跨线程标志，没有任何 `lock`/`Interlocked` 痕迹。【静态推断；若要确认得看 guest 里 `volatile` 字段的用法，待核】

**本模拟器现在给的**【实测】：
- 所有访存 = `memcpy` 到平坦 4 GiB buffer（`memory.h:209-254`），**没有一处 `std::atomic`**（`src/` 下 `thread_local` 只有一处：`g_access`，`memory.h:43` / `memory.cpp:34`）。
- `lock` 前缀**被解码但只用来拼助记符**：`decode.cpp:324` 存 `ins.lock`，`decode.cpp:219-226` 只拼 `"lock xadd"` 这类名字；`exec_integer.cpp:406-428` 的 `xadd`/`cmpxchg` 就是"读-算-写"。
- `Interlocked*` = `rd + wr`（`kernel32_error_tls.cpp:185-222`），注释自己写明"单线程 guest ⇒ 普通读改写即等价"。
- 临界区 = 只改 guest 字段（`kernel32_sync.cpp:292-306`），**不产生任何宿主屏障**。
- 结论：**当前的内存序语义 = "只有一条执行流"**。这条前提一旦撤掉，语义为 0。

### 3.2 宿主 x86 vs 宿主 arm64：分别漏什么

| 语义 | x86 宿主 + 无 GIL | arm64 宿主 + 无 GIL |
|---|---|---|
| Store→Store 保序（TSO 要求） | **硬件本来就保**（TSO 是 x86 的模型） | ⚠ **漏**：ARM 允许 W→W 重排 ⇒ 别的线程可能先看到 B 后看到 A |
| Load→Load 保序 | 硬件保 | ⚠ **漏**：ARM 允许 R→R 重排（用 `mov` 读两个字段的顺序会变） |
| Load→Store 保序 | 硬件保 | ⚠ **漏** |
| Store→Load 不保序（TSO 允许） | 硬件确实允许（正是 TSO 的特征） | 硬件也允许 ⇒ 这一项**反而一致** |
| 对齐 4 字节访问的原子性 | 硬件是原子的（但 C++ 层面仍是 data race / UB；编译器有权拆分或合并，`memcpy` 尤其） | 单次 `ldr/str` 是原子的；但编译器同样可能拆分；**对齐 8 字节不保证** |
| `LOCK` / `Interlocked` 的原子 RMW | ❌ **漏**：两个宿主线程各自 `rd+wr` ⇒ 丢更新（x86 上也是错的，只是更"像能跑"） | ❌ 漏，同上 |
| 编译器层面的重排/缓存 | ⚠ 编译器可以跨 `memcpy` 合并/下沉（LTO 下更可能）；标准层面就是 UB | 同左，且更凶 |
| `volatile`（若被用来当同步） | 对 `-O2` 也只约束"这一个访问"，不产生 `mfence` | ⚠ 完全不产生 `dmb`/`ldar`/`stlr` ⇒ 跨线程看不到 |

⇒ **一句话**：x86 宿主会"掩盖"大部分顺序错误（因为硬件替你保了 TSO），只在**原子 RMW**上出错；arm64 宿主会把顺序错误、原子性错误**一起**暴露，而暴露方式多半是**数据看着像对的、偶发一次错的**。

### 3.3 `std::atomic` / acquire-release / seq_cst 能不能表达？`volatile` 为什么不够

**能表达的**（我们自己在 shim 里实现的那部分）：
- `SyncObj::signaled/count` → `std::atomic<uint32_t>` 或（更好）整把 `std::mutex` 保护。
- 令牌（GIL）→ `std::mutex` + `std::condition_variable` 或 `std::atomic<uint32_t>` + `atomic::wait`（C++20）。
- 线程"已退出"标志 → `std::atomic<bool>` + `notify_all`。
- 虚拟时钟 → `std::atomic<uint64_t>`（若要无锁读）。
- 这些都是**有界的、少数几个对象**，用 acquire/release 就够（seq_cst 只在调试期用来定位）。

**不能表达的**（guest 的普通 `mov`）：
- 你不能把 4 GiB 平坦 buffer 的**每一次**访存都变成原子：`std::atomic_ref` 要求对齐、且 guest 有大量非对齐访问（`memory.h` 的 `rd32` 不做对齐检查）。
- 即便全变成 seq_cst 原子，也只是"每条访问都有屏障"，**不是 TSO**：TSO 允许 Store→Load 重排，seq_cst 不允许 ⇒ 语义**过强**，会把 guest 里依赖"我自己的 store 还没被别人看见"的无锁模式变成不一致（少见但存在）。
- 所以真并行下只有两条路（§3.4 的 B 案）：**store buffer 软模拟** 或 **每条访问带屏障**（选一种，都要接受语义近似 + 性能代价）。

**`volatile` 为什么不够**（四条，逐条可核）：
1. 它**不是原子**：`volatile uint64_t` 的读在 32 位宿主上是两次 `ldr`，中间可以被另一个线程插进来。
2. 它**不产生屏障**：GCC/Clang 在 arm64 上对 `volatile` 访问不加 `dmb`/`ldar`/`stlr` ⇒ 仍然可以乱序可见（这是与 MSVC x86 `/volatile:ms` 的关键差异）。
3. 它**不约束非 volatile 访问**：编译器和 CPU 都可以把相邻的普通访存搬到它前面/后面。
4. 在本模拟器里**根本不成立**：guest 访存走的是 `memcpy`（`memory.h:221-254`），`volatile` 一个都没加；改成 `volatile` 也只是让**编译器**别缓存，跨线程可见性仍然是 0。

### 3.4 两套**可执行**的内存序规则

#### 规则集 A（推荐）：**单令牌（GIL）**

> 不变式：**任意时刻最多一个宿主线程在 guest 代码里**。令牌通过 `std::mutex` 交接。

| 操作 | 规则 |
|---|---|
| 普通读 / 写 | **不需要任何原子或屏障**。因为不变量保证没有并发访问者；跨线程传递发生在令牌交接处 |
| 令牌交接 | 必须走 `std::mutex`（或 `atomic` 的 acquire/release）—— **这是唯一的跨线程同步点**，也是 arm64 上唯一需要正确的地方 |
| 换手点（必须确定性） | ① 每 N 条 guest 指令（固定 N）；② 每 M 个 basic block（固定 M）；③ 所有阻塞 shim（`WaitFor*`/`Sleep`/`EnterCriticalSection` 争用）；④ 线程启动/退出 |
| `LOCK` / `Interlocked*` | 保持"读-算-写"，但**在 RMW 期间不许换手**（否则丢更新）。⇒ 反而**不需要**真的原子指令 |
| `EnterCriticalSection` | 换成宿主 `recursive_mutex`；争用时**让出令牌**（不是 `shimAbort`）。CS 内的 guest 字段仍照写 |
| `SetEvent` / `WaitFor*` | 宿主 `mutex`+`cv`；在 `cv.wait` 前让出令牌，被唤醒后重新拿令牌 |
| 线程句柄 signaled | 原子置位 + `notify`，等待者由 `cv` 唤醒 |
| 虚拟时钟 | 只有一个线程在跑 ⇒ 不需要原子；但**跨线程读时间**（`nowMs`）要拿令牌或改成原子（建议原子） |
| 调试断言 | `LIFT_THREAD_DEBUG=1` 时 `GuestMemory::rd*/wr*` 断言"当前令牌持有者 == `std::this_thread::get_id()`" ⇒ **把不变量变成会红的断言** |

**代价**：不是真并行（音频线程 CPU 时间与主线程串行），但音频这一侧 99% 时间在 `WaitForMultipleObjects` ⇒ 串行完全够。
**收益**：**arm64 与 x86 行为一致**，因为不存在并发访存；不需要 store buffer 模拟；宿主结构一把锁就够（R1–R12 全部消失）。
**代价 2（必须接受）**：换手点是**指令计数**而不是墙钟 ⇒ 与真机的抢占点不同（这与今天"定时器只在块边界触发"是同一种偏差，`x86.h:89` 已登记）。

#### 规则集 B（真并行，只在"确实需要并行"时才做）

| 操作 | 规则 |
|---|---|
| 普通读 / 写 | 必须**可选地**走 `std::atomic_ref<uint32_t>`（对齐时）或"store buffer"；不能留裸 `memcpy` |
| 顺序 | 每条 store 之后要 release 语义、每条 load 之前要 acquire 语义 —— 即在 arm64 上 `str`/`ldr` 换成 `stlr`/`ldar`；x86 上本来就是 TSO，**零成本** |
| TSO 的 Store→Load | 只有它需要**额外**模拟：per-thread store buffer（写先入队，读时**不看**自己的队列之外的东西，队列在 fence/lock/syscall/量子末按序排空） |
| `LOCK` / `Interlocked*` | 必须真原子（`atomic_ref` 或宿主锁），并且是**全屏障** |
| `EnterCriticalSection` | 宿主 `recursive_mutex`（本身带 acquire/release） |
| `SetEvent` / `WaitFor*` | 宿主 `mutex`+`cv`（本身带 acquire/release） |
| 32 位 guest 的 8 字节访问 | 必须走"锁 或 双字读-校验-重试"，**不能**用 `memcpy` |
| `volatile` | 一律不作为同步手段 |

⇒ 规则 B 的工作量集中在两件：**(a) 把每条访存变成原子/带屏障（性能 + 非对齐问题）；(b) store buffer 模拟（正确性 + 可验证性）**。这是"软模拟部分内存序"的真实含义，也是本评估里唯一"研究级"的部分。

### 3.5 arm64 弱序下最容易**静默**出错的三个场景（本模拟器具体路径）

**S1 · "填数据 → 置标志"的发布模式（音频环）**
- 路径：主线程 `sub_4B73E0` 起播 → `Lock/Unlock` 写入 → 置 `[stream+0x2460]`（在播）/ 槽指针 `[Sound+0x654+4*slot]`；音频线程 `sub_4B5AA0` 在 `0x4B5ADD` 读 `[Sound+0x654+4*slot]`、在 `0x4B5AF8` 取 `SoundBuffer`、`0x4B5B03` 调 `sub_4B6C30` 写环。
- 现在这两侧靠 `EnterCriticalSection` 互斥（`.lst 0x4B5AD7` / `0x4B6570`），但**我们的 CS 只改 guest 字段**（`kernel32_sync.cpp:292-306`）⇒ 在 arm64 上没有任何 acquire/release：音频线程**可能看到槽指针已发布、但槽里的字段是旧的**。
- 后果：不是崩，是**偶尔播一段错的 PCM / 拿错 stream**。x86 宿主上不会出现（TSO + 强 store 顺序）⇒ 典型的"只有 mac arm64 偶发"。

**S2 · 计数类 RMW（在飞计数）**
- 路径：`0x4B5AF2 call InterlockedIncrement` / `0x4B5B0C call InterlockedDecrement`（`.lst`，**是导入调用**，不是 `lock inc`）→ shim 是 `rd+wr`（`kernel32_error_tls.cpp:185-197`）。主线程侧同类：`.lst 0x4B6570 / 0x4B73E0 / 0x4B6B60`。
- 无 GIL 时两个线程各自"读-加-写"⇒ 丢更新 ⇒ 在飞计数永久 >0 ⇒ 槽永远"有人正在用" ⇒ 换缓冲/停止路径的判断错。x86 与 arm64 **都会错**，但只在换缓冲时偶发。
- 另外 guest 里还有 **20 条 `lock` 前缀指令**（全在 CRT/STL 区：`0x480038`…`0x483668`、`0x4EC80C`…`0x512B54`，**没有一个在 `0x4B5xxx–0x4B7xxx`**）【实测】——这些同样是 `lock xadd`，我们的 `exec_integer.cpp:406-414` 照样会丢更新。

**S3 · 多字搬运被对面看见一半**
- 路径：`Lock` 拿到 guest 指针后引擎 `memcpy` PCM（`.lst 0x4B6FBD` 一带的 `memset`/`memcpy`；native override 里也有 `memcpy`，`native_overrides.cpp:189`），另一侧（宿主音频回调或另一 guest 线程）按"整段已就绪"去读。
- 在 x86 宿主上：4 字节对齐 store 原子 + store 保序 ⇒ 通常不会看到撕裂；**arm64 上**：编译器可以把 `memcpy` 展开成多条 `str` 并在中间插入别的访存，硬件也可以乱序可见 ⇒ 读到**半新半旧**的 PCM/结构体。
- 后果：波形轻微失真 / 结构体字段互相矛盾——**最难复现**的一类。

> ⚠ 三个场景的共同点：**都不会崩、都不会打日志、都只在 arm64（或高负载）偶发**。这就是"没有稳定可重现"的成因。

---

## 4. 可重现性口径

### 4.1 现在**还要求**确定性的是哪些（不能碰）

| 判据 | 位置 | 要求 |
|---|---|---|
| **A4** boot trace | `tests/boot_trace.cpp`（assets 档，`CMakeLists.txt:402-404`） | 同一条命令跑两次：`reason`/`eip`/`insns`/`hash` 相同；**shim trace 逐字节相同**（`boot_trace.cpp:218-264`）；并且 `LIFT_CHECK(!a.real_time)`（`:198`）——**虚拟时钟是判据的一部分** |
| headless / CPU 光栅 | `main.cpp:351-356`（`--present headless` ⇒ `--raster auto` 选 CPU） | 像素逐字节（`SCOPE.md:33` 的 GPU 偏差只对 `--raster gpu` 开口） |
| `--max-frames` / `--dump-frame` | `x86.h:150-153`（`ShimState::frame_dump` / `max_frames`）、`d3d9.cpp:674-678` | 帧号 → 文件名的对应必须稳定 |
| 全部 pure CTest | `CMakeLists.txt:145-378` | 不接宿主时钟；`RunOptions::real_time` 默认 false（`machine.h:58`） |
| trace 输入清单 | `TraceInputs`（`machine.h:69-84`）、`traceInputsHash()`（`machine.cpp`，快照 `:852-874`） | 输入变了必须换 hash |

### 4.2 真线程应该**只在窗口 / real-time 下启用**

理由（三条，都是现状事实）：

1. **窗口模式已经自动 real-time**：`opt.real_time = real_time || present_window_active`（`main.cpp`，快照 `:720`），并**主动打印跳过 A4 的声明**（`main.cpp`，快照 `:726-731`）。A4 已经在那里不可用 ⇒ 把"非确定"的东西放进去**不新增损失**。
2. **headless 的 `--max-frames` / CTest / boot-trace 是判据本身**，它们跑在虚拟时钟上；一旦有第二个宿主线程，`insns`（`x86.h:217`）与 `clock_.tick()`（`machine.cpp` 指令循环，快照 `:711-712`）就成了共享可变状态，`--max-insns` 的停止点不再确定。
3. **guest 可见值会被线程顺序污染**：句柄值 = `0x100+4*size`（`handles.cpp:36`）、伪线程 id（`x86.h:128`）、guest 堆 EA（`heap.cpp`）、`next_texture_serial`（`d3d9.cpp:777`）——只要创建顺序变，**guest 能看到的值就变**。headless 的逐字节判据直接失效。

⇒ **闸门**（建议实现为硬校验，与 `main.cpp` 现有的参数互斥校验块（快照 `:327-349`）同一处）：

```
--threads <off|audio|full>     默认 off
--threads audio|full  要求 --present window（或 --real-time）且 --real-time 真的生效
否则 ⇒ 直接报错退出（⛔ 不静默降级）
```

★ **若采用方案 D（§5.1）**：开关是 `--audio-feeder <off|pump>`（默认 off），**不要求** `--present window` —— 因为泵的触发点是"虚拟时钟 + 块边界"，在 headless 下**仍然确定**。这时 §4.2 的三条理由里只有第 3 条（guest 可见值不受顺序污染）需要照顾，而 D 天然满足（没有并发）。⇒ D 的闸门只有一条：`--audio-feeder pump` 必须进 `TraceInputs`（否则"泵开/关"两种跑法的输入 hash 相同）。

### 4.3 怎么把"非确定模式"**显式化**

必须四处同时做（缺一条就会有"看起来可比、其实不可比"的跑法）：

| 落点 | 要做的事 | 为什么 |
|---|---|---|
| **CLI** | `--threads off|audio|full`（默认 `off`）**或** `--audio-feeder off|pump`（默认 `off`，方案 D）；与 `--present headless` / `--smoke` / `--report` 互斥校验，放在 `main.cpp` 的参数互斥校验块（快照 `:327-349`） | 现在那里已经有 `--present headless` 不接受窗口参数的先例 |
| **`TraceInputs`** | **必须加字段**（`threads_mode` / `audio_feeder`），并进 `traceInputsHash()`（`machine.cpp`，快照 `:852-874`） | 否则"单线程跑"与"真线程跑"的输入 hash **相同** ⇒ A4 的输入清单说谎。这是最容易漏的一条 |
| **日志** | 启动时打一行 `threads : audio (REAL-TIME, 逐字节断言不可用)`；`dumpRunSummary` 里也带（照 `dumpTraceInputs` 里 `real_time` 那行的做法，快照 `machine.cpp:846`） | 与 `REAL-TIME（⚠ 逐字节断言必须跳过）` 同形，人在日志里就能看出这一轮不可比 |
| **守卫跳过** | `boot_trace.cpp:198` 那类断言扩成"模式不接受"：如果 `threads != off` ⇒ **红**（不是 skip），因为 boot-trace 的职责是钉住确定模式 | 跳过要显式（`t.skip`/`SKIP_RETURN_CODE 77`），⛔ 不许静默 pass |

### 4.4 会**静默**破坏 A4 的三种写法（反模式）

1. **trace 交错**：`shims.cpp:343-351` 是多次 `fprintf` 到同一个 `FILE*`（`trace_out_`）；两个线程各打一半 ⇒ trace 字节不同但**两条 run 都"成功"**。⇒ 要么单令牌（自然串行），要么真线程下**拒绝** `--trace-shims`。
2. **量子用墙钟**：若"每 X ms 换手"，同一份输入两次运行的换手点不同 ⇒ `insns` 与 trace 都变。⇒ **量子必须是指令数/块数**。
3. **宿主调度器语义泄漏进 guest**：例如用 `std::thread::hardware_concurrency()` 决定线程数，或让 OS 决定谁先拿到锁 ⇒ 句柄值/堆 EA 变化（§4.2 第 3 条）。⇒ 交棒顺序必须是**确定性轮转**（按线程创建序），不是 OS 的锁公平性。

---

## 5. 方案对比

### 5.1 方案 D：完全 native override 喂料线程（用户追加问题）

> 问题原话：这个音频喂料线程本质上只是"取数据 + 填充音频输出"，是一个相当闭环的消费者线程；是否可以直接**完全 override 掉**，不需要真线程？

**结论先说**：**能**，而且**比真线程（C）少整整一层**——但"完全 override"只对**控制流**成立，**数据路径（`sub_4B6C30`）必须留给 guest**。理由不是保守：`sub_4B6C30` 里包着三样只有 guest 才有的东西——**解码器对象**（`WavePCM`/`OggVorbis`/`OggStream` 的 vtable 协议）、**引擎自己的环写指针算术**、以及**引擎自己的状态字**（`+0x2450/+0x2458/+0x2468/+0x2470/+0x244C`）。

而 D 最大的有利事实是：**`sub_4B6C30` 本来就是主线程在调的**——全 `.lst` 四个调用点里有 **三个**在主线程【实测】：

| 调用点 | 谁在调 | 场景 |
|---|---|---|
| `.text:0x4B74B7` | `sub_4B73E0`（主线程） | 起播预填：`for (i=1; i<4; ++i) sub_4B6C30(stream, i)` ⇒ 3/4 圈 |
| `.text:0x4B7502` | `sub_4B73E0`（主线程） | seek/起播后按 `[+0x246C]/[+0x444]` 算圈数补齐 |
| `.text:0x418549` | `sub_418520`（主线程，脚本侧） | `sub_4B6B60`(stop) 后循环 `n` 次 `sub_4B6C30(stream, 1)` |
| `.text:0x4B5B03` | `sub_4B5AA0`（音频线程） | 正常播放的持续喂料 ← **只有这一个需要被替代** |

⇒ **D 不是"发明一种新的调用方式"，而是"把第 4 个调用点从'另一个线程'搬到'主线程的确定性边界'"**。

#### D.1 它到底做什么（逐段 EA；哪些可 native、哪些必须 guest）

`sub_4B5AA0` 的循环体（`.lst 0x4B5AA0–0x4B5B3E`）【实测】：

| 段 | EA | 内容 | D 里归谁 |
|---|---|---|---|
| 取参数 | `0x4B5AA5` | `ebx = [ebp+8]` = Sound*（`lpParameter`） | **native**（`CreateThread` 记下来的 param） |
| 等待 | `0x4B5AA9–0x4B5AB6` | `WaitForMultipleObjects(0x3D, Sound+0x424, FALSE, INFINITE)` | **删掉**——泵不需要等；"该填了"由设备模型告诉泵 |
| 退出判据 | `0x4B5ABE–0x4B5AC1` | `cmp esi,3Ch; jz 退出`（index 60 = 退出事件） | **native** |
| 槽号 | `0x4B5AC4–0x4B5AC6` | `edi = esi >> 2` | **native**（同一条公式） |
| 取 CS | `0x4B5AC9–0x4B5AD4` | `CS = Sound + 0x690 + 24*slot`（`(slot*3+0xD2)*8`） | **native**（同一地址公式） |
| 进临界区 | `0x4B5AD7` | `EnterCriticalSection(CS)` | **native**（以 feeder 身份调 shim / 或直接写 guest 字段；见 D.4-③ 的 defer 规则） |
| 槽空跳过 | `0x4B5ADD–0x4B5AE5` | `if ([Sound+0x654+4*slot] == 0) skip` | **native**（读 4 字节 guest 内存） |
| 在飞计数 ++ | `0x4B5AE7–0x4B5AF2` | `InterlockedIncrement(Sound+0x7F8+4*slot)` | **native**（单执行流下就是 `+1`；但必须与 `--` 成对、且在 CS 内） |
| 取 stream | `0x4B5AF8` | `ecx = [Sound+0x654+4*slot]` | **native** |
| 季度号 | `0x4B5AFF` | `esi &= 3` | **native** |
| **真正的活** | `0x4B5B03` | `call sub_4B6C30`（thiscall：ECX=stream，栈 1 个参数） | **必须 guest** ⇒ 用现成的 `startNestedGuestCall(cpu, 0x4B6C30, {quarter}, 4, "audio:feeder", cont, &err, /*callee_ecx=*/stream)`（`native_overrides.h:242`、`native_overrides.cpp:19-29`） |
| 在飞计数 -- | `0x4B5B08–0x4B5B0C` | `InterlockedDecrement` | **native** |
| 出临界区 | `0x4B5B12–0x4B5B16` | `LeaveCriticalSection` | **native** |
| 循环 / 退出 | `0x4B5B1C–0x4B5B3E` | 回到等待 / `xor eax,eax; retn 4` | **native**（退出 = 句柄 signaled，`ExitCode=0`） |

**注意**：`0x4B5B03` 那一行是整个线程体里**唯一的 guest 调用点**。也就是说"把线程 override 掉"= 把 12 段 native + 1 段 guest 重新排成一个泵。

`sub_4B6C30` 内部（`.lst 0x4B6C30–0x4B7030`）【实测】为什么不能顺手 native：

| 段 | EA | 做什么 | 能否 native |
|---|---|---|---|
| `/GS` 帧 | `0x4B6C33`, `0x4B6C39-40`, `0x4B6C63-68` | 0x42C 字节局部 + 栈 cookie（`sub_4E73EB` = `__security_check_cookie`） | guest 自己做；⇒ **我们铺帧必须正确**（`cleanup_bytes=4`），否则看到的是"栈溢出"而不是"约定错" |
| 停机分支 | `0x4B6C49–0x4B6C70` | `if ([+0x2458]==0) { sub_4B6B60(this); return 1; }` | 可以照抄，但 `sub_4B6B60` 是 guest（Stop/状态/错误串）⇒ 不必抄 |
| 算跨度 | `0x4B6C73–0x4B6CB7` | `prev=(q+3)&3`；`size = off[q]-off[prev]; if(size<0) size += [+0x444]` | 公式很短，**可以 native**，但会与 guest 的 `off[]` 表脱钩 ⇒ 不建议 |
| **Lock** | `0x4B6CBD–0x4B6CEA` | `[stream+0x40C]` 的 vtable **+0x2C**（8 参数） | **已是我们**（`dsound.cpp:984-1055`）；D 不需要动 |
| 丢缓冲 | `0x4B6CF4–0x4B6D7F` | `HRESULT==0x88780096` ⇒ `Restore`（+0x50）⇒ 重试 Lock | guest 控制流 + 我们的 shim |
| 填静音 | `0x4B6D91–0x4B6DEA`、`0x4B6FB5–0x4B6FF0` | `[+0x2458]>=2` 或 `[+0x2450]==0` ⇒ `memset(p, [2454]==8?0x80:0, size)` | guest（它同时改 `[+0x2458]`） |
| 残块 | `0x4B6E6F–0x4B6EA7` | `[+0x244C]` 的残余 `memcpy` | guest（跨调用保留的状态） |
| **解码器** | `0x4B6ED0–0x4B6F09`、`0x4B6F4F–0x4B6F70` | `[stream+0x247C]` 的 vtable：+0x08 取状态 / +0x10 剩余长度 / +0x0C 数据指针 / +0x14 前进（重复点） | **必须 guest**（三个容器类 + 各自的解码状态） |
| **Unlock** | `0x4B7002–0x4B7024` | `++[+0x2468]`；`[stream+0x40C]` 的 vtable **+0x4C** | **已是我们**（`dsound.cpp:1057-1103`） |

⇒ **必须 guest 的只有"解码器 + 环写序"这一坨**（`0x4B6ED0` 附近的 6 条 `call [vtable+N]` + 三处 `memcpy` + 状态字）。Lock/Restore/Unlock 本来就在我们的 shim 里；61 事件、4 通知、槽分派、在飞计数、线程生命周期**全是纯 native**。

#### D.2 D 的形态（三层 + 一个泵）

```
层 1（纯 native）  AudioDevice      每缓冲：bytes / fmt / 4 个 (offset,event) / play_cursor / state
                   AudioFeeder      61 事件簿记、槽分派、在飞计数、线程句柄/挂起计数/退出
层 2（必须 guest）  一次 sub_4B6C30 调用（thiscall，ECX=stream，1 个栈参数 quarter）
层 3（可选，不做）  native 解码器 = 音频报告 §3.1 的路径 A（10–20 人日，语义更假）
```

**泵的驱动点（全部确定性）**：

1. dispatcher 的**块边界**（照 `fireDueTimer` 的形状，`machine.cpp:551-554`）：`if (feeder.wants()) feeder.pump();`
2. **`WaitForSingleObject(<feeder 线程句柄>)` 内部必须 pump 到退出**（见 D.4-⑤：否则 `Release pDS`（`0x4B6534`）之后泵还在写 = guest 侧 UAF）。
3. 可选：长 shim（`Present` / `GetMessageA`）前后各一次（只为减少"该填了但还没到块边界"的延迟）。

**一次泵 tick（伪代码）**：

```
while (audio.nextPending(&slot, &quarter)) {         // 由设备游标越过通知点产生
    if (read32(Sound + 0x654 + 4*slot) == 0) { consume(); continue; }   // 槽空
    if (csOwnedByOther(CS(slot), feeder_id)) break;   // ★ defer（见 D.4-③）
    enterCs(CS(slot), feeder_id);                     // guest 可见字段照写
    ++inflight[slot];                                 // read32(Sound+0x7F8+4*slot) += 1（成对）
    startNestedGuestCall(0x4B6C30, {quarter}, cleanup=4, ecx=stream, cont);
    //  ← 这里回 dispatcher；guest 返回后落到 kContinuationEa，续延里 --计数 / LeaveCS / 继续 while
}
```

"铺帧 → 回 dispatcher → 落到续延"这条路径**已经存在且被用了四处**：`GuestCallStack::start`（`guest_call.h:44-50`）、续延桩 `kContinuationEa = 0x7FFEF008`（`guest_call.h:37`）、dispatcher 的续延分支（`machine.cpp:483-491`）、以及"同一次决定点里连调 N 次"的多回调枚举（`guest_call.h:12-16`）⇒ 一个泵 tick 可以连着填多个 1/4 圈，不需要新机制。

**必须做的一件事：整份 guest 上下文交换**（D 的头号静默风险，见 D.4-①）。泵前后要换：`X86::r[8]` / `eflags` / `fpu`（`X87State`，`x86.h:43-57`）/ `xmm[8][4]` / `eip` / `esp` / `fs_base` / `shim.thread_id` / 指令环。⛔ 只存 8 个 GPR 不够——`TimerSavedRegs`（`x86.h:110-119`）就是只存 GPR 的先例，而定时器回调**不跑 Ogg 解码**。
（真实线程里这件事由 OS 免费做；D 里是一段 ~200 B 的 memcpy 级交换。）

**线程状态怎么表达**：

| 事件 | 处理 |
|---|---|
| `CreateThread(start=0x4B5AA0, param=Sound*, flags=4)` | 记 `start/param/id/suspend=1`；**不建线程**（`kernel32_sync.cpp:1081-1117` 的 `PseudoThread` 结构直接复用，只加一个 `runnable` 位） |
| `ResumeThread` | 挂起计数 1→0，`runnable = true`（`0x4B5FF1`） |
| 泵发现 `idx == 60`（退出事件） | `exited = true; exit_code = 0;` 线程句柄 → signaled |
| `WaitForSingleObject(handle)` | 未 exited ⇒ **pump 到 exited**（幂等），返回 `WAIT_OBJECT_0`（`0x4B6522`） |
| `GetExitCodeThread` | 已实现则返回 `exit_code`；引擎不调 ⇒ 可后补 |

**最小可行子集**（建议按这个顺序验收）：

| 里程碑 | 内容 | 机械判据 |
|---|---|---|
| **D1**（不调 guest） | 设备模型 + 4 通知 + 61 事件簿记 + 线程/句柄/退出 | 事件**次序与次数**正确；`WaitForSingleObject(thread,0)` = `WAIT_OBJECT_0`；环内容仍全 0（= 与今天一致，证明"只换了控制流、没动数据"） |
| **D2**（调 guest） | + `sub_4B6C30` 嵌套调用 + 上下文交换 + defer | `Unlock` 记到**非 0** 字节；环逐字节 = 已知 PCM（斜坡） |
| **D3**（可选） | SDL 音频输出（宿主回调只写**原子游标**，事件仍由 dispatcher 投递——音频报告 R2） | `--audio-dump` RMS>0 且无 underrun |

#### D.3 guest 可见状态：必须对齐 vs 可登记偏差

| 项 | 必须对齐？ | 说明 |
|---|---|---|
| `sub_4B5AA0` 的返回值（0） | ❌ 可偏差 | 只有 `GetExitCodeThread` 会读；引擎不读 |
| `[Sound+0x7F8+4s]` 在飞计数 | ✅ **必须**（最终值） | 换缓冲 / 停止路径会读；泵必须**成对**增/减，且在 CS 内 |
| `[Sound+0x654+4s]` 槽指针 | ✅ **必须**（只读） | 泵判"槽空"用它；⛔ 泵**不清空**它 |
| 61 个事件的状态 | ✅ **必须**（不残留） | 线程体不跑了 ⇒ 没有 guest 会再等它们；但**自动复位事件必须被消费**，否则 `SetEvent` 之后轮询者会看到"永远 signaled" |
| 4 个通知点的**次序**与**每圈一次** | ✅ **必须** | 引擎自己用 `off[cur]-off[prev]` 算跨度（`0x4B6CA1–0x4B6CB7`）；次序错 ⇒ 跨度错 ⇒ **PCM 错位**（不是静音，是错音） |
| `Lock`/`Unlock` **成对** | ✅ **必须** | Unlock 失败会走错误串（`0x4B7026–0x4B702D`）并**放弃该流** |
| `[stream+0x2458]`（0 停 / 1 播 / 2 放完） | ✅ **必须** | 泵的"填静音"分支就靠它（`0x4B6D91`、`0x4B6FD6`） |
| `[stream+0x2468]` 已写圈数 | ✅ **必须** | `inc [esi+2468h]`（`0x4B7002`）在 Unlock 前；guest 用它做 fade / 循环判断 |
| `[stream+0x244C]` 残块 | ✅ **必须** | 跨两次泵调用保留的状态 |
| `[stream+0x246C]` / `[+0x2450]` / `[+0x2470]` | ✅ **必须** | seek / 重复起点 / 重复计数 |
| 线程句柄 signaled | ✅ **必须** | 关机 `0x4B6522` |
| `GetCurrentPosition` / 播放游标 | ❌ 可偏差 | 引擎 **0 次**调用（音频报告 E9）；只要求"我们的游标推进与通知点一致" |
| CS 的 `OwningThread` / `RecursionCount` | ⭕ 用 feeder id 写（比真机更接近） | 偏差只发生在"泵在主线程已持 CS 时进入"⇒ 用 D.4-③ 的 defer 消掉 |
| `fs:` / TEB `Self` / `GetCurrentThreadId`（泵内） | 【待核】 | 若 `sub_4B6C30` 及其被调（`_memcpy`/`_memset`/`wsprintfA`/`sub_4034C0`）不读 `fs:` ⇒ 可共享主线程 TEB 并登记偏差；否则给它一个小 TEB |
| `GetLastError`（泵内） | ❌ 可偏差 | 泵会污染主线程的 last_error ⇒ 泵前后保存/恢复 |
| `insns` / `clock`（泵内） | ⭕ 不偏差 | 泵跑的 guest 指令并入总数（与今天 prefill 的 3 次调用同形）⇒ A4 仍成立 |

#### D.4 风险（按"会不会静默"排序）

1. **x87 / XMM 上下文污染（最高）**：`sub_4B6C30` 的解码器（Ogg）用浮点；泵的插入点是**块边界**，主线程的 x87 栈这时**可能是活的** ⇒ 不换上下文 = 主线程后续算术静默用错值。**这是 D 唯一的"必须做且容易漏"的机制**；C 里由 OS 免费提供。
2. **重入**：`Present`（`d3d9.cpp:601-678`）、消息泵（`user32_window.cpp:109`）、`GetMessageA` 都是 shim。从这些地方驱动泵是**允许**的（单执行流、栈更深一点），但**必须**带深度上限并在 `GuestCallStack` 已有帧时拒绝（`guest_call.h:52` 的 `active()`）。
3. **在主线程的临界区内部跑 feeder**：真机的 feeder 会在 `EnterCriticalSection` 上**阻塞**；D 里同一个线程是可重入的 ⇒ 会插进"主线程正在改这个槽"的中途。**必须**在泵的 Enter 之前检查 `[CS+0x0C]`（`kernel32_sync.cpp:299` 的 `OwningThread`）：owner 存在且 ≠ feeder_id ⇒ **本次不填，推迟到下一个块边界**。不做这条 = D 会引入一个真机没有的窗口。
4. **解码器吞吐**：一次 `sub_4B6C30` 要解 1/4 圈 = **0.5 s 音频**（`L = [+0x444] = 352800`）。解释器下 Ogg 解码若 > 一帧预算（~16 ms），D 就变成"每 0.5 s 掉一帧"。**这是 D 相对 C 唯一真正的代价**：C 把它变成 underrun（画面不断），D 把它变成卡顿。
   ✅ **先量再决定，零代码**：引擎**本来**就在主线程连调 3 次（`0x4B74B7` 的 `for(i=1;i<4)`），用 `--profile` / `LIFT_FRAME_TRACE=1` 直接量这 3 次的开销即可。
5. **`WaitForSingleObject(thread)` 的 join**：必须 pump 到 exited，否则 `Release pDS`（`0x4B6534`）之后泵还写 ⇒ **guest 侧 UAF**。
6. **`/GS` cookie**：`sub_4B6C30` 有栈 cookie（`0x4B6C39` / `0x4B6C63`）；铺帧的 `cleanup_bytes` 错 4 字节就会走 `__report_gsfailure` ⇒ 症状是"栈溢出"，**误导**。判据：thiscall 的 `this` 在 ECX，栈上只有 1 个参数 ⇒ `cleanup_bytes = 4`。
7. **guest 堆 / COM**：`sub_4B6C30` 正常路径不分配；错误路径（`sub_4034C0`）可能分配。单执行流下仍然确定，只是堆时序与真机不同（不登记为偏差也行，但要知道）。
8. **音频时钟**：D 下由**我们的**设备游标驱动 ⇒ 虚拟时钟下**完全确定**（CTest 可断言 PCM 字节）；real-time 下用宿主原子游标（音频报告 R2），事件仍由 dispatcher 投递。⚠ 这意味着 **D 能让音频留在 A4 的判据里**，而 C（真线程 + real-time）不能。
9. **`--max-insns` / `insns`**：泵里跑的 guest 指令并入总数；只要泵的**触发条件**是确定量（游标 + 块边界），A4 不受影响。

**"会不会把消费者线程变成主循环里的阻塞点？"—— 会，而且这是 D 的核心代价。** 定量：每 0.5 s 音频需要一次 `sub_4B6C30`；它超过一帧预算就是掉帧。取舍很清楚：

| | D | C |
|---|---|---|
| 解码慢时 | **主循环掉帧**（可见） | **音频 underrun**（可听，画面正常） |
| 解码正常时 | 无差别 | 无差别 |
| CPU 核数 | 单核也够（串行） | 多核才有收益 |

#### D.5 工作量与判据

| 阶段 | 内容 | 人日 | 依赖 |
|---|---|---|---|
| P0 | 设备模型（游标 / 4 通知 / Lock / Unlock / Play / Stop / SetCurrentPosition）= 音频报告 P1 | 2–3 | 无 |
| P1 | 泵 + 簿记（D1）：槽分派 / 在飞计数 / 事件消费 / 线程与句柄 / 退出 | 2–3 | P0 |
| P2 | 嵌套 guest 调用（D2）：上下文交换 + `sub_4B6C30` + defer 规则 | 3–5 | P1 |
| P3 | 输出（D3，可选）：SDL sink + 原子游标 | 2–3 | P0 |
| P4 | 守卫 + 变异自检 | 2–3 | P2 |
| **合计** | **11–17 人日（≈2–3.5 周）** | | |

对照：**C 12–20** · **B 17–28** · **A′ 22–35** · **A 30–55**。

**必备守卫（每条都要"能红"）**：

| # | 守卫 | 判据 | 怎么让它红 |
|---|---|---|---|
| 1 | `audio-notify-order` | 4 个事件每圈各 1 次，次序 `+1/4 → +1/2 → +3/4 → wrap` | 删投递 / 改次序 |
| 2 | `audio-feeder-fill` | 环前 `176400` B 与夹具写入的斜坡**逐字节**相等 | 把 `sub_4B6C30` 调用改成空 |
| 3 | `audio-thread-join` | `SetEvent(quit)` ⇒ `WaitForSingleObject(thread,0)` = `WAIT_OBJECT_0`，且此后环不再变 | 不 pump（返回 TIMEOUT） |
| 4 | `audio-context-isolation` | 泵前把 `ST0`/`control`/`xmm0` 置已知值，泵后**逐字节不变** | 只保存 8 个 GPR |
| 5 | `audio-defer-under-cs` | 主线程持有 slot CS 时泵**必须推迟** | 删掉 `OwningThread` 检查 |
| 6 | `audio-no-double-fill` | 同一圈在 `[+0x2468]` 增加前不会被填两次 | 把"消费事件"放到填之后 |

#### D.6 D 可行吗 / 卡在哪

**可行**。它**不卡**在 `sub_4B6C30` 的任何一条语义上：

- 该函数**本来就是主线程调的**（`0x4B74B7` / `0x4B7502` / `0x418549`）【实测】；
- 它**不做阻塞等待**：`Lock` 的 `dwFlags = 0`（`0x4B6CC6` 的 `push 0`），失败只走 `Restore` 重试【实测】；
- 它的 guest-visible 输出**全在 stream 对象里**（`+0x244C..+0x247C`），泵按同一顺序写即可（D.3 表）；
- 它的"线程语义"只有三样：`WaitForMultipleObjects` 的先后、CS 的互斥、在飞计数——三样都能由 D 的簿记等价表达（D.2 / D.3）。

**唯一"卡"的是吞吐**（D.4-④），而那是**可测量**的，不是语义缺口。若量出来超过一帧预算 ⇒ 升级到 C（把 `sub_4B6C30` 丢给真线程）。**关键**：D 的 P0（设备模型）与 P1（簿记）在 C 里**一字不改**地复用 ⇒ **D 是 C 的严格子集前置**，先做 D 不浪费。

### 5.2 A / A′ / B / C 对比

> 工作量 = **静态推断**（无原型验证），单位人日（1 人）；假设实现者已熟悉本仓。含自测。
> A′ 是 A 的一个**约束变体**（加单令牌），单列是因为它把"arm64 内存序"这一整块风险移出了关键路径。
> **D 的对比维度不同（它不是"调度方式"而是"取消线程"），单列在 §5.1；五方案横向总表见 §5.4。**

| | **A 真宿主线程**（每线程 guest CPU + 全局内存 + 同步软模拟，**真并行**） | **A′ 真宿主线程 + 单令牌**（任意时刻只一个线程跑 guest） | **B 协作式 guest 线程**（第二 guest 栈，**单宿主线程**，音频报告推荐） | **C 混合**（音频线程真跑，业务单线程） |
|---|---|---|---|---|
| **改动面** | `X86` 拆实例 + `ShimState` 拆分 + 12 个宿主结构各配锁 + 全访存原子化 + store buffer + 同步原语重写 + 线程句柄可等待 | 同上去掉"全访存原子化 + store buffer"，加令牌调度器（≈1 处核心循环）+ 让令牌点 | 第二 `X86` 上下文 + **自己写上下文切换**（保存/恢复全部寄存器）+ 块边界调度器 + 所有阻塞 shim 改"让出" + 每线程 TEB/TLS | = A′ 但只允许 2 个线程、只开音频；不含"业务可多线程"的泛化 |
| **风险** | **最高**：TSO 软模拟没有 oracle（见 §6.4）；性能未知；死锁面最大 | 中：令牌交接点/让出点漏一个 = 挂死；但**不挂死就基本对** | 中高：自己造调度器，`ret`/栈切换/`esp` 边界错一次就是静默改坏控制流（本仓已有 `TimerSavedRegs` 那次实测教训，`x86.h:103-119`） | 中低：线程数写死 2，路径窄 |
| **arm64 风险** | **高**：必须软模拟 TSO；漏一处 = 偶发数据错（§3.5 三场景） | **低**：无并发访存 ⇒ arm64 与 x86 同结果（唯一要对的是一次 mutex 交接） | **低**：根本没有第二宿主线程 | **低**：同 A′ |
| **工作量** | **30–55 人日**（≈6–11 周）（其中 TSO 模拟 10–20、并发化宿主结构 6–10、逐线程 ABI 4–6、同步 5–8、验证 5–10） | **22–35 人日**（≈4–7 周） | **17–28 人日**（≈3.5–6 周）（音频报告 P3 估 4–7 人日只含"调度器"，不含 TEB/TLS 与同步重写） | **12–20 人日**（≈2.5–4 周，含前置的 DS 设备模型 2–3） |
| **对现有 CTest** | 全面受影响：`sync-ext` ⑧（`tests/sync_ext.cpp:314-349`，断言"start routine 不执行"）、任何依赖"等待=abort"的用例、A4 的 `insns` 判据 | 只有 `sync-ext` ⑧ 需要按模式拆两套断言；新增"真线程"夹具；A4 在 `threads=off` 下**完全不变** | 同 A′（`sync-ext` ⑧ 要拆），但**不需要**新增模式开关（仍是单宿主线程） | 同 A′ |
| **可复现性** | 丢失（除非额外做确定性调度） | **可以保住**（量子=指令数），于是 A4 有希望扩到 threaded 模式 | 保住（天然确定） | 同 A′ |
| **推荐** | ❌ 现在不做 | ✅ **推荐**（作为 C 的机制底座） | ⭕ 次优（省事但把 OS 已经做好的事自己做一遍） | ✅ **先做这个** |

### 5.3 为什么 A′（真线程 + 令牌）比 B（纯协作式）省事

| 维度 | A′（真线程 + 令牌） | B（协作式） |
|---|---|---|
| 阻塞等待 | **宿主 `cv.wait` 直接给**；释放令牌即可 | 必须自己实现"挂起当前 guest 线程 + 保存全部寄存器 + 切栈"；每个阻塞 shim 都要改 |
| 上下文保存 | OS 帮你保存 | 自己保存（且**必须**保存全 8 个 GPR + eflags + x87——正好是本仓踩过的坑，`x86.h:103-119`、`machine.cpp:493-515`） |
| 中断点 | 天然（线程随时可被停） | 只能在块边界（与今天 `fireDueTimer` 同形，`machine.cpp:551-554`） |
| 确定性 | 需要"令牌轮转 + 固定量子"才确定 | 天然确定 |
| 代码量 | 调度器 ~几百行（锁 + cv + 轮转） | 调度器 + 上下文切换 ~同样几百行，但**新的失效面**（栈切换/返回地址/`ret` 弹参） |

⚠ B 唯一的真优势是"没有宿主并发"⇒ §1.3 那 12 条**全部消失**。但这个优势 A′ 用"一把令牌锁"同样拿到了（因为不变量没变：任何时刻只有一条执行流在 guest 里）。**所以 B 省下的不是风险，是"一个 mutex"**。

### 5.4 五方案总表与排序

| | **D native override 喂料线程** | **C 音频真线程** | **B 协作式** | **A′ 真线程 + 令牌** | **A 真并行** |
|---|---|---|---|---|---|
| 有没有第二宿主线程 | **没有** | 有（1 个） | 没有 | 有（N 个） | 有（N 个） |
| 需要的核心机制 | 设备模型 + 泵 + **上下文交换** + defer | 调度器 + 令牌 + 同步对象重写 + per-thread TEB/TLS | 自造上下文切换 + 调度器 + 阻塞让出 | 令牌 + 同步对象 + per-thread ABI | A′ + TSO 软模拟 |
| arm64 内存序风险 | **无**（单执行流） | 低（令牌） | 无 | 低（令牌） | **高** |
| 确定性 | **天然确定**（虚拟时钟下可进 A4） | 需固定量子才确定；real-time 下不可比 | 天然确定 | 固定量子下确定 | 丢失 |
| 音频质量风险 | **掉帧**（解码超预算时） | underrun | underrun | underrun | underrun |
| 工作量（人日） | **11–17** | 12–20 | 17–28 | 22–35 | 30–55 |
| 对现有 CTest | 只加新用例（`sync-ext` ⑧ 不受影响，因为 `CreateThread` 仍不建线程） | 拆 `sync-ext` ⑧ + A4 分模式 | 拆 `sync-ext` ⑧ | 同 C | 全面 |
| 能不能解决音频 | 能（连续 BGM/SE/语音） | 能 | 能 | 能 | 能 |
| 能不能解决 DirectShow/MF 线程 | ❌（那两个仍要 `_beginthreadex`） | ❌ | ❌ | ✅ | ✅ |
| 排名（只为音频） | **1（推荐）** | 2（吞吐兜底） | 3 | 4 | 5（不推荐） |

**排序理由（一句话）**：D 用"取消线程"消掉了 C 的全部机制债（同步对象、令牌、per-thread TEB/TLS、内存序），只留下一个可测量的代价（解码吞吐）；而这个代价恰好可以**先用零代码的方式量出来**（引擎已在主线程调 3 次，§D.4-④）。**若吞吐够 ⇒ D 收工；若不够 ⇒ 升级到 C，D 的 P0/P1 全部复用。**

---

## 6. 最小可验收实验

### 6.1 第一步该做什么（不含音频）

> ★ **若走方案 D（§5.1，推荐）**：本节**整段跳过**——D 没有真线程 ABI 要钉，第一步是 §5.1 D.5 的 **D1**（设备模型 + 泵 + 簿记，不调 guest）。本节只在走 C/A′/A 时需要。

**T0：线程 ABI 夹具（pure CTest，`initBare` 自造机器码）** —— 用它把 §2.2 的每一条钉住：

```
guest 代码（不用 PE、不用引擎）：
  start:
    push  <shared_ea>            ; 计数器地址
    call  InterlockedIncrement   ; ← 必须是真原子
    push  1 / push <h_event>     ; WaitForMultipleObjects(1, &h, FALSE, INFINITE)
    call  WaitForMultipleObjects
    push  <h_quit>
    call  WaitForSingleObject    ; 再等一次（测"可阻塞"）
    mov   eax, 0x1234            ; 返回值
    ret                          ; ← 返回 = 线程退出（照 sub_4B5AA0 的 0x4B5B3E）
  main:
    CreateThread(start, param, CREATE_SUSPENDED) → 断言 ResumeThread 旧值=1
    ResumeThread → 0
    SetEvent(h_event)            ; 让线程从 Wait 前进
    WaitForSingleObject(h_thread) ; ← 线程句柄必须可等待，返回 WAIT_OBJECT_0
    ExitCode == 0x1234
    断言 shared_ea == 1（且两次运行相同）
```

判据（全部机械）：
1. `shared_ea == 1`（→ `InterlockedIncrement` 真原子）。
2. `WaitForSingleObject(h_thread)` == `WAIT_OBJECT_0`（→ 线程句柄可等待；现在是 `WAIT_FAILED`）。
3. 线程内 `GetCurrentThreadId()` == `CreateThread` 写出的 id（→ 每线程 `thread_id`）。
4. 线程内 `TlsGetValue(slot)` == 线程自己写的值，主线程读到**主线程**的值（→ 每线程 TLS 数组）。
5. 线程内 `fs:[0x18]`（TEB Self）== 该线程的 TEB EA；`fs:[0x04]` == 它的 StackBase（→ 每线程 TEB）。
6. 线程内 `GetLastError()` 独立于主线程（→ 每线程 last_error）。
7. 两次运行 `insns` 相同（→ 确定性调度）。

**这一步不依赖音频、不依赖 PE、不依赖 DS 设备模型，~2–4 人日就能出红/绿。**

### 6.2 第二步才是音频（依赖 DS 设备模型）

> ★ **若走方案 D（§5.1，推荐）**：§6.1 的 T0 夹具**不必做**（没有真线程 ABI 要钉），直接做本节的 P1/P2；开关是 `--audio-feeder pump` 而不是 `--threads audio`。下面的判据（状态 / 字节 / 时序）两方案**完全一样**——这正好是 D 与 C 可以互相替换的原因。

1. 前置（`docs/audio-feasibility.md` §3.6 的 P1）：`SetNotificationPositions` 真存 4 个 `(offset, event)`（现在空实现，`dsound.cpp:339-342`）+ 播放游标（`%bytes`）+ 块边界补投 `SetEvent`。
2. 打开 `--threads audio`，`CreateThread(sub_4B5AA0)` 真跑（`.lst 0x4B5FE4`）；**或**（D）打开 `--audio-feeder pump`，由泵代替 `0x4B5B03` 那一个调用点。
3. 机械判据（**guest 可见状态 / 字节 / 时序**三档）：
   - **状态**：`0x4B5AA0` 被进入的次数 ≥ 1；`[Sound+0x7F8+4s]` 在飞计数往返（增后回到 0）；`[Sound+0x2468]`（已写圈数）随事件递增；`WaitForMultipleObjects` **没有**触发 `shimAbort`（现在必 abort，`kernel32_sync.cpp:747-751`）。
   - **字节**：`Lock` 写入的环内容 → `--audio-dump`（音频报告 P2）的 WAV 前 176400 B 与"斜坡/已知 PCM"**逐字节**相等（证明 guest 解码器产出被宿主读到，不是 memset 0——今天 `nonzero=0` 是正确的，因为没人填）。
   - **时序**：每圈 4 个 notify 事件各 1 次、顺序 `1/4→1/2→3/4→回绕`；两圈之间没有"空圈"（可以按圈计数断言，照音频报告 §3.6(b)(e)）。
4. 反向判据（防"假绿"）：**把 `SetNotificationPositions` 的投递删掉 ⇒ 事件计数为 0 ⇒ 判据必须红**；**把 `CreateThread` 退回伪线程 ⇒ 环永远只有 1.5 s ⇒ 判据必须红**。

### 6.3 怎么证明 arm64 内存序**没漏**（分层，从强到弱）

| 层 | 做法 | 判据 | 成本 |
|---|---|---|---|
| **L0 方案 D（最强，根本不是并发）** | 只有一个宿主执行流：泵在主线程的确定性边界跑 | 连"令牌"都不需要；只剩下"泵的触发点是确定量"这一条要求 | 零 |
| **L1 结构（最强）** | 采用 A′/令牌：**任意时刻只有一个宿主线程执行 guest 代码** | 在 `LIFT_THREAD_DEBUG=1` 下 `GuestMemory::rd*/wr*` 断言"令牌持有者 == 本线程"（`memory.h:209-254`，几十行）；TSAN 全绿 | 低 |
| **L2 交点** | 只需证明**令牌交接**有 acquire/release（`std::mutex`/`atomic`） | 交接点是可枚举的（§3.4 A 表的 4 类）⇒ 逐点 review 就能闭合 | 低 |
| **L3 litmus** | 用 `initBare` 写 x86 内存模型的经典夹具（SB / MP / IRIW），同一份 guest 字节在 x86 与 arm64 各跑一遍 | 观察到的结果集 ⊆ x86-TSO 允许集；且两平台**结果集相同** | 中–高（要有一个"允许集"的判定表） |
| **L4 端到端** | `ctest` 在 `macos-arm64`（`CMakePresets.json` 已有该 preset）跑 `boot-trace` + T0/T1 夹具，与 `windows-x64` 比 `insns`/trace 字节 | 相同 | 中（需要一台 mac 或 CI） |

⚠ **L4 只能证明"这条路径上没漏"，不能证明"没漏全部"**（与 `AGENTS.md` §11 的"投影一致 ≠ 引擎一致"同一条纪律）。
⚠ 若走 A（真并行），L1 失效，**只能靠 L3**——而 L3 需要一个 x86-TSO 的结果集模型，这本身是研究级工作量（这也是 A 的 10–20 人日里最不确定的一块）。

---

## 7. 结论

### 7.1 "直接简单实现成真线程"可行吗

**不可行。** 不是因为"复杂"，是因为**五条具体机制现在不存在**，而其中三条会**静默失败**：

| # | 缺什么 | 具体锚 | 直接后果 |
|---|---|---|---|
| G1 | **可阻塞的同步对象** | `waitOne` / `waitMultiple` 未 signaled + `timeout≠0` ⇒ `shimAbort`（`kernel32_sync.cpp:670-677, 747-751`）；SCOPE §1 把它登记为"硬错误" | 音频线程第一次 `WaitForMultipleObjects(..., INFINITE)`（`.lst 0x4B5AB6`）**当场终止进程** |
| G2 | **临界区的互斥与顺序语义** | `enterCriticalSection` 只改 guest 字段、跨线程拥有者直接 abort（`kernel32_sync.cpp:292-306`）；`TryEnterCriticalSection` 恒 TRUE（`:350-356`） | 主线程持有 CS 时音频线程 `EnterCriticalSection`（`.lst 0x4B5AD7`）⇒ **abort**；即使不 abort，也没有任何 acquire/release ⇒ arm64 上 §3.5-S1 的发布错误 |
| G3 | **原子 RMW** | `Interlocked*` = `rd+wr`（`kernel32_error_tls.cpp:185-222`）；`xadd`/`cmpxchg` 同（`exec_integer.cpp:406-428`）；`lock` 前缀只用于助记符（`decode.cpp:324`） | `.lst 0x4B5AF2` 的在飞计数丢更新 ⇒ 槽永远"在用"⇒ 换缓冲/停止错 |
| G4 | **宿主结构的并发契约** | §1.3 的 R1–R12（`BlockCache` 的 `Block*`、`ShimRegistry` 的 `ShimDef*`、`HandleTable` 的 `HandleEntry*`、`VirtualClock::now_100ns_`…） | 随机崩 / use-after-free / 时间倒退；在 arm64 上表现为"偶发" |
| G5 | **每线程 ABI**（栈/TEB/TLS/last_error/thread id） | `image.h:95-97`、`image.cpp:433-440`、`kernel32_error_tls.cpp:84-146` | 两个线程共用 `fs_base` ⇒ guest 的 `fs:` 访问打到同一个 TEB；`TlsGetValue` 互相踩 |

### 7.2 若可行，**必须**加的前提（真线程五条 / 方案 D 只要两条）

**真线程（C / A′ / A）五条，缺一不可：**

1. **调度方式必须先定**：选 **A′（单令牌）** 则 §3 的内存序问题**整块消失**（arm64 也不需要 TSO 模拟）；选 A（真并行）则必须做 store buffer / 全访存原子化，工作量翻 2–3 倍且没有 oracle。**这是最贵的一个决定，且必须在写第一行线程代码之前定。**
2. **同步对象重做**（G1/G2）：CS/Event/Semaphore/Mutex/Wait 换成宿主 `mutex`/`cv`，线程句柄成为可等待对象；`TryEnterCriticalSection` 必须真 try。
3. **原子 RMW 真原子**（G3）：`Interlocked*` + `lock` 前缀；并在 RMW 期间**不许换手**。
4. **宿主结构并发契约**（G4）：A′ 下 = 一把令牌锁（+ 主线程独占 `WindowManager`/D3D9/GPU/SDL/`FontEngine` 的断言）；A 下 = 逐结构锁 + 锁序文档。
5. **每线程 ABI**（G5）+ **模式闸门**（§4.2/§4.3）：`--threads off|audio|full` 默认 off，且进 `TraceInputs`。

**方案 D 只要两条**（G1/G3/G4/G5 全部不适用——它没有第二线程、也不阻塞）：

1. **DS 设备模型 + 通知投递**（音频报告 P1；把 `SetNotificationPositions` 从空实现改成真存 4 个 `(offset,event)`，`dsound.cpp:339-342`）。
2. **泵的三条纪律**：① 整份 guest 上下文交换（x87/XMM 必换，§5.1 D.2）；② CS 被主线程持有时 **defer**（§5.1 D.4-③）；③ `WaitForSingleObject(<feeder 句柄>)` 必须 **pump 到退出**（§5.1 D.4-⑤）。
   （外加一条：泵的触发条件必须是确定量，否则 A4 保不住——但这条与今天 `fireDueTimer` 的要求完全相同。）

### 7.3 卡在哪一条**具体模型缺口**（一句话）

> **"同步对象"这一层只有"记账"没有"阻塞 + 可见性"。**
> 具体化：`SyncObj` 只有 `signaled/count` 三个字段 + `cv` 的缺失（`kernel32_sync.cpp:133-140`）；`CRITICAL_SECTION` 只有 guest 里的 `RecursionCount/OwningThread` 两个 dword（`:292-306`）；等待只有 `shimAbort` 一条出路（`:670-677`）。**这三处合起来 = "线程之间无法握手"**。音频线程之所以现在"不跑也没事"，正是因为它一旦跑起来，第一次握手就会撞上 G1。

### 7.4 建议的推进顺序（D 优先；每步都能单独验收）

0. **P-1（0 人日，先做）**：**零代码量吞吐**——引擎**本来**就在主线程连调 3 次 `sub_4B6C30`（`0x4B74B7` 的 `for(i=1;i<4)`）。用 `--profile` / `LIFT_FRAME_TRACE=1` 量这 3 次（= 3/4 圈 = 1.5 s 音频）的墙钟。
   判据：**单次 < 一帧预算（~16 ms）⇒ 走 D**；否则直接走 C（并保留这条测量作为基线）。
1. **P0（2–3 人日）**：DS 设备模型（= 音频报告 T1，与线程无关）+ **D1**（泵 + 簿记，**不调 guest**）。判据：事件次序/计数正确、`WaitForSingleObject(thread,0)` = `WAIT_OBJECT_0`、环内容仍全 0。
2. **P1（3–5 人日）**：**D2** —— 整份上下文交换 + `sub_4B6C30` 嵌套调用 + defer 规则。判据：环 PCM 逐字节 = 已知斜坡 + 上下文隔离守卫（§5.1 D.5 守卫 2/4/5）。
3. **P2（2–3 人日）**：6 条守卫 + 变异自检 + `--audio-feeder off|pump` 进 `TraceInputs`。
4. **P3（可选 2–3 人日）**：SDL 输出（宿主回调只写原子游标，事件仍由 dispatcher 投递）。
5. **P4（仅当 P-1/P1 证明吞吐不够）**：升级到 **C**（真线程 + 单令牌）——**D 的 P0/P1 全部复用**，只把"泵"换成"真线程 + 令牌"。

**总推荐**：**先做 D 路线 ≈ 11–17 人日（2–3.5 周）**；若 P-1 的测量或 D2 实测超预算 ⇒ 追加 **6–10 人日**升级到 C（合计 17–27）。两条路线在 `--audio-feeder off`（默认）下，确定性、A4、`sync-ext` ⑧ **全部一字不变**。

### 7.5 D 是否比 C 更推荐？（判据与分水岭）

**对"让音频出声"这条需求：是，D 优先。** 五条理由：

1. **工作量更低**（11–17 vs 12–20），且与 C 的前两级完全重合 ⇒ 不存在"选错要重做"。
2. **不引入并发** ⇒ §1.3 的 R1–R12、§3 的整章内存序、arm64 的三个静默场景**全部不适用**；`ShimState` 拆分、per-thread TEB/TLS、同步对象重写**都不需要**。
3. **确定性保住**：泵由虚拟时钟 + 块边界驱动 ⇒ 音频可以**留在 A4/CTest 的判据里**（能断言 PCM 字节与事件次序）；C 在 real-time 下不可比。
4. **`CreateThread` 语义可以保持"伪线程 + 一个 feeder 例外"** ⇒ `tests/sync_ext.cpp:314-349` 的断言不用拆，`SCOPE.md` §1 的字面也只需加一条"喂料线程由宿主代跑"。
5. **它不改变任何 guest 可观察的"线程"事实**：句柄、id、挂起计数、退出 signaled 全部照旧（D.3 表）。

**什么时候 D 不成立**（三条，任一成立就转 C）：

| 触发条件 | 判据 |
|---|---|
| 单次 `sub_4B6C30` 墙钟 > 一帧预算（~16 ms，4 核以下更严） | P-1 的测量 |
| 需要同时播多个流（多槽并发解码），泵串行化后预算不够 | 2 个 BGM/语音槽同时 active 时的实测 |
| 需要通用线程（`_beginthreadex` / DirectShow / MF） | 那些是**真阻塞 I/O**，泵模型不适用（当前被视频 skip 消化） |

**一句话**：**D 是"用取消线程换掉全部并发债"，代价只有一条可测量的吞吐；C 是"用并发债换回并行吞吐"**。当前需求（一条事件驱动的服务线程 + 音频独奏）落在 D 的甜区里。

---

## 附录 A · 本次新增/纠正的实测事实

| # | 事实 | 证据 |
|---|---|---|
| A1 | `sub_4B5AA0` 正常结束是 **`retn 4`**（返回），**不是** `ExitThread` | `.lst 0x4B5B37-0x4B5B3E`【实测】 |
| A2 | 音频线程的 `InterlockedIncrement/Decrement` 是**导入调用**（`call InterlockedIncrement`），不是内联 `lock inc` | `.lst 0x4B5AF2` / `0x4B5B0C`【实测】 |
| A3 | 全 `.lst` 只有 **20 条** `lock` 前缀指令，**全在 CRT/STL 区**（`0x4800xx`–`0x512Bxx`），**音频模块 `0x4B5xxx–0x4B7xxx` 内 0 条** | `rg '\block\s+(xadd|cmpxchg|inc|dec|add|or|and|sub|xor|xchg|not|neg)'`【实测】 |
| A4 | `lock` 前缀在本实现里**只影响助记符字符串**，不影响语义 | `decode.cpp:218-226, 324`；`exec_integer.cpp:406-428`【实测】 |
| A5 | `SetNotificationPositions` 仍是空实现（只回 `DS_OK`） | `dsound.cpp:339-342`【实测】 |
| A6 | `ShimState` 把进程级字段与线程级字段混在一个结构里 | `x86.h:122-165`【实测】 |
| A7 | 句柄值由 `entries_.size()` 派生 ⇒ **创建顺序决定 guest 可见值** | `handles.cpp:36`【实测】 |
| A8 | `HandleTable::get()` 返回**指向 vector 元素**的指针 | `handles.cpp:46-52`【实测】 |
| A9 | 全仓唯一的 `thread_local` 是 `g_access`（访存意图），其余共享状态都是普通成员/静态 | `memory.cpp:34`【实测】 |
| **A10** | **`sub_4B6C30` 有 4 个调用点，其中 3 个在主线程**：`0x4B74B7`（起播预填 1..3）、`0x4B7502`（seek/起播后补齐）、`0x418549`（`sub_418520` 脚本侧）；只有 `0x4B5B03` 在音频线程 | `.lst` 里 `call sub_4B6C30` 全枚举【实测】 |
| **A11** | `sub_4B6C30` 是 `__thiscall(this=stream, int quarter)`：**ECX=stream、栈上只有 1 个参数**（⇒ 铺帧 `cleanup_bytes = 4`），且带 `/GS` 栈 cookie（`0x4B6C39` / `0x4B6C63` → `sub_4E73EB`） | `.lst 0x4B6C30` 头 + `0x4B6C39-40, 0x4B6C63-68`【实测】 |
| **A12** | `sub_4B6C30` 里 `Lock` 的 `dwFlags = 0`（不请求整块/不等待）；失败只走 `Restore` 重试 | `.lst 0x4B6CC6`（`push 0`）、`0x4B6CF4-0x4B6D7F`【实测】 |
| **A13** | 通知表 = `[stream+0x424 + 8*i]` 的 `{offset, event}` 四对，offset = `0 / L/4 / L/2 / 3L/4`（`L = [stream+0x444]`）；`SetNotificationPositions` 在 `0x4B7369` 经 vtable **+0x0C** 调用 | `.lst 0x4B72DE-0x4B7369`【实测】 |
| **A14** | 引擎 `CreateThread(lpThreadAttributes=0, dwStackSize=**0**, lpStartAddress=sub_4B5AA0, lpParameter=Sound*, flags=**4**(SUSPENDED), &ThreadId)` | `.lst 0x4B5FD6-0x4B5FE4`【实测】 |
| **A15** | 关机序 = `SetEvent([Sound+0x514])` → `WaitForSingleObject([Sound+0x518], 3000)` → `Release pDS`（⇒ 线程句柄 signaling 与 join 是**必需**的） | `.lst 0x4B6509-0x4B6534`【实测】 |
| **A16** | `startNestedGuestCall(...)` **已经存在**且支持 thiscall 的 `callee_ecx` | `native_overrides.h:242`、`native_overrides.cpp:19-29`【实测】 |
| **A17** | 主线程在起播时**已经**同步调 `sub_4B6C30` 三次（1.5 s 音频）⇒ 可以直接量它的开销，不必写代码 | `.lst 0x4B74B4-0x4B74C0`【实测】 |

## 附录 B · 改动面清单

### B-1 方案 D（native override 喂料线程）— 11–17 人日

| 文件 | 为什么 |
|---|---|
| `src/runtime/audio_device.{h,cpp}`（**新**） | 设备模型：播放游标 / 4 个通知点 / 状态机 / Lock-Unlock 记账（D 的 P0） |
| `src/runtime/audio_feeder.{h,cpp}`（**新**） | 泵 + 61 事件簿记 + 槽分派 + 在飞计数 + 上下文交换 + defer（D 的 P1/P2） |
| `src/shims/dsound.cpp` | `SetNotificationPositions` 真存 4 对（`339-342`）；`GetCurrentPosition` 改 `%bytes` |
| `src/runtime/machine.{h,cpp}` | 块边界驱动泵（照 `fireDueTimer` 的形状，`machine.cpp:551-554`） |
| `src/shims/kernel32_sync.cpp` | `CreateThread` 记下 feeder 身份（`1081-1117`）；线程句柄 signaling；`WaitForSingleObject(thread)` pump 到退出 |
| `src/main.cpp` | `--audio-feeder off|pump`（默认 off）+ 启动日志 |
| `tests/audio_feeder.cpp`（**新**，pure） | 6 条守卫（§5.1 D.5） |
| `CMakeLists.txt` | 新 CTest 注册（**顶层** ⇒ pure 档，见 `AGENTS.md` §5） |
| `data/lift/overrides.json` | 若要登记"喂料线程由宿主代跑"这条偏差（SCOPE §3 第 4 条） |
| `README.md` §2.5 / `SCOPE.md` §1 | ⚠ **本次未改**：D 只需要在 §2.5 加一句"音频喂料线程由宿主泵代跑"，**不推翻**"guest 单线程" |

### B-2 真线程（C / A′ / A）— 12–55 人日

| 文件 | 为什么 |
|---|---|
| `src/runtime/x86.h` / `x86.cpp` | `ShimState` 拆线程级/进程级；`BlockCache` 并发契约 |
| `src/runtime/machine.{h,cpp}` | 每线程 `X86` 实例、调度器、令牌、`run()` 的循环改造、`TraceInputs` 加字段 |
| `src/runtime/image.{h,cpp}` | 每线程栈/TEB（`setupThreadStructures`） |
| `src/shims/kernel32_sync.cpp` | CS/Event/Wait/线程句柄/`ResumeThread`/`ExitThread` |
| `src/shims/kernel32_error_tls.cpp` | `Interlocked*` 真原子、每线程 TLS 值、`GetCurrentThreadId` |
| `src/runtime/memory.cpp` | 令牌断言（`LIFT_THREAD_DEBUG`）、`g_memory` 的单例化说明 |
| `src/runtime/clock.h` | `now_100ns_` 跨线程读安全 |
| `src/runtime/present.h` / `present_sdl.cpp` | 主线程独占断言 |
| `src/main.cpp` | `--threads` + 模式互斥校验 + 启动日志 |
| `data/lift/overrides.json` | `ExitThread` / `_beginthreadex` 等条目的口径（SCOPE §3 第 4 条要求） |
| `tests/sync_ext.cpp` | ⑧ 伪线程断言按模式拆分（`tests/sync_ext.cpp:314-349`） |
| `tests/boot_trace.cpp` | 加"模式必须是 off"的断言（`:198` 旁边） |
| `CMakeLists.txt` | 新 CTest（`threads-abi`、`boot-trace-threads`） |
| `docs/README.md` | ⚠ **本次未改**（只允许写本文件）——落地时请把本文件加进索引表 |
| `README.md` §2.5 / `SCOPE.md` §1 | ⚠ **本次未改**（口径变更需用户裁决，见下） |

## 附录 C · 需要用户裁决（V1–V4）

> 编号用 **V** 而不是 **D**（避免与"方案 D"混淆）。

| # | 裁决项 | 选项 | 影响 |
|---|---|---|---|
| **V1** | **路线**：方案 D（native override 喂料线程）还是 C（真线程 + 令牌） | **D（推荐）**：11–17 人日、无并发、确定性保住；C：12–20 人日、要重做同步对象与每线程 ABI | 决定 §5.1 还是 §5.2 先开工；**两者前两级重合，选错不浪费** |
| V2 | （仅当走 C/A′）**调度方式**：单令牌还是真并行（A） | A′：零内存序模拟、arm64 风险低、无 CPU 并行；A：真并行、要做 TSO 软模拟、工作量 ×2–3、无 oracle | 决定 §3 整章是否要写代码 |
| V3 | **线程范围**：只音频还是通用（含 `_beginthreadex` / DirectShow / MF / 定时器队列） | 只音频：路径窄；通用：任意 `CreateThread` 都真跑 | 决定 §2.4 的清单要不要扩 |
| V4 | **可复现性的边界**：非默认模式下是否允许"确定性但抢占点非真机" | D：天然确定，可以进 A4；C：量子=指令数才确定 | 决定 `boot-trace` 的守卫是"红"还是"skip" |

**若 V1 选 D**：V2 暂不需要裁决（没有第二线程）；V3 仍是"只音频"（D 模型不适用于真阻塞 I/O 的线程）；V4 只剩"泵的触发点"一项（已经是确定量）。
**若 V1 选 C**：V2/V3/V4 全部都要定。

---

*报告完。本文件只写这一处，未触碰源码、未运行游戏、未 `git add` / `git commit`。*
