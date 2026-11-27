# 插件触发模型 v2：AGE VM 指令点 + TypeScript 插件（分析/设计）

- id: REQ-01M8EKFAP4QJY9G12K7KB9YEQB
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, plugin, qjs, typescript, design]

## 一句话

把插件触发从 input-plan 遗留的 Win32 读点模型，重构为 AGE VM 指令点模型；统一同步/异步 JS 回调，缓存 ctx + getter 查询，插件直接用 TypeScript（quickts E1）。

## 核实（2026-11-27，证据在 `.tmp/plugin-v2/`）

- **4 条静默缺陷全部成立**，且 hook 泄漏比宣称更宽：只有 `finish`/`quit` 清 hook；`fail` / 预算超限 / `done` / install 拒绝 / 卸载都不清。hook 表是**进程级 static、无 owner、无代际**。
- **指令点 → JS 没有反向通道**：`instr_hook.cpp` 一行 JS 都没有；`quickjs.h` 只许出现在 `ui/quickjs_value.h` ⇒ v2 必须新造一个不含 JS 类型的 sink。
- **drain 能力现成但没接线**：`drainPendingJobs` 只在模块装载与析构调用；插件回调 / render 跑完不 drain。且 CPU deadline 在 `JS_Call` 返回即清零 ⇒ drain 必须纳入同一预算窗。
- **Run 互斥今天不是"已实现"**：`rg RunControl src` 零代码命中，无 dispatch / endpoint；只有单插件 + 单 TriggerRunner。
- **TS 的"已验证"不可复现**：`quickts_e1.h` / patch / 上游副本 / 验证脚本在仓内、git 历史、`.tmp`、dev 树**都不存在**。上游已确认 = `FrameOS/quickts`（MIT；Bellard 2026-06-04 + 少量 commit；45 hunk / +272 / −6 与需求数字逐字对得上）。直接套到 quickjs-ng v0.18.0 = **18 贴上 / 27 reject**，贴上的也编译不过 ⇒ 要手工重写（约 20 钩子 + 1,092 行头）。
- **性能数字无测量产物**（+0.62% / 6–8 ns / 5 µs / 160 ns 的唯一出处是需求单自身）⇒ 本轮只用功能/静态判据，墙钟另开窗口。

## 结论（已核）

- 异步触发 = `TriggerRunner` 在 11 个 Win32 投递点 poll；同步触发 = `instr_hook` 在 3 个脚本派发 EA（`0x411A6F / 0x41284A / 0x41346B`）的每条脚本指令前。
- 触发点换到 VM 指令点不改 guest 语义：`pos = 0x3C + (pc − body)`；`opcode = rd32(pc)` 也在同一现场。
- 渲染只有 `render` 有 builder；指令点没有 `draw`；UI 状态只在 render 改/画；逻辑只改数据。

## 实施分片（子单见下）

- **S0**：4 条缺陷 + hook owner/生命周期 + patch CAS + render 期拒绝 + `--no-s7` 注入可见性。
- **S1**：quickjs-ng 的 TypeScript E1 擦除 + loader 三处 flag + `.ts` 候选 + `tsc --erasableSyntaxOnly` 常设门禁。
- **S2**：指令点 sink（只读命中）→ 每入口 drain（同一预算窗）→ `once`/`task` → `at` 链 → `Tick` 分层。
- **S3**：`Tick.local`、`ui` 只在 RenderCtx、`log` 全局、config 直读。
- **S4**：examples 迁 v2、`plugin.d.ts` / `plugin-runtime.md`、README。
- **S5**：win32/FIELD 实测 + enum/JSX（按需）。

## 子单

1. `REQ-01M8EKFHDFX7DP4FCWVA7MPQET` 现状：读点异步 vs 指令点同步（能力/性能/渲染/缺陷）——已收口，作为事实底稿。
2. `REQ-01M8EKFS5HSQFP93T19JDD50ZD` 新 JS API：`RunCtx`/`Tick` · `at/once/task` · timeout/日志/UI 分层。
3. `REQ-01M8EKFSVE4RF8WZW60C5QHV59` TypeScript：quickts E1 移植 + `erasableSyntaxOnly` + 实施分片。

## 已裁决（父级 2026-11-27）

- **v1 API 保留为 deprecated 兼容层**，v2 是新增面；scenario/lib 迁移另开子单，本轮只迁 examples。
- **S0 语义**：`patch` 恢复 = CAS（当前值仍是我写的值才写回，否则计数 `restore_conflicts` 不覆盖）；hook 归 **Run 序号**（owner），所有收口路径 `instrHookReleaseOwner`；render 期 7 个 hook trampoline 一律 TypeError；`--no-s7` 把注入站点数写进 `lift_pin.h`，0 ⇒ `at()` 返回 `undefined` + 一行 error。
- 不做：逻辑 timer（固定时间触发不知道 guest 位置）、嵌套 Run、enum/参数属性/JSX/装饰器（v1 由 `erasableSyntaxOnly` 排除）。

## 怎么查

- 核实证据：`.tmp/plugin-v2/v1-current-state.md` / `v2-typescript.md` / `v3-api-design.md` / `v4-gates.md` / `plan.md`
- 合同：`docs/03-emulator/plugin-runtime.md`
- 引擎：`apps/emulator-lift/third_party/quickjs-ng`；宿主：`apps/emulator-lift/src/ui/script_quickjs.cpp`
- 进度：`pnpm tools requirements plan`

## 进展（2026-11-28）

- **S0 已实现并合入 main**（dev `177e93d`）：4 条静默缺陷修完（hook 归 Run owner、全部收口路径释放；patch 恢复 = CAS + `restore_conflicts`；render 期 7 个 `ctx.hook.*` 拒绝；`--no-s7` 注入站点进 `lift_pin.h` ⇒ `no_injection`）。复审 accept-with-findings（无 S0/S1 级）。
- **S1 已实现并合入 main**（dev `d4668eb` + `d819864`）：quickts E1 擦除进 quickjs-ng（`.js` flags off/on 字节码 16/16 恒等；examples 9/9 零 module request）；`.ts`/`.mts` 插件端到端；`tsc --erasableSyntaxOnly` 进 examples 门禁。复审 accept-with-findings。
- **S2a 已实现并合入 main**（dev `d72afb2`）：指令点 sink（runtime→ui，无 JS 类型）+ `handle.onTick(fn)` 只读 tick + 每入口 drain（同一 CPU 预算窗）+ Run 收口释放 JS handler。复审 accept-with-findings。
- **进行中**：S2b（`once`/`task`/timeout + Run 存活语义）→ S2c（`ctx.at` 链 + `Tick` 分层）→ S3 → S4。
- 证据：`.tmp/plugin-v2/{plan.md,s0/,s1/,s2a/}`（含复审报告与变异读数）。

- **S2b 已实现**（dev `24d312b` + `bc45164`）：`ctx.once` / `ctx.task` / timeout 泵 / Run 存活（tasks ∪ onces ∪ onTick 订阅）/ `start` 可单独存在；复审的 S2 四条已修。
- **S2c-1 已实现**（dev `a5a99d5` + `5846d23`）：`ctx.at` 链（注册序 / `next()` 截断 / `off`）+ `Tick` 只读（`now/local/log`，DEC + 越界 undefined）+ stale Tick + `{opcode}` matcher；复审 S2（patch `break` 饿死同点订阅）已修。
- **S2c-2 已实现**（dev `4f9278c` + `6807ada`）：`ctx.at` 五种 `where` + `Tick.edit/input/patch` + 多槽 `Pending`；复审 1 条 S2 已修。
