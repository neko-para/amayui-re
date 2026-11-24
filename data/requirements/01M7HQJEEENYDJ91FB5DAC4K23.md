# AOT 侧压力/性能档重建：判据 = AOT 的 IR 执行速度（替代随解释器退役的 6 条 LIFT_STRESS）

- id: REQ-01M7HQJEEENYDJ91FB5DAC4K23
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: 档重建完成：S1 自检（永不满足门 + 变异 1/1）+ S2 boot→TITLE + S3 FELLOW（5 次连续 Passed，6 窗口/exit 0）都在 LIFT_STRESS=1 注册；默认档 141/141。S3 的两个前置 bug（翻页窗、2ms 预算）均已 done。未做：路径 3 独立读数、路径 4 自动档、判据 1 的 3 连跑、判据 2 阈值/A-B、FATAL 资产扩展。
- tags: [emulator-lift, aot, stress, perf]

## 一句话

`LIFT_STRESS` 这个档（唯一允许跑真游戏的档）今天**全部由解释器驱动**，而那 6 条要随解释器退役
（用户 2026-11-16 裁决：AOT 速度已远超解释器，测试目标已不存在）⇒ **AOT 侧的压力/性能档要重建**，
否则端到端速度**没有任何自动化读数**。

## 判据

1. 同一份配方 AOT 连跑 3 次，语义指纹逐位相同（`exit` / `frames` / `stop @` / arena 三元组 /
   `table misses`）。⚠ 本批只做了单跑，3 连跑归 `## 未做`。
2. 速度读数有抖动界线（≥3 对交错 A/B 中位 + 极差，写明本机 <5% 不可分辨）。⚠ 人工 / 文档项，未做。
3. 该档只在 `LIFT_STRESS=1` 下注册（`assets` 档，⛔ 不进默认门禁）；缺资产 ⇒ `FATAL_ERROR`。
   ✅ 已核：stress 档 `ctest -N` = 146；默认档 = 141 且 `aot-stress` 命中 0。
4. 一条"能红"的自检：配方改永不满足的门 ⇒ 必须失败。✅ 已落成 `aot-stress-plan-exit5` + 变异 1/1 红。

## 本批（2026-11-24/25 / win32 / dev）

- **S0**：dev 建 `links/raw` junction（gitignored；不带它 `LIFT_STRESS=1` configure 会 `FATAL_ERROR`）。
- **S1 `aot-stress-plan-exit5`**：夹具 `tests/fixtures/boot-title-exit-never.plan`（门 `0x388C..0x38A4`
  永不成立）⇒ **exit 5** + stderr 点名 `<夹具>:<行>` + `pending != 0`；变异（把 `aot_main` 的
  `recipeOutcomeExitCode(...)` 改 `return 0`，`optInEnv=LIFT_STRESS`）⇒ **1/1 红**（CTest 退出码 8）。
- **S2 `aot-stress-boot-title`**：`scripts/recipes/boot-title.plan`（门 `0x0460..0x0520` + `profile` 窗 +
  `quit`，0 条 `wait`）⇒ `exit 0` / `pending=0` / `— input-plan: quit` / `[profile-window] end
  label=boot-title`；窗读数 **1995 ms / 46,815 读点 / 27 presents**（整进程 ≈3.0 s）。
- **S3 `aot-stress-fellow-switch`**：**已转绿**（2026-11-25）—— 删 `DISABLED`；`LIFT_STRESS=1` 下
  **5 次连续 Passed**（每次 ~241 s，`LastTest.log` = `exit=0 timed_out=0 windows=6`），6 个窗口标签
  （`s1-to-avaro` … `s3-to-fia`）逐字齐、`run=finished exit=0`、无 `over=` / `run failed`。
  两个前置：翻页窗 60→1200（`REQ-01M86VRRTPY8W9RP1SFDT0N200`，done）+ 宿主 `profile.end` 暂停
  （`REQ-01M86VRT1YP53CJS0M885FCCJM`，done）。
- **`aot-quit-exit` 的既存假红已修**：裸 `⛔` 断言被正常 warn 文案触发 ⇒ 收窄成产品响亮错的规范前缀
  `emulator-lift-aot: ⛔`（同处的 `派发表里没有的 EA` / `decode-error` 不动）。
- **门禁**：stress 档 3/3 新用例 Passed（S3 5 次）；默认档 **141/141**；`pnpm test` **627**；
  `pnpm verify` **474 / 472 pass / 0 fail / 2 skip**；`requirements validate` 6/6。

## 待测路径（用户 2026-11-16 指定；逐步补充，不一次做完）

| # | 路径 | 状态 |
|---|---|---|
| 1 | **boot → TITLE 稳定** | ✅ 自动档（`aot-stress-boot-title`） |
| 2 | **打开 FELLOW** | ✅ 自动档（`aot-stress-fellow-switch`；5 次连续 Passed） |
| 3 | **加载存档** | ⚠ 只作为 S3 的 `loadSave(9)` 前缀；无独立窗口读数 |
| 4 | **SAVE05 移动（拖拽）** | ⚠ 人工基准；今天 `saves/SAVE.DAT` 持久化页 1 ⇒ `save05-move.plan` 的 `click 300 388` 必点空 |

## 未做

- 路径 3 的独立窗口读数；路径 4 的自动档（先换 `loadSave(5)` 驱动）。
- 判据 1 的 3 连跑、判据 2 的阈值重定、`tools/` 的 A/B runner（⛔ 不做成 test）。
- `FATAL_ERROR` 的资产盘点扩到 `saves/` + 插件 JS + 安装树脚本（`CMakeLists.txt` 公共面，另单）。
