# 待裁决：平台偏差（6 条）与等待语义是否进 SCOPE

- id: REQ-01M6JHVQJ6Q4YHG8N2BJP2ANJJ
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- done_reason: 7 条平台/等待问题全部裁决并落地：birthtime/ctime 回退、SetFileTime creation 能写就写、libm 1 ulp 进 SCOPE、A4 保持真实路径不处理、fault 归因分平台、宿主页保护粒度进 SCOPE、未 signaled 等待改硬错误（shimAbort，人工介入）。
- tags: [emulator-lift, m1, decision]

## 一句话
用户 2026-11-04 对 7 条平台 / 等待问题的裁决，**全部已落地**（4 是"不处理"，7 是硬错误）。

## 逐条
1. **creation time**：有 birthtime 就用（macOS `stat.st_birthtimespec`），否则回退 ctime。
   `vfs.cpp` 的 `hostFileTimes` 已分平台；Windows 继续 SDL（ftCreationTime）；其它 POSIX 接受 ctime。
2. **`SetFileTime` creation**：Windows `::SetFileTime`、macOS `setattrlist(ATTR_CMN_CRTIME)`；
   其它 POSIX 写不了就跳过并留日志（"不能就算了"）。
3. **超越函数 1 ulp**：`SCOPE.md` §1 已记（宿主 libm；其余 x87 SoftFloat）。
4. **A4 输入路径**：用户裁决"**依然给真实路径，不额外处理**" ⇒ 不固定虚拟路径、不改 `traceInputsHash`；
   A4 的 hash / 指令数仍依赖 `--exe` 的写法（这是已接受的边界；分析报告 `.tmp/path-analysis.md` 保留）。
5. **宿主 fault 归因**：分平台实现（POSIX `sigaction` / Windows VEH），`SCOPE.md` §4 已记，不做抽象层。
6. **宿主页保护粒度**：不需要与真机完全一致；`SCOPE.md` §1 已记（16 KiB 宿主页、非 safe 同页邻页可读写；
   safe 模式仍逐 4 KiB 页检查）。
7. **`WaitForSingleObject`**：用户裁决"命中需要等待的场景 ⇒ **硬错误**，人工介入检查并设计解决方法"。
   已实现：`waitOne` 在未 signaled 且 `dwMilliseconds != 0`（含 `INFINITE`）时 `shimAbort`
   （打印 API / handle / timeout 后终止）；`dwMilliseconds == 0` 的轮询仍返回 `WAIT_TIMEOUT`。
   **`WaitForMultipleObjects(Ex)` 已统一**：没有任何对象 signaled、或 `bWaitAll=TRUE` 且至少一个未 signaled
   且 timeout != 0 ⇒ 同样 `shimAbort`；`bWaitAll=TRUE` 且全部 signaled ⇒ `WAIT_OBJECT_0` 并逐个消费。
   `SCOPE.md` §1 已新增"等待（未 signaled）"行；`shims-core#unsignaled-wait-traps` 用子进程验证 abort。

## 判据 / 门禁
- `file-io#find-data-times`：creation 写回读（Windows / macOS）；变异（退回 ctime）红。
- `shims-core#unsignaled-wait-traps`：子进程 `WaitForSingleObject(h,1)` 必须 `SIGABRT`；
  变异（退回 `WAIT_TIMEOUT`）红。
- `ctest` 23/23；safe 23/23；`pnpm test` 1159 pass；`requirements validate` 6/6；`lift check` 绿。

## 保留
- `SignalObjectAndWait` 走 `waitOne`，已同策略；`WaitForMultipleObjects(Ex)` 已统一（见上）。
- 真实命中等待时游戏会 abort；具体场景要人工看诊断（这正是用户要的"硬错误"）。
