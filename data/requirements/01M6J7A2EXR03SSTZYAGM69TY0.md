# M1 收尾裁决记账（线程/SEH 进 SCOPE · 资源归 M2 · CTest 凭据 · 16K 页缺陷已修 · 技术债拆单）

- id: REQ-01M6J7A2EXR03SSTZYAGM69TY0
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- done_reason: 五条裁决全部落地：线程族 10 键 + SEH 2 键进 SCOPE 与白名单（待批准归零）；资源族归 M2；CTest 名凭据按 emulator-cxx 先例确认；16 KiB 宿主页缺陷修复并新增 memory-pages（macOS ctest 19/19，LIFT_SAFE_MEMORY=1 也 19/19）；技术债拆 4 单挂 M1。M1 自身的 done 要等那 4 单（父不先于子）。
- tags: [emulator-lift m1 followup]

## 一句话
M1 收口前的**裁决记账单**。2026-11-04 用户把五条裁决全部拍定，落地结果见下；技术债已拆成 4 张单挂到 M1 下 ——
**M1 自身的 done 要等那 4 张单**（父不先于子），本单只记裁决与凭据口径。

## 已裁决与落地（2026-11-04 用户）
| # | 裁决 | 落地 |
|---|---|---|
| 1 | 线程 / 定时器队列整族：按计划特殊处理，记进 SCOPE | `SCOPE.md` §1 新增行 + §2 点名 + 依据/退路；`FAMILY_STUB_APPROVED` **+10 键** |
| 2 | 异常（SEH）：**发生即失败**，记进 SCOPE | `SCOPE.md` §1 新增行 + §2 点名；`FAMILY_STUB_APPROVED` **+2 键**（`RaiseException` / `RtlUnwind`）⇒ "待 SCOPE 批准" = **0** |
| 3 | 资源族 4 条等 M2 | 不改 `SCOPE.md`；记进 M2 单（实测无 guest 调用点 + M2 二选一） |
| 4 | 线程档位：先按**硬 trap**，执行到再调 | `SCOPE.md` 写明"硬 trap"；plan D5 的"返回失败或空句柄"作为备选一并记下 |
| 5 | 16 KiB host 页缺陷：**需要解决** | 已修 `apps/emulator-lift/src/runtime/memory.cpp`（提交/退回按宿主页对齐，退回只退完整覆盖的宿主页）；新增 pure CTest `memory-pages`；缺陷单 `REQ-01M6JBQ05P1QF56DQX5B9YJ4V8` 已 done |
| 6 | 技术债逐个开单，挂 M1 下 | 4 张单（见下） |

## 已拆单（挂 M1 下）
- 🐞 `REQ-01M6JCBYBW8DV0920T28337ACF` fault 诊断把宿主 AV 报成上一次 guest 访存（S2，**M2 开工前必修**）
- `REQ-01M6JCC1F886N40CYHJV2KHW1H` GetLocaleInfoA 与 GetLocaleInfoW 统一
- `REQ-01M6JCC22PHPC24ZQN4P65Z1K5` S6 六处语义未复核（打开模式 / 共享冲突 / uUnique / FIND_DATA 时间 / 权限降级 / SetFilePointerEx）
- `REQ-01M6JCC4QZ90GX9450Y2A9RCR2` test:mutation 中途被打断会留下变异态 → 启动自愈
- （原 #5「未归类 stub 152 条」不是欠账，不开单）

## CTest 名当凭据的先例（本次查找结果）
`validator` 只要求"锚点字符串出现在该文件里"，所以 **CTest 名可以直接当锚点**。既有先例在 emulator-cxx：
`verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-selftest` · `#emulator-core-adv-ruby` · `#emulator-core-config-toggle`
（另有 10 余个节点同形；也有 `tests/core_selftest.cpp#check_exit_cleanup` 这种"源码文件 + 用例名"的写法）。
本线的对应写法：`apps/emulator-lift/CMakeLists.txt#boot-trace`（A4）· `#base-coverage`（A5）· `#memory-pages`（平台页语义）。

## 现状（2026-11-04 复验，macOS arm64）
- `ctest` **19/19**（`LIFT_SAFE_MEMORY=1` 也 **19/19**；修复前是 **3/18**）
- `pnpm test` **1152 pass / 0 fail**（本次实跑）
- `lift check` 绿：导入 310 = 实现 146 + 显式 stub 164 + 裸缺口 0；七族 stub 0；「待 SCOPE 批准 **0** 条」
- A4（macOS 本次实跑）：`insns=60652 hash=D06091AF5DD19647 inputs=35CB9281F6D0881B`，run1/run2 相同，停在 `eip=0x004BA890`
  ★ 与早先 Windows 记录（`insns=53134 hash=9C9864F386C36794 inputs=8DEE…`）**不同**：A4 的输入清单含 exe 路径串 / 环境块，
  跨平台本来就不同 ⇒ 两个数各自只在本平台有意义（A4 只要求同一命令跑两次相同）。

## 更正（保留，不静默改写）
- 原"资源族 + `CreateProcessA` 都 PE 未导入"的说法不对：**`CreateProcessA` 是导入的**，`.lst` 三处调用
  （`0x0047824D` 卸载 / `0x004BADF4` `/FIX` / `0x004BAFA8` `_.exe` 回退）；只有资源族 4 条是"注册了、导入表里没有、`.lst` 也没有调用点"。

## 怎么复现
```bash
cd apps/emulator-lift; cmake --preset macos-arm64 && cmake --build --preset macos-arm64
ctest --test-dir out --output-on-failure                          # 19/19
LIFT_SAFE_MEMORY=1 ctest --test-dir out --output-on-failure       # 19/19
pnpm test ; pnpm tools lift check
```

## 交接与真源
- 计划 / 判据：`apps/emulator-lift/docs/plan-m1-cpu-base.md`；口径：`apps/emulator-lift/README.md`；偏差：`apps/emulator-lift/SCOPE.md`
- 下一个里程碑：M2（入口卡点是 `USER32!LoadIconA`，EA `0x004BA9AA`；资源族归 M2）
