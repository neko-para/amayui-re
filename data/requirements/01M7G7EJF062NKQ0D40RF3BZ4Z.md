# 组装输入：外部数据投递到 shim 输入分发点（不在执行循环里判断）

- id: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- tags: [emulator-lift, input]

## 问题（用户的判断，逐字）

> 之前解释器的实现，我认为**最核心的错误是在解释循环里面迭代判断**。输入作为一个**外部数据**，应该类似于一个
> **独立推进的线程**，往我们本身的 **shim 层的输入分发点投递**。对于从内部进行判断的点（比如之前的 wait 特定 script），
> 可以考虑首先从**内部在对应位置显式向外投递事件**。本质上我们的输入**不会要求"在特定 frame"、"在到达脚本后立即"**
> 这种本身人工都无法实现的位置做操作，总是**比较粗糙**的。注意避免像之前那样引入显著的性能损耗。

## 今天错在哪（已定死，带行号）

`machine.cpp:1186-1192` 的 `harness_script` 闸门 + `:1209-1212` / `:1253-1256` 两个 tick，**每个基本块**都过一次；
有脚本时再付一对 `GuestToken::YieldGuard`。
★ 而 `InputScript::tick` **本身已经是 O(1) 快路径**（`input_script.cpp` 的 8 个聚合比较）
⇒ **错不在"扫得慢"，在调用点还在循环里。**
⛔ **不要把"9–16% 墙钟"当本单的收益**（那是 `REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9` 里"窗口 profiler 开着 + 过渡帧 fr110-111 +
host 桶"的口径，而且只存在于**解释器 + `--input-script`**；**AOT 今天为 0**）。本单的收益是**结构性**的：
判据从"每个块"变成"guest 自己读输入时"，并让 AOT 臂第一次可以自动化驱动。

## 设计（已过独立复审；下面是**修正后**的口径，原设计稿见 `.tmp/aot/input/design.md`）

- **状态源不变**：`WindowManager` 仍是唯一状态源；`InputPlan` 只经 `runtime/input.h` 的四个函数施加。
- **推进只发生在 guest 自己的输入读点**（⛔ 没有第二个时机）。事件由内部显式投出，但**只是记账**，
  由**下一个读点**消费 —— ⛔ 不允许任何 `note()`/`sampleMarker()` 类的函数"顺手把到期 step 施加掉"
  （复审指出的最危险处：`sampleMarker` 在 `Machine::run` 的块边界 `if` 里，一旦它能施加 step，就是块边界 pump）。
  ⇒ 结构上保证：**能施加 step 的 API 只有那四个，且它们都要求一个 guest 读上下文**。
- **投递语义按真机分开**：电平类（`GetAsyncKeyState` / `GetCursorPos`）**读时按当前状态求值**；
  队列类（`PeekMessage*` / `GetMessage*`）**push 进队列**；两者都在**同一次读点内、读之前**落地（flush-before-read）。
- **精度（要能写成守卫）**：保证 = 投递后**同一次** guest 读就能看到；不保证 = 第 N 帧 / 脚本进入后立即。
- **确定性**：门只用 **guest 可见**的计数；墙钟只当**看门狗**（到期＝响亮失败）。
  ⇒ AOT 的 real-time 时钟档不破坏语义指纹。
- **配方**：不发明新 DSL，复用 `interaction_graph` 的 `CondKind` / `GraphAction`，只加 `reads`。

### 门的**最小可用集**（复审裁定：原设计过度精确）

**第一版只做 `{Always, Presents}`（+ 可选的 `ScreenSample`）。**
`Reads` / `Insns` / `MessageTaken` / `FileReadDone` / `GuestBlocked` 对"到 TITLE 之后点 OPTION"**都不是必需**，先不做。
★ 另：`Frame` / `ScreenSample` / `Presents` 在源码里是**同一个触发**（`present.cpp` 同一函数的两行）⇒ 别当三件事。
★ `Reads` 那个刻度也不是"读点次数"：`inputSampleCount` 只在 `GetCursorPos` / `GetAsyncKeyState` 自增，
而 `GetAsyncKeyState` 在这条 UI 路径上**一次也没被调用** ⇒ 实际是 `GetCursorPos` 的 20 Hz；
且同一次迭代内常有多个读点 ⇒ "after 2 reads" 可能只是**一次**迭代。

### 投递点 = 11 个已注册 shim（读路径已穷举核过）

9 个读点：`GetAsyncKeyState` · `GetCursorPos` · `PeekMessageA` · `GetMessageA` · `MsgWaitForMultipleObjects` ·
`WaitMessage` · `GetQueueStatus` · `TranslateMessage` · `DispatchMessageA`；+ `SetCursorPos`（写点）；
+ `ReadFile`（**只是事件源**，不是输入读点 —— ⛔ 不要把它算进"输入族"）。
⛔ 排除 `timeGetTime` / `GetTickCount`（时钟不是输入）与 `WaitForSingleObject` 族（今天 `shimAbort`）。
★ 闭合性要按**谓词**写（"读 guest 可见输入态或消息队列的已注册 shim"），⛔ 不要按名单写 ——
名单式断言会必然红（`SetCursor` / `ShowCursor` 已注册但按设计不参与）。

## 已知残留（不是本单要解决的）

1. **`GetProcAddress` 动态解析是静默旁路**：`kernel32_process.cpp:983-999` 给 `0 + 127` ⇒
   guest 动态解析 `GetKeyState` 之类会拿到 0 而不是响亮 trap ⇒ "未注册 ⇒ 响亮"的闭合论证**只在静态导入面上成立**。
2. 两份历史 trace 的 `TraceInputs` 里**没有配方字段** ⇒ 换配方不会被 `traceInputsHash` 看见。
   ★ 但这**不需要裁决**：`machine.cpp:2371-2386` 已有三条"只在开了才混入"的先例 ⇒ 补字段不改缺省档 hash。

## 落地顺序（每步独立可验）

1. **S1** 投递层骨架 + 11 个读点（**无配方**，行为零变化）。
2. **S4** AOT 的输入控制面（`--input-plan` CLI + 装配 + 一行摘要）；缺席时逐字节不变。
3. **S2'** 门 `{Always, Presents}` + flush-before-read + 待抬起队列。
   ⇒ **1+2+3 合起来才是"用 AOT 自动跑到加载存档"的最小闭环**（S4 单独是空操作，它的字段由 S1/S2 提供）。
4. 之后视需要再开：两臂共有的事件 emit 点 / `GetMessageA` 空队列由计划供给 / 输入清单与转换器 + 删旧路径。

## 守卫（形状已按复审修正）

- **① 源码形态**：`src/runtime/machine.cpp` 与 `src/aot/aot_run.cpp` 里，`InputPlan` / `input::` / `deliver(` /
  `atPoint(` / `tickInputScript` / `tickInteraction` / **`note(`** 的命中必须为 0
  （★ 复审点名：原设计的禁用词表漏了 `note(`，而它正是最危险的入口）。变异：塞回一处调用 ⇒ 必红。
- **② 读点闭环**：按**谓词**枚举所有已注册 shim，逐个断言"它读 guest 输入态/消息队列"的判定与"函数体里有 `deliver`"一致。
- **③ 关系断言**（不是取值断言）：`plan.deliverCalls(…)` == `shim.callCount(…)` —— 两个**独立来源**对比。
