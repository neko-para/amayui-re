# M1 收尾：待裁决（SCOPE 三处 / done 凭据）+ 技术债五条

- id: REQ-01M6J7A2EXR03SSTZYAGM69TY0
- type: req
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- tags: [emulator-lift m1 followup]

## 一句话
M1 本体已可收口（D6 七族 stub = 0、A1–A5 机械绿），本单只装**还没定的三件事 + 五条技术债**，避免它们散落在对话与 `.tmp/` 里。

## 现状（2026-11-04 复验）
- `ctest` **18/18**（`LIFT_SAFE_MEMORY=1` 也 18/18）· `pnpm test` **1152 pass / 0 fail** · `pnpm tools lift check` **绿**
- 导入 310 = 实现 **146** + 显式 stub **164** + 裸缺口 **0**；**D6 七族内 stub = 0**（七族逐个 0）
- A4（以 ctest 为准）：`insns=53134 hash=9C9864F386C36794 inputs=8DEE953E04484EFF`，两次相同

## 待裁决（3 条，都要用户点头）
1. **线程族（导入级 10）与 SEH 族（2）要不要写进 `SCOPE.md`**：`CreateThread` / `ExitThread` / `ResumeThread` /
   `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask` / `FreeLibraryAndExitThread` /
   `CreateTimerQueue` / `CreateTimerQueueTimer` / `DeleteTimerQueueTimer`；`RaiseException` / `RtlUnwind`。
   依据：D5（guest 单线程，README §2.5）与 plan §9 R1（不做完整 SEH 展开）。
   ⇒ 批准后把键填进 `tools/lift.mjs` 的 `FAMILY_STUB_APPROVED`（现留空，`check` 靠"七族为 0"保持绿）。
2. **资源族 4 条 + `CreateProcessA`**（`FindResourceA` / `SizeofResource` / `LoadResource` / `LockResource`）：
   注册了但 **PE 未导入**；要不要一并登记进 `SCOPE.md`，还是等 M2 做资源时再说。
3. **M1 现在是否标 `done`**：能力类收口要 `verify: 路径#测试名`，而本线的守卫是 **CTest 名**（`boot-trace` /
   `decode-crosscheck` …），与既有 JS 用例名口径不同 ⇒ 要么先确认 validator 接受 CTest 名，
   要么用 `done_reason` 写裁决。**未定之前不要硬标 done**（会造出"挂空守卫"）。

## 待处理（技术债 / 缺陷，按建议顺序）
1. **fault handler 的归因会撒谎**（`src/runtime/memory.cpp`）：诊断用的是**陈旧的 `g_access`**，
   实测把一次**宿主** AV 报成 `guest WRITE 0x00410000`。M2 的窗口/绘制调试**全靠这套诊断** ⇒ **M2 开工前必修**。
2. **`GetLocaleInfoA` 未与 `GetLocaleInfoW` 对齐**（A 版仍是 bootstrap 的 CRT 兼容 shim）⇒ 应合并成一张表。
3. **S6 未复核语义**：`access=0` 的打开模式、共享冲突、`GetTempFileNameA` 忽略 `uUnique`、
   `FindFirstFileA` 的 `FIND_DATA` 时间近似、`DuplicateHandle` 不做权限降级（只支持同进程）、`SetFilePointerEx` 缺夹具。
4. **`pnpm test:mutation` 全量未通过验证**：C++ 变异要重建，整轮耗时过长；本轮跑到一半被系统休眠打断，
   并把 `packages/age-format/src/engine/layout.mts` **留在了变异态**（`base 0x5d880`，正确是 `0x5d894`）——
   已手工 `git checkout` 恢复。⇒ 建议给工具加一条**中断自愈**：启动时检测未复原的变异并自动恢复（或拒绝运行）。
5. **未归类 stub 152 条**：属 M2/M3 范围（user32 65 / gdi32 27 / d3dx9 19 / winmm 17 / mf+mfplat 13 …），
   **不是欠账**，但 `lift check` 的"未归类"一栏会一直显示它，别误读成"七族没做完"。

## 怎么复现
```powershell
cd apps/emulator-lift; cmake --build --preset windows-x64; ctest --test-dir out --output-on-failure
pnpm test ; pnpm tools lift check ; pnpm tools lift report --bench 5000000
```

## 交接与真源
- handoff：`.tmp/lift-m1a-handoff.md` · `.tmp/lift-m1b-handoff.md` · `.tmp/lift-m1c-handoff.md`（**不入库**）
- 计划 / 判据：`repo:apps/emulator-lift/docs/plan-m1-cpu-base.md`；口径：`repo:apps/emulator-lift/README.md`；偏差：`SCOPE.md`
- 下一个里程碑：M2（入口卡点是 `USER32!LoadIconA`，EA `0x004BA9AA`）
