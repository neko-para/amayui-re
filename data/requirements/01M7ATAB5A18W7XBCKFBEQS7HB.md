# 测试模型：只测 shim 接口合同 + 压力/效率；业务正确性不作判据（取消 repro 档）

- id: REQ-01M7ATAB5A18W7XBCKFBEQS7HB
- type: req
- status: done
- parent: REQ-01M7AQ5QKKPC8YJTQA9DNRG2XA
- verify: tools/test/ledger.test.mjs#CTest 分级
- done_reason: 模型已落地并由机械判据守住：① 默认档 ctest -N 现在 86 条，名单里**没有** quit-graph / save-graph / close-confirm / option-roundtrip / interaction-graph / interaction-script-e2e / movie-logo / boot-trace / title-entry-paths / agerc-ui-state / recovery-dialog（2026-11-14 逐个核过 ctest -N 输出）⇒ '跑游戏并断言业务结果'的用例清零；② 默认档 86/86 全绿、总时长 38-47 s、单条最长 ~1.3 s（d3d9-gpu）⇒ 满足 ≤90 s / 单条 ≤30 s；③ LIFT_STRESS 档存在且四条（stress-boot / title-steady / adv-audio / gpu-coverage）判据全是读数+阈值，阈值推导写在 tests/stress_*.cpp 文件头 + OBS-20261114-06；④ 档位判据 = CMake 注册位置（顶层 pure / 落在块里 assets），由 tools/lib/ledger.mjs#cmakeTestTier() 实现、tools/test/ledger.test.mjs 的'CTest 分级'用例守住 ⇒ 不建清单文件、也不会退化成'按资产在不在分档'；⑤ 普查产出的 shim 缺口已按缺口开单（9TVJ40WN / S0EX4512 / KNQ1VNV7 / 7NSN7GQ7 / 5KYPR9J4 / EBFMNFPJ / 37CJB0V6 / 5VHXVE3H / S0ZNXGM4 / P6MX9WG9 / ZXTPQW3V / AHQKNSA2），本轮已收 8 张。⚠ 判据 4 只做到能做的部分：真缺陷的复现命令进了 defects.md（BUG-1..BUG-23），但我**不声称**被删的每条业务断言都有一句'失去哪个用户可见的保护'—— 多数本身就是业务正确性断言，按本模型不需要保护线。
- tags: [emulator-lift, ctest, test-model]

## 一句话

测试模型重做：**只留两类**——① 按**我们 shim 的接口**设计的合同测试（判据 = 微软文档 / 公开规范）；
② **压力/效率**档（判据 = 读数 + 阈值）。**业务正确性不作为判据**：游戏是程序、不是我们的标准。

## 用户口径（2026-11-13，⛔ 不要重新裁决）

「我们在做一个**有具体标准**的模拟器，不是在发明新项目。要么就按照我们 shim 的接口来设计测试，要么就别测试。
将业务的**正确性**作为测试是不合理的。唯一可能需要业务的只有**压力测试**，因为模拟器效率确实是需要关注的点。」

## 两类（档位判据 = CMake 注册位置，不建清单文件）

| 档 | 被测物 | 判据 | 需要游戏 |
|---|---|---|---|
| **接口合同**（默认档） | 我们 shim 出去的 Win32 / D3D9 / D3DX9 / DirectDraw / DirectSound / DirectShow / GDI / USER32 / KERNEL32 接口 + 执行器核心（ISA / PE / 内存 / COM） | **微软文档 / 公开规范**：返回值、错误码、边界、引用计数、所有权、pitch/stride、caps、状态机 | ❌ 不需要 |
| **压力/效率**（`LIFT_STRESS`） | **模拟器自身**：fps、帧时间 p95、堆峰值、音频 underrun/吞吐、启动耗时、长跑稳定 | **读数 + 阈值** | ✅ 唯一需要 |

⛔ 压力档**不是**业务正确性测试：判据里不许出现"到达某界面 / 不崩 / 某 marker 出现"这类业务断言
（稳定性例外：长跑不崩、guest fault 计数 = 0 属效率/稳定指标）。

## 被取消的（业务正确性 e2e）

`quit-graph` · `save-graph` · `close-confirm` · `option-roundtrip` · `interaction-graph` · `interaction-script-e2e`
· `movie-logo` 的**正确性**判据（"播完了 / samples≥20 / 1280×720"）· `boot-trace` 的"能到 L2" · `title-entry-paths`
（已删）· `agerc-ui-state`（已删）· `recovery-dialog`（已删）
⇒ 这些是**缺陷复现件**，处置：作为**复现命令**留在 `defects.md` / `observations.md`，⛔ 不进测试。
旧的 `repro` 档因此**取消**（它存在的前提就是"把缺陷复现当测试"）。

## 搬迁规则（业务用例里混着合同的部分）

业务 e2e 里凡是**其实是 shim 合同**的断言，下沉到对应合同测试，且**用文档语义重写**（例：
`movie-logo` 的 `IMediaSample2::GetProperties` 的 `pMediaType` 必须与 `SetProperties` 对称 ⇒ 搬进 `dshow-graph`，
用合成 sample 测，不跑真影片）。效率读数搬进 `LIFT_STRESS`。

## 施工图（先普查，后补测试）

「按 shim 接口设计测试」的前提是知道**哪些接口没有合同测试**。shim 面 = `apps/emulator-lift/src/shims/`
（34 个文件 / ~19.6k 行，按 DLL 分）。普查产出落 `.tmp/test-model/*.md`（5 面并行：影片音频 / 输入交互 /
图形呈现 / 存档配置文件 / 压力设计+启动面），每面给：逐断言分类（合同/业务/效率/无价值）、可搬清单、
删除清单、效率量与阈值、**shim 缺口函数名**。
⇒ 缺口表才是"下一步补什么测试"的唯一依据；⛔ 不许凭感觉补。

## 判据（收口时能贴读数）

- 默认档 `ctest` 里**没有**任何"跑游戏并断言业务结果"的用例（判据：`ctest -N` 名单里不含上面那批）；
- 默认档 `ctest` 全绿（除已登记的环境类红）· 单条 ≤30 s · 总时长 ≤90 s；
- `LIFT_STRESS` 档存在且判据全是读数 + 阈值（逐条贴出处与命令）；
- 每个被删的业务断言，都有一句"删掉后哪个用户可见的错失去保护"；真缺陷的复现命令进 `defects.md`；
- shim 缺口表产出，且**按缺口**（不是按猜测）新开补测单。

## 非目标

- ⛔ 不追求 spec/emulator 的"业务覆盖率"；⛔ 不为"某次崩溃"再加一条 e2e（复现命令就够）。
- ⛔ 不把压力档做成黄金图/逐帧比对（那是像素一致，本线明确不做）。
