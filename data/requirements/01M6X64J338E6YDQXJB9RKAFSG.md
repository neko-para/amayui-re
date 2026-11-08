# emulator-lift · 并发契约：guest 可见容器的令牌互斥（feeder × 主线程）

- id: REQ-01M6X64J338E6YDQXJB9RKAFSG
- type: req
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- verify: apps/emulator-lift/tests/guest_token.cpp#guest-token-stress
- tags: [emulator-lift thread audio token]

## 为什么必须存在（否则下一个人会再把 feeder 接进来）

`0xC0000374`（Windows **进程堆**损坏、WER 无应用帧、随机复现）的机制：音频 feeder 是**真宿主线程**
（`AudioEngine::thread_mode_`），执行 guest 代码时与主线程共享 `GuestHeap::blocks_`（`std::map`）/
`HandleTable::entries_`（`std::vector`）/ `Vfs` / `ComRegistry`。两线程同时 `HeapAlloc/HeapFree`
⇒ map 节点/结构损坏 ⇒ 之后某次 ntdll 堆操作炸掉（无应用帧）。设计文档 §5.2 写了"单令牌"，
但代码里从未实现；本单把契约钉进守卫与运行摘要。

## 契约（2026-11-08 性能裁决后的最终口径）

**任一时刻最多一个宿主线程持有令牌；guest 可见的共享容器只在令牌里被访问**
（`GuestHeap` / `HandleTable` / `Vfs` / `ComRegistry` / `SurfaceArena` / guest 内存账本）。
**guest 指令执行可以并行**；互斥点是**每次 shim 调用**（`ShimRegistry::invoke`）——
所有 guest 可见容器访问都发生在 shim 实现里。

## 取 / 放点（⚠ 新增 guest 执行入口必须照这个写）

- **主线程**：`Machine::run` 每个块边界取/放。
- **音频 feeder**：`feederEvent` 的 prologue/epilogue（读槽 / 写 CS 与在飞计数 / 建上下文）取；
  **整次 fill 的 guest 指令执行不取**；fill 里的每次 shim 调用由 `ShimRegistry::invoke` 取。
- **`pump()`** 铺 guest 帧的路径也取（重入）。
- **阻塞等待前必须 `Yield`**：guest `CRITICAL_SECTION`、`GetMessageA` 的宿主事件泵、`join` feeder。
  ⛔ 持令牌阻塞 = 死锁。新增"绕过 shim 的容器入口"必须显式 acquire（violations 可见）。

## 性能裁决与 A/B（同一份 cycle1 脚本、同参数、RelWithDebInfo）

| 方案 | cycle1 wall | 完整 option-roundtrip | notify/fills | dropped/underruns | violations |
|---|---|---|---|---|---|
| 令牌前（并行、**无**容器保护） | 15.1 s | 61–67 s（基线） | 29/29 | 0 / 0 | n/a |
| (a) 逐块硬抢（feeder 每块取/放） | ≈24.2 s | 100.7 s | 41/32、56/30、86/49 | 0 / 0 | 0 |
| 整次 fill 持令牌（上一版） | 34.8–39.1 s | 147–287 s | 67/67、76/76 | 0 / 0 | 0 |
| **(b′) shim 边界（最终）** | **16.4 s** | **62.8 s** | 30/30（+29/29、51/51、71/71） | **0 / 0** | **0** |

**为什么不是 (a)**：整次 fill 单令牌实测 wall 34.8 s / 两线程 CPU 32.8 s（feeder guest 19.0 s +
主线程可运行 13.8 s；wall−CPU≈2 s；yield 自旋≈0）。单令牌串行时 **wall 下界 = 两侧 CPU 之和**，
任何调度（含 feeder 优先）都到不了 17.4 s；逐块硬抢实测 ≈24.2 s 且音频欠料 9–37。
**为什么不是每个容器一把私有 mutex**：`HandleTable::get()` 返回 `HandleEntry*`，调用方在锁外
继续用 —— 私有锁保护不了指针逃逸；shim 边界锁天然覆盖全部容器且无锁序问题，实测已达标。

## 怎么验证违规（可观测面）

- `guestToken().check(site)`：`ShimRegistry::invoke`（`shim.invoke`，每次 shim 一次）、
  `GuestHeap::alloc/free/realloc`、`HandleTable::add`、`machine.block`；未持令牌 ⇒ `violations++`
  并记第一个现场。shim 边界的 check 保证"去 Guard"在**第一次 shim 调用**就红。
- summary 固定打印 `token     : violations=N max-concurrent=M`（`first=<site>` 仅违规时）。
- `--assert-guest-token`：违规 > 0 或同时持有者 > 1 ⇒ exit 4（option-roundtrip 三子进程都带）。
- 压力守卫 `tests/guest_token.cpp`（CTest `guest-token-stress`，pure）：两线程各 200000 次真实
  `GuestHeap` alloc/free（走真实令牌）+ 两线程 × 20000 次**真实 `ShimRegistry::invoke`**（shim
  里 alloc/free）+ `in_use==0 / violations==0 / max-concurrent==1`；另含 `YieldGuard` 深度=2。

## 判据

- `ctest -R guest-token-stress` 绿；把 `GuestToken::acquire/release` 短路 ⇒ 它红（实测：崩在
  `GuestHeap::free` 非法 free；带 `--assert-guest-token` 的短跑 exit 4）。
- `option-roundtrip` 三子进程 `token     : violations=0`；cycle1 ≤17.4 s、`notify==fills`、
  `dropped=0`、`underruns=0`。

## 残余（未做，别当成已覆盖）

- `GuestMemory` 提交位图用 `atomic_ref` 无锁读写（feeder Decoder 读页边界 × 主线程 VirtualAlloc）；
  `regions_`/`va_ranges_` 仍主线程写、只作诊断，未加锁。
- `WindowManager` 的 main-only 在事件泵 Yield 窗口内没有"令牌持有者==主线程"断言（feeder 的
  音频解码路径不碰 user32）。
- 原生 override 只碰 guest 内存、不持令牌；当前音频路径用的是 memcpy 家族（只写 guest 内存）；
  若将来把 `sub_48B090/48AE90/46D9F0/487C60` 这类会碰 d3d9 全局纹理/arena 的项接进 feeder，
  **必须**自己取令牌（或另开单）。
- `lock` 前缀仍按普通读改写执行（`exec_integer.cpp`）：并行 guest 的 `lock` RMW 可能丢更新
  （pre-existing；当前音频在飞计数在 guest CS 内、由宿主锁串行；扩线程前需要真原子 RMW）。
- 同一句柄被另一线程 close 的 `HandleEntry*` 竞态属 guest 语义层；不同句柄由令牌串行化。
- 令牌不模拟 x86 TSO；guest 内存载荷可见性依赖宿主锁的 acquire/release。
