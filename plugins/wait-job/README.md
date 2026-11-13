# `wait_job` —— 可被打断地等一个后台任务

DSH 插件：注册模型工具 **`wait_job`**。落点 `plugins/wait-job/`（源码位置自由；注册走
`dsh plugin ... install`，见 §6）。

## 1. 它补的是什么

等后台任务现在只有 `job_output({ wait: true })`。它有三个出口：**结算 / 超时 / `exec.signal`**。
用户插话（queue 或 steer）**不会**叫醒它 ⇒ 模型只能「停掉整个任务」或者盲猜时长去 `sleep`。

`wait_job` 把等待变成事件驱动，出口是：

| 出口 | `awake` | 触发 |
|---|---|---|
| 结算 | `settled` | 任务进入 `completed` / `killed` / `failed` |
| 打断 | `interrupted` | 用户插话（`agent/inbox/inserted` 且 `source.kind === 'user'`）或 turn 取消（`exec.signal`） |
| 超时 | `timeout` | `timeout_seconds`（**缺省不设** ⇒ 一直等到结算或被打断） |
| 新输出 | `output` | 仅 `wake_on: 'output'`：本次调用之后写入了新字节 |
| 看不见 | `no-such-job` | 未知 id，或属于别的 session（**不是错误**，不抛） |

## 2. 参数（闭集）

| 参数 | 缺省 | 口径 |
|---|---|---|
| `job_id` | 必填 | 非空字符串；`bash` 起的任务或 `job_list` 给的 id |
| `timeout_seconds` | **不设** | 1–86400 的整数。缺省 = 一直等到结算 / 被打断 |
| `wake_on` | `'settle'` | `'settle'` = 等到结算；`'output'` = 有新输出字节就返回（非缺省能力） |
| `include_output` | `true` | **只在「因结算而醒来」时生效**：是否把正文随本次结果带回 |

## 3. ★ 两条路径分工（本插件最重要的不变量，**不要合并**）

**`wake_on: 'settle'`（缺省）**：用 `ctx.jobs.wait(id, timeoutMs, caller, signal)` 等。

> 结算时若有 `wait` 正在等，那条 `settled` 事件的 `awaited` 为 `true`；`dsh-tool-jobs`
> 的完成通知遇到 `awaited` 就**直接 return**（`dsh-tool-jobs/lib/index.js:269`）。
> 而那条通知**只有一行状态指针、从不带正文**（`dsh-tool-jobs/lib/index.js:167`：
> `` `${prefix}${detail}. Read its output with job_output.` ``）—— `awaited` 吃掉的正是
> 那一行「去 job_output 读」的指针。
> ⇒ **正文唯一的通路是结算后我们自己 `jobs.read`** ⇒ `include_output` 缺省**必须**是
> `true`；关掉它，这次结算既没有通知也没有正文（结果真的丢了）。

**`wake_on: 'output'`**：**⛔ 从不持有 `ctx.jobs.wait`**。

> 否则用户插话时任务恰好结算，那次结算也会被标 `awaited` ⇒ 完成通知被吃；而这条路径
> 按规矩**不读正文**（不推进模型游标）⇒ 正文丢失。
> 改用 `ctx.jobs.events.subscribe` 收 `{ type:'output', id }`，用 `readAt(id, from).next`
> 判断是否真有新字节；`from` = **本次调用开始时的 `view.output.total`**（所以它等的是
> 「这之后的新字节」，不是「还没读的老字节」——后者请用 `job_output`）。
> ★ 连**已经结算**的 job 走 `wake_on:'output'` 也**不读正文**（这条路径上 `include_output`
> 恒等于「不读」，见 `index.mjs` 里那句 `wakeOn === WAKE_ON.OUTPUT ? false : includeOutput`）
> —— 正文一律留给 `job_output`。

**因打断 / 超时返回时一律不碰模型游标**：只回 `awake` 原因 + `status` / `bytes` / `progress`，
`jobs.read` 一次都不调。之后那条完成通知（此时 `awaited === false`）照常到达，模型再用
`job_output` 取正文。

**缺省「不设超时」怎么表达**：`dsh-jobs-local` 的 `waitJob` 要求 `timeoutMs` 是正有限数，
且 `dsh-timeout` 的 `deadline()` 拒绝 > `2147483647`（`MAX_WAIT_MS`）⇒ `Infinity` 会被拒。
所以「不设超时」= 按 `MAX_WAIT_MS` 切块逐块重等；**块到期不等于调用方要的 timeout**
（只有 `timeout_seconds` 的绝对 deadline 到了才报 `awake: 'timeout'`）。

**块间竞态**：结算恰好落在两块之间时，下一块 `wait` 会**立刻**拿到终局（真实的 `waitJob`
对已终局的 job 直接返回投影、不再登记 waiter；`get` 也能看见它 ⇒ 就是已结算）。即便真的
漏了一次通知，**损失的也只是一行状态行，不是正文** —— 正文由我们自己 `read`。
（用例：`★ 块间竞态：块到期之后才结算 ⇒ 仍是 awake=settled 且正文非空`。）

**返回前释放所有持有物**：人插话监听器、`exec.signal` 上的 abort 监听、自己的 timer
（settle 路径的 settle 用私有 `AbortController`，**先记因再 abort**，所以能分清
「人插话」与「turn 取消」）。泄漏一条监听器就是 bug —— 有用例断言。

**被打断时要不要收尾本 turn**：**queue 型（next-turn）用户插话会结束本 turn，让那条消息
立即开始跑** —— 那条消息在本 turn 里**看不见**，不收尾的话模型会拿着 `interrupted` 继续
干活、那条消息要等它自己结束 turn 才轮到。实现：本工具把结果标成 turn 收尾
（`exec.concludeTurn()`）。steer 型（下一步就能看见）不收尾，让模型在这一步直接回应。
口径与 `plugins/wait-subagent` 一致。

## 4. 返回格式

一行人类可读状态 + 一行关键字段（`awake` / `status` / `bytes` / `progress` / 用时），
`settled` 且 `include_output: true` 时后面再跟正文（stdout / `[stderr]` 段 / 生产者的
`result`，`log` 旁白不进模型）。**不返回 JSON 大块**。

canonical value 的字段（`output.schema` 就是这份闭集）：
`awake`（必填）· `status` · `bytes`（环的当前总量）· `progress` · `elapsed_seconds` ·
`output`（仅结算时）· `note`（必填，模型可见的那句话）。

`awake: 'no-such-job'` 是**正常返回值**（`defineTool` 的 `isError` 只能靠 throw，而这里
按裁决不抛），但文案明确写成「什么也没等到 + 可能是笔误/已归档 ⇒ 用 `job_list` 看当前
可等的任务」，不让打错 id 读起来像一次成功等待。

模型侧典型用法：

```
wait_job(job_id: "bash-xxxx")                     # 等到结算，结果里带正文
wait_job(job_id: "bash-xxxx", timeout_seconds: 60) # 最多等 60s；到点只回状态，正文留给 job_output
wait_job(job_id: "bash-xxxx", wake_on: "output")   # 有新的输出字节就回来（不消费输出）
```

## 5. 依赖与版本

- 运行时依赖：`@deepseek-ai/dsh-tools`（`defineTool`）与宿主服务 `ctx.jobs`（`inject: ['tools', 'jobs']`）。
- **`@deepseek-ai/dsh-tools` 的版本要与正在运行的 DSH 同步**（理由同 `plugins/deploy/README.md` §5：
  否则插件会解析到另一份 `defineTool`）。
- `plugins/*` 在 `pnpm-workspace.yaml` 的 workspace 列表里 ⇒ 新增/升依赖后跑一次 `pnpm install`
  （装依赖这一步在 Windows 上要提权，见 `plugins/pnpm-priv`）。

## 6. 安装 / 卸载（环境级动作）

宿主插件代码**不热重载** ⇒ 装完必须**重启宿主**才生效。

```bash
pnpm install                                                  # 先让 plugins/wait-job 的依赖落到位
dsh plugin --profile web install "<仓库绝对路径>/plugins/wait-job"
# 然后重启宿主（本会话不生效）
```

卸载：

```bash
dsh plugin --profile web remove @amayui/dsh-plugin-wait-job
```

★ 装插件写 `$DSH_HOME`（工作区之外）⇒ 是环境级动作、要提权。

## 7. 未验证 / 边界（诚实口径）

- **本会话未经端到端验证**：宿主插件不热重载，装不进来 ⇒ `ctx.jobs` 的真实行为、真实
  `agent/inbox/inserted` 派发、真实 `exec.signal` 都**没有在宿主里跑过**。仓库内的用例只
  覆盖 `lib/` 的纯逻辑（fake registry，见 §8）。
- **`index.mjs` / `apply()` 没有仓库内用例**：它要 `import '@deepseek-ai/dsh-tools'`，而
  `pnpm install` 之前 `plugins/wait-job/node_modules` 不存在 ⇒ 这个 import 在测试里解析不到。
  目前对 `index.mjs` 只有 `node --check`（语法）。依赖装好后可以补一条 `apply()` 的接线用例
  （同形先例：`plugins/wait-subagent/test/plugin.test.mjs`）。
- 并发：**不声明** `isConcurrencySafe` —— settle 路径的 `jobs.read` 是**消费模型游标**的写操作，
  两次并发 `wait_job` 会互相吞正文（`job_output` 同样不声称并发安全）。
- 竞态窗口：结算恰好发生在「登记 `wait` 之前」时，这次结算不会被标 `awaited`，完成通知
  **会**另外到达（它只提示模型去 `job_output`，此时正文已在本次结果里，属无害重复）。
- 因打断 / 超时返回后，正文仍要模型自己 `job_output` 取 —— **完成通知不携带正文**，这是
  上游口径，本插件不改写它。

## 8. 怎么跑（零安装）

```bash
cd plugins/wait-job
node --test --test-isolation=none test/*.test.mjs
node --check index.mjs && for f in lib/*.mjs test/*.mjs; do node --check "$f"; done
```

用例全部用 fake registry（`{ get, wait, read, readAt, events }` 注入），不依赖真宿主、
不 import 任何 dsh 包。首行 pragma 把它归进 `@suite toolchain`（`@env pure`）：
`AGENTS.md` 的分档口径是 **工具链档 = 工具链 / 台账 / 写路径 / 知识层**，DSH 插件本身就是
工具链；而且 **`pnpm test` 与 `pnpm verify` 都必绿** ⇒ 这一档**不会**造成覆盖损失
（与 `plugins/wait-subagent` 的先例一致）。
