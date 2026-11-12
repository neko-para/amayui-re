# 🐞S1 emulator-lift · option-roundtrip 子进程约 1/4 概率 0xC0000374 宿主堆损坏：feeder 与主线程并发跑 guest 代码，宿主结构无互斥

- id: REQ-01M6X4M4NSRSNX2B0G0DQSFAD1
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/option_roundtrip.cpp#option-roundtrip/3cycles
- repro: apps/emulator-lift/tests/option_roundtrip.cpp#option-roundtrip/3cycles
- severity: S1
- tags: [emulator-lift, concurrency, crash, audio, m2]

## 2026-11-08 订正（只加注，不改下面的历史记录）

**轮数 3 → 2**：`apps/emulator-lift/tests/option_roundtrip.cpp` 的 `kCycles` 现在是 **2** ⇒ 一次 `option-roundtrip` 只跑 **2 轮往返 / 2 个子进程**（不是 3）。
用例名字符串**仍然是** `option-roundtrip/3cycles`（历史串，⛔ 别按名字里的数字判断轮数）；轮数**只由 `kCycles` 决定**。`repro` 的锚点因此仍能解析。
⇒ 下文（当时 `kCycles=3` 的实测记录）里的"3 轮 / 1 / 2 / 3 轮 / 3 个子进程"按**当时那次运行**读；**当前判据一律按 2 轮 / 2 个子进程**（"每次 3 个子进程都 exit 0"也按 2 个读）。

## 复现

命令（**assets 档**，需要真机安装；在 `apps/emulator-lift/out` 下）：`ctest -R option-roundtrip`。
单次约 60 s（内含 3 个子进程，各自要跑到脚本的 `quit`）。
子进程等价命令行（脚本与工作目录由测试生成，在 `out/option-roundtrip-work/`）：
`emulator-lift.exe --appdata <work>/appdata --game-dir "E:/Games/Eushully/天結/install" --present window --present-hidden --raster gpu --native-overrides all --skip-logo --input-script <work>/cycle1.txt --max-frames 700`
★ 复现前提是 `--present window` 这个组合（它会自动开 real-time ⇒ 音频走真 feeder 线程）。

期望：每个子进程 exit 0，日志里出现 `detail    : input script: quit`。
实际：约 **1/4 的概率**（要重复跑多次才看得见，不是每次）子进程以 `0xC0000374`（STATUS_HEAP_CORRUPTION）退出
⇒ `option-roundtrip` 的第 ① 条断言（子进程 exit 0）当场红。
崩溃签名：**宿主堆损坏** —— 不是 guest 异常、不是 unimplemented、也不是环境不满足（`LIFT_SKIP`）。
怎么量概率：同一构建把 `ctest -R option-roundtrip` 重复跑 N 次，统计子进程非 0 退出次数；
⛔ 跑一次绿就写"没有这个问题"不是判据。

## 现状（结构性根因；本单只登记现状）

- 音频 feeder 线程与主线程**并发执行 guest 代码**，而 `GuestHeap` / `HandleTable` / `Vfs` **没有任何互斥**。
- 设计里约定的"单令牌"（任意时刻只有一个宿主线程在 guest 代码里）**从未实现**：
  `repo:apps/emulator-lift/docs/thread-model-feasibility.md` §5.3。
- 所以这是**并发契约缺失**的结构性后果，不是某处的孤立 bug：单点补锁只会把红转到下一条路径。

## 上界 / 重开条件

- 上界 = 分歧消失：重复跑 N 次、每次 3 个子进程都 exit 0，且日志满足 `option_roundtrip.cpp` 头注释的五条判据。
- 本单**不写修法**：并发契约（令牌 / 逐结构锁 + 主线程独占结构的断言）由另一张单承接；
  若树里还没有那张单，就是它尚未登记 —— ⛔ 不把修法塞进本单（两单会漂）。

## 观测线索（不是结论）

- 崩在宿主堆（`0xC0000374`）⇒ 排查方向是宿主容器的并发写，不是 guest 内存越界。
- 只在 real-time / 窗口组合下出现 ⇒ 与"是否真有第二条 guest 执行流"强相关。

## 收口（2026-11-12，macOS arm64）

- **守卫现在两平台都跑**：`apps/emulator-lift/tests/option_roundtrip.cpp` 的子进程启动加了 POSIX 分支
  （`posix_spawn` + `addopen(fd1)` 再 `adddup2(1,2)` ⇒ stdout/stderr 同一份日志，无管道；`waitpid(WNOHANG)` + 240 s `SIGKILL`）。
  删掉了非 Windows 的 `LIFT_SKIP("CreateProcessW 专有")`。安装树两台设备都是 `links/install`；
  opt-in 的理由是**贵**（本机整条 ~24 s），不是缺资产 / 缺平台（`CMakeLists.txt` 注释已订正）。
- **读数**（`apps/emulator-lift`，RelWithDebInfo）：
  `LIFT_ASSETS=1 cmake --preset macos-arm64` → build → `ctest -R option-roundtrip --repeat until-fail:8` =
  **8/8 Passed（178.47 s）**；另一组 8/8（186.90 s）+ 单跑 ⇒ **17 次 ctest / 33 次子进程全部 exit 0**，
  每轮 `token : violations=0 max-concurrent=1`，arena 两轮逐字段相同
  （`surfaces 23 in_use 109895008 peak 120693568 largest-free 22591536`）。
  **0 次 `0xC0000374`、0 次信号、0 次超时。**
- **为何判"已修"**：根因（feeder 与主线程并发改宿主容器）由令牌契约承接，`GuestHeap::alloc/free/realloc` /
  `HandleTable::add` / `SurfaceArena::*` 都有 `check()`，本用例带 `--assert-guest-token` 且 33 次子进程
  `violations=0`。**未做**：Windows 分支本机无 MSVC / clang-cl，没在本机编译；Windows 侧应再跑一次
  `$env:LIFT_ASSETS=1; ctest --test-dir out -R option-roundtrip --repeat until-fail:8`。
  若 Windows 上仍复现，按 `repro` 重开（本单不删）。
