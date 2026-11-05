# M2 · 混音线程裁决：DSOUND 建线程（0x004B5FE4 CreateThread）是硬错误还是伪线程/合流

- id: REQ-01M6MQQA737CB8BAENQK088GP4
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/sync_ext.cpp#pseudo-thread
- done_reason: 用户 2026-11-05 裁决走伪线程：CreateThread 只发句柄、不执行 start routine；ResumeThread / SetThreadPriority / SetThreadAffinityMask / GetProcessAffinityMask 按单核单线程返回；GetThreadPriority 接受伪线程。boot 越过 0x004B5FE4，随后修 EnumFontFamiliesExA 的 Shift-JIS filter + GetSurfaceLevel / GetBackBuffer / SetRenderTarget，1.2e9 指令内到达 Present 第 1 帧（hash=0xB04C136E75952CDE，非背景 895368/921600，330 色；随后同 hash 稳定到第 494 帧）。SCOPE §1 已从「硬错误」改为「伪线程」；lift check 绿，ctest 41/41 绿。
- tags: [emulator-lift, m2, audio, thread]

## 现场（2026-11-05 实测，DSOUND silent 层落地之后）
```
[dsound] Unlock: p1=0x10246E38 n1=176400 ⇒ DS_OK（本区间非 0 字节 0；累计 PCM 字节 176400，plays=0）
  kernel32!CreateThread  (stub 0x7FFE01CC, conv=stdcall, argc=0)
stop      : unimplemented @ eip=7FFE01CC      insns = 109,536,868
```
DSOUND 已过（`LoadLibraryA` 假句柄 + `DirectSoundCreate` 动态导出 + 两个缓冲 + `Lock`/`Unlock`/`Play` 记账全绿），
boot 现在停在**混音线程**上。

## 取证（`.lst`；锚 = EA）
- `0x004B5FD9 push lpThreadId` / `0x004B5FDA push 4` / `0x004B5FDC push esi` / `0x004B5FDD push offset sub_4B5AA0` /
  `0x004B5FE4 call CreateThread` → `0x004B5FF1 call j_ResumeThread` → `0x004B6000 call j_SetThreadPriority`。
  整个尾巴在 `sub_4B5CF0`（dsCreate，`0x004B5CF0`）里，**无 NULL 检查、无分支** ⇒ 设备建成之后必然走到。
- 线程体 = `sub_4B5AA0`；`dsCreate` 之前还建了 61 个事件（`0x004B5FC8 j_CreateEventA_1` 循环，`0x004B5FBB ebx=3Dh`）
  —— 那是流式缓冲的 position-notify / 唤醒事件，线程体等的就是它们。
- 触发链：`0x00406D2B` LoadDLL 成功 ⇒ `0x00406D86 call sub_4B5CF0` ⇒ 这里。`sound:UseDirectSound` 缺省 = 1
  （`--appdata` 里没有 `SYS4REG.INI` 时就取缺省），所以这条路径**不是**可配置绕开的。

## 为什么要一条裁决（而不是顺手做）
`SCOPE.md` §1 把线程 / 定时器队列整族判**硬错误**（`addUnimplemented`，执行到就 trap），并写明
"**若成功路径真的创建线程 ⇒ 记录并开单**，⛔ 不在本线直接开并发"。现在正是那个"成功路径真的创建线程"。
`README.md` §2.5 的 guest 单线程口径要先改，才谈得上实现。

## 两个候选方向（各有代价，需要拍板）
1. **伪线程**：`CreateThread` 返回非空假句柄、线程体**永不运行**，`ResumeThread`/`SetThreadPriority` 成功返回。
   代价：混音线程不跑 ⇒ 流式缓冲永不消费、那 61 个事件永不被 signal；引擎若在主循环里等它们，会变成**静默挂住**
   （比 trap 更坏）。⇒ 必须先查清 `sub_4B5AA0` 之外有没有"等混音线程"的路径。
2. **单线程合流**：把 `sub_4B5AA0` 的循环体改成"主循环每帧跑一次"（协作式调度），事件用已有的虚拟时钟 + `SetEvent`。
   代价：要给 `CreateThread` 一个显式的"注册协作任务"语义，等于给 `SCOPE.md` §1 开口子。

## 判据（做完长什么样）
- `ctest` 里有一条会红的用例：`CreateThread` / `ResumeThread` / `SetThreadPriority` 三个返回码 + 线程体的
  调度次数（≥1 次/帧 或 明确 0 次），且**不挂住**（`--time-limit` 内到下一个 stop 或 Present）；
- boot 越过 `0x004B5FE4`，报出下一个 stop（理想情况是 `Present` ⇒ 第一帧哈希 + 非背景像素数）。

## 不做
⛔ 本单不含"顺手把 `CreateThread` 改成软 stub"——那会把"线程没实现"伪装成"线程在跑"（正是 `SCOPE.md` §3 禁的）。
⛔ 不碰 `apps/emulator-lift/src/shims/kernel32_sync.cpp` 之外的线程族，也不引入宿主并发。
