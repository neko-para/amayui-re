# win32 证据与构建口径：VEH 归因 / 页大小 / fault hook 的实时性（机器已就位）

- id: REQ-01M6JFZFN15MBPG7Y89WNZACFW
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/fault_report.cpp#host-av-attributed-to-host
- tags: [emulator-lift, m1, platform]

## 一句话
平台相关偏差与 Windows 证据缺口。平台 6 条已由用户 2026-11-04 裁决（1/2/3/5/6 落地，4 待用户批方案，7 待用户选）。
**2026-11-04 情况变了：开发机换到 Windows（win32 就是本仓的优先平台），本单不再"缺机器"。**
**2026-11-04（本轮）原"仍缺"的 4 条已全部落地，每条都有实测过的变异证据 —— 见下。**

## 已落地（2026-11-04，mac 侧）
- 宿主入口的非 ASCII 路径：`openHostFileUtf8`（`_wfopen_s`）。
- creation time：macOS 用 `stat.st_birthtimespec`（有 birthtime 就用，否则 ctime）；Windows 继续 SDL；
  其它 POSIX 接受 ctime 回退（`SCOPE.md` §1 已记）。
- `SetFileTime` creation：Windows `::SetFileTime` / macOS `setattrlist(ATTR_CMN_CRTIME)` / 其它 POSIX 跳过
  （`SCOPE.md` §1 已记）；`file-io#find-data-times` 现在写 creation 再读回，变异实测红。
- 超越函数 1 ulp、宿主页保护粒度：`SCOPE.md` §1 两行；宿主 fault 归因分平台实现写入 §4。
- A4 路径：用户裁决"依然给真实路径，不额外处理"；分析报告 `.tmp/path-analysis.md` 保留，**未改代码**。

## 已落地（2026-11-04，Windows 构建/测试口径 —— 上一轮）
用户确认：mac 侧加的**更强警告**破坏了 win 构建。实测根因不是"警告太多"，而是**同一行 flag 在两种驱动下含义不同**：
- `clang-cl` 把 `-Wall` 当 MSVC 的 `/Wall`（≈ `-Weverything`）⇒ `-Wc++98-compat` / `-Wold-style-cast` /
  `-Wdeprecated-declarations` 全开，配 `-Werror` 让整棵树编挂（这台机器上从未编过）。
- 处置（`apps/emulator-lift/CMakeLists.txt`）：**按驱动分档** —— MSVC 口径（`cl` 与 `clang-cl`）用 `/W4 /WX`
  （`/WX` 是真的"警告即错误"，不像 `-Werror` 会被 `cl` 当 D9002 静默忽略），GNU 口径保持 `-Wall -Wextra -Werror`；
  GNU 那行原本就关着的 `-Wno-unused-variable / -Wno-unused-const-variable` 在 clang-cl 口径下**照旧关掉**。
  另加 `/utf-8`（中日文注释）与 `_CRT_SECURE_NO_WARNINGS`（`getenv` 这类可移植用法）。
- `abort()` 弹模态框：`tests/check.h` 与 `src/shims/register.h` 各加一处 `suppressCrashDialogs()`
  （`_set_abort_behavior(0, …)` + `SetErrorMode(SEM_NOGPFAULTERRORBOX…)`），`finish()` 里 `fflush(stdout)`。

## 已落地（2026-11-04 本轮，原"仍缺" 1–4）
1. **win32 的 VEH 归因有了机械证据**：`tests/fault_report.cpp` 在 Windows 上**真的跑**（删掉 `LIFT_SKIP`）。
   子进程机制：POSIX 仍 fork + 管道；Windows 用"同一个 exe 再启动一次"（`--child-host-av` /
   `--child-guest-fault`）+ 临时文件收回 stderr（写句柄经 `STARTUPINFO` 继承给子进程）。
   `host-av-attributed-to-host` 现在在 Windows 上真跑：先弄脏 `g_access`、再解引用 `0x1`（真实宿主 AV）
   ⇒ 退出码 4、有 `=== host fault`、**无** `access     :` 行；反向 `guest-fault-still-guest` 仍退出码 3 +
   `READ 0x20000000  size=4`。注册仍是**顶层无条件**（pure 档），去掉了 `SKIP_RETURN_CODE 77`（已无 skip 路径）。
2. **`hostPageSize()` 不再硬编码**：Windows 改 `GetSystemInfo().dwPageSize`（POSIX 分支与 16 KiB 宿主页的
   对齐逻辑**不动**），并把它从 `memory.cpp` 的匿名 namespace 提到 `lift::` 供测试断言；
   新用例 `host-page-size-from-system`。
3. **`memory.cpp` 补 `WIN32_LEAN_AND_MEAN`**（`NOMINMAX` 原来就有；改成与 `check.h` / `register.h`
   同形的 `#ifndef` 保护）。
4. **fault hook 不再分配**：`LstIndex` 新增零分配 `textPtr(ea)`（返回内部字符串指针），`machine.cpp` 的
   `liftFaultHook` 用它替代 `textAt`（`std::string` 返回必分配）；`memory.h` 的钩子口径改成
   "不许抛异常、**不许分配**（只许 fprintf / 写调用方给的定长缓冲 / 读已加载的只读表）"。
   `textAt` 保留给**非 fault** 路径（`exec.cpp` 的 `dumpAtEa`）。

## 变异验证（本机实测）
- 归因改回陈旧判据（`inGuestSpace(...) || g_access.size != 0`）⇒ `fault-report` **4 failures / 退出码 1**；
  还原后重建再跑 35/35 绿。
- 页大小来源改错（`dwAllocationGranularity` = 0x10000）⇒ 1 failure（`expected 0x00001000, got 0x00010000`）；还原后绿。
- 页大小改回硬编码 `0x1000` ⇒ **仍绿**：本机 `dwPageSize` 恰好就是 0x1000，判据在这台机器上无法区分
  "系统事实"与"同样数值的常量"（这是本条已知的测试盲区，不是漏写）。

## 仍未做 / 边界
5. "windows-x64 preset 从未被自动构建"的**记忆**（CI）属于基建，另开单；本轮已实测通过（不是本单的活）。
- 本单只做"win32 证据 + 构建口径"；`SCOPE.md` 的平台偏差条目是另一件事（已收口的那几条不动）。
