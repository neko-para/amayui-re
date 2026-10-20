# 启动前段定位与跳过记账（WinMain → 窗口/配置 → 根脚本 0）

- id: REQ-01M5C4MVKBC0QVK4KRZTSBYK5V
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-boot-catalog.test.mjs#★ 关键阶段的处置
- tags: [emulator, audit]

## 为什么要开这一单

`apps/emulator-new --install` 现在从**根脚本 id 0**开始执行。之前的交接没有把差集说清：`--install` **不是** `WinMain`，
它是"`WinMain` 走到 `sub_40ED40(Engine, hWnd, 0)` 之后"的那一段；引擎在脚本之前还有一长串宿主阶段。
本单把"真实启动链"定位出来，并把**没建模的阶段逐条登记**（⛔ 不许静默跳过）。

## 定位到的真实启动链（`_WinMain@16` @ `0x4BA890`）

| # | 阶段 | 证据（指令起点 EA） | 本实现处置 |
|---|---|---|---|
| 1 | 单实例信号量 / 互斥（`ARCGameEngine`） | `0x4BA8F5` | host-only |
| 2 | `CoInitialize` | `0x4BA95D` | host-only |
| 3 | 注册窗口类（`RegisterClassExA`） | `0x4BAA02` | host-only |
| 4 | 构造引擎对象（`sub_415640`；`dword_55E1BC`） | `0x4BAA2E` | partial（key 由 CLI 给） |
| 5 | 解析命令行（`B/SP/PATH=/RPATH=/RKEY=/LF=/NH/D=/DOF=/DL=/DV=/DR=/DE=`） | `0x4BAA59` | host-only |
| 6 | 打开索引 `SYS4INI.BIN` / `SYS3INI.BIN`（`sub_414AC0`） | `0x4BAB26` | modeled（S4 族） |
| 7 | 自更新 / 卸载 / AGERC 版本检查（`_.exe`、`/FIX`） | `0x4BAD94` | host-only |
| 8 | 创建窗口（`CreateWindowExA` / `ShowWindow` / 菜单 / IME） | `0x4BB082`（`sub_4BA310`；`CreateWindowExA` @ `0x4BA5CF`） | host-only |
| 9 | 读显示与运行配置（`display:ScreenMode` / `set:*`） | `0x4BB0DD` | **not-modeled** |
| 10 | 装载根脚本：`sub_40ED40(Engine, hWnd, 0)`，第三实参 = 统一文件 id **0** | `0x4BB4CC`（`push ebx` @ `0x4BB4BE`，ebx=0） | modeled |
| 11 | `SetTimer(hWnd, 2, 10000, …)` | `0x4BB50B` | host-only（虚拟时钟） |
| 12 | 主循环 `sub_412290` | `0x4BB517` | partial |

★ 这张表回答的是"**启动流程是怎么定位的**"：从 `_WinMain@16` 的顺序读，`0x4BB4CC` 的 `sub_40ED40` 就是脚本装载入口，
它的第三个实参 `0` 是根脚本 id；索引在 `0x4BAB26`；窗口在 `0x4BB082`；主循环在 `0x4BB517`。

## 判据（已达到）

1. `apps/emulator-new/src/boot.ts` 的 12 个阶段每个都有 `disposition` / `why` / `anchor`（EA）；
2. `--install` 的 `--json` 报告带 `startup.stages`；文本模式逐条打印**非 modeled** 的阶段；
3. pure 守卫 `emulator-new-boot-catalog` 钉住"配置未建模 / 入口与根脚本已建模 / 窗口与定时器宿主 / 主循环部分"；
4. boot assets 守卫断言报告与 catalog 一一对应；
5. `pnpm test` / `spec validate` / `requirements validate` / `typecheck` 绿。

## 非目标 / 下一步

- **配置服务**（`display-config`）建模：脚本随后用 `load-int` / `load-string` 读它（0x1a2 / 0x1a3 / 0x1a9 / 0x1aa）。
- 消息泵、影片位服务（`main-loop` 的 partial 部分）。
- 自更新 / 窗口 / COM 不打算建模（host-only），但必须一直在报告里可见。
