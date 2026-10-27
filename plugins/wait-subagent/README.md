# plugins/wait-subagent —— 事件驱动的 `wait_subagent` 工具

> 一句话：把“等子代理”从 `bash sleep <猜的秒数>` 换成**等事件** —— 子代理结束、用户插话、
> turn 取消、或可选的安全上限，哪个先到就返回哪个。**不轮询、不猜时长**。
> 一次可以等一组 child；**默认 `any_of`（谁先结束就返回）**，要等全部才显式 `all_of`。

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

### 1.1 等第一个（`any_of`，默认）还是等全部（`all_of`）

并行开了 N 个子代理时，**绝大多数时候要的是“哪个先出结果就先看哪个”** ——
等全部等于被最慢的那个拖住。所以：

| `mode` | 什么时候返回 | 返回里的 `pending` |
|---|---|---|
| `any_of`（**默认**） | **第一个** child 结束（或第一个“本来就结束了”的 child） | 其余仍在跑的 child（它们的 completion notice 照旧会到） |
| `all_of` | **每个** child 都结束 | `[]`（`absent` 里的那些不算“等到”） |

★ `mode` 只影响“**等到哪一步**”，不影响唤醒源：人插话 / turn 取消 / `timeout_seconds` /
“child 没有 live activation” 在两种模式下都立刻返回，并把当时的 `settled` / `pending` 快照带回去。

★ **为什么不说“多个 child 就一次全等”**：`@deepseek-ai/dsh-subagent` 自带的
`waitForChildren(parent)` 等的是**所有**后代、且没有“等任意一个”的版本 ⇒ 要“谁先结束先返回”
只能自己观察生命周期事件；这也正是本插件存在的理由。

## 2. 怎么调 / 返回什么

```jsonc
{
  "agent_ids": ["<child-1>", "<child-2>"],  // 也可以只给一个 agent_id（单 child 的简写）
  "mode": "any_of",                          // 缺省 any_of；要等全部才 all_of
  "timeout_seconds": 120                      // 可选
}
```

* `agent_id`（单个）与 `agent_ids`（数组）可以混用；重复的 id 会去重，一次最多 **32** 个。
* 每个 id 都必须是**直接子代理**（用 parent-owned `subagent/catalog` 校验；不是就报错，不猜）。

返回一个固定对象（模型看到的是 `note` 的一段可执行文字）：

| 字段 | 说明 |
|---|---|
| `status` | 见下表 |
| `mode` / `agent_ids` | 回显本次调用的口径与去重后的 id |
| `settled` | 已经结束的 child：`[{ agent_id, stop_reason? }]`（按确认顺序） |
| `pending` | 返回时**仍在跑**的 child |
| `absent` | 调用时**没有 live activation**（既没有结束记录、也没有本地 Agent）的 child |

| `status` | 什么时候 | 模型应该做什么 |
|---|---|---|
| `finished` | 等到了 `subagent/end`（`any_of` 是第一个，`all_of` 是最后一个） | 继续下一步；`pending` 里的 child 各自会送来 completion notice，**不要 sleep 等** |
| `already_finished` | 调用时条件就已满足（`any_of` 有一个已结束 / `all_of` 全部已结束） | 不要再等；结果在 completion notice 里 |
| `user_interjected` | 用户 queue / steer 了消息 | `delivery=steer` ⇒ 下一步处理；`delivery=queue` ⇒ 工具已 `concludeTurn()`，loop 会关掉本 turn 让消息跑 |
| `not_running` | 一个可等的 activation 都没有（本地已 idle / 外部已结束 / 插件重启前就结束） | 已结束就看 completion notice；idle 的 continuable child 要先用 `send_message` 唤醒 |
| `timed_out` | 给了 `timeout_seconds` 且到点 | 不要轮询；结果里带 `settled` / `pending` 快照，可据此决定结束 turn 还是只再等一次 |
| `cancelled` | turn 取消 | 无需处理 |

★ **`absent` 不是 `settled`**：`all_of` 里没有 live activation 的 child **不会**被永久等待，
而是进 `absent` 并在 `note` 里明说 —— 否则一个“早就没了”的 child 会让 `all_of` 挂死。

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
  **没有** `waitForActivation(childId)`、也没有“等任意一个”的 API。按 child 等、按任意一个等
  都只能观察生命周期事件。
* `subagent/lib/index.js` 的 `createLifecycleEmitter()`：`subagent/start` / `subagent/end` 用
  `scopeTarget(parent)` 派发；`finishDisposal()` 在 `notifySettlement()`、结果 resolve 之前
  `observer.settle(terminal)` ⇒ **`subagent/end` 是可靠的终态边**。
* `dsh-agent` 的 README + `lib/index.js`：`agent/inbox/inserted { message }` 在每次插入时同步派发；
  人消息的 `source.kind === 'user'`（`user-rpc` 也归并成这个 kind）。
* `dsh-scope` 的 README：`agent.ctx` 的 scope listener 收到 **tag 为 dispatch key 或其祖先**的事件
  ⇒ 用 `exec.agent.ctx.on(...)` 正好只收这个 agent 的 `subagent/end` 与 `agent/inbox/inserted`。
* `dsh-tools` 的 README：tool body 必须观察 `exec.signal`；取消是协作式的。

## 4. 竞态与边界（这是实现的重点）

**注册顺序是契约**（`lib/wait.mjs` 的 `waitForSubagents`）：

1. 先挂 `subagent/end`、`agent/inbox/inserted`、abort 监听；
2. 再复查 inbox 里已经排队的人消息；
3. 再复查 tracker（补上“挂监听前刚好结束”的窗口）；
4. 最后才设 timeout / 进入 await。

边界：

* **只等直接子代理**：用 parent-owned `subagent/catalog` 一次性校验全部 id；混进陌生 id 就报错
  并**点名**是哪一个，不去等一个陌生 session。
* **已结束不挂等待**：tracker 保留 `settled` 状态，正是为了回答“调用时它是不是早就结束了”
  —— `any_of` 因此能在第一个 child 早已结束时立刻返回 `already_finished`。
* **旧 activation 迟到的 end 不覆盖新 run**：tracker 按 `runId` 比对；cold resume 新 activation
  会覆盖成 `running`。等待时也按 `runId` 过滤：旧 run 的 end 不能让新 run 的等待提前返回。
* **人插话优先于同一时刻的 end**：两者在同一同步派发里撞上时，报 `user_interjected`
  —— 人不能被漏掉。
* **不把子代理完成通知当成人插话**：只认 `source.kind === 'user'`。
* **不轮询**：没有 `setInterval`、没有 sleep；只监听事件 / abort / 可选 timer。
* **`all_of` 不赌“它会结束”**：没有 live activation 的 child 进 `absent`、不参与等待；
  已经结束的进 `settled`；只有真正在跑的那些才挂等待。
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
  lib/wait.mjs         # ★ 纯逻辑：tracker / 人插话判定 / 竞态安全的 waitForSubagents
  test/wait.test.mjs   # 等待、插话、pending、abort、timeout、竞态、any_of vs all_of
  test/plugin.test.mjs # defineTool 吃下 spec、apply 注册、各 status / mode 的模型可见结果
```

## 7. 怎么查 / 怎么改

* **查工具是否被注册**：会话里看 `wait_subagent` 的 schema；或跑本目录的
  `node --test --test-isolation=none test/*.test.mjs`。
* **查等待为什么返回**：看 `status` / `mode` / `settled` / `pending` / `absent` /
  `delivery` / `message_preview`；`already_finished` 与 `finished` 的区别是
  “调用时条件已经满足” vs “等到了 end”。
* **改唤醒条件**：只改 `lib/wait.mjs` 的监听与 `pendingHumanMessages` / `isHumanMessage`；
  **改“等谁 / 等到哪一步”**：改 `lib/wait.mjs` 的 `waitForSubagents`（`remaining` / `settled` /
  `conditionMet`）与 `index.mjs` 的 child 三分类（已结束 / 要等 / absent）。
  `index.mjs` 只负责身份校验、分类与状态翻译，不自己挂事件。
* **不要做**：把结果复制进工具返回值（会和 settlement notice 重复）；用 `setInterval` 轮询；
  把 `source.kind === 'subagent-settled'` 当成用户插话；等非直接子代理；
  在 `all_of` 里把“没有 live activation”的 child 当成会自己结束。
