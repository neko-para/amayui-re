# S6 前三族：内存 / TLS / 同步族的可再校验观察缺失 + Virtual*/等待语义偏差

- id: REQ-01M6JFZ73GTZE9F6967RRSH6CE
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/shims_core.cpp#heap-alloc-zero-and-size
- tags: [emulator-lift, m1, s6, tech-debt]

## 一句话
S6 前三族（同步 / 内存 / 错误码·TLS）的"每族一组夹具"原先只对同步族部分成立：内存族与 TLS/Fls 在 `tests/` 里 0 命中；同步族 20 条行为没有断言。**本单补上这些可再校验观察**。

## 落地（2026-11-04）
- 新增 pure CTest `apps/emulator-lift/tests/shims_core.cpp`（顶层注册，**23/23** 条 CTest）：
  - 内存：`heap-alloc-zero-and-size`（普通 / ZERO_MEMORY 写脏→free→复用同块→读回 0 / 0 字节 + `HeapSize`）、
    `heap-free-invalidates-size`（释放后 / NULL / 非堆地址 ⇒ `0xFFFFFFFF + 87`）、
    `heap-realloc-keeps-content`（保内容；`0xFFFFFFF8` 请求 ⇒ 0 + 8 且原块不动）、
    `local-and-global`（LocalAlloc/LocalSize/LocalFree + GlobalAlloc/Lock/Size/Unlock/Free）、
    `virtual-alloc-commit-release`、`virtual-decommit-recommit`；
  - 错误码 / TLS：`last-error-and-ids`、`tls-slots`（TlsAlloc/Set/Get/Free + 清 last error）、`fls-slots`；
  - 同步：`event-set-reset-wait`（WAIT_TIMEOUT=258 / WAIT_OBJECT_0 / Reset）、
    `semaphore-release-previous`（previous count、超上限 298）、
    `mutex-release-owner`（非持有者 288 / 递归 / 两次 release）、
    `critical-section-reentrant`（RecursionCount / OwningThread）、`interlocked-increment-exchange`。
- 夹具用生产路径 `GuestHeap::init` + `setupProcessStructures`（不手抄 TEB 偏移）。
- 变异：`ReleaseMutex` 去掉持有者校验 ⇒ `shims-core` 的 `mutex-release-owner` 红（实测 CTest 退出码 8）。

## 拆出去的部分（不在本单）
- **`Virtual*` 三处与文档冲突**（hint+MEM_COMMIT 不校验 reserve / MEM_RELEASE 非 LIFO / MEM_DECOMMIT dwSize=0）
  ⇒ 已开单 `REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4`；本单夹具只覆盖正常分配 / RELEASE / DECOMMIT。
- **等待是否真阻塞** ⇒ 已归入待裁决单 `REQ-01M6JHVQJ6Q4YHG8N2BJP2ANJJ`；本单只钉
  "已 signaled 立即返回 / 未 signaled 1ms 超时" 这两个当前行为，不把它写成真机等价。

## 判据 / 门禁
- `shims-core`：14 cases / **203 checks / 0 failures**；`LIFT_SAFE_MEMORY=1` 同绿。
- 全量 `ctest` **23/23**；`pnpm test` 1159 pass；`requirements validate` 6/6；`lift check` 绿。

## 保留（未覆盖 + 原因）
- double free / 野指针 free：设计上 abort（进程级）。
- 线程创建：SCOPE §1 硬 trap；`WaitForMultipleObjects` 阻塞行为属待裁决。
- 可移动 `GlobalUnlock` 锁计数（偏差 2 把 MOVEABLE 当 fixed）。
- 命名对象 / Open\* / DuplicateHandle / SList：已由 `sync_ext` 覆盖。
- `HeapCreate/Destroy`、`VirtualProtect`、`CoTaskMem*`、`LocalRealloc/GlobalRealloc`、`InterlockedExchangeAdd/CompareExchange`：本单未点名。
