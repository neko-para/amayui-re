# emulator-lift · 测试按「比例」瘦身：默认档 ctest 收敛（先砍无意义用例，再谈 CPU 光栅迁移）

- id: REQ-01M7AQ5QKKPC8YJTQA9DNRG2XA
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 收敛完成，裁决表进正文（12 行：删/合/降档各带一句'为什么正常团队不会为它写用例'）。读数：默认档注册 96 → 86，期间新增 4 条合同（cursor-warp / cli-cursor-warp / advapi32-registry / softfloat-tls）⇒ 实际删/并 14 条；全量 86/86 passed、空载 38 s（有并发负载 55 s）、单条最长 ~10 s（门槛 30 s）；旧'卡住 ≥10 分钟'的 overlay-window-smoke（TIMEOUT 1200 s）已删。ctest -N 名单里已无任何'跑游戏并断言业务结果'的用例。引用不断链：requirements validate 6/6、pnpm verify 500/0（子单 FBEQS7HB 的 verify 锚挂在 tools/test/ledger.test.mjs#CTest 分级）。⚠ 本单旧的 LIFT_ASSETS / LIFT_REPRO 两档**已在正文里改成现行口径**（repro 档取消）—— 那两档的原文与子单的模型冲突，属于过期描述，不是这次新删的。
- tags: [emulator-lift, ctest, tests]

## 一句话

默认 `ctest` 曾**偏高、偏碎、偏慢**：判据被用成"只要能静默坏掉就该有一条用例"。本单按"正常项目的比例"审计并收敛；
**测试模型**本身（只留 shim 合同 + 压力/效率、取消 repro 档）由子单 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB` 定案，本单负责**收敛与引用迁移**。

## 用户口径（2026-11-13，⛔ 不重新裁决）

1. **「测试有价值」≠「应该加/留着测试用例」。**
2. 判据按**正常开发项目**的比例：一个正常团队会不会真的写这条、留着它。
3. 优先**合并**：同一子系统的碎用例（每个状态位一条 / 每个参数组合一条）合成一条有信号的。
4. `AGENTS.md` §10 的三问（"删掉会不会让错误静默通过"）**不再单独构成留的理由**。

## 判据

- 默认档（顶层注册 ⇒ pure）的条数与总时长**显著下降**；
- **不丢真信号**：用户可见的坏画面 / 起不来 / 崩溃 / 存档坏 各留一条能红的；
- 每个被删/被合的条目都有一句「为什么正常团队不会为它写用例」（见下面裁决表）；
- 引用不断链（`requirements validate` / `pnpm verify` 绿，⛔ 不留悬空锚）；
- 收口证据：`ctest --test-dir out` 的条数 + 时长 + 失败数。

## 2026-11-14 收口读数（重跑：`cd apps/emulator-lift && ctest --test-dir out`）

- 注册条数 **96 → 86**；期间**新增** 4 条合同（`cursor-warp` / `cli-cursor-warp` / `advapi32-registry` /
  `softfloat-tls`）⇒ 实际**删/并 14 条**。
- 全量：**86/86 passed**，空载 **38 s** / 有并发负载时 55 s；单条最长 `d3d9-pipeline-gpu` ≈10 s（门槛 30 s）。
- 旧"卡住 ≥10 分钟"的那条（`overlay-window-smoke`，`TIMEOUT 1200 s`）已随资产档重组删除。
- 默认档 `ctest -N` 名单里**已无**任何"跑游戏并断言业务结果"的用例（逐名核过，见子单 done_reason）。

## 裁决表（删 / 合 / 降档，一行一条）

| 条目 | 处置 | 为什么正常团队不会为它写用例 |
|---|---|---|
| `quit-graph` / `save-graph` / `close-confirm` / `option-roundtrip` | 删 | 缺陷复现件：为某个已修缺陷临时写的 UI 路径，不是 shim 合同；复现命令进 `defects.md` |
| `interaction-graph` / `interaction-script-e2e` | 删 | 判据是"走通某段交互"= 游戏的业务，不是我们的接口 |
| `movie-logo` 的**正确性**判据 | 删 | "播完了 / 1280×720 / samples≥20" 是业务；其 shim 合同部分（`IMediaSample2` 属性对称）下沉到 `dshow-graph` |
| `boot-trace`（"能到 L2"） | 删 | 启动进度不是合同；启动**耗时**改由 `stress-boot` 的读数 + 阈值承担 |
| `title-entry-paths` / `agerc-ui-state` / `recovery-dialog` | 删 | 界面状态与弹窗路径 = 业务；真缺陷的复现命令进 `defects.md` |
| `mip-e2e` | 删 | CPU 光栅的逐像素 oracle；随 CPU 光栅退休一起消失 |
| `native-override-e2e` / `skip-logo` / `base-coverage` | 删 | 被测物或判据随 CPU 光栅 / flag 变化失效；读数迁 `observations.md` |
| `lift/raster-flag-gone` | 摘注册 | `--raster` 删除后是**空壳**：注册了但没有分派分支 ⇒ 恒绿、什么都不验 |
| `dispatch-tax` | 降档 → `LIFT_STRESS` | 它是吞吐测量，不是接口合同；判据是读数 + 阈值 |
| `overlay-window-smoke` | 删 | 真窗口 smoke，`TIMEOUT 1200 s` 本机挂 ≥10 min；覆盖由 `overlay` / `overlay-gpu` 两条纯用例承担 |
| 像素断言（`d3d9` / `d3d9_pipeline`） | 合/删 **139 → 54** | "每个 blend / 采样组合一条"那类；留少量能红的代表 + GPU 臂 |

## 档位（现行；⛔ 旧的 `LIFT_ASSETS` / `LIFT_REPRO` 两档已取消）

| 档 | 内容 | 怎么开 |
|---|---|---|
| 默认（pure） | **设计的合同**（快、确定性；单条 ≤30 s） | 无条件顶层注册 |
| `LIFT_STRESS` | **压力 / 效率**（判据 = 读数 + 阈值；唯一允许跑真游戏的一档） | `$env:LIFT_STRESS=1` 重新 configure |

复现件**不进测试**：只作复现命令留在 `docs/03-emulator/defects.md` / `observations.md`。
判据 = CMake **注册位置**（`tools/lib/ledger.mjs#cmakeTestTier()`），不建清单文件。

## 非目标

- ⛔ 不为变快而**放宽断言** / 把 SKIP 当 pass / 调大容差（那是拿绿换绿）。
- ⛔ 不删掉"真的会把引擎跑坏"的那几条（起不来、黑屏、存档坏、崩）。
- ⛔ 不动 `apps/emulator-lift/tests/` 以外的东西来凑时长。
