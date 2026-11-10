# 修退出游戏的 C++ EH 展开（ESC / 关窗）：RaiseException / RtlUnwind 未实现 ⇒ Command_Exit_Exception 一抛就停

- id: REQ-01M708DF24KY0QZNEHY66S0HYW
- type: bug
- status: done
- parent: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- verify: apps/emulator-lift/tests/seh.cpp#seh/B-unhandled-is-loud
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#RaiseException
- severity: S2
- done_reason: SEH 派发机制落地并真机跑通：新 runtime/seh.{h,cpp}（RaiseException/RtlUnwind 状态机：guest 栈铺 EXCEPTION_RECORD/CONTEXT/DISPATCHER_CONTEXT + 宿主深拷贝、沿 fs:[0] 调 handler、EAX=0/1/2 三种 disposition、链走到底 ⇒ UnhandledException 响亮停；RtlUnwind 置 flags|=2、收口 fs:[0]、TargetIp!=0 按 call 前+0x10 跳走）；块边界 pumpSeh 接线；影子栈 dropFramesBelowEsp。真机 trace：RaiseException → handler 0x513B90 → RtlUnwind(TargetIp=0x4E706B) → 落 catch 0x41389A → WinMain 返回 → guest ExitProcess。守卫 seh A/B/C/D 59 断言 + dispatch cld/std 两组，变异 2 条实测红，ctest 74/74。判据修订：ESC 是退出游戏不是回 TITLE ⇒ room-exit.graph 的 exit 0 移交新子单（终态口径）。设计更正：catch 转移走 handler 自己的 _JumpToContinuation，不是 disposition=1 装 CONTEXT。归属已确认（spike REQ-01M71P87BMTZK4MHT8468BMC3G）：本单修的是退出链（ESC/关窗），不是出房间链。
- tags: [emulator-lift, seh, exception]

## 判据（存在性 + 观察）

**现象**：在 `EU-ROOM` 里按 ESC（或任何界面关窗）走退出路径时，`room-exit.graph` 停在 `stop: stopped @ eip=004123C4` /
`detail: unimplemented kernel32!RaiseException`，没有 `task done`。（2026-11-10 归属确认：这是**退出游戏（关窗）**，不是"点「戻る」回 TITLE"。）

**根因（已取证）**：`0x4123C4` 是 `__CxxThrowException(Command_Exit_Exception)` 的调用点；CRT **静态链接**（`_CxxThrowException`=0x4E8A6E、
`___CxxFrameHandler3`=0x4E7094 都在镜像内）⇒ 宿主**不需要**写 C++ EH 展开器，缺的只有两个 OS 原语：
`kernel32!RaiseException`（IAT 0x51D1DC）与 `RtlUnwind`（IAT 0x51D268）。

**判据（本轮修订，见下"判据修订"）**：
1. **机制级**（本单自己的验收）：真机 trace 必须出现完整一轮 `RaiseException → handler → (嵌套) RtlUnwind → 落到 catch 0x41389A`，
   并配 pure 夹具 A/B/C/D（含"链走到底 ⇒ 响亮停"）。
2. **终态级**（`room-exit.graph` = exit 0 + `task done`）**不是 ESC 这条路的验收** —— 见"判据修订"，已移交子单。

## 取证 + 设计（2026-11-10）

- 抛点：0x412399 `jnz`（`GetMessageA` 返回 0 = WM_QUIT 才抛）→ 0x4123B8 `push pThrowInfo(0x544774)` → 0x4123C4 `call __CxxThrowException@8`。
  record 在 `ebp-0xC`（Handler 0x513B90）、抛点 `tryLevel=0`、无嵌套 try。
- 展开目标：catch handler **0x413551**，funclet 用返回值给恢复点 ⇒ **0x41389A**（复位 translator、`mov fs:0,ecx`、`mov esp,ebp; pop ebp; retn` 回 `_WinMain@16`）。
  元数据逐字：FuncInfo 0x5447B4 · TryBlockMap 0x5447EC · HandlerType 0x544800。
- 抛出参数（补字节取证）：`RaiseException(0xE06D7363, flags=1, 3, &Info)`，`Info = {0x19930520, pExceptionObject, 0x00544774}`；
  且 **guest 自己校验 record**（`CallCatchBlock` 的 `0x4F00F8/0x4F0100/0x4F0109`，`Info[0]` 只接受 `0x19930520/21/22`）⇒ 宿主必须**原样透传**。
- `j_RtlUnwind` = 6 字节 thunk；3 处调用点一致要求：到 `TargetIp` 时 `esp == call 前 + 0x10`（形同被 `ret 16` 正常返回）。
- ⚠ **设计更正**：真机的 catch 转移**不是**"disposition=1 装载 CONTEXT"，而是 handler 自己走 `_JumpToContinuation`（0x4E7007：`esp=[pRN-4]` 再 `jmp`，**不 ret**）；
  `_UnwindNestedFrames`(0x4E7066) 的续点确证了 `+0x10` 口径。⇒ design 里"必须经过 `RtlUnwind(TargetIp)`"不成立，实现支持两种形态、由夹具 A/C 各钉一条。

## 已实现（2026-11-10）

- 新 `repo:apps/emulator-lift/src/runtime/seh.h` / `seh.cpp`：`RaiseException` / `RtlUnwind` 的**派发状态机** —— 在 guest 栈上铺
  `EXCEPTION_RECORD`(0x50) / `CONTEXT`(x86) / `DISPATCHER_CONTEXT`(0x40 全 0) 并留**宿主深拷贝**，沿 `fs:[0]` 逐帧调 `handler(pExcRec,pRN,pCtx,pDC)`，
  读 EAX：0 ⇒ 推进 record、1 ⇒ 整份装载 CONTEXT、2 ⇒ continue-search + 日志；走到 `0xFFFFFFFF` ⇒ `StopReason::UnhandledException`（**响亮**，⛔ 不 no-op）。
  `RtlUnwind` 置 `ExceptionFlags|=2`、收口 `fs:[0]=TargetFrame`、`TargetIp≠0` 时按"`call` 前 + 0x10"跳走并不返回。
- 接线：`Machine::run` 的**块边界**（`fireDueTimer` 同形）`pumpSeh`；shim 只搬参数 + `take_control` + `eip = kSehDispatchEa`（⛔ 不在 shim 里跑 guest）；
  展开期间关掉 5 处宿主注入（`!seh_.busy()`）。
- **影子栈**：`WindowProfiler::pushCall(..., frame_esp)` + `depth()` + `dropFramesBelowEsp` —— 展开是**不经 ret 的跳转**，落地时按 esp 丢掉被跳过的帧
  （夹具 D 钉住；⛔ 不让 profiler 静默偏大）。
- **副产物**：解释器缺 `cld`(FC) / `std`(FD) ⇒ 已补（`Op::Cld/Std` + `kOpMap` + exec；`cld` 清 DF、`std` 置 DF 并**首次执行打一条默认可见 Warn**）。
  计数：`.lst` 指令级 0 条；二进制 **23 条**（14 `cld` + 9 `std`，全在 CRT 折叠区）；本次运行 `std` 执行 **0 次**、`cld` ≥1。
  ★ 事实更正：本线串操作**确实按 DF 反向推进**（`exec_string.cpp` 的 `advance` 读 `cpu.df()`）⇒ 告警写的是真话，不是"尚未按 DF 走"。
- **守卫**：新 CTest `seh`（顶层注册）**A/B/C/D 四组 59 断言**；`dispatch` 扩两组 ⑬`cld-clears-df-only` / ⑭`std-sets-df-and-warns`（107 断言）。
  变异 2 条（`seh` 的"未处理 ⇒ 静默"、`cld` 退回 `Op::Other`）实测红。`ctest` **74/74**。
- **真机 trace（原文，`--interaction-graph room-exit.graph --present headless --raster cpu`）**：
  `[seh] RaiseException(code=0xE06D7363 flags=0x1 nargs=3) …` → `[seh] 调 handler 0x00513B90（pRN=0x6FFFFB74）` →
  `[seh] RtlUnwind(targetFrame=0x6FFFFB74 targetIp=0x004E706B …)` → `[seh] RtlUnwind 收口：fs:[0]=…落点 eip=0x004E706B esp=0x6FFFEE60` →
  `[seh] handler 0x00513B90 弃帧 ⇒ 派发结束，落到 eip=0x0041389A esp=0x6FFFF320` ⇒ 之后 `stop: stopped`、`detail: shim 主动结束运行：kernel32!ExitProcess`。
  ⇒ **SEH 这条链完整跑通**（不再停 `0x4E7123C4`/`0x4E709D`）。

## 归属确认（2026-11-10，spike `REQ-01M71P87BMTZK4MHT8468BMC3G`）

- `0x4123C4` **只**可能是"主循环取到 WM_QUIT"（`0x412391 GetMessageA` → `0x412399 jnz`）；引擎侧 WM_QUIT 的唯一来源是
  WM_CLOSE（ESC 在 `0x4B9367` 起被无条件转成关闭消息）→ `DestroyWindow`（`0x4B967B`）→ WM_DESTROY → `PostQuitMessage(0)`（`0x4B9691`）。
- ⇒ **本单修的是"退出游戏（关窗）"的 SEH**；"它是 EU-ROOM 退出的前置"**不成立**（已从标题 / 正文改掉）。
- 真机复跑（2026-11-10，macOS arm64）：`PostQuitMessage(0)` → `"Destroyed"` → `"Exit"` → `RaiseException`/`ExitProcess`；`key_up` 的 `samples=0`。

## 判据修订（父级裁决，2026-11-10）

**ESC 不是"回 TITLE"，是"退出游戏"**（已由上面的归属确认钉死）：`GetMessageA` 返回 0（WM_QUIT）→ `Command_Exit_Exception` → catch →
`_WinMain@16` 返回 → guest 调 `ExitProcess`。所以 `room-exit.graph` 的 `back` 节点（等 TITLE hash）**永远等不到** ⇒ exit 5 是**交互图的口径问题**，不是 SEH 缺陷。

⇒ 本单的验收拆成两层：**机制级留在本单**（上面那条真机 trace + 夹具）；**终态级移到子单"room-exit 的终态判据"**
（要么走「戻る」`REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`，要么改图的终态口径）。⛔ 不许为了凑 `exit 0` 改 `room-exit.graph` 或业务逻辑。

## 明确不做

- `__finally` / 嵌套异常（已有派发时 `beginRaise` 响亮停）/ VEH 优先级 / handler 里再抛 / 宿主 fault 变 guest SEH（`catch(SE_Exception)` 仍不可达）。
- ⛔ 不 override `_CxxThrowException`；⛔ 不实现 C++ EH 展开器（FuncInfo/RTTI/catch 拷贝全在 guest）。
