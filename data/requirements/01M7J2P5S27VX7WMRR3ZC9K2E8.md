# M1 · 动态投递：暂停态入队 click，读点施加（同一 applyStep 通道，不越过未到期的门）

- id: REQ-01M7J2P5S27VX7WMRR3ZC9K2E8
- type: req
- status: done
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- verify: apps/emulator-lift/tests/control_endpoint.cpp#click-enqueue-at-read-point
- tags: [emulator-lift, control-plane]

## 一句话

M0 只能"停住 → 看 → 继续"。M1 补上**投递一条输入**：`click` 在**暂停态入队**，`resume` 之后由**下一个 guest 读点**施加
（⛔ 不是在控制线程里直接动 guest 状态，⛔ 也不是在执行循环里加第二套 pump）。

## 为什么（用户口径，2026-11-16）

> shim 的存在允许我们完全控制整个二进制的执行，比如"请求截图""投递输入"，这样 agent 就可以逐步操作，最后汇总 graph。

⇒ 有了 M0（停 / 采样位置 / 截图）之后，探索者还差"**按当前画面决定下一步点哪**"这一步。

## 判据（每条能红）

1. **同一条输入、两种生产者，读数逐位相同**：`click 1070 480` 写成配方一条 vs 暂停后入队再 `resume`
   ⇒ 两边 `region` 三数、`fired`、`insns` 口径相同（同一 `applyStep` 通道，⛔ 不许有第二套坐标映射）。
2. **不推进配方的步进指针**（这条钉的正是"插在 `next_` 会跳过门"的错法）：配方里有 `await script SN0000 timeout 35000`
   尚未满足时入队并施加一条 `click` ⇒ 该 click **在下一个读点就生效**（这是要的行为），但配方的 `next_` **不动**：
   那条门仍在等、`pending` 计数不变。⇒ 判据 = "click 生效" 与 "配方步进没被跳过" 两件事同时成立。
3. **暂停态之外的入队要响亮**：未暂停时 `click` ⇒ `error.code:"not_paused"`（⛔ 静默丢掉）。
4. **`key` 本轮不做**（用户裁决 2026-11-16："click 够用即可"）：`capabilities` 里不列它 ⇒ `unknown_op`；
   将来要做时走 `input::key`（`src/runtime/input.h:58`），⛔ 不自己拼 WM_KEYDOWN 消息。
5. **队列深度有界**：暂停态连投 100 条不能把内存/日志打成无界（给上界 + 超界 `bad_request`）。

## 已裁决

- **DSL 不扩**（配方仍是三指令 + 动作）：`click` 复用 `recipe.h` 里**已有**的 click step，⛔ 不为控制层加新语法。
- **立即队列与配方文件是两条生产者、一条消费者**：配方里未到期的门照旧按顺序等待（判据 2）。
- `wait <ms>` 控制命令 = "让 guest 跑 N ms 后重停"（**不是**往配方里塞一条 step），归属 M0 的 `resume --for-ms`。

## 落点（预计）

`src/runtime/control_ws.cpp` 的 `click` 分支（入队）+ `InputPlan` 的立即队列（读点先消费，**不推进**配方里的 `next_`）
+ `cursor_map.h` 的 `referenceToClientPixel`（1280×720 参考坐标 → guest 客户端坐标，**唯一**除法点）。

## 未做

- `key`（服务端没有这个 op；客户端继续明确回"未实现 + exit 2"）。
- **"点击是否真被施加"没有回执字段** ⇒ e2e 断言只能看配方摘要的 `fired`；逐位证据在 pure（两种生产者的 lParam 相同）。
- 上界 64 是**每条命令**的上界，不是"每次 resume / 每会话的投递预算"（多轮 pause/resume 仍可无限投）⇒ 防失控要另一层。
- M2（录制：把一次交互导出成 `.plan`）。`resume --until-ea` / `--until-screen-change` 不做（没有消费者）。
