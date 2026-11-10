# 🐞S3 响亮失败既不响也没人守：ddraw「无法回收」分支无守卫 + com 两处拒绝走 LogLevel::Info（默认看不见）

- id: REQ-01M70FP71PWDFKGQ06QE37EQJY
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/mechanism.cpp#com-rejection-visible-at-default-level
- repro: apps/emulator-lift/src/runtime/com.cpp#comLog
- severity: S3
- done_reason: #2 已修：com.cpp 新增 comWarn（Warn），两处响亮拒绝改用它（comLog 其余保持 Info），守卫在默认级别用 stderr tee 断言可见、变异实测红。#1 父级裁决：destroySurfaceState 的「无法回收」分支不配守卫，维持默认可见的防御性日志——造未 wire 的 X86 只能给生产代码加测试钩子或拆 Machine::wire，代价大于守住一条不可达分支；复审已用「删掉整段 ⇒ 全套 CTest 仍绿」证明它只能人工复核（事实，非猜测）。
- tags: [emulator-lift, ddraw, com, logging]

## 判据（存在性 + 观测）

本批收口时由独立复审（自造变异 C）实测暴露的两个"响亮失败其实不响 / 没人守"的缺口。**同一条根因：宿主侧资源记账的失败信号既看不见、也没人钉。**

| # | 位置 | 现象 | 判据 |
|---|---|---|---|
| 1 | `repo:apps/emulator-lift/src/runtime/ddraw_surface.cpp` 的 `destroySurfaceState`（`machineOf(cpu) == nullptr` 分支） | owner 为空而 `arena_ea != 0` 时打 `[ddraw] … [warn]`。**复审把整段删掉（连 `ddrawWarn` 一起，否则 `/WX` 的 unused-function 挡在编译期）⇒ `ddraw` / `d3dx9` / `d3d9-pipeline` 全绿** ⇒ 该"醒目告警"没有任何守卫 | 需要一个**能造出未 wire X86** 的夹具入口（或一个可注入的"无归属"路径），否则这条永远只是"写了但没人验" |
| 2 | `repo:apps/emulator-lift/src/runtime/com.cpp` 的两处**响亮拒绝**（`owns_owner && owner == 0`、以及对象到 0 时 `giveBackOwner` 的失败出口） | 都走 `comLog`，而 `comLog` 的级别是 `LogLevel::Info`（`com.cpp` 顶部），**默认 `--log-level` 是 Warn** ⇒ 默认跑法下这两行根本看不见。同目录的 `ddrawWarn` / `d3dxWarn` 都是 Warn ⇒ 资源记账的失败信号级别不一致 | 默认级别下必须可见（升 Warn）；并有一条守卫断言"默认级别能看见这行" |

## 为什么现在不做（严重度）

- 两条都**不影响 guest 可见结果**（一条是纯告警、一条是日志级别），按"现在不触发 / 不改变行为"取 S3。
- #1 只在未 `Machine::wire()` 的 X86 上可达（生产路径不存在），所以夹具要**专门造**这种状态 —— 这就是它一直没守卫的原因。
- #2 的后果是**归因成本**：真出现 double-release / 漏 release 时，默认跑法里没有任何一行提示。

## 修法边界（给将来的 agent）

- #2 优先且便宜：把两处 `comLog` 升到 `LogLevel::Warn`（⛔ 不要顺手把所有 com 日志都升，那会淹没默认输出），
  并在 `repo:apps/emulator-lift/tests/d3dx9.cpp` 或 `tests/mechanism.cpp` 里加一条"默认级别可见"的断言
  （拷 stderr 子串，形态照 `tests/` 里既有的 log-capture 用法）。
- #1 需要先裁决"要不要给这类纯防御分支配守卫"：⛔ 不要在 `destroySurfaceState` 里加 `#ifdef TEST` 钩子去绕过 wire ——
  那会把生产代码改成夹具友好。候选：让 `Machine::wire()` 拆出"只装归属"的最小步骤，夹具用它造"有归属但没设备"的 X86；
  或承认"该分支只能人工复核"，**在正文里写明理由**并把它从"响亮失败"降级为防御性日志。
- ⛔ 不许把这两条并进别的生活周期单（它们的失效方式是"信号看不见"，不是"资源漏了"）。

## 收口（2026-11-10）：#2 修了 + #1 裁决为"防御性日志、不配守卫"

- **#2 已修**：`com.cpp` 新增 `comWarn`（`LogLevel::Warn`），只给**两处响亮拒绝**用（`owns_owner && owner == 0`、对象到 0 时
  `giveBackOwner` 的失败出口）；`comLog` 其余**保持 Info**（⛔ 没有顺手把所有 com 日志升上去，那会淹没默认输出）。
  守卫 `repo:apps/emulator-lift/tests/mechanism.cpp` 的 `com-rejection-visible-at-default-level`（**默认级别**下用 stderr tee 断言那行可见）；
  变异（`comWarn` → Info）实测红。
- **#1 裁决（父级）**：`destroySurfaceState` 的"无法回收"分支**不配守卫**，维持**默认级别可见的防御性日志**。
  理由：造出"未 `Machine::wire()` 的 X86"要么给生产代码加测试钩子（⛔ 明令禁止），要么把 `Machine::wire()` 拆出一个
  只为夹具服务的半装步骤（收益=守卫一条**不可达分支**的日志，代价=装配路径多一个状态）。⇒ 它是**只能人工复核**的那一类，
  复审已用"删掉整段 → 全套 CTest 仍绿"证明了这个事实（不是猜的）。
  ⚠ 同一分支的"显式告警"本身在 `ddrawWarn`（Warn）里 ⇒ **默认可见**这条要求仍然成立，只是**没有自动化**守着它。

## 与别的单的关系

- 发现于 `REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`（04C）与本批三张修复的**独立变更复审**（自造变异 C 存活）。
- 同根但不同失效方式：`REQ-01M701NYF2B3YV1YFSZBMPQZ41`（按 EA 认对象 ⇒ **读到错的东西**）；本单是**信号看不见**。
