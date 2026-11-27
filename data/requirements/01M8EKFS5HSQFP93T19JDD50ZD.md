# 新 JS API：RunCtx/Tick · at/once/task · timeout/日志/UI 分层

- id: REQ-01M8EKFS5HSQFP93T19JDD50ZD
- type: req
- status: open
- parent: REQ-01M8EKFAP4QJY9G12K7KB9YEQB
- tags: [emulator-lift, plugin, qjs, api, design]

## 目标

把插件 API 统一成“VM 指令点 + 可组合 async task”，去掉字符串状态机；类型上用 `RunCtx`/`Tick` 拆分，让 TS 能检查“位置是否有效”。

## 类型

- `RunCtx`：per-Run、长寿命。`at`/`once`/`task`/`off`/`finish`/`quit`/`fail` + `config`/`appdata`；**没有** `ui`、`now`、`local`。
- `Tick`：只在某个入口有效。`now`（script/pos/carrier）、`local`（int/ptr/ptrDword）、`edit`、`input`、`patch`。
- `Tick` 只能来自 `at` 回调参数或 `once` 的 resolve；`RunCtx` 上误写 `ctx.local` 应被 TS 拒绝。
- `RenderCtx`：`draw`/`ui`/`config`/`game`；`ui` 只在这里。
- `log`：独立全局（info/warn/error/debug），timeout/render/tick 都可用的。

## 触发与任务

- `ctx.at(where, (tick, next) => {...})`：同步订阅；持久；调 `next()` 继续链，不调即截断；不需要 `stopPropagation`。
- `ctx.once(where, { timeoutMs? })`：async 等待；返回 `Promise<Tick>`；命中节点时 resolve。
- `ctx.task(async (ctx) => {...})`：协程；`start(ctx)` 同步只做注册，不写 async start。
- `where`：`{script,pos}` / `{script,lo,hi}` / `{script}` / `{opcode}`；输入触发在 AGE input opcode，不用 Win32 读点。
- task 组合 = `await` / `Promise.all` / `Promise.race`；不需要每件事都新开 Run。

## 事件与投递

- 同步链：同一节点多个 handler 按注册顺序，由前一个 `next()` 决定是否继续；不调即截断。
- async task：独立协程，不参与 `next`；同一节点多个 task 按注册顺序 resume，都看到同一节点的 `ctx`。
- 每个 host→JS 入口 **drain 到静止**：`await Promise.resolve()` 的 continuation 在**当前入口**跑完，不会漏到下一个节点。
- `await ctx.once(B)` 的 continuation 在 B 命中时 resume；`ctx` 查询 B；不需要 local 快照。
- 输出：`edit` 立即持久写；`patch` 瞬时覆盖、下一 tick 恢复（CAS）；`input` 入队、下一读点落地；`config` 标脏、tick/帧末 flush；`ui` 只在 render。

## timeout / ctx 绑定

- `once` 到期 ⇒ reject；catch 在固定的非 tick 点 drain。
- timeout 时 `ctx` 为 null / 无当前 `Tick`：不能做位置查询或 guest 写；`log` 全局可用；宿主记录 timeout 并 fail Run。
- `ctx` 是“当前入口”的绑定：入口时指向稳定 `__ctx`，drain 完置 null；present/render 也用独立的 `rctx`。
- 捕获的旧 `Tick` 跨入口使用由 generation/epoch 抛 stale；不做 Proxy/对抗。
- 不做逻辑 timer；固定时间触发不知道 guest 位置。

## local / Run 生命周期

- `ctx.local`：int `0..count-1`、ptr `0..count-1`、ptr 指向的 dword；**没有**栈/参数区；无效返回 `undefined`（真 0 返回 0）；ENC/DEC；回调返回后 stale。
- Run done = 所有 task settled + 没有 sync 订阅；`finish`/`quit`/`fail` 显式覆盖。
- Run 互斥：同一 endpoint 只有一个 active Run；新 dispatch 直接 busy，不排队；不允许嵌套 Run。
- 业务插件内部复用逻辑用 task 组合，不新开 Run；v1 约定一个 Run 内只有一个 input-driving task，input token 暂不做。
- 不做 handoff：要切到另一个 Run，当前 Run `finish`，由 UI/控制层 dispatch 下一个。
- 不依赖 `ctx.state`：逻辑数据用模块变量或 per-Run 数据对象；`ctx` 只做调度/查询。

## 配置 / UI

- `config` 可直接 `get`（≈0.2 µs/次），不强制缓存；每帧读 + cache 是热路径优化，不是 API 要求。
- 设置页 onclick 可直接 `set`；外部改文件最迟下一帧生效。
- 逻辑只改数据；`render(rctx)` 读数据并 `rctx.draw`/`rctx.ui`；tick 不碰 UI。

## TS

- `RunCtx`/`Tick`/`RenderCtx` 作为 `@amayui/lift-plugin` 类型；插件源码用 TS，`tsc --noEmit --erasableSyntaxOnly` 门禁。
- 不用 nullable 全局 `ctx`；TS 走参数（`start(ctx)`、`task(async (ctx) =>)`、`at((tick) =>)`、`render(rctx)`）。

## 待定

- timeout 检测点（系统定时器/present/读点）；input token 是否做；`Tick` 越界访问抛错还是 undefined；是否保留全局 `ctx` 便利。
