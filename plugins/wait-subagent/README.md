# plugins/wait-subagent —— 事件驱动的 `wait_subagent` 工具

> 一句话：把“等子代理”从 `bash sleep <猜的秒数>` 换成**等事件** —— 子代理结束、用户插话、
> turn 取消、或可选的安全上限，哪个先到就返回哪个。**不轮询、不猜时长**。

## 1. 它解决什么

`subagent` / `subagent_fork` 工具只返回 `started subagent <id>`，子代理的结果通过
**settlement notice** 异步投递。但模型有时必须在本 turn 内等到它结束才能继续
（例如要根据子代理的结论决定下一步）。现在只能：

```bash
sleep 30   # 短了再睡，长了白等；用户插话也打断不了
```

`wait_subagent` 订阅 DSH 的真实生命周期事件：

| 唤醒源 | DSH 事件 / 信号 | 说明 |
|---|---|---|
| 子代理的 activation 结束 | `subagent/end`（scope 派发到 delegating parent） | managed activation 的**唯一终态边**；清理与通知之后发出 |
| 用户插话 | `agent/inbox/inserted` 且 `message.source.kind === 'user'` | Web 的 queue / steer 都走这里；子代理完成通知是 `subagent-settled`，不会误判 |
| turn 被取消 | `exec.signal`（`abort`） | 用户点 Stop / 上层取消 |
| 可选安全上限 | `timeout_seconds` | 缺省不设；只用于“我也想偶尔查一下”的场景 |

## 2. 返回什么

工具返回一个固定对象（模型看到的是 `note` 的一段可执行文字）：

| `status` | 什么时候 | 模型应该做什么 |
|---|---|---|
| `finished` | 等到了 `subagent/end` | 继续下一步；**不要再 sleep / 再等同一个 child** |
| `already_finished` | 调用时 activation 已经结束 | 不要再等；结果在 completion notice 里 |
| `user_interjected` | 用户 queue / steer 了消息 | `delivery=steer` ⇒ 下一步处理；`delivery=queue` ⇒ 工具已 `concludeTurn()`，loop 会关掉本 turn 让消息跑 |
| `not_running` | 没有 live activation（本地已 idle / 外部已结束 / 插件重启前就结束） | 已结束就看 completion notice；idle 的 continuable child 要先用 `send_message` 唤醒 |
| `timed_out` | 给了 `timeout_seconds` 且到点 | 不要轮询；结束 turn 依赖完成通知，或只再等一次 |
| `cancelled` | turn 取消 | 无需处理 |

★ **最终输出不由本工具投递**：子代理的 closing message / structured result / diagnostic 仍由
runtime 的 settlement notice（`source.kind='subagent-settled'`）送达。本工具只回答
“结束了没有、为什么结束”，不复制、不改写结果 —— 否则同一个结果会在上下文里出现两份。

★ **queue 的“自动”发生在工具侧**：`delivery=queue` 时 `execute` 调 `exec.concludeTurn()`，
loop 在本 tool group 提交后关闭 turn，不需要模型“记得”结束；`steer` 不标 `concludesTurn`，
消息在下一步进入上下文，模型照常回复。

## 3. 为什么这样实现（文档 + 源码依据）

先读文档，再读源码；关键结论：

* `@deepseek-ai/dsh-tool-subagent` 的 README 明确 **“This tool does not wait for results”**；
  programmatic consumers 用 subagent service。
* `@deepseek-ai/dsh-subagent` 的 README 只公开了 `waitForChildren(parent)`（等**所有**后代），
  **没有** `waitForActivation(childId)` 这种按 child 等的 API。按 child 等只能观察生命周期事件。
* `subagent/lib/index.js` 的 `createLifecycleEmitter()`：`subagent/start` / `subagent/end` 用
  `scopeTarget(parent)` 派发；`finishDisposal()` 在 `notifySettlement()`、结果 resolve 之前
  `observer.settle(terminal)` ⇒ **`subagent/end` 是可靠的终态边**。
* `dsh-agent` 的 README + `lib/index.js`：`agent/inbox/inserted { message }` 在每次插入时同步派发；
  人消息的 `source.kind === 'user'`（`user-rpc` 也归并成这个 kind）。
* `dsh-scope` 的 README：`agent.ctx` 的 scope listener 收到 **tag 为 dispatch key 或其祖先**的事件
  ⇒ 用 `exec.agent.ctx.on(...)` 正好只收这个 agent 的 `subagent/end` 与 `agent/inbox/inserted`。
* `dsh-tools` 的 README：tool body 必须观察 `exec.signal`；取消是协作式的。

## 4. 竞态与边界（这是实现的重点）

**注册顺序是契约**（`lib/wait.mjs` 的 `waitForSubagent`）：

1. 先挂 `subagent/end`、`agent/inbox/inserted`、`abort` 监听；
2. 再复查 inbox 里已经排队的人消息；
3. 再复查 tracker（补上“挂监听前刚好结束”的窗口）；
4. 最后才设 timeout / 进入 await。

边界：

* **只等直接子代理**：用 parent-owned `subagent/catalog` 验证 child 身份；不是直接子代理就报错，
  不去等一个陌生 session。
* **已结束不挂等待**：tracker 保留 `settled` 状态，正是为了回答“调用时它是不是早就结束了”。
* **旧 activation 迟到的 end 不覆盖新 run**：tracker 按 `runId` 比对；cold resume 新 activation
  会覆盖成 `running`。
* **不把子代理完成通知当成人插话**：只认 `source.kind === 'user'`。
* **不轮询**：没有 `setInterval`、没有 sleep；只监听事件 / abort / 可选 timer。
* **parked inbox 不算结束**：child 已经 idle、但 inbox 里还有 pending 消息时，activation 仍是
  resident，`subagent/end` 不会来；这种“等一个不会自己结束的 child”要用 `timeout_seconds` 给自己边界。
* **外部 child**：没有 local Agent，但同样有 `subagent/start` / `subagent/end`；start 在
  delegation 工具返回前发出 ⇒ tracker 正常能看到。插件重启前就结束的外部 child 会返回
  `not_running`，而不是永久等待一个已经过去的终态边。

## 5. 装（环境级）

```bash
# ① 装（写 $DSH_HOME ⇒ 受限沙箱里要提权；这是人做的一步）
dsh plugin --profile web install "<仓库绝对路径>/plugins/wait-subagent"

# ② 仓库内契约测试（不依赖 DSH 宿主进程）
node --test --test-isolation=none plugins/wait-subagent/test/*.test.mjs
# 或只跑这个包：
pnpm --filter @amayui/dsh-plugin-wait-subagent test

# ③ 真正的验证：注册后由会话调一次 wait_subagent。
#    宿主插件代码不热重载 ⇒ 装完/改完 .mjs 要重启 DSH。
```

* 包必须在 `package.json` 声明 **`dsh.bundle.patch`**（见本目录的 `dsh` 字段 +
  `cordis.patch.yml`）；否则 `dsh plugin install` 只当普通依赖装，不会成为 profile 的一层。
* 依赖是普通 `dependencies`（`@deepseek-ai/dsh-tools`），`@deepseek-ai/cordis` 声明为 peer；
  `plugins/*` 是 workspace 成员，由根 `pnpm install` 装。
* ★ 依赖版本要与正在运行的 DSH 同步：升 DSH 时同步升 `package.json` 里那行，否则可能解析到
  另一份 `defineTool`。
* 撤回：`dsh plugin --profile web uninstall "@amayui/dsh-plugin-wait-subagent"`。

## 6. 目录

```text
plugins/wait-subagent/
  index.mjs            # 插件入口：name / inject / apply → 注册 wait_subagent
  cordis.patch.yml     # bundle patch：一条 loader entry
  package.json         # dsh.bundle.patch + @deepseek-ai/dsh-tools 依赖
  lib/wait.mjs         # ★ 纯逻辑：tracker / 人插话判定 / 竞态安全的 wait
  test/wait.test.mjs   # 等待、插话、pending、abort、timeout、竞态
  test/plugin.test.mjs # defineTool 吃下 spec、apply 注册、各 status 的模型可见结果
```

## 7. 怎么查 / 怎么改

* **查工具是否被注册**：会话里看 `wait_subagent` 的 schema；或跑本目录的
  `node --test --test-isolation=none test/*.test.mjs`。
* **查等待为什么返回**：看 `status` / `stop_reason` / `delivery` / `message_preview`；
  `already_finished` 与 `finished` 的区别是“调用时已经结束” vs “等到了 end”。
* **改唤醒条件**：只改 `lib/wait.mjs` 的监听与 `pendingHumanMessages` / `isHumanMessage`；
  `index.mjs` 只负责 child 身份校验与状态翻译。
* **不要做**：把结果复制进工具返回值（会和 settlement notice 重复）；用 `setInterval` 轮询；
  把 `source.kind === 'subagent-settled'` 当成用户插话；等非直接子代理。
