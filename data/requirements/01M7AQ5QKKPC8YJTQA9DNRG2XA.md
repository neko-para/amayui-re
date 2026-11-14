# emulator-lift · 测试按「比例」瘦身：默认档 ctest 收敛（先砍无意义用例，再谈 CPU 光栅迁移）

- id: REQ-01M7AQ5QKKPC8YJTQA9DNRG2XA
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, ctest, tests]

## 一句话

默认 `ctest` 这一档**偏高、偏碎、偏慢**：判据被用成了"只要能静默坏掉就该有一条用例"，
结果是 96 条注册用例里有一批是正常项目不会写的东西。本单按**比例**（正常项目的流程）审计并
收敛用例集，**先砍无意义用例，再谈 CPU 光栅迁移**（迁移量取决于先砍掉多少条像素断言）。

## 用户口径（2026-11-13，本单的裁决，⛔ 不要重新裁决）

1. **「测试有价值」≠「应该加/留着测试用例」。** 不要因为"这里可能会被破坏、且静默"就判"必须加"。
2. 判据按**正常开发项目**的比例：一个正常团队**会不会真的写这条**、**留着它**。
3. 优先**合并**：同一子系统的碎用例（每个状态位一条 / 每个参数组合一条）应合成一条有信号的。
4. 旧口径 `AGENTS.md` §10 的三问（"删掉会不会让错误静默通过"）**不再单独构成留的理由** ——
   它只说明"这条断言有意义"，不说明"它值得占一条用例"。本仓历史上的过度守卫正是从这里长出来的。

## 判据（本单收口时要能贴读数）

- 默认档（pure，注册位置在顶层）的 `ctest` 总时长与条数**显著下降**，且：
- **不丢真信号**：用户可见的坏画面 / 起不来 / 崩溃 / 存档坏 这几类仍各有一条能红的；
- 每个被删/被合的条目，都有一句「**为什么正常团队不会为它写用例**」（写在提交正文或本单的裁决表里）；
- 引用不断链：被删条目若是某张需求单的 `verify:` / `repo:`、或 `data/spec` 的证据锚、或 `docs/` 里的可复跑命令，
  **同批**改指向留下的那条（或降级为 `done_reason` / 残差），⛔ 不许留悬空锚（`pnpm tools spec validate` /
  `requirements validate` 必须绿）；
- 收口证据：`ctest --test-dir out --output-on-failure` 逐条时间表 + 条数 + 失败数。

## 非目标

- ⛔ 不为变快而**放宽断言** / 把 SKIP 当 pass / 调大容差（那是拿绿换绿）。
- ⛔ 不删掉"真的会把引擎跑坏"的那几条（起不来、黑屏、存档坏、崩）。
- ⛔ 不动 `apps/emulator-lift/tests/` 以外的东西来凑时长。

## 已测的现状（读数，不是断言；重跑命令见下）

- `ctest --test-dir out -N`：**96 条注册**（本机 Windows x64 + 安装树存在 ⇒ assets 档也注册）。
  CMake 里共 **102** 个 `add_test`，按注册位置分档：**pure 79 / assets 23**。
- `overlay-window-smoke` 的 `TIMEOUT` 是 **1200 s**；本机实测它**挂住 ≥10 分钟**（子进程 `emulator-lift.exe`
  在跑、几乎不吃 CPU）⇒ 全量 `ctest` 的墙钟不由"多数用例"决定，而由少数几条决定。
- 开工前就存在的红（与本单无关，另行开单/已开）：`host-encoding`（19 checks / 1 failure）、
  `gpu-batch-barrier`（367 checks / 1 failure，`gpu-batch/headless-hidden-window` 子进程 exit 3）。
- 逐条墙钟表：`.tmp/suite-audit/timings.tsv`（`pwsh -File .tmp/suite-audit/time-all.ps1` 生成，
  ★ `.tmp/` 是本地临时区 ⇒ 本单收口前要把结论（不是脚本产物）写成本节的读数）。

## 步骤

1. **普查（5 面，只读并行）**：核心语义 / Win32 面 / D3D-GPU 面 / 音频-影片 / CLI-e2e。落 `.tmp/suite-audit/g*.md`。
2. **父级裁决**：合并成一张「删 / 合 / 降档」表，冲突的留待核（⛔ 不选一个了事）。
3. **落地（串行，每批一层复审）**：合并 → 删 → 引用迁移 → 门禁。
4. 回头再把 `REQ-01M7A74YG08R65NJBFXF7DEJDK`（CPU 光栅迁移）按"先砍完"的新规模重排。


## 裁决补遗（2026-11-13，用户指出病根）：**缺陷复现件不是守卫**

`quit-graph` / `save-graph` / `close-confirm` / `option-roundtrip` 这类 e2e，是**发现某个缺陷时临时加的复现路径**，
没有经过设计，后来被直接搬进测试用例 ⇒ 等于"每次测试都回归历史上发现过的所有 UI 缺陷"。这不 make sense，
也正撞 `AGENTS.md` §10 的红线（❌ 不要用 test 记住发生过的事 —— 那是 `defects.md` / `observations.md`）。

⇒ **档位（判据 = CMake 注册位置，不建清单文件）**：

| 档 | 内容 | 怎么开 |
|---|---|---|
| 默认（pure） | **设计的合同**（快、确定性；单条 ≤30 s） | 无条件顶层注册 |
| 验收（`LIFT_ASSETS`） | **贵 / 要真跑一遍**的验收面 | `LIFT_ASSETS=1` 重新 configure |
| 复现（`LIFT_REPRO`） | **缺陷复现件** | `LIFT_REPRO=1` 重新 configure |

★ **档位按用途，⛔ 不按"资产在不在"**（用户 2026-11-13）：资产 / 语料 / 字体**都在场**（`links/install` + LFS）。
旧的 `if(EXISTS …)` 当开关在本机**恒真**，唯一效果是让"哪条用例存在"随机器漂移、并把 README 逼成假话
（第 1 批复审抓到的就是它）。现在输入**假定在场**，缺了在 configure 的 STATUS 盘点里可见；
**显式打开某档而输入缺失 ⇒ `FATAL_ERROR`**（响亮，不静默缩水）。

规则：新增一条用例，先回答"这是**设计的合同**还是**某个缺陷的复现**"；复现件一律进 `LIFT_REPRO` 档，
并在 `docs/03-emulator/defects.md` 或 `observations.md` 有对应条目 + 头注释写明"这是谁的复现"。
⛔ 不许再把"再回归一遍历史缺陷"的用例加进默认门禁。
