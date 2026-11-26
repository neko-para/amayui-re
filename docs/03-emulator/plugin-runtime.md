# 业务外策略入口的能力整合：v1（合同）· plan-as-callbacks + 操作类

> 2026-11-19 / macOS arm64 · **合同 v1**（= 设计 v5.3.2 + Timeout 合同）。
> 本文是**已入库的合同**：API 声明 = `apps/emulator-lift/scripts/plugins/plugin.d.ts`；
> 示例 = `apps/emulator-lift/scripts/plugins/examples/`（9 个 `.ts`，strict `tsc`）；
> 需求单 = `data/requirements/01M7T0VJPG2S0RZ1HNWW0HH3HY`。
> 正文裸文件名在 `apps/emulator-lift/src/{runtime,ui,aot}/`；`tests/...` 在 `apps/emulator-lift/tests/`；
> 完整路径见 §8.1；文档名带 `docs/03-emulator/`。
> 设计与 API 的可用性由 **5 轮“只看 d.ts 的翻译测试”**验证（见 `examples/`）；原始记录在
> `.tmp/control-unify/usability*.md`（未入库）。
>
> ★ **v4 架构修正（用户 2026-11-19）**：
> “qjs 提供的是一种 callback，而不是提供一个 run loop，因此要么就是用 js 预生成一个实际的 plan
> （你之前提到的），要么就是让 js 自身的 callback 变成一个状态机。我理解如果在 js 侧做一些适当的封装
> （比如 callback 链条，每一个函数返回下一个响应触发的），可以让 plan 不需要特殊存在于 emulator 里面。
> plan 模式实际上会需要 emulator 里面自己有一个状态机（来推进 plan），而且可能需要一些专门的手段
> （语法？）来调试/打日志。”
>
> ⇒ **v4 把 plan-as-callbacks 作为核心模型**，不再在 v3 的 B2/Q7 上打补丁。
> 核心只留：`TriggerRunner` · `IntentQueue` + 执行器 · `EngineRead` · `RunnerControl`/`RunControl` · `Context` · `Owner/Receipt/Budget`
> · 能力表。**核心没有** step 列表 / 门推进 / `pending` 完成度这套“plan 状态机”。
>
> ★ **v5 修订记录**：
> - v3 的 **B2（JS 预生成 Step 流）** 降为**可选/兼容**，不再是核心；`Step`/`planDumpText`/
>   `globalThis.main` 从核心删除。
> - v3 的 **Q7/S5（读点触发 + QJS handler）** 升级为 **C1/C2** 的主线，但触发层只保留
>   `TriggerRunner`，不保留 plan VM。
> - v3 的 **S1（静态 global_set）/S2（QJS 预生成 plan）** 不再是前置；v5 进一步把 `global.set` 归为
>   **Edit（EditSink 立即）**，Input 走 `IntentQueue`；文本 recipe 冻结、`InputPlan` 保留、C3 只 parity。
> - **v5.2 核心操作类**：Edit（立即）/ Input（串行）/ EngineRead（引擎读，无暂停）/ Trigger；
>   `sample`/`screenshot` 是 `emu-ctl` 组合命令（不是核心类）；`Run` 是执行实例（不是操作类）。
> - v3 仍然适用的裁决：`after position` 语义（无效位置不改变状态）、Input 一读点一条、owner/预算/回执。

---

## §0 结论摘要

### 0.1 总判

**推荐：plan-as-callbacks（C 模型）+ 单插件运行时 + 核心操作类（Edit / Input / EngineRead / Trigger）+ Run 生命周期**：

- **核心操作类只有 4 个（不是“功能/驱动”二分）**：
  1. **Edit（编辑/状态写）**：触发点/安全点**立即同步**（`ctx.edit.global.set`）；⛔ 不进 Input 队列；
     暂停/控制点也可执行。
  2. **Input（输入注入）**：**必须串行**；进 `IntentQueue`，guest 读点“一读点最多一条”；owner/互斥；
     与 Edit 同点发生时 Edit 立即、Input 下一读点/按预算。
  3. **EngineRead（引擎数据读）**：`global.get`/`frames`/`scriptPos`/纯 `frame/hash`；
     **同步、无暂停、便宜**；`ctx.read.*`；与 Edit 同点组合 `read → edit`。
  4. **Trigger（触发）**：`script.pos`/读点/帧（帧只 UI）；注册/Once/Reenter/无效位置/预算。
- **`Run` 不是操作类**：是**一次 callback 链的执行实例**（见 §4.4b）；`render` 不是 Run；
  一个 endpoint 同时一个 active Run（Input owner）；`ctx.dispatch`/`emu-ctl run` 创建 Run。
- **`sample`/`screenshot` 不是核心操作类**：它们是 **`emu-ctl` 的组合命令**：
  - `sample` = `RunnerControl.pause`（若未停）+ `EngineRead.{scriptPos,frame,hash}` + 控制面元数据
    （`paused_total`/`state_seq`）+ 序列化；
  - `screenshot` = `RunnerControl.pause` + 宿主/呈现侧抓最后一帧 + 写 BMP（`HostAction`）；
  - ⛔ 都不暴露给插件回调；`state_seq`/BMP 是控制面语义。
- **UI 与 control**：**只有 `render` 画**；`onTrigger` 只改 `ctx.ui` 宿主本地状态，下一次 `render` 才可见。
  触发点可以来自 `script.pos`/读点（进屏 show/离屏 hide），这是期望能力，不是漏洞。
- **一个 JS 插件运行时/上下文**：同一个 `ctx` 提供 **可选** `render`（每帧）、**可选** `onTrigger`
  （Run 的 callback 链）、`ctx.edit.*`、`ctx.input.*`、`ctx.read.*`、`ctx.trigger.*`、`ctx.dispatch`/`ctx.run.*`。
  插件可同时 UI + Edit + Input + Run；**没有 `--ui-control` 第二上下文/安全拆分**。
- **R7 改写**：删除“overlay 不写 guest / 只读承诺”；插件受信，可以读也可以影响 guest；
  QJS 沙箱只为**确定性/可移植**，不是安全隔离；WS 的 loopback+token 只是**传输访问控制**。
- **保留的只有重入/生命周期约束**（不是安全）：回调里 `pause/quit` 拒绝/入队；一个 active Run（Input owner）；
  `releaseOwner`；预算/超时。
- **核心（emulator 内）只做**：
  1. **`TriggerRunner`**：具名 armed 集合；读点判定；一读点最多触发一个；handler 返回新集合。
  2. **`ControlBus`** = **`EditSink`（立即写）** + **`EngineRead`（引擎读）** + **`IntentQueue` + 执行器（Input）**
     + **`RunnerControl`（pause/resume/quit/预算）** + **`RunControl`（dispatch/status/abort）**；
     可选 **`HostAction`**（screenshot/profile/clipboard 等宿主副作用）。
  3. **`Context`**：把 Edit/Input/EngineRead/Trigger/Run 暴露给插件；**没有 sample/screenshot**。
- **`emu-ctl` 是组合/传输适配器**：逐 op 组合核心操作（§4.4a），保留协议/时序胶水；
  “一处实现”指**能力逻辑**，不是适配器零逻辑。
- **文本 recipe 冻结，`InputPlan` 保留为历史执行器**：⛔ 不再给 plan 加任何新能力；
  `StepKind`/parser 冻结；`InputPlan` 保留 summary/failureReport/fault/trace/exit5，**不退役**，
  放在 `src/runtime/input_plan.*` + `src/runtime/recipe.*` 作**冻结兼容执行器**，不是核心；
  C3 只做 parity/验证（**已落地 · main `eecbfac`**），**不替换生产执行器**；
  parity 是“语义等价 + 已知差异表”，不承诺逐位；旧执行器仍是历史行为真源。
- **B2/B1 都不做核心**：B2 只在 C3 parity 中作为历史 `Step[]` 数据；B1 不是模型；
  QJS 是 callback，由 TriggerRunner 驱动。
- **`RunControl`**：一个 endpoint 同时一个 active `Run`（Input owner）；已有 active ⇒ `dispatch`/
  `emu-ctl run` 返回 **`busy`**（不排队）；插件与 `emu-ctl` 都通过它创建 Run；
  ⛔ 不让插件 eval 任意源码；Run 不是安全边界。

### 0.2 为什么不是 v3 的 B2/Q7 继续打补丁

v3 的核心仍然是 `InputPlan` 的 step 列表 + 门推进 + `pending` 完成度；Q7 只是在这个 plan VM 旁边加
“读点触发”。这有两个成本（用户点名）：

1. **emulator 里必须有一个 plan 状态机**（`next_/applied_/pending`、门的推进、完成度、exit 5），
   即使 plan 的作者是 JS；
2. 为了调试/打日志，必须给 plan 发明**专用语法/调试图元**（`--plan-dump`、`stepText`、
   `planDumpText`、`trigger:` 之外的 plan trace）。

callback 模型把这两件事都移到 JS：
- 状态 = JS 闭包/链；emulator 只存“具名 armed 集合”；
- 调试 = `ctx.log` + runner 的 `trigger:` / `intent:` trace；文本 recipe 保留它自己的 trace。

### 0.3 最终推荐（一句话）

> **核心 = TriggerRunner + ControlBus（EditSink + EngineRead + IntentQueue/Executor + RunnerControl + RunControl；可选 HostAction）
> + Context + Owner/Receipt/Budget + 能力表；一个 JS 插件运行时（可选 render/onTrigger + edit/input/read）；
> 文本 recipe 冻结，`InputPlan` 保留为历史执行器，C3 只做 parity；官方 plan/repro 锚不迁移。**

### 0.4 切片与裁决

- **切片 C0–C5**（§5）：C0 能力表/接口/9 项同源 · C1 TriggerRunner + IntentQueue + executor + **EngineRead** + **C++ handler**
  （不接 `input.h`、不迁 control click）· C2 单插件运行时（`render`+`onTrigger`+`edit/input/read`）·
  C3 parity only（JS driver 离线对比 `InputPlan`）· C4 `emu-ctl` 组合核心操作 + HostAction·
  C5 UI→`ctx.dispatch` Run e2e（read → edit → input → dispatch）。
- **最小闭环**：**C0 → C1 → C2**；C3 parity、C4 bus、C5 task 可与 C2 并行/后置。
- **未决 8 条 + 6 条补充裁决**（§7）：D1 单插件入口/flag/headless；D2 `onTrigger`；D3 时钟；
  D4 历史 recipe/parity；D5 错误/预算；D6 `emu-ctl`；D7 dispatch/run；D8 Run 事件/owner；
  R1–R6 = 入口互斥、`ui-control`+overlay、输出面、`Step[]` 版本、parity 运行方式、`global.set` 时机。

---

## §1 现状与能力矩阵（只保留与 v5 相关的）

### 1.1 三个入口的真实代码面

- **`--input-plan`（当前 = 一个 plan 状态机）**：
  - 入口/帮助 `aot_main.cpp:91-309`；载入 `730-762`。
  - 语法/门/完成度：`recipe.h:41-55`（注释写“九条”，实际 `StepKind` 10 项，见 §1.4）、
    `70-81`；解析器 `recipe.cpp:318-503`。
  - 推进/写预算/闸门：`input_plan.cpp:186-217`（`atPoint` 是唯一施加口）、
    `input_plan.h:233-247`、`249-283`；完成度/`exit 5`：`recipe.h:169-180`、`input_plan.cpp:781-810`。
  - **它就是一个 plan VM**：`next_/applied_/pending`、门 `stepGateHolds`、`failed_`、`RecipeOutcome`。
    v4 把它降为**历史 plan 的 C3 JS driver + parity**，不留在核心。
- **`emu-ctl`（控制层）**：
  - 9 个 op：`pause/resume/sample/screenshot/move/click/global-get/global-set/quit`
    （`control_ws.cpp:488-491`；`control.h:55-66`；`parseCommand` `877-886`）。
  - `click/move` 只在暂停态入队（`control_ws.cpp:1409-1413/1437-1441`），由下一个读点经
    `InputPlan` 的立即队列施加（`input_plan.h:212-230`）。
  - `global-get/set` 在块边界执行（`control_ws.cpp:1470-1507`），不要求暂停（`1475`）。
  - `wait` 是 **`emu-ctl` 客户端糖**（= `resume --for-ms`，`tools/lib/emu-ctl.mjs:799`），
    **不是 `ControlOp`**；`hello`/`caps=` 只有 9 项。
  - `caps=` 启动 readout 现状只有 6 项（`aot_main.cpp:1560`，漏 `move/global-get/global-set`）。
- **QJS binding（overlay）**：
  - `--overlay`（预设）/ `--plugin hud`（`aot_main.cpp:221-233`）；`drawOverlay` 在 Present 跑
    （`present_sdl.cpp:521-609`）；只读 `ui/game/host`（`script_quickjs.cpp:892-981`）。
  - 入口是 `globalThis.render`（`script_quickjs.cpp:1416-1429`）；module loader 根 = 入口目录
    （`1141-1175`）。
  - **只在 Present、只读、没有读点触发/输入注入**；失败 = 卸载插件（`present_sdl.cpp:1251-1255`）。
  - ★ **S2 起 `hud.js` 走插件入口**（`export default { render(RenderCtx) }`）：`--overlay` = 保留 id
    `hud` 的预设 + 窗口 flag，窗口档由 adopt 接进同一个叠层 host；旧 `globalThis.render`
    只剩**内部 hud 回退**（`--overlay` + 无 render 的 `--plugin`；`--ui` 已删，§4.6）。行号表在本轮**未逐条重核**（文件改动会让本节的 `NNN` 漂）。

### 1.2 当前能力矩阵（压缩版）

符号：`✅` 有 · `◐` 形状不同 · `⛔` 无。

| 能力 | `--input-plan`（plan VM） | `emu-ctl` | QJS overlay |
|---|---|---|---|
| move / drag | ✅ `mouse_move/down/up` | ◐ 只有 `move`，无 down/up | ⛔ |
| click | ✅ | ✅ 暂停态入队 | ⛔（只读 `game.click`） |
| key / wheel | ⛔ | ⛔ | ⛔ |
| global.get | ⛔（`await global` 另单） | ✅ | ⛔（M3 计划） |
| global.set | ⛔（冻结，不再加） | ✅ | ⛔ |
| sample | ◐ 只有 trace 字段 | ✅ | ◐ 每帧读数 |
| script-pos | ✅ 门 | ✅ `sample.script.pos` | ✅ `local_frames()[top].pos` |
| frames | ⛔ | ⛔ | ✅ `game.local_frames()` |
| screenshot | ⛔ | ✅ | ⛔ |
| pause/resume | ⛔ | ✅ | ⛔ |
| wait | ✅ 宿主墙钟 deadline | ✅ `resume --for-ms`（guest 时钟） | ⛔ |
| await script/position | ✅ | ⛔ | ⛔ |
| after position 回调 | ⛔（只有停住等） | ⛔ | ⛔ |
| read-point 回调 | ⛔ | ⛔ | ⛔ |
| profile | ✅ 宿主窗口 | ⛔ | ✅ `host.profileBegin/End` |
| quit | ✅ | ✅ | ⛔ |
| 参数化/复用 | ⛔ | ◐ Node 客户端 | ◐ 有语言，无 plan API |

### 1.3 具体缺口（v5 视角）

| # | 缺口 | 证据 | 后果 |
|---|---|---|---|
| **G1** | recipe 没有 global 读写 | `recipe.h:70-81`；`input-plan.md:233-239`；先例 `.tmp/deleteobject-profile/report.md:104-123` | 只能 `wait` + 旁路 `emu-ctl` 抢时机；实测竞态（`report.md:113-118`） |
| **G2** | 控制层没有门/拖拽 | `control_ws.cpp:877-886`；`control-plane.md:133-136` | “到屏再动手”只能外部轮询；拖拽无法表达 |
| **G3** | QJS 不能注入、没有位置/读点触发 | `script_quickjs.cpp:892-981`；`present_sdl.cpp:521-609` | HUD 只能看；Present 不是 `SCRIPT.POS` 触发 |
| **G4** | plan 需要 emulator 内的状态机 + 调试图元 | `input_plan.cpp:186-217`、`781-810`；`recipe.h:169-180` | 每个 plan 入口都要复刻 step/gate/pending/exit5；用户点名的成本 |
| **G5** | recipe 无状态载体（读改写） | `recipe.h:28-30`；`Step` 无变量 | `global-get` trace-only 不解决问题；`global-set-bitor` 是怪原语 |
| **G6** | 无 owner/回执/预算的统一模型 | `input_plan.h:430-458`；`control_ws.cpp:548-559` | 脚本/控制层/plan 的 hold 释放、失败、入队回执各自为政 |
| **G7** | 三处能力清单会漂 | `hello` 9（`control_ws.cpp:490`）；`caps=` 6（`aot_main.cpp:1560`）；`control.h:13-21` 注释漏 `move`；`ui.d.ts` 无守卫 | “只有某一个入口有”再次发生 |
| **G8** | 复用/参数化只在 recipe 外 | 官方 `.plan` 重复加载前缀（`save04-fault.plan:17-26`、`sc0000-wipe.plan:19-28`）；文档教 `cat 前缀 + tail`（`SAVE.md:52`） | 复制/行号引用；无法 `loadSave(slot)` |

### 1.4 注释声称 vs 代码实际

| 声称 | 落点 | 实际 | 判定 |
|---|---|---|---|
| recipe 九条指令 | `recipe.h:41-55` | `StepKind` 实际 **10 项**（`recipe.h:70-81`）；v5 **冻结**，不再加指令 | **注释旧数**；v5 核心不再有 Step，文本 plan 由 C3 JS driver 消费历史 `Step[]` |
| 控制层 9 项 | `control_ws.cpp:488-491`；`control.h:13-21` | `ControlOp` 9 项；`hello` 9；`caps=` 6（漏 move/global-*）；`control.h` 注释漏 move | **真实漂移**；C0 守卫必须抓 `caps=` |
| QJS 不写 guest，且有形态守卫 | `overlay.md:64/317` | 当前代码 `src/ui/**` 无 `input::`/`writeGlobal`；但 v5 **改写 R7**：插件受信、可影响 guest，不再有“只读”承诺；QJS 沙箱只为确定性 | **旧文档口径需改**；不再按 R7 守“overlay 只读” |
| `wait` 让 guest 跑 N ms | `control-plane.md:130-131` | control `wait` = `resume --for-ms`（guest 时钟）；recipe `wait` = 宿主 deadline | **同名不同语义，文档已注明**；C3 保留 recipe 语义 |
| `host.profileBegin/End` 与 recipe `profile_*` 同源 | `aot_host.cpp:1352-1416` | 都走 `beginWindow/endWindowText` | **一致** |
| QJS `frame` = control `sample.frame` | `present_sdl.cpp:533` vs `aot_host.cpp:1203` | 两个 Present 计数器，未逐位断言 | **待核**；v4 不合并这两个读数 |

---

## §2 根因：为什么 plan 不该住在 emulator 里

### 2.1 时间模型：三种“现在”

| 入口 | “现在” | 代码 | 后果 |
|---|---|---|---|
| recipe | guest **读点** | `input.h:80-114` → `InputPlan::atPoint` | 没有读点就不推进；暂停 ⇒ 冻结 |
| control | 宿主汇点 + 暂停状态机 | `aot_run.cpp:595-609`；`aot_host.cpp:1184-1198`；`control_ws.cpp:1255-1262` | 命令在下一个汇点生效；pause 冻结一切 |
| QJS overlay | 每次 Present | `present_sdl.cpp:521-609` | 只在有帧时跑；Present 不是 `SCRIPT.POS` |
| （v5）trigger | **读点上的条件**（位置/延迟/读点） | 新 `TriggerRunner` 在 `atPoint` 判定 | 与 recipe 同一精度合同；**不是** Present |

### 2.2 操作类模型（不是“功能/驱动”二分）

| 操作类 | 何时/哪里 | 路径 | 暂停态 | 预算/约束 |
|---|---|---|---|---|
| **Edit**（`ctx.edit.*`，如 global.set） | 触发点/安全点**立即** | `EditSink` 同步写；⛔ 不进 Input 队列 | ✅ 可执行（控制点/暂停也可） | 不能延迟；宿主一致性在安全点保证 |
| **Input**（`ctx.input.*`） | guest 读点 | `IntentQueue` → executor；**一读点最多一条** | ⛔ 暂停无读点；恢复后按读点 | 串行/owner/预算；click 两段/闸门 |
| **EngineRead**（`ctx.read.*`） | 安全点/触发点 | `EngineRead` 同步读（global.get/frames/scriptPos/纯 frame/hash） | ✅ 无暂停 | 便宜；可与 Edit 同点组合 |
| **Trigger**（`ctx.trigger.*`） | 读点/帧（帧只 UI） | `TriggerRunner` | 暂停冻结 | Once/Reenter/无效位置/预算 |
| **Run**（`ctx.dispatch`；不是操作类） | 生命周期 | `RunControl`（一个 active） | 可取消/abort | 不是权限；owner/busy/releaseOwner |

- **Edit 先于 Input**：同一触发点，Edit 同步立即；Input 进 `IntentQueue`（下一读点/按预算），
  ⛔ 不让 Input 抢“立即”语义。
- **R7 改写**：插件受信（开发者/用户分发），可以读也可以影响 guest；QJS 沙箱只为**确定性/可移植**，
  不是安全隔离；WS 的 loopback+token 只是**传输访问控制**。保留的只有重入/生命周期约束
  （pause/quit 拒绝、一个 active Input owner/Run、releaseOwner、预算/超时）。
- **EngineRead ≠ sample/screenshot**：EngineRead 是引擎数据读（`ctx.read.*`，不暂停，可与 Edit 读→改→写）；
  `sample`/`screenshot` 是 `emu-ctl` 的组合/控制面命令（暂停+`state_seq`），不是核心操作类。
- 没有“功能 JS 只读 / 驱动 JS 才写”的第二套权限；操作类由**代码调用哪个 API**决定，不由上下文决定。

### 2.3 失败 / 所有权 / 回执

- 旧 recipe：失败 = 整条任务失败 `exit 5`；control：一条命令 `ok:false`；QJS overlay：卸载插件。
- 旧 `InputPlan` 的 `pending_/armed_/immediate_` 没有 owner；`releaseAll` 会把所有 hold 抬起
  （`input_plan.h:290-293`、`430-458`）。
- v5 核心必须有：**owner**（trigger/intent 属于谁）、**receipt**（accepted/queued/applied/code）、
  **budget**（callback deadline/op）、**releaseOwner**（脚本错误/卸载只释放自己的 hold）。

### 2.4 plan 状态机的成本（用户原话的核心）

- `InputPlan` = step 列表 + 门推进 + `next_/applied_/pending` + 完成度 + `exit 5` + 失败报告
  （`input_plan.cpp:186-217`、`781-810`）。
- 每加一个入口（control/QJS）都要么复用这套 VM，要么复刻；每加一个调试手段都要给 plan 发明
  `--plan-dump`/`planDumpText`/`stepText` 这类**DSL 专用**读物（v3 自己就长出了 `planDumpText`）。
- ⇒ **把状态机放 JS**：emulator 只保留“armed 集合 + intent + read”；plan 的“下一步”由 JS 闭包决定。

### 2.5 复用/参数化

recipe 是纯线性文本，没有函数/模块/参数/循环（`recipe.h:28-30`；`recipe.cpp:402-503`）。
官方配方/文档已经用“复制前缀 + 行号区间”复用（`save04-fault.plan:17-26`、`sc0000-wipe.plan:19-28`、
`SAVE.md:52`、`FELLOW.md:50`、`SN0000.md:47`）。callback 链把这件事放回 JS：`loadSave(slot)` 返回
一条链，参数/循环/错误处理都是语言现成的。

### 2.6 结论

**不是“语法统一”问题，是“状态机住在谁那里”的问题。**
- plan-as-data：状态机在 emulator ⇒ 需要 plan VM + 调试语法；JS 只能预生成。
- plan-as-callbacks：状态机在 JS ⇒ emulator 只做 trigger 分发 + intent 执行 + read；
  plan 不需要特殊存在。

---

## §3 方案对比

### 3.1 A：能力核 + 薄前端（v3 方向）

- 能解决：能力对齐、权限矩阵、owner/回执、控制层/QJS 的语义共享。
- 不能解决：**plan 状态机仍在 emulator**（如果保留 `InputPlan` 作为核心）；调试/完成度仍是 plan 专用；
  JS 仍只能“预生成”或被 Q7 旁路。
- 结论：A 的“能力核”是对的，但**核心不能包含 `InputPlan` 的 step VM**；A 必须叠加 callback 模型。

### 3.2 B1：QJS 作为 run loop

- 做法：整个 plan 在 QJS 里跑，emulator 提供 `input.*` / `game.*`。
- 问题：QJS 没有跨帧 async/job 泵（`overlay.md:67` D10；`overlay-quickjs.md:151-153`）；
  要让 `await` 在 guest 读点推进，需要新调度器/暂停 tick/错误隔离；热路径 JS。
- 结论：**不推荐**；v4 的 callback 不是 run loop，而是由 TriggerRunner 驱动的单次回调。

### 3.3 B2：JS 预生成 Step 流（v3 的主交付）

- 做法：JS 在 boot 前把 plan 编译成 `Step` 列表；emulator 的 `InputPlan` 执行。
- 优点：线性/确定性/可 diff；文本与 JS 共用 Step 模型；参数化在编写期。
- 缺点（用户点名）：emulator 仍要有 plan VM（`next_/pending/exit5`）；调试要 DSL 图元；
  运行时读改写（global.get→set）仍要 Q7；JS 只能“预生成”，不能按运行期状态决定。
- 结论：**冻结**；不再作为核心/可选前端，也不新增 plan 能力。历史 `.plan` 只作为 C3 的 `Step[]` 数据，
  由 JS recipe driver 翻译成 callback 链，并用 parity 验证。

### 3.4 C：plan-as-callbacks（推荐）

- 做法：`start(ctx)` / `onTrigger(ctx, ev)` 返回 `Trigger | Trigger[] | null`（数组替换 armed 集合）；
  `loadSave(slot)` 按 name 分派；
  emulator 只做 TriggerRunner + Edit/Input/EngineRead/ControlBus；`ctx` 通过 `input`/`edit` 写，
  通过 `read` 只读；它们是**同一个插件运行时**的能力，不是两套权限。
- 优点：
  - plan 状态机在 JS（闭包/链），emulator 没有 plan VM；
  - 运行时读改写/条件/循环/参数化都是 JS 现成能力；
  - 调试 = `ctx.log` + runner trace，不需要给 plan 发明 DSL 图元；
  - 文本 recipe 只由 C3 的 JS driver + parity 验证/支持，不再演进。
- 缺点/成本：
  - 确定性/可复现依赖 JS 代码 + trace；没有“Step 列表 diff”那么直观；
  - 回调在读点上跑 JS（热路径），需要预算/上限/错误隔离；
  - 需要新的 runner/owner/回执/超时语义；
  - 文本 recipe 的 `exit 5`/`wait` 语义由 C3 driver 复刻，不能污染核心。

### 3.5 D：C3 = JS recipe driver + parity（历史 plan 的验证/支持）

- 做法：
  - C++ parser 只产**数据**：`Recipe/Step`（含 `timeout`/`origin`）→ `Step[]`（JSON/ABI），**不执行**；
    这个**唯一** JSON 出口是独立 TU `src/runtime/recipe_json.{h,cpp}`（只读 `Step`）；
  - **JS driver**（`scripts/plugins/lib/recipe.js`，跑在单插件运行时）：`runRecipe(steps)` 用
    `ctx.trigger.*`/`ctx.input.*`/`ctx.read.*` 返回 callback 链；覆盖九条指令 + 行尾 `timeout` + `quit`
    + `profile_*` + `finish(0)/finish(5)`（**只做翻译**，门/超时/完成度判据仍只有一份）；
  - **parity harness**：官方 `scripts/recipes/*.plan` + `repro:` 锚的**指令行子串**（由
    `tools/test/recipe-freeze.test.mjs` 逐字钉住，⛔ 不做配方 replay），旧 `--input-plan`（oracle =
    `InputPlan::atPoint`）vs 新 JS driver，逐项比对消息/电平/`fired/applied/pending`/exit/trace；
    **语义等价 + 已知差异表**，不承诺逐位；含 `wait` 标 `NON-DIFFABLE`；
  - 任何一条旧 plan 表达不出来 = **callback API 缺口/已知差异**，列成覆盖/缺口表；
  - **`InputPlan` 保留**（不退役）：summary/failureReport/fault/trace/exit5 不动，仍是生产真源；
    `src/runtime/recipe.*` / `src/runtime/input_plan.*` 保留 parser/执行器（一个字符没动）；
    C3 只加离线 JS driver（`scripts/plugins/lib/recipe.js`）/parity；官方 `.plan`/`repro:` 锚不动。
  - **不新增 plan 能力**。
- 优点：历史 plan 得到验证/支持；核心没有 plan VM；旧 plan 不再产出新能力。
- 缺点：JS driver 要覆盖全部历史语义（包括 `exit 5`/trace 的 parity）；parity 需要专门的差分夹具。
- 结论：**C3 采用 JS driver + parity**；文本 recipe 不再演进。

### 3.6 对比表

| | plan-as-data（B2） | plan-as-callbacks（C） |
|---|---|---|
| 状态机在哪 | emulator（`InputPlan`） | JS（闭包/链） |
| 核心需要 | Step/gate/pending/exit5 + 调试图元 | TriggerRunner + Edit/Input/EngineRead/Run |
| 运行期读改写 | 还要 Q7/额外状态 | 回调里 `ctx.read.global.get`/`ctx.edit.global.set` 天然 |
| 复用/参数 | 编写期（预生成） | 闭包/模块/参数 |
| 确定性 | 强（Step diff） | 依赖 JS + trace；`afterDelay` 用宿主墙钟（NON-DIFFABLE） |
| 调试 | `--plan-dump`/`stepText` 等 DSL 图元 | `ctx.log` + `trigger:`/`edit:`/`intent:`/`run:` |
| 文本 recipe | 同一 Step 模型 | C3 的 JS driver 消费历史 `Step[]`（冻结） |
| 结论 | 冻结/历史 | **核心推荐** |

---

## §4 推荐架构

### 4.1 分层

```text
前端（作者面）          单插件运行时（同一个 ctx；可选 render / onTrigger）                  C3 JS recipe driver
                        │ ctx.edit(立即) / ctx.input(串行) / ctx.read(只读/无暂停) /          │ 历史 Step[]（parity only）
                        │ ctx.trigger / ctx.ui / ctx.dispatch(Run) / ctx.log / ctx.profile   │
                        ▼                                                                   ▼
核心（emulator） TriggerRunner ──► Context ──► ControlBus
                  │                           ├── EditSink（立即写；暂停/控制点可用）
                  │                           ├── IntentQueue/Executor（Input；一读点一条）
                  │                           ├── EngineRead（global.get/frames/scriptPos；无暂停）
                  │                           ├── RunnerControl（pause/resume/quit/预算）
                  │                           ├── RunControl（dispatch/status/abort；一个 active）
                  │                           └──（可选）HostAction（screenshot/profile/clipboard 宿主副作用）
                  │  RunControl ──► TriggerRunner（Run 的 start/onTrigger）
                  └── Owner/Receipt/Budget
                  ▼
guest 状态         InputDevice / WindowManager / GuestMemory / engine_marker / script_globals
```

**硬规则**：

1. 核心**没有** `Step`/`pending`/`next_`/`applied_`/`RecipeOutcome`；这些只属于
   `src/runtime/input_plan.*` / `recipe.*` 的**冻结历史执行器/parser**（`InputPlan`），
   不 include/引用到核心（`src/runtime/core/**` + `input.h` 投递口）。
2. **Input 唯一路径** = `IntentQueue` + executor；⛔ 没有第二个 Input 施加点。
   **Edit 是另一操作类**：`EditSink` 在安全点立即同步写（global.set 等），⛔ 不进 Input 队列；
   Edit 不是“第二个 Input 施加点”。
3. `TriggerRunner` 只在 guest 读点判定（`atPoint`）；⛔ 不在块边界 / Present / 独立线程判 control trigger。
4. `EngineRead` 只在主线程、安全点执行；`sample`/`screenshot` 是 `emu-ctl` 组合命令（不属于核心类）；
   ⛔ 控制线程不读 guest。
5. 所有 frontend 共用同一张能力表 + 同一套 `Edit`/`Input`/`EngineRead`/`Trigger`/`Run` 类型。
6. **操作类不是权限**：同一个插件可以 `render` + `onTrigger`，既 Edit 又 Input；一个 endpoint 一个 active Run；
   `RunControl` 管 owner/abort；⛔ 没有“功能 JS 只读 / 驱动 JS 才写”的第二套权限。

### 4.2 TriggerRunner（具名 armed 集合；一读点最多一个）

```c++
enum class TriggerKind : uint8_t { AfterScript, AfterPosition, AfterDelay, AtReadPoint, Frame };
enum class TriggerMode : uint8_t { Once, Reenter };

struct Trigger {
    std::string name;          // ★ 必填；状态名；同一 armed 集合内唯一
    TriggerKind kind;
    std::string script;        // AfterScript/AfterPosition
    std::uint32_t lo, hi;      // AfterPosition：半开 [lo,hi)；AfterScript 忽略
    std::uint64_t delay_ms;    // AfterDelay：宿主墙钟（AOT setRealTime；NON-DIFFABLE）
    TriggerMode mode;          // Once / Reenter
    InputPoint point_filter;   // AtReadPoint：Count = 任意
    std::uint32_t max_fires;   // AtReadPoint 必填 >0；其余可选（0=不设硬上限）
    std::uint64_t timeout_ms;  // AfterScript/AfterPosition/AtReadPoint；AfterDelay 忽略/不暴露；0=不设
    std::uint32_t owner;
};

struct TriggerEvent {
    const char* kind;          // "trigger" | "timeout" | "start"
    std::string name;          // ★ 被触发的 trigger name
    TriggerKind trigger_kind;
    std::string script;
    std::uint32_t pos;
    bool pos_ok;
    InputPoint point;
    std::uint64_t read_index;
};

class TriggerHandler {
public:
    using TriggerSet = std::vector<Trigger>;  // empty = done
    // 同步、有预算；可 Edit/EngineRead；input 只入队；返回**新的 armed 集合**（替换旧集合）。
    virtual TriggerSet start(Context& ctx, std::string* err) = 0;
    virtual TriggerSet onTrigger(Context& ctx, const TriggerEvent& ev, std::string* err) = 0;
    virtual ~TriggerHandler() = default;
};
```

**集合语义（v5.3.1；写死）**：

- **返回数组 = 完整的新 armed 集合（替换）**：旧集合里没出现在返回数组里的 trigger **全部消失**；
  **包括本读点刚触发的那个**。想保留其它 trigger，就把它们**再放进返回数组**（回调可从
  `ctx.state`/闭包持有对象）。
- **`null` / `[]` = done**（⛔ 空集合不是“永远等”）；重复 `name`（同一返回数组内）⇒ **fail the Run** 并点名。
- **一个 guest 读点最多触发一个 Trigger**：按**当前 armed 集合的注册顺序**扫描；第一个满足的
  从集合移除；**不级联**；其余 trigger **不自动保留**（除非回调返回它们）。
- **触发的那个**：不自动放回；回调把它放回 = 重新 arm；换新对象 = 新预算。
- **`maxFires` 属于 Trigger 对象、跨 arm 累计**：同一对象重新 arm **继续计数**；耗尽后再返回该对象
  ⇒ **fail**（不是静默重置/静默丢掉）。要新预算就**新建一个 Trigger 对象**；旧对象已被消费；
  `name` 可以复用，但同一 armed 集合内必须唯一。
- **`reenter` 边沿状态属于对象**：重新 arm = 边沿从头（需要新的有效外→内）；`maxFires` **不重置**。
- **四种组合**：
  1. 返回 `[]` / `null` ⇒ done；清 armed。
  2. 返回新集合（含/不含刚触发对象）⇒ 完整替换；含 = 重新 arm（maxFires 续计），不含 = 消费。
  3. 返回已耗尽 maxFires 的同一对象 ⇒ fail（点名）。
  4. 返回**新对象**（同名可复用）⇒ 新预算、新边沿；旧对象不复活。
**重新 arm 语义表（写死）**：

| Trigger | 触发后 | 同一对象重新 arm | `maxFires` |
|---|---|---|---|
| `Once` | 耗尽 | **fail**（同 maxFires 耗尽） | 续计；耗尽再返回 fail |
| `Reenter` | 消费；`last_inside` 保留 | 边沿从头：需先有效“外”→再“内”；不会立刻再触发 | 续计 |
| `AfterDelay` | 消费 | 计时从新 arm 开始 | 续计 |
| `AtReadPoint` | 消费 | 从下一个匹配读点重新计 | 续计 |

⇒ 要“新预算/新边沿”就**新建 Trigger 对象**；旧对象不要复用。

- **`AfterScript`**：只比当前脚本名 == name（电平门；无区间）；`Once` 首次匹配；`Reenter` false→true。
- **`AfterPosition`**：`[lo,hi)`；`Once` 首个有效 inside；`Reenter` 有效外→内；无效位置不改变状态；
  ⛔ 不是“区间内每读点都触发”。
- **`AfterDelay`**：`ms` 是**延迟本身**；宿主墙钟累计 ≥ ms；暂停不冻结；NON-DIFFABLE；`Once`/`Reenter`。
  ⛔ **不接受/不叠加 `timeoutMs`**（`afterDelay` 没有 timeout；要超时就用别的 trigger 或外部逻辑）。
- **`AtReadPoint`**：下一个匹配读点；`maxFires` 必填 >0；多个同时满足时仍按注册顺序只触发一个。
- **`Frame`**：只给 overlay UI；control 不使用。
- **超时**：`AfterScript/AfterPosition/AtReadPoint` 可配 `timeout_ms`；**arm 时记 deadline（宿主墙钟）**，
  `TriggerRunner` 在**每个 guest 读点的 `atPoint`（采样后、`releaseDue` 前）poll**；
  `elapsed >= timeout_ms` ⇒ **从 armed 集合移除** + `onTrigger(TimeoutEvent{kind:"timeout", name, trigger})`。
  **timeout 不是 fire**：不计 `maxFires`；`Once` 的“耗尽”只在**触发**时发生，timeout 后同一对象**可以**
  重新 arm（deadline 重置）；要“超时即作废”就新建对象。
  `timeout_ms = 0` = **不设超时**（不是立即）。
  **不自动 finish**；插件用 `ctx.finish(5, reason)`（旧 exit5）或返回新集合 re-arm。
  **per-arm**：重新 arm 重置 deadline；`maxFires` 继续累计（timeout 不增计数）。
  **暂停/headless/无读点 ⇒ 不 poll、不会超时**（不是独立 wall-clock timer；恢复后到下一个读点才可能触发）。
  `TimeoutEvent.name/trigger` 必填；`trigger` 是超时的那个对象。
- **done/fail**：`start`/`onTrigger` 返回空集合 = done；`ctx.fail(name?, reason?)` = fail（name 可点名状态）；
  trace `trigger:<name>:done`。
- **owner**：每个 armed trigger 带 owner；Run 结束/卸载 ⇒ `releaseOwner` 清该 owner 的 armed + hold。
- **回调内顺序**：JS 决定 Edit/EngineRead 调用顺序（宿主逐个同步执行）；Input 只入队；返回新集合。
- **budget**：每次 start/onTrigger deadline + op limit；超限 ⇒ 清 armed + fail(owner)。
  ★ 2 ms **只罩用户 JS 本体**（模块装载 / ctx 构造 / TriggerSet 收集 / 宿主调度不计），且量的是
  **本线程 CPU 时间**（`pluginThreadCpuNs()`；被 OS 抢占 / 等 I/O / 睡眠**不计**）；宿主段另有
  10×（20 ms）**墙钟**安全网，reason 带 `(host)`（见 §4.5 D5 与 4.6「预算」）。
  ★ **宿主能力调用暂停**（2026-11-25）：`ctx.profile.end` 是宿主能力（生成报告 + 写系统剪贴板），
  它的 CPU 成本**不计入** 2 ms —— `HostPause` 把宿主调用段的 CPU 累加进 `paused_ns`，`js_cpu_ns`
  与中断判据都饱和扣除；诊断行 `paused_us` 给出扣除量。⛔ 本轮**只接 `profile.end`**（`log` /
  `profile.begin` / `edit` 未接；真出现超预算再按同一原则逐个接），2 ms 数值与 `kCallbackOpLimit`
  不变（真跑飞的 JS 忙循环仍 `over=js`，`plugin/callback-js-band`）。
- **可观测性**：`RunStatus.armedNames`/`armed`、`trigger:<name>` trace、`RunEvent.name`；
  name 是**插件侧状态名**，不是 guest 数据，不参与 Edit/EngineRead。

### 4.3 IntentQueue + Executor（click 两段 / 一读点一条写）

```c++
enum class IntentKind : uint8_t { Move, Click, MouseDown, MouseUp, Key, Wheel,
                                  ProfileBegin, ProfileEnd };  // GlobalSet 不在这里：走 EditSink
enum class IntentClass : uint8_t { GuestWrite, HostEffect };  // Screenshot/Profile 走 HostEffect
struct Intent {
    IntentKind kind;
    IntentClass cls;
    Origin origin;        // QjsControl / RecipeFrontend / Control
    uint32_t owner;
    // x/y/button/delta/global kind/index/value...
};
struct Receipt { bool accepted; const char* code; uint64_t seq; uint64_t queued; uint64_t id; };
```

- **click 两段**：`arm`（映射 + `input::mouseMove`）→ 后续读点 `press`（`input::mouseButton(down)`）
  → 抬起的 KeyLevel/墙钟闸门（`input_plan.h:249-283` 的既有语义，核心 executor 保留）。⛔ 不要复制
  第二套坐标映射/闸门；executor 就是现有 `armClickAt/pressDue/emitRelease` 的核心。
- **一读点一条 Input 写**：`writes_this_point_` 只数 Input（move/click/down/up）；同读点第二条留到下一读点。
  `GlobalSet` **不在**这个预算里（EditSink 立即写，见 §4.4a）；⛔ 不要把 Edit 塞进 Input 队列。
- **trigger 的 Input 一律下一读点**（v3 P0-1c 保留）：`ctx.input.click/move` 进 `staged_next_`；
  本读点只 fire，不写 guest；下一读点 `staged_apply_` → `immediate_`，在
  `releaseDue → pressDue → drainImmediate → flush` 的 drain 里尝试；budget 被占用则继续等。
  **Edit（`ctx.edit.*`）不在这里**：先于 Input、同步立即（§4.4a）。
- **回执**：`accepted/queued` 立即返回；`applied` 由 executor 更新；trace `intent: <kind> owner=… seq=…`。
- **owner/releaseOwner**：intent 带 owner；脚本错误/卸载/chain done ⇒ 只释放该 owner 的 hold
  （`input::releaseProgram` / 既有 `releaseAll` 的 owner 化）。
- **global.set 失败**（EditSink）：`id_out_of_range/type_mismatch/encode_failed` ⇒ receipt + `ctx.fail`/一行 stderr；
  核心**不** `exit 5`；`InputPlan`/C3 parity 才映射历史 `FAIL data`。
- **HostAction**：`screenshot`（控制面组合）与 `profile.*`/`clipboard` 这类宿主副作用；
  screenshot 由 `emu-ctl` 组合，不进插件回调；HostAction 不计 guest 写预算。

### 4.4 EngineRead（引擎数据读；不是 sample/screenshot）

```c++
class EngineRead {                 // 引擎侧数据读：同步、无暂停、便宜
public:
    virtual GlobalValue globalGet(GlobalKind, uint32_t) = 0;
    virtual std::vector<FrameInfo> frames() = 0;
    virtual MarkerSample scriptPos() = 0;   // marker 纯读
    virtual std::uint64_t frameHash() = 0;  // 纯 frame/hash 读
};
```

- **EngineRead（`ctx.read.*`）**：`global.get`/`frames`/`scriptPos`/纯 frame/hash；**同步、无暂停、便宜**；
  在 `onTrigger` 里与 **Edit 同点组合**：`read → edit → input`；`emu-ctl global-get` 是同一个 EngineRead
  的控制传输；**不暂停**（`control_ws` 现状 `leave_paused=!paused`）。
- **`sample`/`screenshot` 不是 EngineRead，也不是核心操作类**：它们是 `emu-ctl` 的**组合命令**：
  - `sample` = `RunnerControl.pause`（若未停）+ `EngineRead.{scriptPos,frame,hash}` + 控制面元数据
    （`paused_total`/`state_seq`）+ 序列化；**暂停/元数据/序列化不是插件回调 API**。
  - `screenshot` = `RunnerControl.pause` + **HostAction**（宿主/呈现侧抓最后呈现帧 + 写 BMP）；
    ⛔ 不进插件回调。
- **插件要 script/pos**：用 `ctx.read.scriptPos`（无暂停），**不要**复用 sample。
- **`global.set` 不是 EngineRead/Input**：是 **Edit**，走 `EditSink` 立即写（§4.4a）；`ctx.edit.global.set` 同步。
- **frames**：复用 `EngineFrameSource`（`engine_frames.cpp:81`）只读视图。

### 4.4a ControlBus 与 `emu-ctl` 组合（5 个组件 + 可选 HostAction；v5.2）

```c++
class EditSink {                    // 立即、同步；不排 Input
    virtual Receipt globalSet(GlobalKind, uint32_t, const GlobalValue&) = 0;
};
class ControlBus {
public:
    EditSink& edit;                 // Edit：立即（暂停/控制点也可）
    EngineRead& read;               // EngineRead：global.get/frames/scriptPos；无暂停
    IntentQueue& input;             // Input：串行、一读点一条
    RunnerControl& runner;          // pause/resume/quit/预算
    RunControl& runs;               // dispatch/status/abort（一个 active Run）
    HostAction* host = nullptr;     // 可选：screenshot/profile/clipboard 等宿主副作用
};
```

- **EditSink**：安全点**同步立即** `global.set`；⛔ 不进 `IntentQueue`；暂停/控制点也可；失败回 receipt。
- **EngineRead**：`global.get`/`frames`/`scriptPos`/纯 frame/hash；同步、无暂停；可与 Edit 同点组合。
- **IntentQueue**：Input；串行、一读点一条；owner/互斥；click 两段/闸门。
- **RunnerControl**：pause/resume/resumeFor/quit/预算；`pause` 冻结 TriggerRunner；墙钟不冻结。
- **RunControl**：dispatch/status/abort；一个 endpoint 一个 active Run；已有 ⇒ `busy`（不排队）。
- **HostAction（可选）**：`screenshot` 抓最后呈现帧/写 BMP、`profile`、`clipboard` 等宿主副作用；
  不是 guest 操作类，不计 Input 预算；screenshot 由 `emu-ctl` 组合，⛔ 不进插件回调。
- **`sample`/`screenshot` 不是核心类**：
  - `sample` = `RunnerControl.pause`（若未停）+ `EngineRead.{scriptPos,frame,hash}` +
    控制面元数据（`paused_total`/`state_seq`）+ 序列化；
  - `screenshot` = `RunnerControl.pause` + `HostAction`（最后呈现帧 BMP）；
  - ⛔ 不暴露给插件回调（回调里 sample 会冻住 runner）。
- **`control_ws.cpp` 不是纯传输**：`sample`/`screenshot` 的暂停/`state_seq`、`resume` 的 `controlOnResume`、
  `click/move` 暂停准入是**控制点语义**；WS 保留协议/时序胶水；“一处实现”指**能力逻辑**，不是零胶水。
  ⛔ 不再直接 `input::*`/`writeGlobal`；只组合 `ControlBus`/`RunControl`/`HostAction`。

**`emu-ctl` 逐 op 组合（WS）**：

| WS op | 组合 | 传输/控制点特有 |
|---|---|---|
| `pause` | `RunnerControl::pause` | 连接/超时/`state_seq` |
| `resume`（`--for-ms/frames`） | `RunnerControl::resume/resumeFor` | `controlOnResume`/谓词超时 |
| `quit` | `RunnerControl::quit` | `bye`/`requested_by` |
| `move` / `click` | `IntentQueue`（Input） | 暂停准入/`queued` |
| `global-get` | `EngineRead::globalGet`（**不暂停**） | `0x` 校验/reason；`leave_paused=!paused` |
| `global-set` | `EditSink::globalSet`（**立即**） | `applied`+reason；暂停也生效 |
| `sample` | **`pause` + `EngineRead` + 元数据 + 序列化** | `paused_total`/`state_seq`/JSON |
| `screenshot` | **`pause` + `HostAction`（最后呈现帧 BMP）** | auto-pause/`state_seq`/文件 IO |
| `run/status/abort`（Run） | `RunControl` | 不是 ControlBus 9 项；`run_capabilities` 3 项 |
| `wait`（客户端糖） | `RunnerControl::resumeFor(ms)` | **不是 ControlOp** |
| `list`/`gc`（客户端） | 无 | 端点扫描/判活 |

- **传输特有**：`hello`/`capabilities`/`schema`/`nonce`/`session`/`pid`/`exe_sha256`/`shape`/`present`、
  WS 握手/Origin/loopback、idle/orphan/predicate 超时、`bye`、单客户端、`gc`。`run/status/abort` 是 Run
  控制入口，**不计入 ControlBus 9 项**；`hello.capabilities` 仍 9 项，另列 `run_capabilities` 3 项。
- **回执**：Edit 同步 `applied/reason`；Input 先 `queued` 后 `applied`；EngineRead 同步值；
  sample/screenshot 回 `state_seq`/产物；⛔ 不把 `queued` 冒充“已施加”。
- **权限/生命周期**：没有安全二分；WS loopback+token 是传输访问控制；插件进程内受信；
  bus 管 owner/一个 active Run；回调里 pause/quit 拒绝；`releaseOwner`。
- **WS 取舍**：保留 WS 作为传输（已有实现/DSH/独立进程；适配器薄化）；换本地 RPC 只换传输，
  核心不变；仅本地会失去远程 inspect。推荐保留 WS。

### 4.4b Run（执行实例）与 RunControl（改名：Task → Run）

**最终定义（采用 `Run`，不再用 `Task`）**：

- **Plugin** = 代码/上下文：`render` + 可选 `start/onTrigger` + 模块状态；`render` **不是 Run**。
- **Run** = **一次 callback 链的执行实例**：`start` 返回初始 armed 集合；每次 `onTrigger` 返回新的
  armed 集合（替换旧集合）；`null`/`[]`/fail/abort 结束。字段：
  ```c++
  using Params = std::map<std::string, std::string>;
  struct RunHandle { std::uint32_t id; };
  struct RunSpec { std::string module; Params params; std::string origin; };
  struct DispatchResult { bool ok; bool busy; RunHandle handle; const char* code; };
  struct Run {
      std::uint32_t id;
      std::string plugin;          // 模块/插件 id
      enum class State : uint8_t { Running, Waiting, Done, Failed, Aborted } state;
      std::vector<Trigger> armed;             // 当前 armed 集合（Waiting 时非空）
      std::vector<std::string> armed_names;   // 派生；去重后的 name 列表
      std::string last_error;
      std::uint32_t owner;
  };
  ```
- **不是操作类、不是权限/安全边界**；一个 endpoint **同时一个 active Run**（Input owner），
  已有 active ⇒ `dispatch`/`run` 返回 `busy`，不排队。只读/只 Edit 的 Run 也按“唯一”处理
  （简单优先；将来若要多 Run，再立 Input owner/优先级规则）。
- **`RunStatus`**：`armedNames`（注册顺序）与 `armed`（同序、一一对应，**都必填**）、
  `state/lastError/owner`；`RunEvent.name`。owner 是 **Run 级**（创建者插件 id / 控制 session）。
- **`dispatch` 同步**：返回时 `start` 已跑完，`armedNames` 已就绪（除非 `start` 返回 `[]`/`null`，则 Run 直接 done）。
- **`RunControl`**：`dispatch(RunSpec)` / `status(RunHandle)` / `abort(RunHandle)` / `current()`；
  `RunStatus.armedNames` 是当前具名集合；`ctx.run.status/abort/events` 观察；
  `emu-ctl run` 创建 Run；插件 `ctx.dispatch(module, params)` 创建 Run；
  Run 结束/abort ⇒ `releaseOwner`（释放按住的键/按钮）。
- **派发形态**：模块/注册 id + 参数；受控根/allowlist；⛔ 不 eval 任意源码。
- **`initialState`**：`PluginModule` 可选；宿主在 `start` 前创建 `ctx.state`；无 ⇒ `{} as State`。
- **R7**：插件受信，可读也可写 guest；Run 只是执行实例，不是“唯一写者”的安全角色。

### 4.4c UI 与 control 如何结合（`render` 只画；`onTrigger` 只改 UI 状态）

- **绘制只在 `render`（Present/每帧）**：`onTrigger` **不画**，只改**插件本地 UI 状态**
  （`ctx.ui.show/hide/set`）；下一次 `render` 用 **`ctx.draw` display list** 按状态画。
  **不承诺“触发即画”**。`render` 也可写 `ctx.ui`（hover/动画），但同样只影响本帧/下一次绘制。
- **触发点可以来自 `script.pos`/读点**：`onTrigger` 在 `afterPosition` 命中时 `ctx.ui.show("hud", …)`，
  在另一区间命中时 `hide`；这是**期望能力**（进某屏显示 HUD / 离屏隐藏），不是漏洞。
- **UI 状态是宿主本地的**：不是 guest 状态；不进 Edit/EngineRead、不存档、不影响输入；
  `ctx.ui` 只影响下一次 render 的显示列表。
- **暂停/headless/无 Present**：`onTrigger` 仍可改 UI 状态，但**要等到下一次 render** 才可见；
  没有 Present 时状态只留在插件内存（或随插件卸载丢失）。
- **`render` 约束**：只拿当帧 readings（`game.*`/`ctx.read`），不阻塞、不做 guest 写；
  `render` 用 `ctx.draw.rect/text/measure` 发本帧 display list；按钮可以 `ctx.run.dispatch`（C5）；
  `render` 可写 `ctx.ui`，但仍不直接 Edit/Input guest。
- **负向边沿**：**不加** `afterLeave`/`whenNot`；`afterPosition` 是正向电平门。HUD 的 hide 用
  **另一个已知正向位置触发**（链上下一步）实现；如果玩家从 A 直接切到第三个屏、不经过 B，HUD 会保持
  到下一个正向 hide 触发。⛔ d.ts/注释不能承诺“离开区间自动 hide”。
- **“UI 只应在某些屏出现”**：推荐 **script.pos 触发 + `ctx.ui`**（显式 show/hide）；
  **不新增** 核心 `visibleWhen` 声明式 DSL（评估结论：先让插件用 `ctx.ui` 包装，避免又长出一套
  UI 条件语法；若将来要，做插件侧 helper 或独立 UI 层，不进 Core）。
- **渲染出口**：`RenderCtx.draw` 提供 `rect/text/measure`（复用 overlay 的 UI 形状）；
  **坐标 1280×720 逻辑系**，宿主缩放；**z 序 = 调用顺序**；**每帧开始清空**，render 返回后提交；
  `text` 的 `size/align/fill` 同 style；`measure` 用同一套 style。
  `RenderCtx.ui` 是读写（hover/动画），但只影响 UI 状态；模块级变量是 per-plugin 状态（render/onTrigger 共享）。
  ⛔ `RunCtx` 没有 `draw`；⛔ `onTrigger` 不能画。
- **并发 watch**：一次 arm 多个具名 trigger，`onTrigger` 按 `ev.name` 分派；例如
  `[{name:"hud:show",afterPosition A}, {name:"hud:hide",afterPosition B}, {name:"edit:flag",afterPosition C}]`；
  一读点最多触发一个；其余不自动保留（要保留就放进返回数组）；不需要优先级/owner 复杂度。
- **一个插件可以同时有 `render` + `onTrigger`**；没有双上下文/权限二分；`ctx.ui` 是两者共享的
  宿主本地状态。
- **合同 API**：`apps/emulator-lift/scripts/plugins/plugin.d.ts`（`RenderCtx`/`RunCtx`，不含 sample/screenshot）；
  下一轮“只给 d.ts 让另一个 agent 翻译旧 plan/global/HUD 用例”的输入，会被测试修订。
- **HUD 生命周期示例**：`afterPosition("TITLE.BIN", lo, hi, once)` → `ctx.ui.show("hud", {mode:"title"})`；
  `afterPosition("FIELD.BIN", lo2, hi2, reenter)` → `ctx.ui.set("hud", {mode:"field"})`；
  **另一个正向位置** `afterPosition("OTHER.BIN", lo3, hi3, once)` → `ctx.ui.hide("hud")`；
  `render` 每帧 `if (ctx.ui.visible("hud")) ctx.draw.text(...)`。
  `onTrigger` 从不调用 draw；`render` 从不改 guest。

### 4.5 Context（操作类 API；插件运行时）

```c++
struct TriggerReadings {          // 值快照；没有可变宿主/guest 句柄
    std::string script; std::uint32_t pos; bool pos_ok;
    int mouse_x, mouse_y; bool mouse_ok;
    InputPoint point; std::uint64_t read_index;
};
struct Context {
    const TriggerReadings& now;
    const GuestMemory& mem;        // 只读：readGlobal / readScriptPositionSample
    EditSink& edit;                // Edit：立即（暂停/控制点也可）
    IntentQueue& input;            // Input：串行、一读点一条
    RunControl& runs;              // Run：dispatch/status/abort
    UiApi& ui;                     // UI 状态（宿主本地；onTrigger 写，render 写/读）
    ProfileApi& profile;           // profile_begin/end（HostAction）
    RunStateStore& state;          // per-Run 状态容器（不是 guest；不跨 Run 共享）
    // ⛔ 没有 sample/screenshot：它们是 emu-ctl 组合命令
    Log& log;                      // ctx.log（有界）
    const Params& params;          // `--plugin-param k=v`（只读；夹具 / 诊断口）
    AppDataApi& appdata;           // `ctx.appdata`：同步只读 stat（只 appdata 子树；宿主 I/O）
    ConfigApi& config;             // `ctx.config`：域式 JSON 配置（`<--appdata>/plugin-config.json`）
    // Trigger helper
    Trigger afterScript(std::string name, std::string script, TriggerMode, uint32_t max_fires=0,
                        uint64_t timeout_ms=0);
    Trigger afterPosition(std::string name, std::string script, uint32_t lo, uint32_t hi, TriggerMode,
                          uint32_t max_fires=0, uint64_t timeout_ms=0);
    Trigger afterDelay(std::string name, uint64_t ms, TriggerMode);  // 宿主墙钟；NON-DIFFABLE
    Trigger atReadPoint(std::string name, InputPoint filter, uint32_t max_fires);
    // Input（串行）
    Receipt click(int x,int y,MouseButton); Receipt move(int x,int y);
    Receipt down(MouseButton); Receipt up(MouseButton);
    InputStatus inputStatus(std::uint64_t id) const;  // queued/applied/failed/unknown
    // EngineRead（只读/无暂停）
    struct ReadApi {
        GlobalValue globalGet(GlobalKind,uint32_t);
        MarkerSample scriptPos();
        std::vector<FrameInfo> frames();
        std::uint64_t frameHash();
    };
    ReadApi read;          // `ctx.read.global.get/scriptPos/frames/frameHash`
    // Edit（立即）
    struct EditApi { Receipt globalSet(GlobalKind,uint32_t,const GlobalValue&); };
    EditApi edit_api;       // `ctx.edit.global.set`
    void profileBegin(const char* label); void profileEnd();
    DispatchResult dispatch(const std::string& module, const Params& p);
    void done(); void fail(const char* name = nullptr, const char* reason = nullptr);
    void finish(int exit_code, const char* reason = nullptr);  // 结束当前 Run；记录 exit code
    void quit(const char* reason = nullptr);  // 请求宿主停机（WM_QUIT/stop_requested）
};
```

- **`ctx.edit.global.set`**：同步立即（`EditSink`）；暂停/控制点也可；⛔ 不进 `IntentQueue`。
- **`ctx.input.*`**：进 `IntentQueue`；下一读点/按预算；一读点一条；⛔ 不能延迟 Edit。
- **`ctx.read.*`**：同步只读（global.get/frames/scriptPos/纯 frame/hash）；**无暂停**；
  与 Edit 同点组合 `read → edit → input`。
- **`ctx.state`**：per-Run 状态容器；同一模块两个 endpoint/Run 不共享；`render` 可读。
- **`ctx.appdata`**（2026-11-25；**只 `RunCtx`**，⛔ `RenderCtx` 没有 —— render **直接碰不到**
  appdata 子树；它只经宿主 config 桥写 `plugin-config.json`，不做任意路径 I/O）：
  `{ available, path, stat(rel) }`，`stat` **同步**返回结果对象（⛔ 不返回 Promise：`start`/`onTrigger`
  必须同步返回）。只读、**只 appdata 子树**：`rel` 相对 `--appdata` 根，绝对路径 / `..` 段 / NUL /
  空串 ⇒ `error:"invalid_path"`；canonical 后跑出根外（符号链接 / junction）⇒ `error:"escape"`；
  不存在 ⇒ `exists:false, error:"not_found"`；权限 / I/O 分别是 `denied` / `io`（⛔ 不把 errno
  漏给 JS）。`available=false` ⇒ `path=null`、`stat` 一律 `{exists:false, error:"no_appdata"}`。
  `available=false` 的第二种来路：`--appdata` 落在 `--game-dir`（只读安装树）里，或被指向一个
  **普通文件**（`attach` 拒绝，`aot_main` 一行 warn + 继续 ⇒ 插件能力缺失不是启动失败）。
  ★ **非法 UTF-8 的 `rel` ⇒ `invalid_path`**（QJS 把孤立代理项编成 WTF-8；`utf8ToWide` 返回空串
  ⇒ 不拒绝就会退化成"appdata 根存在且是目录"）。
  ★ **宿主段不计入 2 ms**：`stat` 的 C++ 实现包 `HostPause`（与 `ctx.profile.end` 同一原则）。
  ★ 宿主**不知道** title / `set:SavePath`（`set:UseAppDataFolder != 1` 时存档根本不在 appdata）⇒
  存档目录由插件常量拼；`ctx.appdata` 不提供 read / write / 列目录。
- **`ctx.config`**（2026-11-25 `RunCtx`；**2026-11-26 加回 `RenderCtx`**（7→8 成员，父级裁决）——
  render 可经宿主桥**写** `plugin-config.json`，仍⛔ 不做 guest 写 / 任意路径 I/O）：
  配置文件 = **`<--appdata>/plugin-config.json`**（appdata 根；⛔ 不放 `SAVE/`；`clean-appdata.sh`
  的 `rm -rf` 会删它）。信封 `{"version":1,"domains":{"<id>":{...}}}`；值 = **原生 JS 值**
  （对象 / 数组 / 字符串 / 有限数字 / 布尔 / null；⛔ 不是 JSON 字符串 ⇒ 沙箱里不需要 `JSON`）。
  API = `{ available, domain(id) }`；域对象 = `{ get(key, fallback?), has(key), set(key, value),
  delete(key), all() }`。
  · **`ConfigReceipt = {applied, code?}`**：`applied` = **内存已改**（⛔ 不代表已落盘）；
    `code` 只有 `read_only`（配置文件不可写）/ `no_appdata`（没有 appdata）。写盘失败**不回填**
    receipt —— 回调末 flush 失败只有一次 `ctx.log("warn")`（v1 的诚实口径；⛔ 没有不可达的 `io`）。
  · 域**自动创建**、不注册、可共享；域 id = `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 都拒
    `__proto__` / `prototype` / `constructor`。未知顶层键 / 未知域内键**保留**（前向兼容）。
  · `set` / `delete` 只改内存 + 标脏；宿主在**每个插件回调末**（`PluginHost::runCallback`）与
    **每帧 render 之后**（`present_sdl` 的 `drawOverlay` → `UiScript::flushConfig`，**无条件**，
    不看成败）各原子 flush 一次（临时文件 +
    `MoveFileEx(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`）。**I/O** 失败 ⇒
    回调侧 `ctx.log("warn", …)` / 渲染侧一行 `[ui] warn: …`，**一次**、之后不再重试（⛔ 不重试风暴）。
  · 校验错误（域 id / key / 值类型 / 深度 / 节点 / 串长 / 非法 UTF-8 / 域数 / 每域 key 数）⇒
    **TypeError**。上限（`set` 与**装载期**同一套，`runtime/config.h` 是唯一真源）：值嵌套
    **8 层**（根值 = 1）/ 值树 **1024 节点** / 单个字符串与对象 key **8192 字节** / key 长 **128** /
    域数 **256** / 每域 key **1024** / 文件 **1 MiB**。超限的文件 ⇒ **装载失败 + exit 2**
    （`attach` 用显式栈校验，⛔ 不在 `get`/`all` 递归爆栈）。
  · **没有 `--appdata`** ⇒ `available=false`、`domain()` 抛 `Error("no_appdata")`（⛔ 不做内存态）；
    坏 JSON / 顶层不是对象 / `version != 1` / `domains` 不是对象 / 域值不是对象 / 值超限 /
    非法 UTF-8 ⇒ **装载失败 + 进程 exit 2**（错误消息带文件路径，坏 JSON 还带 byte 位置）。
    一个 appdata 只跑一个实例（v1；⛔ 不做文件锁）。
  · **宿主段不计入 2 ms**：每次 get/set/delete/all 的 store 调用**与**值树 ↔ JS 值的转换都包
    `HostPause`；`set` 里用户对象 getter 执行的那几段例外（仍是用户时间）。
- **经典回退 HUD 的 `host.config`**（D2，2026-11-25 父级裁决；与 `ctx.config` **同一份 JS 形状**、
  同一个 `ConfigBridge` / 同一套 trampoline 校验）：`--overlay` + **无 render** 的 `--plugin`
  （= `field-point-visible` 那条用法）走**内部 hud 回退**，经典脚本只有 `globalThis.render`，
  拿不到 `ctx` ⇒ 配置面挂在 `host.config`。
  · 形状 = `{ available, domain(id) }`（域对象同 `ctx.config`）；**只在 render 内**可用。
  · **无桥 ⇒ `host.config` 是 `undefined`**（⛔ 不是抛错）：这是 HUD **每帧**都读的读数面，
    一个**可选**能力不该把整个叠层打黑。⚠ **生产上只有测试替身会造 `undefined`**：`--overlay`
    自 S2 起 = `--plugin hud` 预设，`aot_main` 在 `--plugin` 装配段**无条件**
    `setConfigBridge(plugin_host->config())` ⇒ 有 `--overlay` 就有桥（没有 `--appdata` 时是
    `available:false`，⛔ 不是 `undefined`）。⚠ 与
    `host.profileBegin/End` 的"缺接线 = 调用时 TypeError"**故意不同**：那条是**点击时**的一次性调用，
    静默 no-op 才是"按了没反应且没有原因"。
  · 写盘 = 每帧 render 之后由 `present_sdl` 调一次 `UiScript::flushConfig`（见上）；桥的实现
    `PluginHost` 比窗口对象**先析构** ⇒ `aot_main` 在 run 收口处先 `releaseOverlayConfig()`
    （flush + 摘脚本缓存）再摘 adopted；`disableOverlay` / `shutdown` 也各 flush 一次。
  · `ctx.config` / `host.config` 的实现体只有**一份**（`runtime/config.h` 的 `ConfigBridge`：
    `available/get/has/set/erase/all/flushIfDirty`；`PluginHost` 薄适配 `PluginConfig`）。
  · ⚠ **已知限制（不是沙箱边界）**：`__render` 是 config / domain 对象上的**不可枚举 / 不可写 /
    不可配置**属性（`markConfigObject`），但**脚本可以 `Object.defineProperty` 在自己的 config
    对象上伪造它**：普通对象可扩展，而门只看"这个对象上有没有标记"（`configCall`）。伪造之后，
    存下来的 **RunCtx** `config` 在 render 里也能调（两个面现在都能合法拿到 config，所以这里
    不再有"哪种 config 不该存在"的收窄）—— 同一个 `ConfigBridge` / 同一套校验 ⇒ **无权限提升**，
    只是选错了门。⇒ 它挡的仍是"把对象存下来、离开 render / 回调再调"，⛔ 不是安全边界；
    要真收紧得换帧令牌 / 弱引用表。
  · ⚠ **`--overlay` 预设模式（= `--plugin hud`）没有注册设置**：预设的 hud 不调
    `registerSettings` ⇒ `settings.schema` 是**空表** ⇒ 即使 `--appdata` 可用，点 `settings…`
    也只留一行 `settings: 没有注册设置`（⛔ 不打开一张只有 `SETTINGS` + live + `BACK` 的空白页）；
    没有 `--appdata` 时更早一步停在 `settings: 没有 appdata`。要有设置面就显式
    `--plugin field-point-visible --appdata <根>`（它自己 `registerSettings`）。
- **`ctx.input.status(id)`**：`queued/applied/failed/unknown`；click 只保证入队，完成要查询。
- **`ctx.finish(code, reason)`**：结束当前 Run（可映射历史 exit 5）；**`ctx.quit()`** 请求宿主停机。
- **`sample`/`screenshot` 不在 Context**：它们是 `emu-ctl` 的组合命令（pause + EngineRead/HostAction + 元数据）；
  插件要 script/pos 用 `ctx.read.scriptPos`（无暂停）。
- **没有 mutable `WindowManager&` / `GuestMemory&`**：C++ handler 也不能 `wm.mouseButton`/`writeGlobal`；
  Edit 只经 `EditSink`，Input 只经 `IntentQueue`；这不等于“插件只读”。
- **没有第二个 Input 施加点**：Input 的唯一路径是 `IntentQueue`；EditSink 是 Edit 类，不是 Input。
- `ctx.log`：一行一条、有界/去重；⛔ 不每读点刷屏。
- `ctx.profile`：HostAction（`profileBegin/End`）；在下一个汇点执行；不直接改 guest。
  ★ **S2 起 `RenderCtx` 也有**：`begin(label)` 是 `void`（失败 = TypeError，与 RunCtx 同形）；
  **`end()` 返回 `{ok, text, error}`**（与旧 UiScript 的 `host.profileEnd()` 同形；S2 复审 I3 的
  HUD 回传路，报告文本**不再**额外写 stdout）；`end` 走 `HostPause`。RunCtx 的 `end()` 仍是 `void`
  （报告走 stdout，D-C2-4 不变）。
- **`RenderCtx` 的成员**（S2；**2026-11-26 起八成员**）：`ui` / `draw` / `read` / `game` / `params` /
  `log` / `profile` / `config`（`Object.keys(ctx)` **恰好**八个；`run` 仍属 C5 ⇒ `undefined`）。
  · `config` = `{ available, domain(id) }`，与经典 `host.config` **同一份**
    `makeConfigObject(impl, /*render=*/true)`（同一套校验 / 转换 / 回执）；**没有桥 /
    没有 `--appdata` ⇒ `available=false`**（⛔ 不是缺成员：HUD 靠"成员在不在"区分
    `设置不可用` 与 `没有 appdata` 两种状态行）。写盘 = 每帧 render 之后由 `present_sdl`
    调一次 `UiScript::flushConfig`（与 `RunCtx` 的回调末 flush 同一口径）。
  · `game` = `localFrames()` / `mouse()` **方法** + 经典 `game` 的**只读 getter**
    （`frame` / `fps?` / `script?` / `worst?` / `click?` / `hover?` / `active?` / `overlayKeySeq?`）
    —— 同一个
    `jsGameProperty`、同一份当帧 readings ⇒ 与 `--overlay` 的全局 `game.*` 逐条同口径。
    ★ `overlayKeySeq?`（D4，2026-11-25）= 固定 **F9** 的**单调按下沿计数**（`runtime/overlay_key.h`；
    无按键源 ⇒ `undefined`，⛔ 不编 0；⛔ 不吞键 —— F9 仍到 guest）。
    `FrameInfo` = `{index, script, top, pos?}`（`top` = `slot == cur`，与经典 `local_frames()` 同一个
    布尔；⛔ 不从 `pos` 反推栈顶）。
  · `draw.rect` 的 style 支持 `key`（命中区，与 `ui.rect({key})` 同一个 `Element.key`）；
    ★ `fill` 的 alpha **= 0** ⇒ **不画**（`src/ui/raster.cpp` 在逐像素覆盖**之前**跳过）：这种矩形
    只作**命中区**（整行热区就这么写），⛔ 不是"擦成透明"——叠层是逐元素**覆盖**、不是混合，写
    `(0,0,0,0)` 会把底下已画的面板 / 文本擦掉。守卫 = `overlay` 的 `overlay/rect-alpha-zero`。
    `draw.text` 的 style 支持 `bold`（与 `ui.text({bold})` 同一条渲染路径）。`text/measure` 上传
    `key` ⇒ TypeError（D-C2-3 同口径）。
  · ⛔ `RenderCtx` 仍**没有** `edit`/`input`/`state`/`now`/`trigger`/`fail`/`finish`/`quit`/
    `sample`/`screenshot`；存下来的 **RunCtx** 在 render 里调 `profile.*` 仍 TypeError
    （两份 trampoline，`plugin/render-stored-runctx-rejected`）。
- `ctx.fail/finish/quit`：只影响 runner/owner，不直接改 guest；`ctx.finish(code, reason)` 结束本 Run
  （映射历史 exit 5；C2c 的 `loadSave` 门超时用它），`ctx.quit()` 请求宿主停机。

### 4.6 JS 单插件契约（render / start / onTrigger 可选）

**入口**：`--plugin <id|path>` 是**唯一**脚本装载点（D1）。`--overlay` 自 **S2** 起是
「装载内置 `hud`（保留 id）+ 要求窗口」的**布尔预设**（⛔ 不是第三个装载器）；旧 `--ui`
已在 **S3 删除**（⇒ 未知参数 exit 2）。**一个**运行时/context；可选 `render` / `start` / `onTrigger`：
只有 `render` = UI-only；有 `start`/`onTrigger` = Run（可同时 UI）；没有 `render` 时可 headless；
有 `render` 时需窗口。

★ **S1（2026-11-25）：`--plugin` 收两种 spec**（机械判据只一条）：
`id := ^[A-Za-z0-9_-]{1,64}$`，其余一律当**路径**（含 `.` / `/` / `\` / `:`、绝对路径、`.js` 后缀 ⇒
`x.js` 天然是路径，行为与旧版逐字相同）。id = 文件名 stem，只查**两处非递归根**：
① `<exeDir>/scripts/plugins/<id>.js`（发行）② 编译期 `LIFT_PLUGIN_DIR/<id>.js`（开发，= 源码
`apps/emulator-lift/scripts/plugins`）；**⛔ 不查 `lib/`、⛔ 不递归子目录**。两根都命中 ⇒ `exit 2`
并列出全部候选；都不在 ⇒ `exit 2` 并列出搜过的根与每个候选。命中 ⇒ 打**一行**
`emulator-lift-aot: --plugin id=<id> ⇒ <绝对路径>`，再用绝对路径交给 `initFromFile`（一次性，⛔ 不周期）。

★ **S2（2026-11-25）：`--overlay` = `--plugin hud` 预设**（用户口径，命令逐字不变）：
1. **触发** = 显式 `--overlay` 且**没有** `--plugin`；此时内部解析保留 id `hud`。
   ★ **`--overlay` + 显式 `--plugin` 保持 S2 之前的形状**（复审 I2）：默认 hud **照装** —— 插件**有
   `render`** ⇒ adopt 取代它（取代语成立）；插件**没有 `render`** ⇒ hud 留着照画（⛔ 不静默无 HUD）。
   ★ S3 起没有 `--ui`：旧 UiScript loader 只剩这条内部 hud 回退路径。
2. **`hud` 是保留 id**：解析先于普通插件根 —— `<exeDir>/scripts/ui/hud.js`（发行）→
   编译期 `LIFT_UI_SCRIPT_DIR/hud.js`（开发回退）；两份都在场时 **exe 赢**（与 S2 之前的
   `--overlay` 同一条优先序，⛔ 不把"发行优先"变成歧义 exit 2）；id 判据**ASCII 大小写不敏感**
   （`hud` / `HUD` / `Hud` 同一件事 —— Windows 卷不敏感，精确串会让 `--plugin HUD` 绕过遮蔽）。
3. **用户插件根里的 `hud.js` ⇒ `exit 2`**（`<exeDir>/scripts/plugins/hud.js` 或
   `LIFT_PLUGIN_DIR/hud.js`；列出路径 + "`hud` 是保留 id，改名或给路径"）——⛔ 不静默被内置件遮住；
4. 解析命中 ⇒ **与普通 id 同形**的一行 `--plugin id=hud ⇒ <绝对路径>`（`--overlay` 也打）；
5. **`--plugin hud` 不隐含开窗**：render-only + `--present headless` ⇒ 一次性
   `[plugin] render is defined but no window`（D-C2-5，⛔ 不 exit 2、⛔ 不自动开窗）；
   `--overlay` 仍是**窗口参数**（`--present headless` ⇒ exit 2）；
6. **预设路径**（`--overlay` 且没有 `--plugin`）的窗口档 hud 走 **adopt**：`SdlWindow::init`
   **不再**为它装载 hud.js。**显式 `--plugin` + `--overlay` 仍走旧路径**（见 1）。
   ★ `--overlay` + 预设装到的 hud **没有 `render`** ⇒ 一次性
   `[plugin] --overlay 的 hud 没有 render：叠层不会出现`（⛔ 静默无 HUD）。
7. **`--overlay` 不吃 `--plugin-param`**：`--overlay --plugin-param k=v` 仍 `exit 2`（hud 没有参数面；
   参数面**不等价**，与 S2 之前同一条早报）。
8. **PROFILE 四态**：`--overlay` 的 HUD 走 `ctx.profile`，其中 `end()` 回传 `{ok,text,error}` ⇒
   `INVALID（已复制）` 那一态与旧入口 **逐字相同**（S2 复审 I3）。

★ **S3（2026-11-25）删 `--ui`**：`--ui` ⇒ 未知参数 **exit 2**（与其它未知参数同形；⛔ 不再有任何
deprecation warn / 专门文案）。旧 UiScript loader **保留**，但只服务内部 hud 回退（上面第 1 条）。
★ 显式空串 `--plugin ""` ⇒ **exit 2**（空串不是 id，也不是可装载路径；⛔ 不静默当成"没有插件"）。

**窗口档的 `render` = adopt 进已有叠层 host**（C2b-2）：`--present window/auto` + 有 `render`
⇒ `aot_main.cpp:1395-1429` 的 ⑤' 调 `SdlWindow::adoptOverlayScript`（`present_sdl.cpp:543-568`），
复用**同一个**叠层 host 的 `presentUi` / 宿主字体 / 画布 / 命中表 / `--ui-input` 注入 router；
每帧仍走同一条 `drawOverlay`（`present_sdl.cpp:596-691`）管线。⛔ 不新开第二套 UI 运行时 / 第二个 QJS context。
`--plugin`（有 `render`）**不隐含** `--overlay`：headless 照样能跑（D-C2-5）；窗口档由 adopt 打开叠层。

- `--overlay` 与**显式**插件 `render` 同时给 ⇒ **插件 render 取代 HUD**（一次 stderr 说清，
  ⛔ 不静默）。S2 起 `--overlay` 的 HUD **就是** adopt 进来的内置 `hud` 插件 ⇒ 不再有"先装 hud.js、
  adopt 再丢掉"那一段（`SdlWindow::init` 的默认 hud 装载只由内部 hud 回退触发）；取代语只在
  **显式** `--plugin` 时打（预设不打 —— 那不是取代，就是它自己）。
- adopt 失败（宿主字体缺失 / 窗口没建）⇒ **`exit 2`**（与 `--overlay` 的 R5 同口径；除字体可能已建好外不改叠层状态）：
  `ensureOverlayFont` 缺字体 ⇒ nullptr，`adoptOverlayScript` 窗口没建 / 拒收自己 ⇒ false。
- **内置 hud 的契约**（`scripts/ui/hud.js`，S2）：`export default { render(ctx) }`；用
  `ctx.draw.rect/text/measure`（`key` / `bold` 见 §4.5）、`ctx.game.*` 读数、`ctx.profile.*`。
  ★ 同一份文件**还保留** `globalThis.render = …`（内部 hud 回退 loader 仍要它；S3 删的是 CLI `--ui`），
  两个入口共用同一份绘制体、只换薄适配器 ⇒ 不可能"旧版一套、新版一套"。
  ★ `ctx.profile.end()` 返回 `{ok,text,error}`（S2 复审 I3）⇒ `INVALID（已复制）` 四态与旧入口逐字相同；
  报告文本不再额外写 stdout。`--overlay --plugin hud` 是**同一个** HUD ⇒ ⛔ 不打"被取代"那一行。
- `game.mouse()` 形状 = `{x, y, buttons?}`，`buttons = {left, right, middle}`：**有生产读数源**才装 ——
  `WindowManager::mouseButtons()`（`host_window.h:181-184`）= `InputDevice` 的三键 VK 电平
  （`input_device.h:84-90`，唯一电平表）。没有源（headless / 没有 `WindowManager` 的测试 sink）⇒
  **缺键**（⛔ 不是 `false`：脚本写 `mouse.buttons?.left ?? false`）。`--overlay` / 旧入口也拿到该键
  （`scripts/ui/ui.d.ts` 的 `UiMouse.buttons?`）。

**读数**（2026-11-20 / macOS arm64 / 手工 run；`.tmp/plugin-runtime/c2b2-review.md` §1.4）：

```bash
# 同一构建：窗口档 + 有 render 的插件夹具（夹具见 `tests/plugin_overlay.cpp` 与 `.tmp/plugin-runtime/`）
./out/emulator-lift-aot … --present window --present-hidden --max-frames 1 \
  --overlay --plugin <有 render 的夹具> --present-screenshot <bmp>
# (0,0)=#3366CC（插件 fill，不是 hud 的 #0D131A）+ 恰 1 条"HUD 已被插件的 render 取代"
# 去掉 --plugin ⇒ (0,0)=#0D131A、无取代语
# headless + render ⇒ exit 0 + 一次 "render is defined but no window"、无 [overlay]
```

**模块契约**（ES module；specifier `@amayui/lift-plugin`）：

```js
// @amayui/lift-plugin 只提供类型；插件文件 export default 一个 PluginModule<State>。
import type { PluginModule, RunCtx, FiredEvent, TimeoutEvent } from "@amayui/lift-plugin";

interface MyState { flags: Record<string, boolean> }

const plugin = {
    initialState: { flags: {} } as MyState,   // 宿主在 start 之前初始化 ctx.state
    render(ctx) { /* 可选：每帧 UI/状态回显 */ },
    start(ctx: RunCtx<MyState>) {
        return ctx.trigger.afterPosition("TITLE.BIN", 0x0460, 0x0520,
                                         { name: "load:title", timeoutMs: 180000 });
    },
    onTrigger(ctx: RunCtx<MyState>, ev: FiredEvent | TimeoutEvent) {
        return ev.name === "load:title" ? nextTriggerOrNull : [];
    },
} satisfies PluginModule<MyState>;

export default plugin;   // 宿主 QJS 读 default 导出；⛔ 不读 globalThis/named
```

- `render(ctx: RenderCtx)` 可选；有它就每帧跑（UI），用 `ctx.draw.rect/text/measure` 发本帧
  display list；没有就纯 Run/headless。
- `start(ctx)` 可选；Run 用它返回初始 `Trigger | Trigger[] | null`（`null`/`[]` = 立即 done）。
- `onTrigger(ctx, ev)` 在 trigger/超时/start 后调用；返回**新的 armed 集合** `Trigger[]`（替换旧集合）、
  单个 `Trigger`，或 `null`/`[]` = done；`ev.name` 是触发的具名状态。
- `ctx.edit.*`（立即）、`ctx.input.*`（串行，`ctx.input.status(id)` 查完成）、`ctx.read.*`（无暂停）、
  `ctx.trigger.*`、`ctx.ui.show/hide/set/visible`、`ctx.state`（per-Run）、`ctx.profile`、`ctx.dispatch`；
  **`ctx.appdata`（只 RunCtx）/ `ctx.config`（RunCtx 与 RenderCtx）**（宿主 I/O；§4.5）、`ctx.log`；
  `ctx.finish(code, reason)`（结束本 Run，映射历史 exit 5）/ `ctx.quit()`（请求宿主停机）。
  一个 endpoint 一个 active，已有 ⇒ `busy`（不排队）；`render` 里也可 dispatch。
- **chain 接线（用 name，不用 step 计数器 / ctx.state）**：
  ```js
  const plugin = {
    start: (ctx) => loadSave(Number(ctx.params.slot), { expect: ["FIELD.BIN"] }).start(ctx),
    onTrigger: (ctx, ev) => loadSave(Number(ctx.params.slot), { expect: ["FIELD.BIN"] }).onTrigger(ctx, ev),
  } satisfies PluginModule;
  export default plugin;
  ```
- `loadSave(slot, { expect, timeoutMs, onLoaded })`：**`expect` 必填**（`string[]`，非空、元素非空字符串）
  —— 载入完成后**可以算落点**的脚本名由**调用方按预期给**（照宿主读数拼写、带 `.BIN`）：
  **ADV = `SC*` / `SG*` / `SP*`**（如 `SC0000.BIN`）、**战斗 = `BTL.BIN`**、**地图 = `FIELD.BIN`**（可能还有别的名字）。
  ⛔ 本库**不内置场景清单**（旧版把 `FIELD` / `SC0000` 写死成"三选一终端"就是错的来源）。
  返回 `{ start, onTrigger }`；它是**纯 JS 的回调链构造器**（真源 `scripts/plugins/lib/save.js`），
  ⛔ 不是 `ctx.run`（Run/dispatch 属 C5）、⛔ 没有新宿主 API；宿主每次回调新建实例 ⇒
  ⛔ 不把进度塞进闭包 / `ctx.state`。
  `onLoaded(ctx)` 是"载入**到终态之后**"的接续点（终态先 log 再回调，返回值 = 本次 `onTrigger` 的返回值；
  ⛔ 不给它时行为不变）。**本库不认识场景**：把载入链和场景拼起来是使用方用 `chains.js` 的 `route` 做的
  （§4.6a）。
- **`loadSave` 的缺文件前置检查**（2026-11-25）：`start(ctx)` 的**第一件事**是查
  `<saveDir>/SAVE<NN>.DAT`（`saveDir` 缺省 = `save.js` 的 `SAVE_DIR_REL`
  = `Local/Eushully/天結いキャッスルマイスター/SAVE`，⛔ 宿主不提供它；`loadSave(slot, { saveDir })`
  可覆盖，值**相对 appdata 根**）。缺 ⇒ `ctx.finish(5, …)` + `[]`，**一个 click 都不发**：
  `load-save: save file missing: <绝对路径> (slot 15)`（`ctx.finish` 不改进程 exit ⇒ readout 是
  `run=finished exit=5`）。
  · **只查 `.DAT`**：`.STH` 缺失**不阻止读档**（`save-format.md` §1）；`SAVE.DAT` 不是槽索引
    （缺它最多让页号回 0）⇒ 都不查。
  · **只查 0..99**：`slot >= 100` 的 `%2.2d` 三位字形未取证 ⇒ 跳过 + 一行 warn。
  · **跳过 + 一行 warn**（⛔ 不 fail，否则误杀合法读档）：没有 `ctx.appdata` / `available=false`；
    或 stat 报 `denied` / `io`（无法证明"缺文件"）；或存档目录 stat 报 `escape` / `invalid_path`
    （路径非法 / 符号链接逃逸）。
  · ★ **存档目录不存在不跳过**（2026-11-25 复审 I5 订正）：全新 / 空 appdata 里 `SAVE/` 还没被
    引擎建出来时，`SAVE<NN>.DAT` **确实缺** ⇒ 仍按缺文件 `finish(5)`（reason 额外点名
    "存档目录 … 不存在"）。旧版把 `not_found` 当"存档在安装树"静默跳过 ⇒ 又要等 30 s 门超时。
  · ★ **逃生口 `precheck:false`**（缺省 true）：`set:UseAppDataFolder != 1`（存档在安装树）时
    固定的 `SAVE_DIR_REL` 必然 `not_found` ⇒ 会 fail。此时用 `loadSave(..., { saveDir })` /
    配置 `save.saveDir` 指到实际目录，或显式关掉前置检查（**只打一行 info**，⛔ 不是让
    "真缺文件"静默通过）；`load-save.js` 参考入口也认 `save.precheck`（boolean / "0"/"1"）。
  · 有文件 ⇒ 这条链的形状 / 坐标 / 门 / 超时**逐行不变**。
- **`.BIN` 身份**：`PluginReadings.script` 是 marker **文件名**（`TITLE.BIN` / `SAVE.BIN` / `SBUNKI.BIN` /
  `FIELD.BIN`）；C1 的 `afterScript` / `afterPosition` 是精确 `==`（`trigger_runner.cpp:70,82`），
  ⛔ 不做 recipe 那层的"剥 `.BIN` + ASCII 大小写不敏感"（`recipe.cpp:141-153`）⇒ 链里的名字**必须带后缀**。
- **链（slot 5 实测）**：★ **前提 = 调用方保证已经在 TITLE**（2026-11-21 起 `loadSave` **不再等 TITLE**；
  TITLE 门搬进了 `scripts/plugins/lib/boot-to-title.js` 的 `bootToTitle`）。`start(ctx)` 直接
  `click(1070,480)`（TITLE 的 LoadData 入口）→ `SAVE [0x1104,0x11C4)` → `move(300,rowY)` → 读点 →
  `click(300,rowY)` → `SBUNKI [0x0B18,0x0C58)` → `click(640,323)` → 终态 `[expect… | SBUNKI 0x0AF8]`；
  `0x0AF8` ⇒ `click(690,323)` → `[expect…]` ⇒ done。
  ⚠ 不在 TITLE ⇒ 这一下点不进 LoadData ⇒ **SAVE 门超时**（`load-save: gate timeout (load:save)`）=
  响亮失败；⛔ 不自己再等一轮 TITLE（那会把"启动没到"与"TITLE 上没点进去"混成同一条超时）。
  从冷启动跑 ⇒ 把 `bootToTitle({ onDone: (ctx) => load.start(ctx) })` 盖在前面（§4.6a 的三段样板）。
  ★ 确认点击为什么等 `[0x0B18,0x0C58)`：`save07-battle.plan` 头的 trace 读数 —— 同一张「要读取吗？」对话框，
  轮询体 `[0x09A4,0x09F8)` 与读段 `[0x0AF8,0x0B18)` 里 click 会被吞（press 只跨 2 个读点），
  **读段之后** `[0x0B18,0x0C58)` 里 3/3 被消费。旧窗口 `[0x09F8,0x0B18)` 的落点多数在 `0x0AF8` ⇒ 点击被吞
  ⇒ 链停在那里（`REQ-01M7W2X81F1ASB488EJFQGMQSH` 的签名：`gate timeout (load:field-2)`）。
  `rowY = {0:91,1:150,2:209,3:269,4:328,5:388,6:447,7:507}`（实测表；`7` = `save07-battle.plan` 的槽 007 读数；⛔ 不用公式外推 8–9）。
  ★ **数值只在 `scripts/plugins/lib/script-positions.js` 写一次**（位置）与 `guest-globals.js`（global id / 位）；
  消费方 `import`，⛔ 不裸写 `0x…`（守卫 `tools/test/script-map.test.mjs`）。
  ★ `script-positions.js` 现在是**生成物**（真源 = `tools/script-positions.json` 的锚表）；`patch.json` 改完后跑
  `pnpm tools script-positions check`（红 = 忘了 `gen`）。
- **失败面**（全部响亮）：slot 非整数 / 越界 ⇒ **构造时 throw**；`expect` 缺 / 空 / 元素非法 ⇒ **构造时 throw**
  （消息给出 ADV / 战斗 / 地图三类别名）；未知 `ev.name` ⇒ `ctx.fail` + `[]`；
  门超 `timeoutMs`（缺省 30000）⇒ `ctx.finish(5, "load-save: gate timeout (…)")` + `[]`（⛔ 不用 `ctx.quit`）。
- **参考入口**（S1 起在**根**）：`scripts/plugins/load-save.js`（id = `load-save`）负责
  config 域 `save` > `--plugin-param` > 默认，再调 `lib/save.js` 的 `loadSave`；
  `lib/save.js` 只是**库**（没有 `export default`、⛔ 不是 `--plugin` 的 id 目标）。
  `--plugin load-save` 与 `--plugin scripts/plugins/load-save.js` 等价。
  ★ 2026-11-25 起参数来源 = **config > `--plugin-param` > 默认**：config = `<--appdata>/plugin-config.json`
  的域 **`save`**（`slot` number / `expect` string[] 或逗号串 / 可选 `saveDir` / `timeoutMs`）；
  没有 appdata ⇒ **完全回退**到 params（四个 key 都支持 params 回退；后两个是本轮新加）；
  `expect` 两处都没给 ⇒ 默认 `FIELD,SC0000` + 一行 info。
  `--plugin-param` 保留为**夹具 / 诊断口**（形状错误仍 exit 2），⛔ 不再是内建插件的正常用法。
  库消费者用**具名** `loadSave(slot, { expect })`（签名与链形状**不变**）。
- **三个内建插件的域**（配置的 owner 表见 `apps/emulator-lift/scripts/plugins/README.md`）：
  `save`（根入口 `load-save.js`，库是 `lib/save.js`）/ `scenario.save10-field`（根 `scenario-save10-field.js`，
  key `slot`/`expect`）/ `field.point-visible`（根 `field-point-visible.js`，
  key `on`/`types`/`diag`（判据 = **已知脚本位置**：一条
  `afterPosition(…, { mode:"reenter" })` —— `DRAWOBJ.BIN` 的 `[0x003C,0x06BC)`，即
  #0 `detach-texture` → 点位槽循环尾的**入口窗口**，`name = "fp:draw"`；全语料 4 处
  `call-script 5117` 全部从这里进 ⇒ 一条钩子覆盖全部调用者；⛔ 不轮询（没有 `afterDelay` / `poll` /
  `FIELD_MAIN_LOOP`）；旧配置的 `static` / `poll` 无害忽略）——
  ★ 它是**常驻开关**：⛔ 不读档、⛔ 不点鼠标、⛔ 不 `finish`；施加 = 对类型命中的槽
  **只补 `50ff9f` 的 bit0**（`flags | 0x1`；`DRAWOBJ` 的 bit0=1 才画），**⛔ bit1 / bit2 一个都不写**。
  ★ **为什么只补 bit0**：`SETOBJ` #2 每渲染帧 `fill-zero (global-int 50ff9f) 32` 后逐槽按"发现 / 触发
  状态"重算 ⇒ bit1（静态门的一半）/ bit2（静态↔动效选择位）**由引擎决定**；插件强加 bit2 的旧写法
  `|= 0x5` 才会出现"没采集过的点也在动"（真机读数 2026-11-26 / 存档 012：slot3 `0x5` = 已触发 ⇒ 动效，
  slot4/5/6 `0x3` = 未采集 ⇒ 静态）。
  ★ **为什么必须在 `DRAWOBJ` 入口**：同一渲染帧的 `RENDERMAP` 先 SETOBJ（`fill-zero` 再重算）⇒ 插件
  写在派生表上的位必被抹掉；两条 `FIELD.BIN` setmap 钩子因此已删（写在 SETOBJ 之后但同帧就被重算）。
  写集只有 `50ff9f+slot` 的 bit0；读只有 `50ffd1+slot`（类型）与 `50ff9f+slot`（flags）；
  `25c00` / `14d73c` / `14d76e` / `14d8cc` / `eb5c1` / `511080` **一个都不读也不写**。
  `fp:draw` 每帧 fire ⇒ 日志只在首帧 / `matched/patched` 变化时一行（逐槽行只为真的改了的槽；
  `diag=1` 每 60 帧一行），⛔ 不每帧刷屏。
  `?`/`!` 是 A 链格标记、不是点位。旧配置里的 `slot`/`hold` 被**无害忽略**。验收时**玩家自己读档**，见
  `docs/03-emulator/scripts/FIELD.md` §⑦）。
  三者一律"先看 `ctx.config.available` 再回退 params"；`on:0` / `slot:0` 这种 falsy 值也算"给了"
  （判据是 `domain.has(key)`，⛔ 不是真值）。

```js
// scripts/plugins/lib/save.js（节选；行表是实测值；位置常量 import 自 ./script-positions.js）
// ★ 前提：**已经在 TITLE**（TITLE 门在 ./boot-to-title.js：`afterPosition TITLE [0x0460,0x0520)`）。
const SCRIPT_SAVE = "SAVE.BIN", SCRIPT_SBUNKI = "SBUNKI.BIN";
const ROW_Y = { 0: 91, 1: 150, 2: 209, 3: 269, 4: 328, 5: 388, 6: 447, 7: 507 };
const LOAD_EXPECT_PREFIX = "load:expect:";
export function loadSave(slot, options = {}) {
    const rowY = ROW_Y[slot];
    if (!Number.isInteger(slot) || rowY === undefined) throw new Error(`slot ${slot} …`);
    const expect = options !== null ? options.expect : undefined;
    if (!Array.isArray(expect) || expect.length === 0) throw new Error("loadSave: options.expect 必填…");
    const onLoaded = options !== null ? options.onLoaded : undefined;
    const timeoutMs = options !== null && options.timeoutMs !== undefined ? options.timeoutMs : 30000;
    const expectTerminals = (ctx) =>
        expect.map((s) => ctx.trigger.afterScript(s, { name: LOAD_EXPECT_PREFIX + s, timeoutMs }));
    const terminalAfterConfirm = (ctx) => expectTerminals(ctx);
    const terminal = (ctx) => {
        const gates = expectTerminals(ctx);
        gates.push(ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CONFIRM_READ_SEG_LO, SBUNKI_CONFIRM_READ_SEG_HI,
                                            { name: "load:confirm2", timeoutMs }));
        return gates;
    };
    const reached = (ctx, script) => {
        ctx.log("info", `load-save: reached ${script} (slot ${slot})`);
        return onLoaded === undefined ? [] : onLoaded(ctx);   // ⛔ 不 try/catch：抛错交给宿主记 fail
    };
    return {
        start(ctx) {
            // ★ 前提：已经在 TITLE ⇒ 直接点 LoadData 入口；点不进去 ⇒ SAVE 门超时（响亮失败）。
            ctx.input.click(1070, 480);
            return ctx.trigger.afterPosition(SCRIPT_SAVE, SAVE_INPUT_CALLBACK_LO, SAVE_INPUT_CALLBACK_HI,
                                             { name: "load:save", timeoutMs });
        },
        onTrigger(ctx, ev) {
            if (ev.kind === "timeout") { ctx.finish(5, `load-save: gate timeout (${ev.name})`); return []; }
            switch (ev.name) {
                case "load:save":
                    ctx.input.move(300, rowY);
                    return ctx.trigger.atReadPoint("any", 1, { name: "load:row", timeoutMs });
                case "load:row":
                    ctx.input.click(300, rowY);
                    // ★ 确认点击等**读段之后**的 [0x0B18,0x0C58)：在它之前点会被吞（见上）。
                    return ctx.trigger.afterPosition(SCRIPT_SBUNKI, SBUNKI_CONFIRM_CLICK_LO, SBUNKI_CONFIRM_CLICK_HI,
                                                     { name: "load:sbunki", timeoutMs });
                case "load:sbunki":
                    ctx.input.click(640, 323);
                    return terminal(ctx);
                case "load:confirm2":
                    ctx.input.click(690, 323);
                    return expectTerminals(ctx);   // 第二道确认之后：只剩 expect 里的脚本
                default:
                    if (ev.name.startsWith(LOAD_EXPECT_PREFIX)) {
                        return reached(ctx, ev.name.slice(LOAD_EXPECT_PREFIX.length));   // 终态
                    }
                    ctx.fail(ev.name, `unknown state: ${ev.name}`); return [];
            }
        },
    };
}
```

**读数**（2026-11-20 / macOS arm64 / 手工 headless；`.tmp/plugin-runtime/c2c-headless3.{out,err}`；
⚠ 这组是**拆分前**的形状 —— 当时 TITLE 门还在 `loadSave` 里。2026-11-21 起参考入口把 TITLE 门换成
`bootToTitle`，**回调数不变**（一个门换一个门 ⇒ `calls` 仍 6 / 7，日志首行变成 `boot: waiting for TITLE …`））：

```bash
cd apps/emulator-lift && ./out/emulator-lift-aot --exe ../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE \
  --game-dir ../../links/install --appdata ../../.tmp/plugin-runtime/appdata-c2c \
  --present headless --skip-logo --audio off --time-limit 90 \
  --plugin load-save --plugin-param slot=5 --plugin-param expect=FIELD,SC0000
# （S1 起参考入口 = 根 `scripts/plugins/load-save.js`；旧写法 `--plugin scripts/plugins/lib/save.js` 已不成立）
# plugin    : … params=expect=FIELD,SC0000,slot=5 calls=7 run=done
# [plugin] boot: waiting for TITLE input callback → boot: TITLE hit
#          → load-save: start (slot 5) ⇒ click LoadData → SAVE hit → row hovered → SBUNKI hit
#          → SBUNKI confirm read-segment ⇒ second confirm → reached FIELD.BIN (slot 5)
# stop      : time-limit（正常结局）
```

`calls=6` 是另一条合法终态（`expect` 里的脚本直接命中，不再走第二道确认）；`calls=7` 是落进
`0x0AF8` 的形态。⛔ 不把某个 `calls` 数当唯一合法值。

- **loader**：读 ES module 的 **default 导出**（`PluginModule` 对象）；`render`/`start`/`onTrigger` 都可选，
  至少一个；有 `start` 就必须有 `onTrigger`，反之亦然；类型不是函数 ⇒ `exit 2` + `file:line`。
  ⛔ 不再读 `globalThis.render/start/onTrigger`/命名导出。
- **`initialState`**：`PluginModule<State>` 可选字段；宿主在 `start` **之前**初始化 `ctx.state: State`；
  省略 ⇒ `{} as State`（有必填字段的插件必须提供 initialState）；`start`/`onTrigger` 首次读也保证有值。
- **泛型 state**：`PluginModule<State>` / `RunCtx<State>`，`ctx.state: State`（per-Run，只放数据）；
  用 `satisfies PluginModule<State>` + `import type` 做编译期检查（不用运行时 `definePlugin`）；
  `render` 的 per-plugin 状态用模块级变量（与 Run 的 `ctx.state` 区分）。
- **同步**：`start`/`onTrigger` 必须同步返回；返回 Promise/async ⇒ 明确错误（D10 没有跨帧 async）。
- **按操作类**：`ctx.input.click/move/down/up` → IntentQueue（串行）；`ctx.edit.global.set` → EditSink（立即）；
  `ctx.read.*` → EngineRead（只读/无暂停）；`sample/screenshot` 不暴露（emu-ctl 组合）；
  ⛔ 没有 `input::*`/`writeGlobal`/`wm.*` 宿主函数（不是“插件只读”，而是操作类/唯一路径）。
- **预算**：每次 callback deadline **2 ms** + op limit **100k**（`runtime/budget.h` 的
  `kCallbackDeadlineNs` / `kCallbackOpLimit`，D5）；超限 ⇒ fail + 摘 trigger。
  ★ **2 ms = 用户 JS 本体的 CPU 时间**（2026-11-21 口径收窄 + CPU 钟裁决，用户裁决 D5 可改）：
  deadline 起点 = 组完 ctx / 事件对象、**即将调用插件函数之前**
  （`script_quickjs.cpp` 的 `call()`），中断回调拿**同一把 CPU 钟**（`pluginThreadCpuNs()`）比；
  ⇒ **模块装载 / ctx 与事件对象构造 / 返回值→TriggerSet 收集 / 宿主调度都不计**，被 OS 抢占 / 等 I/O
  也不计。
  ★ **宿主能力调用（本轮只接 `ctx.profile.end`）也不计**（2026-11-25，缺陷单
  `REQ-01M86VRT1YP53CJS0M885FCCJM`）：`HostPause` 在宿主调用前后取 CPU 差，累加进 `paused_ns`，
  `js_cpu_ns` 与中断判据都扣掉它；诊断行 `paused_us` 是扣除量。⛔ 不是放宽 2 ms（见
  `plugin/callback-js-band` 与 `plugin/callback-host-pause`）。★ **粒度**（2026-11-21 / win32 实测）：POSIX `CLOCK_THREAD_CPUTIME_ID` = ns 级；Windows
  主源 `QueryThreadCycleTime` ≈ ns 级（首次用 4×200 µs 忙等标定，本机 ≈3.97 cycles/ns，`call()`
  在计时窗前预热）；`GetThreadTimes` 是 **15.625 ms 整 tick 记账**（只作兜底 ——
  0.5 ms 的忙等读到 0 或 156250，判不了 2 ms）。⚠ 按 MSDN，`QueryThreadCycleTime` **不保证精确**
  （CPU 频率变化 / 跨核迁移都可能偏）⇒ 它只服务"2 ms 判据"，⛔ 不要当"精确 CPU 时间"用。
  宿主段另有**安全网** = 10×（2 ms ⇒ **20 ms**），**仍是墙钟**：防"宿主被卡死 ⇒ guest 被拖住"，超了照样 fail，
  但 reason 必须可区分：
  - `onTrigger: callback budget exceeded: <name>`（用户 JS 本体超；`start: …` 同形无 name）
  - `onTrigger: callback budget exceeded (host): <name>`（宿主安全网；`start: … (host)` 同形）
  ★ 读数（2026-11-20 / macOS arm64 / 手工 headless 5 跑 2 次；**旧口径** —— 那时 2 ms 罩整个宿主段）：
  `start` 与 `onTrigger`（`load:sbunki`）都会偶发超 2 ms —— `calls=1 run=failed reason=start: callback budget exceeded`
  与 `calls=5 run=failed name=load:sbunki reason=onTrigger: callback budget exceeded: load:sbunki`。
  ★ 读数（2026-11-21 / win32 / `ctest -R plugin-headless`；**旧口径**）：拆分"启动到 TITLE"之后，红点变成
  `onTrigger: callback budget exceeded: load:confirm2`（**10 跑 1 绿**）；同一台机器上 HEAD 的 `save.js`
  走同一 ctest **3/3 绿**、且手工 headless 新旧两版都绿（`calls=7 run=done`）。⚠ 未定论的是**为什么**：
  e2e 成功时不回显子进程日志 ⇒ 无法确认 HEAD 那 3 次是否也走了第二道确认分支。两种可能都成立：
  ① 新链让 e2e 每次都走第二道确认（分支选择变了）；② 那条回调在旧链上本来也在 2 ms 边缘。
  ⇒ **2026-11-21 起按新口径收口**：2 ms 不再罩 ctx 构造 / 收集 / 装载（缺陷单
  `REQ-01M7VVDV69MJXRHNDGR50H7VHC`）；⛔ 不据此删日志，也⛔ 不把 deadline 放宽当修法。
- **owner**：模块实例 = owner；`done/fail`/卸载 ⇒ `releaseOwner`；不残留按住。
- **trace**：`ctx.log` + runner `trigger:`/`intent:`；不要求 plan DSL 图元。

### 4.6a 新场景怎么写（不要重写载入存档 / 也不要重写"启动到 TITLE"）

**是三件东西，不是一个成品**：`boot-to-title.js` 只提供**启动到 TITLE**（一道门）；
`save.js` 只提供**载入链**（前提 = **已经在 TITLE**，落点 `expect` 由调用方给）；
`chains.js` 只提供**通用的链路由** `route(isFirst, first, then)`；把三者拼起来是**使用方**的事
（`save.js` 不认识场景，也不替谁拼）。

**规则 1（前提：已经在 TITLE）**：`loadSave` 的 `start(ctx)` 第一件事就是 `click(1070,480)`（LoadData 入口），
然后 arm SAVE 门 —— ⛔ **不等启动**。冷启动跑 ⇒ `bootToTitle({ onDone: (ctx) => load.start(ctx) })` 盖在前面：
它只有一道门 `afterPosition TITLE [0x0460,0x0520)`（吃 click 的窗口，15/15；数值在 `script-positions.js`），
命中才拉起载入链；trigger 名一律 `boot:` 前缀。门超时 ⇒ `ctx.finish(5, "boot: gate timeout (…)")`。
⚠ 拆分后两类超时**判据不同**：`boot: gate timeout` = 启动没到 TITLE；`load-save: gate timeout (load:save)` =
TITLE 上那一下没被吃（⛔ 不自己再等一轮 TITLE）。

**规则 2（终态由调用方给）**：`loadSave(slot, { expect })` 的 `expect` 是"这些落点算载入完成"的**脚本名表**
（照宿主读数拼写、带 `.BIN`），按**这次载入的预期**给：**ADV = `SC*` / `SG*` / `SP*`**（如 `SC0000.BIN`）、
**战斗 = `BTL.BIN`**、**地图 = `FIELD.BIN`**（地图可能还有别的名字）。⛔ 工具**不再内置场景清单**；
`expect` 缺 / 空 / 元素非法 ⇒ 构造时 throw。

**规则 3（三段、三套前缀，`route` 嵌套一次）**（`scenario` 只是 `{ start, onTrigger }`，且**无状态**：
进度只在 `ev.name`，⛔ 别用 `ctx.state` 记步）：

```js
// scripts/plugins/lib/my-scenario.js（★ 必须在本目录，见下面第一条约束）
import { bootToTitle } from "./boot-to-title.js";
import { route } from "./chains.js";
import { loadSave } from "./save.js";

const myScenario = {
    start(ctx) {
        hideBattleHelp(ctx);   // 规则 4：载入之后、在 FIELD 上做
        return ctx.trigger.afterPosition("FIELD.BIN", LO, HI, { name: "mine:field" });
    },
    onTrigger(ctx, ev) {
        switch (ev.name) {
            case "mine:field":
                ctx.input.click(X, Y);
                return ctx.trigger.afterPosition("SELACT.BIN", A, B, { name: "mine:next" });
            case "mine:next":
                ctx.finish(0, "scenario: done");
                return [];
            default:
                ctx.fail(ev.name, `unknown state: ${ev.name}`);
                return [];
        }
    },
};

const load = loadSave(5, { expect: ["FIELD.BIN"], onLoaded: (ctx) => myScenario.start(ctx) });
const boot = bootToTitle({ onDone: (ctx) => load.start(ctx) });
// `route` 只剥一层 ⇒ 嵌套一次：外层剥 `boot:`，内层剥 `load:`，剩下的就是场景自己的前缀。
const afterBoot = route((ev) => String(ev.name).startsWith("load:"), load, myScenario);
export default route((ev) => String(ev.name).startsWith("boot:"), boot, afterBoot);
```

**规则 4（场景第一件事：关掉「战斗中的帮助界面」）**：`guest-globals.js` 的 `hideBattleHelp(ctx)` 读
int global `0xb1b6` → `|0x10` → 写回（`BATTLE_HELP_GLOBAL_ID` / `BATTLE_HELP_HIDE_BIT`）。
**必须在"载入之后、在 FIELD 上"做** —— 载入会还原脚本 / global 状态，在 TITLE 上置位会被覆盖。
写失败（`applied === false`）⇒ `ctx.log("warn", …)` 带 `code` / `accepted` / `before` / `after` + 返回 `false`，
⛔ 不静默、⛔ 不代调用方 `ctx.fail`。旁证：§C1 的 `global.get(0xb1b6)` → `set(…, v|0x10)` 例子、
`save05-black-rect.md`（`global-get --index 0xb1b6` 的 "SO033A 覆盖图那位"）。

三条硬约束：

- **入口在上、库在下**（S1 改写）：宿主 QJS 的 import 根 = **入口文件所在目录**
  （`script_quickjs.cpp` 的 `moduleNormalize`），解析结果必须在该根**之下**（`../` 越根会被拒）。
  ⇒ 场景 / 参考入口住在 `scripts/plugins/` **根**，用 `./lib/boot-to-title.js` 这类**向下** import；
  `lib/` 里的库彼此用同目录 `./x.js`。⛔ 入口不能住在库的下层（以 `lib/` 为入口目录时 `../` 会红）。
  ★ 同一布局也是 `--plugin <id>` 的判据：id 只查 `scripts/plugins/` 根（非递归），`lib/` 永远不是 id 目标
  ⇒ "需要单独加载的东西"必须在根上（`load-save.js` / `scenario-*.js`）。
- **每条链的 trigger 名字必须两两不相交**（`boot:` / `load:` / 场景自己的前缀，本案 `battle:`）：
  `route` 只看 `isFirst(ev)`，名字串了会被另一条链抢走、**静默不触发**。
- **位置 / global 的数值只在常量表里写一次**（`lib/script-positions.js` / `lib/guest-globals.js`）：消费方 `import`，
  ⛔ 不裸写 `0x…`（守卫 `tools/test/script-map.test.mjs`，剥注释后扫 plugins 根**与** `lib/` 两个目录）。

样板：`scripts/plugins/scenario-save07-battle.js`（启动到 TITLE → 载入 007 → 关战斗帮助 → 结束回合 →
決定 → BTL，全篇零 `wait`；门 / 坐标与 `scripts/recipes/save07-battle.plan` 同源；
**`expect: ['FIELD.BIN']`** —— SAVE07 落在图上，战斗是之后由用户动作进的）。
纯档用例：`control-script/load-save-onloaded`（样例入口，含 boot 门）+ `control-script/chains-route`。

### 4.7 C3：JS recipe driver + parity harness（历史 plan 只验证/支持；**已落地（main `eecbfac`）**）

**定位**：文本 recipe **冻结**，⛔ 不再产出新能力；C3 只做“历史 plan 的 driver 与 parity”——**已落地**
（`eecbfac`），下面写的是**实际落点**，不再是拟名/拟路径。

**C++ parser 只产数据（唯一 JSON 出口 = 独立 TU）**：

- `Recipe/Step` 保持现状（`src/runtime/recipe.{h,cpp}`，含 `timeout`/`origin`/line），但**不执行**；
  parser / 执行器**一个字符没动**（冻结面）。
- `src/runtime/recipe_json.{h,cpp}` 是 `Step[]` 的**唯一** JSON 出口（独立 TU；只读 `Step`，不执行）：
  `kind/script/posFrom/posTo/waitMs/x/y/button/label/timeoutMs`。
- `Step[]` 的 `id`/`origin`/`line`/`label` **全量导出但不参与判定**（trace / 报错定位用；parity 只比语义面）。
- `StepKind`/parser **冻结**：⛔ 不加 `global_set`/`global_get`/变量/表达式/新指令；⛔ 不做 JS→Step 生成。
  `global.set/get` 只存在于 callback API（§4.6）。

**JS driver**（`scripts/plugins/lib/recipe.js`；`runRecipe(steps)`；每步用 `recipe:<index>` 作 name）：

- **只做翻译**：九条指令 + 行尾 `timeout` + `quit` + `profile_*` + `finish(0)/finish(5)`；
  门 / 超时 / 完成度判据仍只有**一份**（`src/runtime/recipe.*` + 宿主 `TriggerRunner`），driver 不另建状态机。
  - `await script` → `ctx.trigger.afterScript(...)`；`await position` → `ctx.trigger.afterPosition(...)`
    （Once；`timeout` 作为 trigger timeout）；
  - `wait <ms>` → **宿主墙钟**（`afterDelay`）；**NON-DIFFABLE**；
  - `click/mouse_move/down/up` → `ctx.input`；`profile_begin/end` → HostEffect；
  - `quit` → `ctx.finish(剩余 ? 5 : 0, …)` + `ctx.quit(…)`（旧执行器：`quit` 之后还有指令 ⇒ 非 0）。
- 完成度/`exit 5` 映射：driver 用**具名 Trigger 集合**（`recipe:<index>`）推进，不维护 step 计数器；
  全部消费完 ⇒ **`ctx.finish(0)`**（正常收口，⛔ 不是 `quit`）；
  门超时/失败 ⇒ `ctx.fail(reason)` + **`ctx.finish(5, reason)`**（C3 driver-only grant）；旧 `stepText`/trace
  由 driver 用 `ctx.log` 复刻（或 parity harness 只比对事件序列）。
  ⚠ 合同早先写的 **`ctx.done()` 是笔误** —— `plugin.d.ts` 里不存在这个成员；消费完就是 `finish(0)`。
- **不新增 plan 能力**；如果某条旧 plan 表达不出来，那是 **callback API 缺口**，不是“给 plan 加指令”。

**parity harness**（CTest `recipe-driver-parity`，**pure 档**）：

- 落点：`tests/recipe_driver_parity.cpp`（oracle = `InputPlan::atPoint`）+ `tests/plugin_fixtures/recipe-driver.js`
  （插件入口）+ `scripts/plugins/lib/recipe.js`（driver）；**顶层无条件注册 ⇒ pure**
  （零 `Machine` / 零 GPU / 零真窗口 / 零游戏安装 / 零 LFS 资产）。
- 输入：`scripts/recipes/*.plan` 全部官方配方（逐条 replay）+ `repro:` 锚的**指令行子串**
  （由 `tools/test/recipe-freeze.test.mjs` 逐字钉住，⛔ **不做配方 replay**）。
- 两路：旧 `--input-plan`（oracle，仍用现有 `InputPlan`）vs 新 C3 JS driver（同一 `Step[]`）。
- 逐项比对：消息序列（message/wParam/lParam/电平）、`fired/applied/pending`、exit、`stepText`（driver arm 的那一步
  ↔ oracle 卡住的那一步）/trace。
- **语义等价 + 已知差异表**，不承诺逐位；含 `wait` 的标 `NON-DIFFABLE`；旧执行器仍是历史真源。
- **已知差异 10 条**（`tests/recipe_driver_parity.cpp` 的 `kKnownDiffs[]`；⚠ 这里的 D 编号属于**这张差异表**，
  与 §7 的 D1–D8 未决问题**不是**同一套编号；逐条标“旧执行器保留 / JS 部分覆盖 /
  不覆盖”，⛔ 不声称全覆盖）。其中**两条必须点名**：
  - **D5** 显式 `mouse_down`/`mouse_up` 在 C1 executor 里**不施加**（`intent_executor.cpp`）⇒ 带 `mouse_*`
    的配方（含 `save05-move.plan`）是 **NON-DIFFABLE**（只承诺“到断点为止逐条相同”）；
  - **D10** `click` 与 `quit` **同一回调** ⇒ 插件路径会丢掉刚 staged 的 input（旧执行器不丢）⇒
    `click; quit` 这种形状 driver **少落一次写**（官方 `.plan` 里没有这种形状）。
  - 其余 8 条：D1 `timeout` `>=` vs 旧执行器严格 `>` 的 1 ms、D2 script 名口径（旧执行器不敏感 + 剥 `.BIN`）、
    D3 `wait` 墙钟、D4 `ctx.input.*` 下一读点落地、D6 `quit` 机制（不产生虚拟 `WM_QUIT`）、
    D7 `profile_end` 无 label、D8 `exit 5` 只能映射 `finish(5)` 的 receipt、D9 `Step[]` 以生成的 ES module 进
    driver（⛔ 不给 `ctx` 加成员）。
- `InputPlan` **保留**（不退役）：summary/failureReport/fault/trace/exit5 不动；生产仍走 `--input-plan`；
  `src/runtime/recipe.*` / `input_plan.*` 保留 parser/执行器，C3 只加离线 driver/parity。官方 `.plan`/`repro:` 锚**不迁移**。

**为什么不是 C++ 兼容前端**：用户明确“旧 plan 用一个预编写的 js 就能直接驱动它”，正好验证 callback API
的完整性；C++ 前端会留下第二个 step 状态机，和“核心没有 plan VM”冲突。⇒ C3 用 JS driver。

### 4.8 单插件运行时（删除双上下文）

- **一个 QJS 插件运行时/上下文**：`--plugin <id|path>`（`--overlay` 是它的窗口预设）；同一个 `ctx` 同时提供
  **可选** `render`（每帧；有它就需要窗口）与 **可选** `start/onTrigger`（读点 callback 链）；
  一个插件可以**既 UI 又 Run**，也可以只做一种；同时可 Edit/Input/EngineRead。⛔ 没有 `--ui-control` 第二上下文。
- **overlay 不再是“另一种插件”**：`--overlay` 只是“启用 render + 显示”的插件用法；
  它和 Run 共用同一个运行时、同一套 `ctx.edit/input/read/trigger/run`。
- **窗口档 = adopt 进已有叠层 host**（C2b-2）：`--present window/auto` 且插件有 `render` ⇒ 由
  `aot_main.cpp:1395-1429` 的 ⑤' 调 `SdlWindow::adoptOverlayScript`；复用同一份 `presentUi` / 宿主字体 /
  画布 / 命中表 / `--ui-input` 注入 router，每帧仍走同一条 `drawOverlay`。⛔ 不新开第二个 QJS context。
  `--overlay` 同时给时，hud.js 由 `SdlWindow::init` **先装**、adopt 成功后再 `ui_script.reset()` /
  `ui_runtime.reset()` **后弃**（结构上仍需 hud.js / kit.js 在场）；一次 stderr 说明取代（⛔ 不静默）。
  adopt 失败（字体缺失 / 窗口没建）⇒ `exit 2`（与 `--overlay` 的 R5 同口径）。
- **control（WS/emu-ctl）不是 QJS 上下文**：它是 `ControlBus`/`RunControl` 的传输适配器；
  `emu-ctl run` 在**同一个插件运行时**装载/执行一个 Run（一个 active slot）。
- **失败隔离**：一个插件实例的异常/超预算 ⇒ 停该插件的 Run/render + 一行 stderr；run 继续；
  ⛔ 不需要“两个 runtime 互相隔离”的安全设计。
- **单 TU 是编译/依赖约束，不是安全**：`quickjs.h` 仍只在 `script_quickjs.cpp`（现有硬约束）；
  该 TU 暴露语言无关的 `PluginRuntime`（`render/start/onTrigger` 的 trampoline + host 回调函数指针）；
  **`src/plugin/plugin_host.cpp`** 实现 `TriggerHandler`/`ControlHostOps`，只调 `EditSink`/`IntentQueue`/
  `EngineRead`/`HostAction`/`const GuestMemory`，⛔ 不 include `quickjs.h`。不增加第二个 quickjs TU。
- **R7**：插件受信；没有“overlay 只读”这层；保留的只有重入/生命周期（pause/quit 拒绝、一个 active、
  releaseOwner、预算/超时）。

### 4.9 能力表（唯一真源）与 control 的 9 项

- 核心能力表覆盖：`AfterScript` / `AfterPosition` / `AfterDelay` / `AtReadPoint` / `Frame` / `Move` / `Click` /
  `MouseDown` / `MouseUp` / `GlobalGet` / `GlobalSet` / `Sample` / `Frames` / `Screenshot` /
  `ProfileBegin` / `ProfileEnd` / `Done` / `Fail` / `Quit` / `RunDispatch` / `RunStatus` /
  `RunAbort`（control 仍有 pause/resume/quit）。
- **control 的 9 项不变**：`pause/resume/sample/screenshot/move/click/global-get/global-set/quit`
  （`control_ws.cpp:488-491`）。`Wait` 是 `emu-ctl` 客户端糖（= `resume --for-ms`，`tools/lib/emu-ctl.mjs:799`），
  **不是 `ControlOp`**；`hello`/`caps=`/`endpoint-surface` 精确 9 项。`caps=` 现状 6 项
  （`aot_main.cpp:1560`）是已知漂移，C0 修成 9（readout 修正，guest 行为不变）。
- **mapping 表**（名字不 1:1）：

| 能力 | 文本 recipe 前端 | control op | overlay `.d.ts` | 插件（单运行时） |
|---|---|---|---|---|
| Move | `mouse_move` | `move` | n/a（`game.mouse` 读数） | `ctx.input.move` |
| Click | `click` | `click` | n/a | `ctx.input.click` |
| MouseDown/Up | `mouse_down/up` | n/a | n/a | `ctx.input.down/up` |
| GlobalSet | 若前端支持 | `global-set` | n/a | `ctx.edit.global.set`（立即，EditSink） |
| GlobalGet | n/a | `global-get` | n/a | `ctx.read.global.get`（EngineRead；不暂停） |
| Sample | n/a | `sample` | n/a（overlay 是 `game.*` 子集） | ⛔ 不暴露（emu-ctl 组合） |
| Screenshot | n/a | `screenshot` | n/a | ⛔ 不暴露（emu-ctl 组合） |
| Frames | n/a | n/a | `game.local_frames` | `ctx.read.frames` |
| Wait | `wait`（宿主墙钟） | n/a（客户端糖） | n/a | `ctx.afterDelay`（宿主墙钟；NON-DIFFABLE） |
| AfterPosition | `await position`（停住等） | n/a | n/a | `ctx.trigger.afterPosition`（回调） |
| AfterScript | `await script`（停住等） | n/a | n/a | `ctx.trigger.afterScript`（脚本电平门） |
| AtReadPoint | n/a | n/a | n/a | `ctx.atReadPoint` |
| Profile | `profile_*` | n/a | `host.profileBegin/End` | `ctx.profile.*`（**RunCtx 与 RenderCtx 同形**；S2） |
| Quit | `quit` | `quit` | n/a | `ctx.quit()`（请求宿主停机）/`ctx.finish(code)` |
| AppData | n/a | n/a | n/a | `ctx.appdata.stat`（宿主 I/O；只 appdata 子树、只 RunCtx，⛔ 不是 guest 能力） |
| Config | n/a | n/a | `host.config`（经典回退 HUD；与 `ctx.config` **同一形状**；无桥 ⇒ `undefined`） | `ctx.config.domain(id).get/has/set/delete/all`（域式 JSON 配置，文件 = `<--appdata>/plugin-config.json`；**RunCtx 与 RenderCtx**） |
| RunDispatch | n/a | `run/status/abort` | n/a | `ctx.dispatch` / `ctx.run.*` |

- **`RunStatus.armedNames` / `RunEvent.name`**：当前具名 armed 集合/事件名；只用于可观测性，
  不参与 Edit/EngineRead。
- **`ctx.ui` 不是 ControlBus 能力**：UI 状态是宿主本地、插件内；不进 guest/Edit/EngineRead/存档/输入，
  也没有 WS op；只在 `render` 读、`onTrigger` 写。
- **C0 生成源**：`hello`/`caps=` 从 control yes 的 9 项生成；`Wait` 标 `client-sugar`，不进服务端；
  `OPERATIONS` 可含 `list/gc/wait` 客户端命令。

### 4.10 调试 / 日志

- **JS 侧**：`ctx.log(level, msg)`；有界/去重；一行一条；⛔ 不每读点刷。
- **Timeout**：`trigger:<name>:timeout` 一行；**不自动 finish/退出**；插件 `ctx.finish(5, reason)` 或 re-arm。
- **`RunEvent.type:"start"`** 是伪事件（只用于 trace/观察），不会作为 `onTrigger` 参数。
- **runner trace（opt-in）**：每个 `trigger:<name>`（fired/timeout/done/fail）、`edit:`（applied/reason）、
  `intent:`（Input enqueued/applied/code）、`run:`（dispatch/start/done/fail/abort，含 module/owner/handle）一行；
  默认关，`--plugin-trace` 打开；
  ⛔ 不默认周期性输出（AGENTS §12.8）。
- **文本 recipe**：`InputPlan` 保留自己的 `input-plan:` 摘要 / `plan:` trace / `exit 5`；核心不提供 plan DSL 图元。
- **不需要** `planDumpText`/`stepText` 作为核心读物；C3 parity 只对比旧执行器 trace。

### 4.11 确定性

- `AfterDelay` 用**宿主墙钟**（AOT 强制 `setRealTime(true)`）：**NON-DIFFABLE**，不作为判据；
  ⛔ 不声称 guest 虚拟时钟可复现。
- JS 回调本身若无 `Date`/`Math.random`/外部 IO，则确定；QJS 沙箱已无这些（`overlay-quickjs.md:45-93`）。
- ★ **`ctx.appdata.stat` 是外部 IO**（文件系统状态）；**`ctx.config` 会跨 Run 持久化**
  （`plugin-config.json`，且以后可能被别的进程改）⇒ 带它们的 Run 是 **NON-DIFFABLE**：
  ⛔ 不把配置 / stat 的读数当确定性判据（⛔ 不给它们做"逐位相同"的断言）。
- 文本 recipe 的 `wait` 仍是**宿主墙钟**（C3 兼容），带 `wait` 的 trace 仍 `NON-DIFFABLE`
  （`interaction-model.md:90-103`）。
- control/trigger 的运行期 `global.get/set` 是 `NON-DIFFABLE`；要可复现就写常量/静态断言。
- trace 是 callback 模型的“可读状态”：`trigger:`/`edit:`/`intent:`/`run:` + `ctx.log`；没有 Step 列表 diff。

---

## §5 迁移切片 C0–C5

> 每片小、可独立验收、可回退。**现有 CTest 名**来自 `ctest -N`（§8.1）；
> **〔拟名·新增〕** 是提案，当前不存在。行为按 guest 可见/CLI 输出判。

### 5.0 总表

| 片 | 主题 | 行为 | 依赖 | 何时做 | 回退 |
|---|---|---|---|---|---|
| **C0** | 能力表/接口 + `hello`/`caps=` 同源 | guest 不变；`caps=` 6→9（readout 修正） | 无 | 现在（最小闭环 ①） | 删表/守卫 |
| **C1** | TriggerRunner + Intent/Edit/EngineRead 核心 + **C++ handler** 证明读改写 | 新增核心，未接 CLI/未改 input.h（无用户可见变） | C0 | C0 后（最小闭环 ②） | 删核心/测试 |
| **C2** | QJS 单插件运行时（可选 `render`/`start`/`onTrigger` + `loadSave`）+ headless e2e | 新增 opt-in（`--plugin`） | C1 | C1 后（最小闭环 ③） | 删 flag/适配器 |
| **C3** | JS recipe driver + parity only（历史 plan；`InputPlan` 保留） | **不变**（旧执行器仍生产；JS driver 只对比） | C1/C2 | 可与 C4 并行 | 关 parity；`InputPlan` 不动 |
| **C4** | ControlBus（EditSink/EngineRead/HostAction/control-point）+ `emu-ctl` 组合 | **不变**（9 op/协议/退出码保持） | C1 | 可与 C2/C3 并行 | 回退旧 `control_ws` 分支 |
| **C5** | 单插件 `RunControl`：UI 按钮 → dispatch Run → 执行 → `render` 回显 | **新增 opt-in**（插件 dispatch） | C2/C4 | 可与 C3/C4 并行；建议单列 | 删 dispatch/UI 按钮 |

**S1/S2/B2/Q7/S5 的映射**：
- v3 **Q7/S5** → **C1（C++ handler）+ C2（QJS control）**；不再需要“B1-lite”这个别名。
- v3 **S1（静态 global_set）** → **删除**：plan 不再产出；`global.set` 只在 callback API / ControlBus。
- v3 **S2（QJS 预生成 Step）** → **删除**；文本 plan 由 C3 的 JS driver 消费历史 `Step[]`，不再生成新 plan。
- v3 **B2** → **冻结/历史**；只在 C3 parity 中作为数据。
- **文本 recipe 冻结**：`StepKind`/parser 不再加能力；C3 只验证/支持历史 plan。

### C0 — 能力表 + 接口 + control 9 项同源

**改哪些文件**：

- 新增 `runtime/trigger.h`（`Trigger`/`TriggerEvent`/`TriggerHandler`/`TriggerRunner` 接口）、
  `runtime/intent.h`（`Intent`/`Receipt`/`IntentQueue` 接口）、
  `runtime/engine_read.h`（`EngineRead`）、`runtime/host_action.h`（`HostAction`）、
  `runtime/control_bus.h`（`ControlBus` + `RunnerControl`）、
  `runtime/context.h`（只读 Context）、`runtime/budget.h`、`runtime/capability.h`（能力表）。
- `control_ws.cpp`：`hello.capabilities` 从表生成（9 项，`488-491`）；`aot_main.cpp:1560` 的 `caps=`
  同表生成（6→9，readout 修正）；`control.h:13-21` 注释标“非真源”或一起生成。
- 新增 `tools/test/capability-matrix.test.mjs`〔拟名·新增〕：四上下文/`no:<token>`/`planned:<Qn>`；
  `hello`/`caps=`/`endpoint-surface` 精确 9；`Wait` 是 client-sugar；mapping 表。
- 没有 `InputPlan`/`Step` 改动；C3 仍用旧前端。

**判据**：

- `capability-matrix` 绿；`lift-control-endpoint`（`endpoint-surface`，`tests/control_endpoint.cpp:506`）绿；
  `caps=` 9 项；变异：删 `OPERATIONS` 一项 / `caps=` 一项 ⇒ 红。
- `hello` 9 项、顺序不变；`--help` 文本不改（除 `caps=` 一行）。

**行为**：guest 不变；`caps=` 6→9（已知漂移修复）。

### C1 — TriggerRunner + Intent/Edit/EngineRead 核心 + C++ handler

**做什么**：

- 实现 `TriggerRunner`（具名 armed 集合；`AfterScript/AfterPosition/AfterDelay/AtReadPoint`；Once/Reenter；
  AfterScript；max_fires；timeout；owner；budget）在 `atPoint` 采样后、releaseDue 前 poll。
- 实现 `IntentQueue` + executor（沿用 `armClickAt/pressDue/emitRelease` 的 click 两段/闸门；
  Input 一读点一条 guest 写；trigger Input 进 `staged_next_`，下一读点 drain）。
- 实现 `EngineRead`（`global.get/frames/scriptPos/纯 frame/hash`；无暂停）。
- **C1 不接 `input.h`、不迁 control click、不做 ControlBus/RunControl**；这些留给 C4/C5。
  为了 C++ handler 证明，只提供一个**测试用 EditSink stub**（同步 `global.set`）与 `IntentQueue`；
  ⛔ 不把它接进生产 `WindowManager`/`control_ws`。
- 一个 **C++ handler**（测试夹具）返回具名集合，证明：
  `after position`（脚本/区间）→ `global.get(0xb1b6)` → `global.set(0xb1b6, v|0x10)` → `click` →
  下一个具名集合 → done。headless；不依赖 QJS/recipe。

**改哪些文件**：

- 新增 `runtime/trigger_runner.{h,cpp}`、`runtime/intent_queue.{h,cpp}`、`runtime/engine_read.{h,cpp}`、
  `runtime/edit_sink.h`（接口 + 测试 stub）、`runtime/context.{h,cpp}`、`runtime/budget.h`；
  `input_plan.cpp` 的 click/闸门核心抽到 executor（C1 只抽逻辑，不改投递口）。
- **C1 不动 `input.h` 的四个投递口**、不迁 control `click/move`；C4 才做 `ControlBus`/control-point。
- `WindowManager`/`InputDevice` 仍是唯一 guest 状态（`input_plan.h:11-31` 的铁律不变）。
- 测试夹具：`tests/trigger_runner.cpp`〔拟名·新增〕（CTest `trigger-runner`〔拟名·新增〕，顶层 pure）。

**判据**：

- 〔拟名·新增〕`trigger-runner/after-position-once`：outside→inside 一次；inside 不再；outside 不；
  块边界/Present 零回调；无效位置不改变状态。
- 〔拟名·新增〕`trigger-runner/after-script-once`：只比 script 名（无区间）；Once/Reenter；无效位置不变。
- 〔拟名·新增〕`trigger-runner/named-set`：start 返回多个具名 trigger；一读点最多触发一个（注册顺序）；
  返回新集合替换旧集合（不自动保留）；`[]`=done；重复 name ⇒ fail 且点名；同一对象重新 arm **maxFires 续计**，
  耗尽再返回 ⇒ fail；新对象才是新预算。
- 〔拟名·新增〕`trigger-runner/rearm`：`Once` 触发后再返回同对象 ⇒ fail；`Reenter` 重新 arm 需新的
  有效外→内（不会立刻触发）；`AfterDelay` 计时从头；`AtReadPoint` 从下一读点重新计。
- 〔拟名·新增〕`trigger-runner/global-read-modify-write-click`：假 Engine/池基址/key；
  `read.globalGet b1b6` → **`edit.global.set` 立即** `|0x10` → `input.click`；ENC 往返；
  Edit 立即生效（同触发点 `readGlobal` 得 `0x10`）；Input 一读点一条/click 两段；
  ⛔ Edit 不占 Input 预算。
- 〔拟名·新增〕`trigger-runner/one-write-per-read-point`：release/press 先占预算时，staged 动作等下一点。
- 〔拟名·新增〕`trigger-runner/owner-timeout`：arm 记 deadline；atPoint 采样后/releaseDue 前 poll；
  暂停/headless 不 poll；`elapsed >= timeout` 移除 + TimeoutEvent；per-arm 重置 deadline；
  不自动 finish；fail/done、`releaseOwner` 不残留按住。
- `input-injection` / `lift-control-endpoint` 不回归（C1 未接 CLI/未改投递口）；
  `input-plan-shape.test.mjs` 在 C1 不变（C4 才改 control-point）。

**行为**：新增核心；未接 CLI，无用户可见变化；`InputPlan`/control 仍走旧路径。

### C2 — QJS 单插件运行时（可选 `render`/`start`/`onTrigger` + `loadSave`）

**做什么**：

- `--plugin <id|path>`：**一个** QJS 运行时；ES default `PluginModule`；
  同步、预算、写只经队列；可 headless；`--overlay` 用法不变（`--ui` 已在 S3 删除）。
- `ctx` API：`afterPosition/afterDelay/atReadPoint`、`ctx.input.*`、`ctx.edit.global.set`、
  `ctx.read.*`、`log`、`profile.*`、`done/fail`；底层是 `ControlBus`（Run/dispatch 留给 C5）。
- `loadSave(slot, { expect, timeoutMs, onLoaded })`：真源 `scripts/plugins/lib/save.js`（**纯 JS** 回调链构造器；
  `.BIN` marker 身份；`rowY` 实测表；**终态 = 调用方的 `expect`**），`start/onTrigger` 用闭包接链
  （⛔ 不是 `ctx.run`，C5）；链形状 / 失败面见 §4.6。场景的组合是**另一件工具**
  （`scripts/plugins/lib/chains.js` 的 `route`，见 §4.6a）。
- QJS 适配器：`src/plugin/plugin_host.cpp` 实现 `TriggerHandler`/`ControlHostOps`，只调
  `IntentQueue`/`EngineRead`/`const GuestMemory`；JS trampoline 在 `script_quickjs.cpp`；不 include
  `quickjs.h` 的第二个 TU。

**判据**：

- **C2c 用例**（`ctest --test-dir out -N`）：`plugin-runtime` 的
  `control-script/start-on-trigger-chain{,-field-terminal,-second-confirm,-bad-slot}`（真库 `lib/save.js`，经根入口
  `load-save.js` 驱动；
  用例**显式**给 `expect=FIELD,SC0000`；slot 4/5/7 的链发射不同坐标；`calls=5` / `calls=6` 两条合法终态
  —— 2026-11-21 起 `loadSave` 不等 TITLE（老形状的 6/7 各减一：TITLE 门搬进了 `bootToTitle`，
  参考入口那条 e2e 仍是 6/7）；`bad-slot` 构造 throw；重复 name 由
  `plugin/concurrent-arm-duplicate-name` 钉住），
  `control-script/load-save-onloaded`（`expect` 决定终态 / `SC0000` 不在表里就不收 / `onLoaded` 起场景 /
  `expect` 缺或非法 ⇒ 构造 throw）与 `control-script/chains-route`（`route`：`load:*` ⇒ first、其余 ⇒ then、
  **timeout 也按名字**、`start` 只启 first），以及 `plugin-headless` 二进制内的
  `plugin/load-save-e2e`（根入口 `load-save.js` + `<--appdata>/plugin-config.json` 的域 `save`，
  **不给** `--plugin-param`；判据 = `run=done` + `expect` 生效 + `calls ∈ {6,7}` + 终态日志含 `FIELD` +
  无 `run failed`；⛔ 不看 exit code）。该 e2e 只在 Windows 跑
  （子进程 `CreateProcessW`；mac/Linux skip）—— 本机等价证据是 §4.6 的手工 headless（`calls=7` / `FIELD`）。
- **C2b-2 用例**：`plugin-overlay`（顶层 pure；adopt / 插件 display list 上屏 / 三键电平跟随
  `WindowManager` / 叠层不写 guest / release 恢复 guest 画面）；`aot-overlay` 的子用例
  `plugin-render-window`（`--plugin` 有 render + 窗口档 ⇒ 插件被 adopt，`(0,0)` = 插件填充色）。
  读数（2026-11-20 / macOS arm64）：`ctest --test-dir out -R '^plugin-overlay$' -V` ⇒
  `plugin-overlay: 30 checks, 0 failures`。
- `control-script/global-read-modify-write`：同 C1 场景，QJS 版本；`ctx.read.global.get` 读、
  `ctx.edit.global.set` **立即**写；fire 读点写计数 = 1（Edit 不排队），Input 另计。
- `plugin/headless`：`--present headless` + `--plugin`（无 render）可跑；无窗口/Present。
- `plugin/api-surface`：同一个 `ctx` 有 `render`/`start`/`onTrigger`/`edit`/`input`/`read`/`ui`；
  ⛔ 没有第二套 `--ui-control` 上下文；Edit 立即、Input 队列。
- `plugin/initial-state`：`initialState` 在 `start` 前注入；无 initialState ⇒ `{}`；
  `start` 首次读有值；两 Run 不共享。
- `plugin/ui-state`：`onTrigger` 只 `ctx.ui.show/hide`；**下一次 `render`** 才可见；
  `onTrigger` 不调用 draw；headless 下状态保留到下一次 render/卸载。
- `plugin/ui-draw`：`render` 用 `ctx.draw.rect/text/measure` 发 display list；
  `RenderCtx.ui` 可写 hover 状态；`RunCtx` 没有 draw。
- `plugin/concurrent-arm`：一次 arm `hud:show`/`hud:hide`/`edit:flag`；
  一读点最多触发一个（注册顺序），`ev.name` 分派；其余不自动保留（要保留放返回数组）。
- `plugin/finish-quit`：`ctx.finish(5, reason)` 结束 Run/记录 exit；`ctx.quit()` 请求宿主停机。
- `lift-control-endpoint` / `input-plan-read-points` 不回归。

**行为**：新增 opt-in；不打开 flag 时逐字节不变；headless 可跑。

### C3 — JS recipe driver + parity only（历史 plan 验证；**已落地（main `eecbfac`）**）

**做什么**（以下为**实际落点**，`eecbfac` 已合入 main）：

- **C++ parser 只产数据**：`Recipe/Step`（含 `timeout`/`origin`/line）→ `Step[]`（JSON/ABI），**不执行**；
  `StepKind`/parser **冻结**（⛔ 不加 `global_set`/`global_get`/变量/新指令；⛔ 不做 JS→Step 生成）；
  唯一 JSON 出口 = 独立 TU `src/runtime/recipe_json.{h,cpp}`。
- **JS driver**（`scripts/plugins/lib/recipe.js`，跑在单插件运行时；仅 parity）：`runRecipe(steps)` 用
  `ctx.trigger.*`/`ctx.input.*`/`ctx.read.*` 返回 callback 链；
  覆盖九条指令 + 行尾 `timeout` + `quit` + `profile_*` + `finish(0)/finish(5)`（**只做翻译**）：
  - `await script` → `afterScript`；`await position` → `afterPosition`（Once；`timeout` 作为 trigger timeout）；
  - `wait` → **宿主墙钟**（`afterDelay`）；NON-DIFFABLE；
  - `click/mouse_*` → `ctx.input`；`profile_*` → HostEffect；`quit` → `ctx.finish(剩余 ? 5 : 0, …)` + `ctx.quit(…)`；
  - 门 / 超时 / 完成度的判据仍只有**一份**（`src/runtime/recipe.*`）；消费完 ⇒ `ctx.finish(0)`，
    门超时/失败 ⇒ `ctx.fail` + `ctx.finish(5, …)`。
    ⚠ 合同早先写的 `ctx.done()` 是**笔误**（`plugin.d.ts` 里不存在；消费完就是 `finish(0)`）。
- **parity harness**：官方 `scripts/recipes/*.plan`（逐条 replay）+ `repro:` 锚的**指令行子串**
  （由 `tools/test/recipe-freeze.test.mjs` 逐字钉住，⛔ 不做配方 replay）；旧 `--input-plan`（**生产真源**）vs
  新 JS driver（离线），逐项比对消息（message/wParam/lParam/电平）、`fired/applied/pending`、exit、
  `stepText`（driver arm 的那一步）/trace；**语义等价 + 已知差异表**，不承诺逐位；含 `wait` 的标 `NON-DIFFABLE`。
- **不替换生产执行器**：`InputPlan` 保留 summary/failureReport/fault/trace/exit5，仍是历史行为真源；
  C3 只验证 JS driver 能覆盖什么；不需要 C++ 只读镜、不需要 same-read-point 兼容模式。
- **已知差异 10 条（列出，不声称全覆盖；表 = `tests/recipe_driver_parity.cpp` 的 `kKnownDiffs[]`；
  D 编号属该表、与 §7 的 D1–D8 不是同一套）**：
  - `exit 5`/失败报告是执行器状态机语义，JS driver 只能映射 `ctx.finish(5)` receipt（D8）；
  - `timeout` 边界：新 API 合同是 `elapsed >= timeout_ms`；旧执行器 `stepWindowExpired` 是**严格 `>`**
    （`recipe.cpp:249`），C3 parity driver 为对齐旧行为按 `>`，与新 API 有 **1 ms 差异**（D1）；
    `Step[]` 的 `id`/`origin`/`line`/`label` **全量导出但不参与判定**（D9：以生成的 ES module 进 driver）；
  - **D5** 显式 `mouse_down`/`mouse_up` 在 C1 executor 里**不施加**（`intent_executor.cpp`）⇒ 带 `mouse_*`
    的配方（含 `save05-move.plan`）是 **NON-DIFFABLE**；
  - **D10** `click` 与 `quit` **同一回调** ⇒ 插件路径会丢掉刚 staged 的 input（旧执行器不丢）；
  - `global_set` 不存在于历史 plan（冻结），不补。
- **入口不变**：`--input-plan <file>` 生产仍走旧 `InputPlan`；JS driver 只由 parity 夹具加载；
  parity 通过也不切换生产（用户裁决：`InputPlan` 保留）。
- 官方 `.plan`/`repro:` 锚**不迁移**；旧 plan 不再产出新能力。

**改哪些文件**（已落地，`eecbfac`）：

- `src/runtime/input_plan.*` / `src/runtime/recipe.*`：**一个字符没动**（冻结 parser/执行器；
  summary/failureReport/fault/trace/exit5 不动）。
- `src/runtime/recipe_json.{h,cpp}`（新增·独立 TU）：`Step[]` 的**唯一** JSON 出口（只读 `Step`，不执行）。
- `scripts/plugins/lib/recipe.js`（新增）：`runRecipe(steps)`；九条指令 + 行尾 `timeout` + `quit` + `profile_*`
  + `finish(0)/finish(5)`；**只做翻译**，⛔ 不接生产。
- `tests/recipe_driver_parity.cpp` + `tests/plugin_fixtures/recipe-driver.js`（新增，CTest `recipe-driver-parity`，
  **pure 档**：顶层无条件注册，零 `Machine`/GPU/窗口/安装/LFS）：oracle = `InputPlan::atPoint`；
  官方配方逐条 replay + 已知差异 10 条输出。
- `tools/test/recipe-freeze.test.mjs`（新增）：`StepKind` 闭集合 + 线上名字 = driver `KIND_*`
  + 5 条 `repro:` 的**指令行子串**逐字钉住（⚠ **不是**“replay 配方”）。

**判据**：

- 官方配方 + `repro:` 锚的指令行子串：**语义等价 + 已知差异表**；确定性配方尽量逐项相同，含 `wait` 标
  `NON-DIFFABLE`；不要求“逐位全同”，旧执行器仍真源。
- 任何旧 plan 表达不出来 ⇒ 列入 **已知差异/API 缺口**（⛔ 不给 plan 加指令；⛔ 不宣称全覆盖）。
- 旧 `--input-plan` 的 `exit 5`/`pending`/`trace` 语义**不变**；C3 不切换生产。
- `input-plan-read-points` 的 control 队列子用例迁到 C4 core；C3 只保留 recipe/parity。

**行为**：生产不变（旧 `--input-plan` 保留）；C3 只加离线 parity/报告。

### C4 — ControlBus（EditSink/EngineRead/HostAction/control-point）+ `emu-ctl` 组合

**做什么**：

- 建 `ControlBus` 五个组件：`EditSink`（**生产实现**，立即 global.set）、`EngineRead`、
  `IntentQueue`（C1 已有）、`RunnerControl`、`RunControl`（C1/C2 只留接口；C4 接生产）；
  可选 `HostAction`（screenshot/profile/clipboard）。
- `control_ws.cpp` 不是纯传输：保留 `screenshot` auto-pause/`state_seq`、`resume` `controlOnResume`、
  `click/move` 暂停准入等**控制点语义**；能力逻辑只调 `ControlBus`，⛔ 不再直接 `input::*`/`writeGlobal`。
- 9 op 映射（§4.4a）：`global-set` → **EditSink（立即，暂停也生效）**；`move/click` → IntentQueue；
  `sample` → pause+EngineRead+元数据（组合）；`screenshot` → pause+HostAction（组合）；
  `global-get` → EngineRead（不暂停）；`pause/resume/quit` → RunnerControl。
- 另加 `run/status/abort` → `RunControl`（C5；不是 ControlBus 9 项；`hello.run_capabilities` 3 项，
  `hello.capabilities` 仍 9 项）。
- 把 control 的 click/move 迁到核心 `IntentQueue`，把 `input.h` 投递口从 `InputPlan` 改为 core `IntentDriver`
  （C3/InputPlan 仍可走旧路径，不接生产）。
- 共享契约：同一组 `ControlBus` 契约同时跑 WS 与 JS `ctx`；同一能力只有一处实现。

**判据**：

- `lift-control-endpoint` 全绿（9 op/协议/退出码不变）；`lift-control-globals`/`lift-control-snapshot` 不回归。
- 〔拟名·新增〕`control-bus/edit-immediate`：`global-set` 在**暂停态/触发点**立即生效（同拍 `readGlobal` 得新值）；
  ⛔ 不排进 Input 队列。
- 〔拟名·新增〕`control-bus/input-one-write`：move/click 一读点最多一条；第二条下一读点。
- 〔拟名·新增〕`control-bus/global-get-no-pause`：EngineRead `global-get` 不暂停、无 `state_seq`。
- 〔拟名·新增〕`control-bus/sample-screenshot-composite`：`sample`/`screenshot` 暂停+`state_seq`；screenshot 最后呈现帧。
- 〔拟名·新增〕`control-bus/sample-not-plugin`：插件 `ctx` 没有 `sample/screenshot`（只在 emu-ctl 组合）。
- 〔拟名·新增〕`control-bus/contract`：`global-set`/`global-get`/`click` 在 WS 与 JS 两传输产生同一组核心效果/回执；
  `screenshot` 只在 WS 组合测（插件无 sample/screenshot）；⛔ 不得出现第二套实现。
- 能力表新增/删除 ⇒ `hello`/`caps=`/`--help`/`.d.ts` 同源检查红；`caps=` 9 项；`Wait` client-sugar。

**行为**：协议/退出码/暂停语义保持；`control_ws` 变薄（能力逻辑一份，控制点胶水保留）。

### C5 — 单插件 `RunControl`（UI 按钮派发 Run；生命周期）

**做什么**：

- 同一个插件同时有 `render` 与 `start/onTrigger`（或只做 UI）：`render` 里画按钮，按钮命中（`game.click`）
  → `ctx.dispatch("runs/jump-map.js", { target })`；Run 在**同一个运行时**启动，
  按 `read → edit → input → dispatch` 组合，逐步 move/click；
  `render` 用 `ctx.run.status(handle)` / `ctx.run.events()` 回显状态；完成后清状态。
- 一个 endpoint 同时一个 active：已有 active ⇒ `dispatch`/`emu-ctl run` 返回 **`busy`**，不排队；
  UI 自行重试/提示。`abort(handle)` 由 dispatch owner / 控制层发起，Run `releaseOwner`。
- 派发 = **模块/注册 id + 参数**；模块来自受控根/allowlist；⛔ UI eval 任意源码。
- **为什么单列（不并入 C4）**：C4 是“emu-ctl 组合/HostAction 收编”，C5 是“Run/dispatch e2e”；
  判据不同（C4 比 9 op/EngineRead/sample-screenshot；C5 比 UI→dispatch→Run→状态），单列可独立验收/回退。
- 这是 R7 改写后的正常产品形态：**插件受信**，可以 UI + 写 guest；派发有审计/owner/abort。

**判据**：

- 〔拟名·新增〕`plugin/dispatch-jump-map`（window e2e）：overlay 按钮 → dispatch `runs/jump-map.js`
  → Run `read → edit → input` → 逐步移动 → `render` 显示 running/done；guest 收到预期 move/click 序列。
- 〔拟名·新增〕`plugin/dispatch-busy`：已有 active 时第二次 `dispatch` / `emu-ctl run` ⇒ `busy`，
  ⛔ 不排队、不覆盖第一个 Run。
- 〔拟名·新增〕`plugin/abort-release`：`abort(handle)` ⇒ Run 停止、`releaseOwner`、guest 不残留按住。
- 〔拟名·新增〕`plugin/headless-dispatch`：无 `render` 的插件在 headless 下 dispatch Run 可跑
  （不需要 Present/窗口）。
- 〔拟名·新增〕`plugin/status-events`：`status`/`events` 与 `run:` trace 一致；done/fail/aborted 可达。
- `ui-script-quickjs` / `input-plan-read-points` / `lift-control-endpoint` 不回归。

**行为**：新增 opt-in；没有 dispatch 的旧插件不变；overlay 可经 Run 影响 guest（R7 改写）。

### 5.5 最小闭环

**C0 → C1 → C2**：
- C0：表/接口/9 项 readout；
- C1：TriggerRunner + IntentQueue/executor + C++ handler 证明 `after position → Edit global 读改写 → Input click`
  （不接 `input.h`/不迁 control）；
- C2：单插件运行时 + `render`/`onTrigger` + `edit/input/read` + `loadSave` + headless e2e。

**C3/C4/C5 不在最小闭环**：C3 是历史 plan 的 parity 线（`InputPlan` 保留）；C4 是 ControlBus/control-point 收编线；
C5 是单插件 Run/dispatch 线；三者可与 C2 并行/后置。v3 的 S1/S2 不再作为前置。

---

## §6 守卫

> 判据用 `docs/03-emulator/README.md:35-51` 的三问；只有“变了就是 bug 且静默”才进 test。

| # | 不变量 | 红了意味着什么 | 落点 |
|---|---|---|---|
| 1 | **核心没有 plan 状态**：`src/runtime/core/**` + `input.h` 不出现 `Step`/`InputPlan`/`pending`/`RecipeOutcome`/`next_`（C3 落点 = `src/runtime/recipe_json.*`） | 又长出一个 emulator 内 plan VM（用户点名的成本） | 源码形态守卫 `tools/test/core-shape.test.mjs`（**逐文件白名单**：⛔ 不是目录/glob 规则；C3 落点不在那份白名单里） |
| 2 | **TriggerRunner 只在 `atPoint`**：`aot_run.cpp`/`machine.cpp` 不出现 `TriggerRunner`/`fire(`/`onTrigger` | 第二 pump / 块边界触发；时序静默漂 | 扩展 `input-plan-shape.test.mjs` 禁用词 |
| 3 | **Edit 立即 / Input 下一读点**：Edit 在触发点同步写；Input 进 `staged_next_`，下一读点 drain；⛔ 不让 Edit 排队 | Edit 被延迟（违反“立即”）；Input 抢立即语义 | `control-bus/edit-immediate` + `trigger-runner/one-write` |
| 4 | **Input 一读点一条**（move/click/down/up；staged 也受约束）；Edit 不在这个预算里 | 同拍两条 Input ⇒ 顺序合并；或 Edit 被塞进 Input 队列 | `control-bus/input-one-write` + `trigger-runner/one-write` |
| 5 | **操作类唯一路径**：Input 只经 `IntentQueue`，Edit 只经 `EditSink`，EngineRead 只经 `EngineRead`；sample/screenshot 不进插件 Context；无 mutable `WindowManager`/`GuestMemory` | C++ handler 直接 `wm.mouseButton`/`writeGlobal`，第二 Input 施加点 | `trigger.h`/`context.h` 静态字段解析 + `tools/test/core-shape.test.mjs`（`src/plugin/**` 不直调 `input::`/`writeGlobal`）+ `tools/test/input-plan-shape.test.mjs` 的 `LOOP_FORBIDDEN`（`machine.cpp`/`aot_run.cpp` 里 0 命中 `input_plan`/`input::`） |
| 6 | **after position 语义**：`[lo,hi)`；Once 首次有效 inside；Reenter 有效外→内；无效不改变状态；跳过不触发 | 多/少触发，只有业务现象不对 | `trigger-runner/after-position-*` |
| 7 | **ReadPoint/loop 有界**：`AtReadPoint.max_fires` 必填 >0；`staged_`/队列有界 | 无限灌输入、饿死 plan/测试 | C1/C2 用例 |
| 8 | **owner 清理**：脚本错误/卸载/done/fail ⇒ `releaseOwner`；guest 不残留按住 | 永久按住；运行期无人发现 | `trigger-runner/owner-timeout` + C2 e2e |
| 9 | **单插件运行时**：没有 `--ui-control`/第二个 QJS context；同一个插件可选 `render`/`start`/`onTrigger`；写只经 `ctx.input`/`ctx.edit` | 又拆回“功能/驱动两套上下文/权限”（用户明确否掉） | 静态 flag/上下文守卫 + 插件 API 单测 |
| 10 | **control 9 项同源**：`hello`/`caps=`/`endpoint-surface` 精确 9；`Wait` client-sugar | 加 op 漏一处；C0 行为不一致 | `capability-matrix` + `endpoint-surface` |
| 11 | **QJS 单 TU（编译约束，不是安全）**：`quickjs.h` 只在 `script_quickjs.cpp`；`src/plugin/**` / `src/runtime/input_plan.*`、`recipe.*`、`recipe_json.*` 不 include quickjs.h、不调 `input::`/`writeGlobal` | 第二 quickjs TU / 第二施加点 | `tools/test/core-shape.test.mjs` ④（`src/plugin/**` 的 `PLUGIN_FORBIDDEN`）+ ⑤（`quickjs.h` 的 include 点**恰好**一处、消费者**恰好** `script_quickjs.cpp`） |
| 12 | **预算**：每次 `onTrigger`/`start` deadline（**CPU 钟**）+ op_limit；超限 fail + 摘 trigger | 回调跑飞、guest 卡住；或判据退回墙钟 ⇒ 被抢占的回调又被诬告 | `plugin/callback-cpu-budget`（`op_limit=0` ⇒ 打断只可能来自 CPU deadline，且 reason 点名 `interrupted` + 预算 hint）+ `plugin/callback-wall-only`（墙钟 6 ms、CPU 远 < 2 ms ⇒ **不**失败）+ `plugin/callback-host-pause`（假宿主 5 ms 忙等 ⇒ 扣除后 `js_cpu` ≪ 2 ms）+ `plugin/callback-js-band`（原生大循环仍 `over=js`）+ `trigger-runner/named-set`（宿主安全网仍是墙钟） |
| 13 | **文本 recipe 冻结/parity**：`StepKind`/parser 不加能力；C3 JS driver 覆盖九条+timeout/quit/profile；旧 `--input-plan`（oracle = `InputPlan::atPoint`）vs 新 driver 的 parity | 旧 plan 语义被静默破坏；或又给 plan 加能力 | `recipe-driver-parity`（已落地，pure）+ `tools/test/recipe-freeze.test.mjs` 冻结守卫 |
| 14 | **能力表/映射**：`Move→mouse_move`、`Sample` overlay n/a、`Frames→game.local_frames`、`Wait` client-sugar | 名字漂/“只有某个入口有” | `capability-matrix` |
| 15 | **plan 不再产出**：`StepKind`/parser/help 不新增 `global_*`/变量/指令；`Step[]` 只读数据 | 又给历史 plan 加能力（用户明确不加） | 静态 diff/白名单守卫 + `capability-matrix` |
| 16 | **C3 parity 诚实**：官方配方（逐条 replay）+ `repro:` 锚的**指令行子串**（`recipe-freeze` 逐字钉住）old/new 语义等价 + 已知差异 10 条；缺口必须列出（不声称全覆盖） | 把“JS 没覆盖”藏成“等价”；或声称逐位全同 | `recipe-driver-parity`（**pure**）；覆盖/缺口表 = `kKnownDiffs[]` |
| 17 | **ControlBus 单一能力逻辑**：WS `control_ws.cpp` 与 JS `ctx` 都只调 `EditSink`/`EngineRead`/`IntentQueue`/`RunnerControl`/`RunControl`；sample/screenshot 是组合；control-point 胶水可在 WS | 同一能力实现两次；修一处漏一处 | `control-bus/contract` + 形态守卫（`control_ws.cpp` 不出现 `input::`/`writeGlobal` 直写） |
| 18 | **emu-ctl 9 op 组合/控制点**：pause/resume/quit→RunnerControl；move/click→Intent；sample→pause+EngineRead+元数据；screenshot→pause+HostAction；global-get→**EngineRead（不暂停）**；global-set→**EditSink（立即）**；hello/caps/timeouts/owner 是传输 | 把 global-set 做进 Input 队列；漏 op/控制点语义 | `lift-control-endpoint` + `capability-matrix` + `control-bus/edit-immediate` |
| 19 | **Run 互斥**：一个 endpoint 一个 active Run（Input owner；只读/只 Edit 也唯一）；`dispatch`/`run` 已有 active ⇒ `busy`（不排队）；abort/releaseOwner | 两个 Run 同时写输入；owner 混乱；按键残留 | `plugin/dispatch-busy` / `plugin/abort-release` |
| 20 | **R7 改写落地**：设计/文档不再宣称“overlay 只读”；插件受信，QJS 沙箱只为确定性；WS loopback+token 只是传输访问控制 | 旧口径与新能力冲突；实现被“只读”守卫挡住或绕过 | 文档/形态守卫 + `capability-matrix`（不把“只读”当合同） |

| 21 | **Edit 立即（暂停/控制点）**：`global-set` 在暂停态/触发点同步生效；同拍 `readGlobal` 得新值 | 把 Edit 当 Input 延迟；暂停态不可 edit | `control-bus/edit-immediate` |
| 22 | **无第二 Input 施加点**：Input 只经 `IntentQueue`；EditSink 不是 Input；⛔ 不用 `wm.mouseButton` 直写 | 两条 Input 路径；顺序静默漂 | 形态守卫 + C1/C4 用例 |
| 23 | **`InputPlan` 保留**：`src/runtime/input_plan.*` / `recipe.*` 冻结兼容执行器；C3 只 parity；不退役、不接核心 | 误删历史执行器/exit5/summary；或把 plan VM 塞回核心 | `input-plan-read-points` + 代码形态守卫 |

| 24 | **`global-get` 不暂停**：EngineRead 读；`leave_paused=!paused`；不产生 `state_seq` | 把 global-get 做成 sample 组合，暂停/副作用静默变 | `lift-control-globals` + `control-bus/global-get-no-pause` |
| 25 | **`sample`/`screenshot` 是组合**：pause+EngineRead/HostAction+元数据；paused/state_seq/产物；最后呈现帧 BMP | 把 sample 当普通读；或把 screenshot 暴露给插件 | `lift-control-snapshot` + `control-bus/sample-screenshot-composite` |
| 26 | **sample/screenshot 不进插件回调**：`Context`/QJS `ctx` 没有它们；要 script/pos 用 `ctx.read.scriptPos` | 回调里 sample 冻住 runner；或把组合命令暴露成普通读 | `plugin/api-surface`（无 sample/screenshot）+ 形态守卫 |

| 27 | **UI 只改状态、只在 render 画**：`onTrigger` 不 draw；`ctx.ui` 是宿主本地状态，不进 guest/Edit/EngineRead | 任意回调里随时画；UI 状态混进 guest/存档 | `plugin/ui-state` + 形态守卫（onTrigger 无 draw/guest 写） |

| 28 | **`afterScript` = 纯脚本电平门**：当前 script == name；Once/Reenter；无区间；无效位置不变 | 只能靠 afterPosition 补区间；旧 `await script` 翻译不了 | `trigger-runner/after-script-*` + C3 parity |
| 29 | **`render` 有 draw / ui 写口；onTrigger 不画**：`RenderCtx.draw` + `ctx.ui`；`RunCtx` 无 draw | “render 才画”是空话；HUD 写不出来 | `plugin/ui-draw` + 类型/形态守卫 |
| 30 | **`ctx.finish/quit` 有出口**：finish 结束 Run/exit code；quit 请求宿主停机 | 旧 `quit`/超时失败无法表达 | `plugin/finish-quit` + 类型测试 |

| 31 | **具名唯一**：同一 armed 集合内 name 唯一；重复 ⇒ fail Run 并点名 | 两个同名状态无法区分/静默覆盖 | `trigger-runner/named-set` + C3 parity |
| 32 | **一读点最多一个**：按注册顺序；其余不自动保留（要保留放回返回数组）；不级联 | 同点触发多个；误以为其余自动保留 | `trigger-runner/named-set` + `plugin/concurrent-arm` |
| 33 | **replace/empty/maxFires**：`Trigger[]` 替换旧集合；`[]`/`null`=done；同一对象重新 arm maxFires 续计，耗尽返回=fail；新对象才新预算 | 旧集合残留；空集合永远等；计数被静默重置 | `trigger-runner/named-set` + C3 parity |

| 34 | **模块装载契约**：`@amayui/lift-plugin` 类型；插件 `export default PluginModule`；宿主只读 default，不读 globalThis/命名 | 装载入口分叉；类型与运行时不一致 | `plugin/module-default` + 形态守卫 |
| 35 | **initialState 先于 start**：宿主在 start 前建 `ctx.state`；无 ⇒ `{} as State`；两 Run 不共享 | start 读到 undefined；Run 间串状态 | `plugin/initial-state` |
| 36 | **rearm 语义**：Once 再返回=fail；Reenter 需新外→内；AfterDelay 计时从头；AtReadPoint 下一读点；maxFires 续计 | 立刻重复触发/计时不重置/预算静默重置 | `trigger-runner/rearm` + C3 parity |

| 37 | **Timeout 只读点 poll**：arm 记 deadline；atPoint 采样后/releaseDue 前 poll；暂停/headless 不 poll；不自动 finish | 独立 timer 超时；暂停仍触发；自动 exit | `trigger-runner/owner-timeout` + C3 parity |

| 38 | **`ctx.appdata` 只 appdata 子树 / 只 RunCtx**：绝对路径 / `..` / NUL / 空串 / **非法 UTF-8** ⇒ `invalid_path`；canonical 出根 ⇒ `escape`；`available=false` ⇒ `path=null` + `no_appdata`；`--appdata` 是普通文件 / 落在安装树 ⇒ `available=false`（warn + 继续）；`RenderCtx` 没有它 | 路径穿越静默成功（能读安装树 / 用户目录），或非法 UTF-8 退化成"根目录存在"，或 render 里做任意路径 I/O / guest 写 | `plugin-appdata`（`stat-escape` / `stat-absolute` / `stat-nul` / `stat-invalid-utf8` / `stat-symlink-escape` / `attach-not-a-directory`）+ `plugin/api-surface`（RunCtx 有 `appdata`）+ `plugin/render-surface`（精确八成员） |
| 39 | **`loadSave` 缺 `.DAT` 在第一个 click 之前收口**：缺文件 **或存档目录不存在** ⇒ `finish(5)` + **0 click**；只查 `.DAT`、只 0..99；无 appdata / `escape` / `invalid_path` / `denied` / `io` / slot≥100 ⇒ 跳过 + 一行 warn | 缺文件退回 30 s 门超时（与"点击被吞"混在一起）；或合法读档被误杀 | `control-script/save-missing-precheck`（临时 appdata，`stagedDepth/depth=0`）+ `loadsave-page/save-missing` / `save-dir-missing` / `save-dir-escape` / `save-present` / `no-appdata` + 变异 `if (!st.exists)` |
| 40 | **`ctx.appdata.stat` 的宿主段不计入 2 ms**（`HostPause`；与 `ctx.profile.end` 同一原则） | 网络盘 / 杀软 stat 被记成用户 JS 超预算 ⇒ 误报 `callback budget exceeded` | 实现落点 `script_quickjs.cpp` 的 `jsCtxAppDataStat`；现有预算守卫（`plugin/callback-*`）覆盖同一机制，stat 自身暂无专门用例 |
| 41 | **`ctx.config` 的文件形状 / 值 / 持久化 / 装载期上限**：信封 `version=1` + `domains` 对象 + 域值对象；未知顶层键 / 域内键保留；域自动创建；值只收原生六种（深度 8 / 节点 1024 / 串 8192 / key 128 / 域 256 / 每域 key 1024 / 文件 1 MiB；**set 与 attach 同一套**）；非法 UTF-8 在 set 期 TypeError、装载期 exit 2；`__proto__` / `prototype` / `constructor` 在域 id 与 key 都拒；没有 appdata ⇒ `domain()` 抛 `Error("no_appdata")`；`applied` = 内存已改（只有 `read_only` / `no_appdata` 两个 code）；`set` 只改内存 + **回调末**原子 flush（I/O 失败 warn 一次、后续不重试；序列化/超限失败不熔断）；`HostPause` 含值转换 | 配置文件被静默重写（丢前向兼容键）/ 深层文件在 `get` 递归爆栈而不是 exit 2 / 坏配置被当成"没有配置"继续跑 / `__proto__` 污染原型 / 写盘每 set 一次 / 宿主转换被记进用户 2 ms | `plugin-config` 的 `roundtrip` / `unknown-preserved` / `domain-autocreate` / `bad-json-exit2`（attach 判据）/ `bad-top-level` / `bad-domain-value` / `proto-key` / `limits` / `limits-on-load` / `invalid-utf8` / `no-appdata` / `missing-root` / `document-size` / `read-only-receipt` / `flush-on-callback` / `cross-instance-read` + `plugin/bad-config-exit2`（**进程 exit 2**）+ `control-script/config-roundtrip`（真宿主 + 回调末 flush）+ `plugin/config-host-pause` |
| 42 | **内建插件参数来源 = config > params > 默认**（`has()` 判"给了"，falsy 也算）；没有 config 时**完全回退**到旧 params 行为 | 内建插件仍只认 `--plugin-param`（用户口径的迁移没发生）；或 `on:0` 被当成"没给" | `plugin/load-save-e2e`（⑥ 用 `plugin-config.json`、**不带** `--plugin-param`）+ `control-script/config-roundtrip` + 三个库文件的域表（`scripts/plugins/README.md`） |
| 43 | **S2 `--overlay` = `--plugin hud` 预设**：`hud` 保留 id 先于普通根（exe → `LIFT_UI_SCRIPT_DIR`，exe 赢；ASCII 大小写不敏感）；用户插件根里的 `hud.js` ⇒ exit 2；`--overlay` 仍是窗口参数（headless exit 2）；`--plugin hud` 不隐含开窗；**`--overlay` + 显式 `--plugin` 保持旧形状**（有 render ⇒ adopt 取代；无 render ⇒ hud 照画，⛔ 静默无 HUD）；预设不改互斥（`--overlay --input-plan` 仍 exit 5 / `--plugin hud --input-plan` 仍 exit 2）；`--overlay` 不吃 `--plugin-param`；**`--overlay` / `--plugin hud` / 内部 hud 回退（`--overlay` + 无 render 的插件）三方窗口 BMP + guest dump 逐字节相同**；`RenderCtx` = 八成员（含 `config` 与 `profile.end()` 回 `{ok,text,error}` ⇒ PROFILE 四态）；`measure` 的 `bold` 真参与度量；`game` getter 与经典同口径 | `--overlay` 静默换 HUD（装了另一份 hud.js / 被用户文件遮住 / 入口像素不同 / 无 render 插件让 HUD 消失）；或预设把"要窗口"/互斥语义吃掉；或 PROFILE 少一态 / `measure.bold` 被静默忽略 | `plugin-resolve`（`resolve/hud-*` 含大小写）+ `cli-aot-flags`（`overlay-headless-rejected` / `overlay-plugin-param-rejected`，help 面）+ `aot-overlay`（`overlay-headless-rejected` / `plugin-hud-headless` / `plugin-hud-window` / `plugin-hud-equivalence` / `overlay-plus-plugin-wins` / `overlay-with-run-only-plugin` / `overlay-input-plan-exit5` / `overlay-plugin-hud-input-plan-mutex`）+ `plugin/hud-equivalence`（旧/新 loader display list 逐字段，含 INVALID）+ `plugin/render-surface`（八成员）+ `plugin/render-profile` + `plugin/ui-draw-measure-bold` |

**不进 test（文档/读数）**：JS 回调性能、QJS 内存、`afterDelay` 的墙钟读数、文本 plan 的复用行数、
callback 链的可读性（进 `ctx.log`/设计文档）。

---

## §7 未决问题（D1–D8 + R1–R6）

> v5 的编号；旧的 B2/Q7/S5 已映射到 C1/C2/C3/C4。⛔ 不再有“plan 是否加 global/变量”这类问题
> （答案：不加，plan 冻结）。

### D1：单插件入口 / flag / headless

- **已落地（S1 + S2）**：`--plugin <id|path>` 是**唯一**脚本装载点；`--overlay` = 「内置 `hud`
  （保留 id）+ 要窗口」的**布尔预设**（`--overlay` + **显式** `--plugin` ⇒ 旧形状：有 render 的
  插件取代 hud，无 render 的插件让 hud 照画）；`--ui` 已在 S3 删除（⇒ 未知参数 exit 2）+ `--plugin-param k=v`
  （可重复；重复 key ⇒ exit 2）；**一个** QJS 运行时/context；
  同一个插件可选 `render`/`start`/`onTrigger`（至少一个；`start`/`onTrigger` 必须成对；只有 `render` 也可）；
  没有 `render` 时可 headless；有 `render` 时由**CLI flag** 决定要不要窗口（⛔ 不从模块导出反推）。
  `ctx.params` 只读；`ctx.dispatch` 是任务，不是权限。
  ★ **2026-11-25 定位**：`--plugin-param` 是**夹具 / 诊断口**（形状错误仍 exit 2）；内建插件的具体参数
  走 `<--appdata>/plugin-config.json` 的 `ctx.config`（§4.5），优先级 **config > params > 默认**。
  ⇒ 同一条命令面下，改参数不需要重拼命令行；`--plugin-param` 只在没有 appdata / 做单点诊断时用。
  ★ **未决**：发行打包（R10）；`--plugin hud` 在 headless 只是"允许跑"，不产生
  任何 HUD（D-C2-5 的 warning 一次）。
- **影响面**：一个 loader 查三个可选全局；`quickjs.h` 仍只在 `script_quickjs.cpp`（编译约束，不是安全）；
  `src/plugin` 适配器。
- **删除**：`--ui-control`/“功能 JS vs 驱动 JS 两套上下文/权限”的裁决项（用户明确否掉安全拆分）。

### D2：`onTrigger` 返回形状 + done/fail/timeout（v5.3.1 具名集合）

- **推荐**：`Trigger` 必带 `name`；`start`/`onTrigger` 返回 `Trigger | Trigger[] | null`：
  - `Trigger[]` = **新的 armed 集合**，**替换**旧集合；`null`/`[]` = done；
  - 重复 `name` ⇒ fail Run 并点名；`maxFires` 跨 arm 累计（同一对象续计，耗尽返回=fail；新对象新预算）；
  - **一个 guest 读点最多触发一个**（armed 注册顺序），其余不自动保留（要保留放回返回数组）；
    触发的那个不自动放回。
- **影响面**：runner 从“一个当前 trigger”改为“具名集合”；trace `trigger:<name>`；`RunStatus.armedNames`。
- **若答“数组是追加不是替换”**：会无限增长/优先级不明；不推荐。
- **若答“空数组=永远等”**：Run 无法收口；不推荐。

### D3：`afterDelay` 的时钟

- **推荐**：**宿主墙钟**（AOT 强制 `setRealTime(true)`，`aot_run.cpp:391`）；`afterDelay`/`timeout` 都是
  **NON-DIFFABLE**，不进确定性判据；`pause` 不冻结墙钟，恢复后按下一个读点检查。
  ⛔ 不声称 guest `VirtualClock` 可复现；若将来要虚拟时钟档，必须另立 D2b/判据。
- **影响面**：`afterDelay` 在暂停/阻塞时的行为；C3 `wait` 同口径；trace 标 `NON-DIFFABLE`。
- **若答“显式冻结 clock”**：需新增 clock 档 + 全局冻结语义，成本高；本轮不做。

### D4：历史 recipe 的冻结条件 + parity 判据

- **推荐**：文本 recipe **冻结**（⛔ 不加 `global_set`/变量/新指令；`StepKind`/parser 只服务历史 plan）。
  **`InputPlan` 保留**（不退役）：summary/failureReport/fault/trace/exit5 不动；生产仍走 `--input-plan`。
  C3 = **JS driver + parity only**（离线）：
  - C++ parser 只产 `Step[]`（唯一 JSON 出口 = `src/runtime/recipe_json.*`）；JS `runRecipe(steps)`
    （`scripts/plugins/lib/recipe.js`）返回 callback 链；
  - parity 覆盖 `scripts/recipes/*.plan`（逐条 replay）+ `repro:` 锚的**指令行子串**（由
    `tools/test/recipe-freeze.test.mjs` 逐字钉住，⛔ 不做配方 replay）；old `--input-plan`（真源）vs 新 driver；
  - **语义等价 + 已知差异表**，不承诺逐位；含 `wait` 标 `NON-DIFFABLE`；
  - 已知缺口：`exit5`/`ctx.finish` 映射、timeout 新 API `>=` vs 旧执行器严格 `>` 的 1ms 差异、`Step[]` 字段
    （`id`/`origin`/`line`/`label` 全量导出但不参与判定）；**parity D5** 显式 `mouse_down`/`mouse_up` 在 C1 executor 里
    不施加 ⇒ 带 `mouse_*` 的配方（含 `save05-move.plan`）是 **NON-DIFFABLE**；**parity D10** `click` 与 `quit`
    同一回调 ⇒ 插件路径丢掉刚 staged 的 input（旧执行器不丢）；
    逐条标“旧执行器保留 / JS 部分覆盖 / 不覆盖”，⛔ 不声称全覆盖；
  - 任何旧 plan 表达不出来 = **API 缺口**（列覆盖/缺口表）；⛔ 不用给 plan 加指令绕过。
- **影响面**：历史 plan 得到验证；核心无 plan VM；`InputPlan` 仍是历史真源；`repro:`/官方 `.plan` 不动。
- **若答“让 C3 替换生产”**：需要逐位 parity + 退役条件；本轮不做（用户裁决保留 InputPlan）。

### D5：错误/超时/owner + QJS 预算默认值

- **推荐**：callback 异常/超预算 ⇒ fail + 摘 trigger + `releaseOwner` + 一行 stderr，run 继续；
  trigger 超时 ⇒ timeout 事件（D2）；QJS 预算 2 ms / 100k ops（读数后再调）；
  C3 driver 把 fail 映射成 `exit 5`（历史兼容）；control 只报 receipt，不杀 run。
- ★ **口径（2026-11-21 用户裁决：D5 可改）—— 2 ms 的窗口 = 用户 JS 本体，钟 = 本线程 CPU 时间**：
  deadline 起点 = 组完 ctx / 事件对象、**即将调用插件函数之前**（`script_quickjs.cpp::call()` 的
  `js_start_cpu_ns` 起算），中断回调拿**同一把 CPU 钟**（`pluginThreadCpuNs()`）比 —— ⛔ 不能
  "CPU 起点 + 墙钟比较"（CPU 读数比墙钟小几个数量级 ⇒ 第一次中断就误判）。**模块装载 / ctx 构造 /
  TriggerSet 收集 / 宿主调度不计**；被 OS 抢占 / 等 I/O / 睡眠**也不计**（那是缺陷
  `REQ-01M7VVDV69MJXRHNDGR50H7VHC` 的修法）。
  ★ 用户在 JS 里调**宿主能力**的那段 CPU 同样不计，**本轮只接 `ctx.profile.end`**（2026-11-25，
  缺陷单 `REQ-01M86VRT1YP53CJS0M885FCCJM`）：`HostPause` 累加进 `paused_ns`，`js_cpu_ns` 与中断判据
  饱和扣除；诊断行 `paused_us` 可见。⛔ 不是放宽 2 ms；`log`/`profile.begin`/`edit` 未接。
  ★ **粒度**（2026-11-21 / win32 实测）：POSIX ns 级；Windows 主源 `QueryThreadCycleTime` ≈ ns 级；
  `GetThreadTimes` 是 **15.625 ms 整 tick 记账**（只作兜底：0.5 ms 忙等读到 0 或 156250，
  **判不了 2 ms**）。⇒ 守卫只断言"单调不减"，⛔ 不断言具体数值。
  ★ **钟的形状**：Windows 首次调用用 4 个 ~200 µs 的忙等窗口把 cycles 标定成 ns（取
  max(cycles/wall)，本机 **≈3.97 cycles/ns**），只做一次；`call()` 在计时窗**之前**预热，
  那 ~0.8 ms 不进判据窗。⚠ MSDN 明确 `QueryThreadCycleTime` **不保证精确**（CPU 频率变化 /
  跨核迁移都可能偏）⇒ 它只服务"2 ms 判据"，⛔ 不要当"精确 CPU 时间"用。
  ★ **读数**（2026-11-21 / win32 / `LIFT_PLUGIN_CALLBACK_ELAPSED=1` + `--plugin
  load-save --plugin-param slot=5 --plugin-param expect=FIELD,SC0000`；S1 前写作 `lib/save.js`）：7 个回调的
  `js_cpu_us` = 137 / 70 / 82 / 16 / 137 / 159 / 47（另几轮 134 / 27）—— **全都不是 15625 的倍数**
  （`GetThreadTimes` 的整 tick 记账必然是它的倍数）⇒ 亚 tick CPU 钟的独立佐证；同轮
  `load:confirm2` = `elapsed_us=306`（墙钟）/ `js_cpu_us=159`。再压 6 个忙等进程重跑：
  `load:confirm2 elapsed_us=641` vs `js_cpu_us=140`、`load:row elapsed_us=92` vs `js_cpu_us=27`
  —— 墙钟涨、CPU 不涨 ⇒ **并发不再诬告回调**（缺陷 `REQ-01M7VVDV69MJXRHNDGR50H7VHC` 的修法）。
  **宿主段另有安全网** = `budget.deadline_ns × 10`（2 ms ⇒ 20 ms，`budget.h` 的 `kHostSafetyNetFactor`），
  **仍是墙钟**（`pluginNowNs()`）：超了照样 fail，但 reason 带 `(host)`；用户 JS 超限的 reason 不带。
  ⇒ 两种形态：`<start|onTrigger>: callback budget exceeded[: <name>]` vs
  `<start|onTrigger>: callback budget exceeded (host)[: <name>]`（拼法只有 `callbackBudgetReason()` 一处）。
  缺陷单 `REQ-01M7VVDV69MJXRHNDGR50H7VHC` 按此收口；⛔ 不要把两边的 reason 混成同一条（守卫与 e2e
  靠它判"哪一侧超的"）。
- **影响面**：错误可恢复 vs 终止；owner 释放必须可靠；预算过小会误杀合法回调。
- **若答“fail 即停”**：control 会话会被脚本错误杀掉；不建议（overlay 的 `disableOverlay` 是另一档）。

### D6：`emu-ctl` 的合并 / 传输（保留 WS vs 本地 RPC）

- **推荐**：**一个 `ControlBus`**（`EditSink` + `EngineRead` + `IntentQueue` + `RunnerControl` + `RunControl`；可选 `HostAction`）；
  `control_ws.cpp` 保留**控制点语义**（screenshot auto-pause/`state_seq`、`resume` `controlOnResume`、
  click/move 暂停准入）与协议/时序胶水；能力逻辑只调 ControlBus。`emu-ctl` 只是客户端；
  插件是同一 bus 的进程内绑定。
  - 保留 WS：已有实现/DSH/独立进程；适配器薄化，最小改动；
  - 本地 RPC / 仅本地：省不下核心逻辑，只换传输；会失去远程 inspect 或重写客户端；
  - 权限：WS loopback+token 是传输访问控制；插件进程内受信；核心只认 owner/一个 active Run。
- **影响面**：`control_ws` 保留控制点胶水；9 op/协议/退出码不变；同一能力逻辑只有一处；
  `hello`/`caps=`/`--help`/`.d.ts` 同源检查。
- **若答“换传输”**：核心不变，只重写 adapter/client；成本在传输层，不在能力层。

### D7：`run/dispatch` 的形态（plan vs js/module id、源码 vs 参数、busy/排队）

- **推荐**：`dispatch(module, params)` / `emu-ctl run <module> [params]`；module = 注册 id/受控根内路径；
  ⛔ 不接受任意源码/eval；`--plan` **不是** Run 输入（历史 plan 仍走 `--input-plan`/C3 parity）；
  Run 只接受 module id + params。已有 active ⇒ `busy`，**不排队**（避免优先级/owner 复杂化）。
- **影响面**：模块注册/allowlist、参数形态、busy 回执；`run` 的退出码/错误面。
- **若答“要源码/排队”**：需要新的安全/审计边界 + 队列优先级/owner 规则，成本高。

### D8：Run 事件/状态 + 插件 dispatch 与 `emu-ctl run` 的 owner

- **推荐**：`ctx.run.status(handle)`（含 `armedNames`）+ 有界 `events(since?)`（running/done/failed/aborted，
  事件带 `name`）；`render` 里轮询；
  `dispatch` owner = 调用插件/客户端；`abort` 只允许 owner/控制层；一个 active slot；
  插件 dispatch 与 `emu-ctl run` 互斥（busy）；Run 结束/abort ⇒ `releaseOwner`。
- **影响面**：status/events/armedNames 形状、owner 生命周期、busy 提示；需要 e2e（C5）。
- **若答“要共享/排队多个 Run”**：需要多 active/优先级/每 Run owner，C1/C5 要扩展。

### R1–R6：review 漏的 6 条补充裁决

- **R1 入口互斥**：`--plugin`（插件）/`--input-plan`（历史执行器）/`emu-ctl run`（Run）**最多一个 Input owner**；
  当前 `--plugin` 与 `--input-plan` **启动即互斥**（`exit 2`），插件 dispatch 与 `emu-ctl run` 也互斥（busy）。
- **R2 `--ui-control` + overlay 并存**：v5 已删 `--ui-control`；只有 `--plugin` / `--overlay`，一个插件可
  render+Run；不存在“两上下文并存”问题。
- **R3 输出面归属**：guest stdout 归 run；`ctx.log`/trace/插件失败摘要走 stderr；`InputPlan` 保留 exit5；
  `emu-ctl`/RPC 回执是 JSON；插件失败默认不改进程 exit（除非 C3 parity/显式策略）。
- **R4 `Step[]` 版本化**：`Step[]` 带 `version`（int）；parser 写入，JS driver 拒绝未知版本；
  字段增删走版本，不静默改历史字段。
- **R5 parity 运行方式**：`recipe-driver-parity` 标 **pure**（顶层无条件注册；零 `Machine`/GPU/真窗口/
  游戏安装/LFS 资产 ⇒ 与 `plugin-runtime`/`plugin-overlay` 同档）；官方 `.plan` 逐条 replay 用的是
  **注入时钟 + 无 guest 的 parity 夹具**；`repro:` 锚只钉**指令行子串**（`recipe-freeze` 逐字），不做配方
  replay；旧执行器是真源，JS driver 只对比。
- **R6 `global.set` 时机**：**EditSink 立即**（安全点/暂停/控制点）同步生效；⛔ 不进 IntentQueue；
  `emu-ctl global-set` 与插件 `ctx.edit.global.set` 同一实现；Input 另走读点预算。

## §8 参考落点

### 8.1 代码

| 主题 | 文件:行 |
|---|---|
| `--input-plan` 帮助/载入 | `apps/emulator-lift/src/aot/aot_main.cpp:105-146`、`420-421`、`843-848` |
| 控制层 9 op / hello / serve | `apps/emulator-lift/src/runtime/control_ws.cpp:488-491`、`877-886`、`1192-1519` |
| 启动 `caps=`（现状 6） | `apps/emulator-lift/src/aot/aot_main.cpp:1326-1331` |
| 旧 plan VM（C3 前端） | `apps/emulator-lift/src/runtime/input_plan.cpp:186-217`、`781-810`；`input_plan.h:233-283`、`430-458` |
| 旧 recipe | `apps/emulator-lift/src/runtime/recipe.h:41-55`、`70-81`；`recipe.cpp:318-503` |
| C3 `Step[]` 唯一 JSON 出口 | `apps/emulator-lift/src/runtime/recipe_json.h:1-44`；`recipe_json.cpp:1-93`（独立 TU，只读 `Step`） |
| C3 JS driver / parity / 冻结守卫 | `apps/emulator-lift/scripts/plugins/lib/recipe.js:126-293`（`runRecipe`）；`apps/emulator-lift/tests/recipe_driver_parity.cpp:606-635`（`kKnownDiffs[]`）；`apps/emulator-lift/tests/plugin_fixtures/recipe-driver.js`；`tools/test/recipe-freeze.test.mjs` |
| click 两段/闸门 | `apps/emulator-lift/src/runtime/input_plan.h:249-283`、`392-417` |
| 四个投递入口 | `apps/emulator-lift/src/runtime/input.h:80-114` |
| 块边界 / Present 闸门 | `apps/emulator-lift/src/aot/aot_run.cpp:595-609`；`apps/emulator-lift/src/aot/aot_host.cpp:1184-1198` |
| 快照 / global | `apps/emulator-lift/src/runtime/control_snapshot.h:39-66`；`script_globals.h:86-90` |
| QJS 宿主 API / loader | `apps/emulator-lift/src/ui/script_quickjs.cpp:886-1063`、`1223-1287`、`1412-1459` |
| QJS 叠层 / Present | `apps/emulator-lift/src/runtime/present_sdl.cpp:596-691`、`1240-1250` |
| 插件 render adopt / release / 字体（C2b-2） | `apps/emulator-lift/src/runtime/present_sdl.h:208-221`；`present_sdl.cpp:528-582` |
| 窗口档 ⑤' 装配（adopt / `exit 2` / 取代语） | `apps/emulator-lift/src/aot/aot_main.cpp:1395-1429` |
| `game.mouse().buttons` 装配 / 三键唯一源 | `apps/emulator-lift/src/ui/script_quickjs.cpp:652-690`、`:3227`；`apps/emulator-lift/src/plugin/plugin_host.cpp:104-108`；`apps/emulator-lift/src/runtime/present_sdl.cpp:612-625`、`host_window.h:181-184`、`input_device.h:84-90` |
| QJS 预算/interrupt（含宿主暂停） | `apps/emulator-lift/src/ui/script_quickjs.cpp:107-113`、`197-212`、`253-285`、`317-329`、`1392-1395`、`3387-3390`、`3438-3447` |
| C2c `loadSave` 库 / 参考入口 | `apps/emulator-lift/scripts/plugins/lib/save.js`（`loadSave` / `SAVE_DIR_REL` / 前置检查 / `expect`）；参考入口在**根** `apps/emulator-lift/scripts/plugins/load-save.js`（id = `load-save`） |
| A 阶段 `chains.js`（通用路由） / 可跑样例 | `apps/emulator-lift/scripts/plugins/lib/chains.js`（`route`）；`apps/emulator-lift/scripts/plugins/scenario-save07-battle.js` |
| `.BIN` 精确相等（C1） vs recipe 剥后缀 | `apps/emulator-lift/src/runtime/trigger_runner.cpp:70,82`、`recipe.cpp:141-153` |
| 帧列表 | `apps/emulator-lift/src/runtime/engine_frames.cpp:81` |
| emu-ctl 命令表 | `tools/lib/emu-ctl.mjs:785-810`；`tools/emu-ctl.mjs:1-20` |

**已核 CTest 名**（`ctest --test-dir out -N`）：
`cli-aot-flags`、`lift-control-endpoint`、`lift-control-snapshot`、`lift-control-globals`、
`input-plan-read-points`、`input-injection`、`overlay`、`aot-overlay`、`ui-script-quickjs`、
`ui-frames`、`ui-script-modules`、`ui-hud-script`、`ui-kit-script`、`plugin-runtime`、
`plugin-headless`、`plugin-overlay`。

`recipe-driver-parity`（C3，**pure**）由 `apps/emulator-lift/CMakeLists.txt` 顶层 `add_test` 注册
（外层 `if(LIFT_BUILD_TESTS)` 被 `cmakeTestTier()` 当透明）。
〔拟名·新增〕在 §5 标出，当前不存在。

### 8.2 文档

- `docs/03-emulator/input-plan.md:20-312`（读点/门/完成度）、`:343-356`（守卫）
- `docs/03-emulator/control-plane.md:60-136`（协议/边界/未做）、`:147-204`（global）
- `docs/03-emulator/interaction-model.md:15-56`（顺序模型）、`:90-103`（确定性）、`:120-130`（缺什么）
- `docs/03-emulator/input-device.md:219-239`（四驱动者）、`:265-291`（推进时机）
- `docs/03-emulator/overlay.md:54-70`（D7/D10）、`:124-162`（API）、`:233-279`（M2a/M3）、`:287-323`（测试/风险）
- `docs/03-emulator/overlay-quickjs.md:45-112`（沙箱/预算/错误）、`:130-153`（load/Promise）
- `docs/03-emulator/README.md:35-51`（什么进 test 的三问）
- `apps/emulator-lift/scripts/recipes/README.md:14-28`（repro 锚/逐字不许改）
- `docs/03-emulator/scripts/SAVE.md:52,112-121`、`FELLOW.md:50`、`SN0000.md:47`（前缀复制/槽位行）

### 8.3 需求台账

- `REQ-01M7JHSFB2SSBBNSJ82YDED8M1`（工具/测试/文档口径；`doing`）
- `REQ-01M7HC1WJ9X0QCQ51Q9F01988W`（交互式控制层；`open`）
- `REQ-01M7QYCYA8XBG7G78SZBS3QGG5`（typed global 原语；`done`）
- `REQ-01M7J97QY66KMP5SBPCTR0S3TP`（`await global/local`；`open`）
- `REQ-01M7M7C7JFYHQ587TP75HVQZQN`（控制层读 local/global 槽；`open`）
- `REQ-01M7SB9R0BAYYY4H1VSHZA4GAJ`（输入注入重设计；`done`）
- `REQ-01M7S5N4VG3GZXDQH1VY8FMS9Y`（配方鼠标原语；`done`）
- `REQ-01M7HP2Q1N0N004M8R4RPABWSV`（语法收敛 ADR；`done`）
- `REQ-01M7QF01A00F9946DX0GPH23JQ`（overlay PROFILE；`doing`）
- `REQ-01M7PE0D9MJA4REXQ1YJFVPHGF`（UI 插件宿主；`doing`；M3 计划有 global/local 只读）

### 8.4 `.tmp` 证据（只读）

- `.tmp/deleteobject-profile/report.md:104-126`：recipe 无 global-set、pause 冻 plan、drag 表达不了。
- `.tmp/deleteobject-profile/recon.plan:2-9`、`.tmp/save06-blue-mask/save06-blue.plan:5-12`：
  同一加载前缀、只差槽位 y。
- `.tmp/control-unify/review.md` / `review-v2.md`：v1/v2 复审；v4 只吸收仍适用的结论。
- `docs/03-emulator/scripts/{SAVE,FELLOW,SN0000}.md` 的“共用前缀”段：文档层的手工复用。

### 8.5 未提交 diff

以 `git diff --numstat` 为准（不在此固化数字）：当前是 `d3d9.cpp`/`d3d9_pipeline.{cpp,h}`/
`d3dx9_sprite.cpp`/`shims/d3dx9.cpp` 的 save06 临时诊断改动，不属于本报告证据面；本报告未引用这些文件。
复核 D3D/present 结论用 `git show HEAD:<path>`，⛔ 不用工作树临时改动。
