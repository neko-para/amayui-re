# 现状：读点异步触发 vs 指令点同步钩子（能力/性能/渲染/缺陷）

- id: REQ-01M8EKFHDFX7DP4FCWVA7MPQET
- type: req
- status: done
- parent: REQ-01M8EKFAP4QJY9G12K7KB9YEQB
- done_reason: 现状与评估已完成；结论并入父单与 API/TS 子单，不再单独跟踪实现。
- tags: [emulator-lift, plugin, qjs, analysis]

## 目的

记录现状与评估，作为 v2 设计的事实底稿；结论已定，不再单独跟踪实现。

## 异步触发（TriggerRunner）

- 触发点 = 11 个 `InputPoint`（`input_point.h`）：KeyLevel / CursorPos / PeekMsg / GetMsg / MsgWait / WaitMessage / QueueStatus / Translate / Dispatch / SetCursorPos / FileRead；9 个是语义输入读点，`SetCursorPos` 是写点、`FileRead` 是事件源。
- 4 个 inline 投递口（`input.h`）在控制层 `atPoint` 之后调 `PluginHost::onReadPoint`；非 owner 线程早退。
- `AtReadPoint` 语义 = 下一个匹配投递点；`maxFires` 必填；一读点最多一个事件；fire 优先 timeout；回调里 `ctx.input` 入队，下一读点才落地。
- 位置触发被读点量化：`AfterPosition` 只在一个恰好落在区间内的读点求值；区间里没有读点就永不触发。`field-point-visible` 的 `DRAWOBJ` 入口实际靠体内 `peekMsg` 读点才 fire。
- 读数是引擎帧记账（`cur`/`frame[cur]`），不是“正在执行哪条指令”；`cur` 有瞬态窗口。真实样本：`point=fileRead script=$5$AUTORUN.BIN posValid=false`。
- 预算：2 ms 用户 JS CPU + 20 ms 宿主墙钟安全网；一读点一事件；返回数组 = 完整替换 armed 集合。

## 同步触发（instr_hook）

- 入口 = 3 个脚本派发 EA（`aot_hook_ea.h` 的 `kScriptDispatchEas`），每条脚本指令执行前一次；不是每条 x86 指令。
- 能力 = `at`（纯观察）/ `capture`（读 local）/ `patch`（指令前覆盖、下一条派发前恢复）；没有 JS 回调。
- `patch` 的“净变化 0”有前提：恢复是无条件写回 `pending.raw`，窗口内同地址写会被吞；窗口止于下一次脚本指令派发，shim/等待期间一直有效。
- 缺省只读一个 relaxed atomic；激活后每拍约 10 次 guest 读 + entries 线性扫。
- `--no-s7` 下注入前就 `ret`，`at()` 仍成功但永不命中；hook 零 CTest、零需求单。

## 性能读数

- 1 条永不命中 hook：+0.62%（95% CI −0.1%…+1.3%），≈23 ns/脚本指令（上界）；8 条测不出。
- 病态注册数：2048 条 +30%、4096 条 +48%（3 帧档）；线性扫 ≈0.37 ns/entry/probe。
- 频率：脚本指令 : 输入读点 = 2.35 : 1（重段）/ 3.2 : 1（boot）；换触发点只放大 2–3 倍。
- 今天读点 `sampleNow` ≈160 ns/读点；完整采样搬到每个指令点 = +3.0%（必须拦）；cheap key 6–8 ns ⇒ ≈0.6%，比今天更便宜。
- JS 空回调 ≈5 µs（其中 ~5 µs 是每次重建 ctx）；每帧 1/100/1000 次 = 5 µs / 0.5 ms / 5 ms；顶满 2 ms 预算 = 2 ms / 200 ms / 2 s。

## 渲染 vs 逻辑

- `render` 是独立调用栈：guest `Present` → `present_sdl` `drawOverlay` → `presentUi` → `script.render`；帧边界 = guest Present；headless 下 render 0 次。
- `ctx.draw` 的硬门是 `builder == nullptr`；指令点没有 `UiBuilder`/display list/可画 surface。
- `ui` 是渲染侧状态：逻辑只改数据，render 读数据再 `rctx.draw`/`rctx.ui`；不应在 tick 里改 UI。
- `config.get` 是内存读 ≈0.2 µs；`appdata.stat` 是同步 I/O，NON-DIFFABLE，热路径慎用。

## 今日静默缺陷

1. `ctx.fail` / 预算超限 / `start` 返回 `[]` 不 `instrHookClear()` ⇒ Run 结束后 hook 继续改 guest。
2. `patch` 恢复无条件写回 ⇒ 窗口内同地址 `ctx.edit`/引擎写被静默抹掉。
3. `--no-s7` 下 `ctx.hook.at()` 静默永不命中。
4. render 期 `ctx.hook.*` 只判 `callHost` ⇒ 存下来的 RunCtx 能在 render 里改 hook。

## 未证

- FIELD 稳态 / win32 的 probe 税与 JS 回调成本；`sampleNow` 160 ns 的原始命令；feeder 线程是否命中脚本派发点；3 个派发点覆盖 100% 脚本指令的残余。
