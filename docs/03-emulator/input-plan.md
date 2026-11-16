# 组装输入：外部数据投递到 shim 输入分发点

> 状态口径：本文只写**不变量 / 落点 / 怎么查**。读数与历史进 `observations.md`；
> 缺陷进 `defects.md`（`BUG-26`）；进度真源 `pnpm tools requirements plan`。
> 设计经独立复审（`.tmp/aot/input-review/review.md`），**本文是修正后的口径**；原始稿 `.tmp/aot/input/design.md` 有错，⛔ 不要照它实现。

## 0. 为什么这么做（口径来源）

用户的判断：**"最核心的错误是在解释循环里面迭代判断"**。输入是**外部数据**，应该类似**独立推进的线程**，
往 **shim 层的输入分发点投递**；内部判定点（等 script 之类）**由内部在对应位置显式向外投递事件**。
输入**不需要"特定 frame"、"到脚本后立即"**这种人工都做不到的精度 —— **只需要粗糙**。

## 1. 今天错在哪（改前，带落点）

- `src/runtime/machine.cpp` 的 `harness_script` 闸门 + 两处 `tick`：**每个基本块**都过一次；有脚本时再付一对 `GuestToken::YieldGuard`。
- ★ **错不在"扫得慢"**：`InputScript::tick` 本身已是 O(1)（几个聚合比较）。**错在调用点还在循环里。**
- ⛔ **不要用"那一段占墙钟 9–16%"当本单收益**：那是 `REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9` 里
  "窗口 profiler 开着 + 过渡帧 + host 桶占比"的口径，且只存在于**解释器 + `--input-script`**；**AOT 臂为 0**。

## 2. 设计（已裁决）

### 2.1 状态源不变

`WindowManager` 仍是唯一的状态源。`InputPlan` **只经 `src/runtime/input.h` 的四个入口**施加到它
（电平读 / 队列 push / 写点 / 事件源），落点 `src/runtime/input_plan.{h,cpp}`。

### 2.2 推进只发生在 guest 读点（⛔ 没有第二个时机）

★★ **能施加 step 的 API 只有那四个，且都必须要求一个 guest 读上下文。**
任何记账函数（`note()` 之类）**不许拿到 plan**，只能累加计数，由**下一个读点**消费。

> 这条是复审抓到的最大风险：`sampleMarker` 在 `Machine::run` 的**块边界 `if`** 里。一旦记账函数能"顺手"施加到期 step，
> 那就是**块边界 pump**，正是要消灭的那个错误。守卫的禁用词表**必须含 `note(`**。

### 2.3 投递点：按**谓词**闭合，不按名单

**谓词**：「读 guest 可见输入态或消息队列的**已注册** shim」。
当前 11 个（9 读点 + `SetCursorPos` 写点 + `ReadFile` 事件源）：

```
GetAsyncKeyState · GetCursorPos · PeekMessageA · GetMessageA · MsgWaitForMultipleObjects
WaitMessage · GetQueueStatus · TranslateMessage · DispatchMessageA · SetCursorPos
```
- ⛔ 排除 `timeGetTime` / `GetTickCount`（时钟不是输入）与 `WaitForSingleObject` 族（今天 `shimAbort`）。
- ⛔ 不要按**名单**写守卫：`SetCursor` / `ShowCursor` 已注册但按设计不参与 ⇒ 名单式断言从诞生起就红。
- **已知残留**：`GetProcAddress` 动态解析是**静默旁路**（`kernel32_process.cpp` 给 `0 + 127`）⇒
  "未注册 ⇒ 响亮 trap"这条闭合论证**只在静态导入面上成立**。

### 2.4 两种投递语义按真机分开

- **电平类**（`GetAsyncKeyState` / `GetCursorPos`）：**读时按当前状态求值**；
- **队列类**（`PeekMessage*` / `GetMessage*`）：**push 进队列**；
- 两者都在**同一次读点内、读之前**落地（**flush-before-read**）。

### 2.5 精度：只承诺能被守卫写下来的那两条

- **保证**：投递之后**同一次** guest 读就能看到；
- **不保证**：第 N 帧、脚本进入后立即。
- 配方里⛔ **没有 frame 单位**（帧号会漂）。

### 2.6 确定性：门只用 guest 可见计数，墙钟只当看门狗

- 门的计数只来自 **guest 可见**事件（如 `Presents` 由 `present.cpp` 在 Present 成功那一刻推）；
- **墙钟只当看门狗**（到期 = 响亮失败），⛔ 不许让墙钟影响"哪一步生效"。
  ⇒ AOT 的 real-time 时钟档不破坏语义指纹。

### 2.7 门的**最小可用集**

**第一版只做 `{Always, Presents}`（+ 可选 `ScreenSample`）。**
`Reads` / `Insns` / `MessageTaken` / `FileReadDone` / `GuestBlocked` 对"到某界面之后按键"**都不是必需**。
★ 陷阱：`Reads` 的刻度**不是"读点次数"** —— `inputSampleCount` 只在 `GetCursorPos` / `GetAsyncKeyState` 自增，
后者在这条 UI 路径上一次也没被调用 ⇒ 实际是 `GetCursorPos` 的 20 Hz；且同一次迭代常有多个读点 ⇒
"after 2 reads" 可能只是**一次**迭代。

### 2.8 配方格式：⛔ 不造第二种 DSL

复用 `interaction_graph` 的线性写法与 `CondKind` / `GraphAction`，只加 `presents` 一个词
（且**在图文件里写 `presents` 是解析期硬错**，防止两种配方语义互相污染）：

```
# 注释
await always            # 或 await presents <n>（guest 绝对 Present 帧数）
click 1070 480 left     # 或 key 0x0D
```
- 只有门没有动作 ⇒ 解析报 `线性 step step1 缺动作行`、`exit 2`。
  ⚠ 所以**"同一份计划去掉动作"表达不了**，对照只能用"不给 plan"或"惰性点击"两档。
- 文件不存在 / 语法错 / 不支持的 cond ⇒ `exit 2` + **点名行号与原因**（⛔ 不许静默忽略）。

## 3. 落点地图

| 件 | 位置 |
|---|---|
| 计划对象 + 门 + 待抬起 + 摘要 | `src/runtime/input_plan.{h,cpp}` |
| 四个投递入口（inline，无计划时**一条 null 检查**） | `src/runtime/input.h` |
| 装配点 | `WindowManager::setInputPlan()` + `HeadlessPresenter`（两处，**非拥有指针**） |
| AOT CLI | `src/aot/aot_main.cpp` 的 `--input-plan`（解析在 **GPU bring-up 之前** ⇒ 文件错不需要 GPU） |
| 逐帧计数推点 | `src/runtime/present.cpp`（Present 成功那一刻 `note(Frame)`） |

★ **`src/runtime/machine.cpp` 与 `src/aot/aot_run.cpp` 里不许出现**：
`InputPlan` / `input_plan` / `input::` / `deliver(` / `atPoint(` / `note(`（守卫钉住，0 命中）。
★ 旧 `harness_script` 闸门**故意保留**（删它是后续单）⇒ 守卫把"新层不许进循环"与"旧层还在"分开判：
新层 5 个词必须 0，旧 tick 只许钉在已知的两个位置。删旧路径后自然退化成"7 词全 0"。

## 4. 性能判据（可判，别写成数字）

- **静态**：`llvm-objdump -d` 的指令行数**增量**（`OBS-20261116-03`：解释器 +152 / AOT +169）。
- **动态**：300 帧交错 A/B ≥3 对取中位；本机分辨不了 <5% ⇒ 结论只许写"在噪声内"或"显著"。
- **成本上界**：无计划时每个读点**一条 null 检查**；频次上界 = guest 读输入次数。

## 5. 守卫（三种，**证明力不同，别混着说**）

| # | 形状 | 证明力 |
|---|---|---|
| ① | **源码形态**：执行循环文件里那 5 个词命中 0（`tools/test/input-plan-shape.test.mjs`） | 强：谁把推进塞回循环就红 |
| ② | **读点闭环（按谓词）**：每个已注册 shim"是否读输入态"的判定 ⟺ "函数体里调了投递入口" | 强：两边都不许单边为真 |
| ③ | **关系断言**：`plan.deliverCalls(…)` ⟺ `shims.callCount(…)`（两个**独立来源**，不是取值断言） | 中：S1 无计划时两侧同为 0，需挂计划才有效 |
| ④ | CLI 面：`--input-plan` 缺文件 / 语法错 ⇒ `exit 2` + 点名行号；缺省档不出现 `input-plan:` 行 | 强 |

★ **一条要记住的分级**：如果一条守卫的"红"只是因为**编译不过**（断言用到新增 API），
它并没有证明**运行期行为** ⇒ 必须在报告里标成"证明力弱"，⛔ 不许与运行时红并列。

## 6. 已做 / 未做

- **已做**：S1（投递层 + 11 读点，行为逐字节不变）· S4（AOT `--input-plan` + 一行摘要）· S2′（门 `{Always, Presents}` + flush-before-read + 待抬起 + `releaseAll`）。
- **未做**：`GetMessageA` 空队列由计划供给 · 两臂共有的事件 emit 点（`ScreenSample` 等）· 旧路径删除 ·
  `Reads`/`Insns` 门 · 干净的无动作对照（`noop` 动作）。
- **两臂分歧**（打开中）：`defects.md#BUG-26` —— 同一点击在 AOT 生效、在解释器无反应。
  ★ 这条不查清，所有"解释器能跑通 / AOT 跑不通"的结论都要重打折扣。

## 怎么查

```bash
node tools/requirements.mjs --plan                 # 进度真源（父单 REQ-01M7G7EJF062NKQ0D40RF3BZ4Z）
cd apps/emulator-lift && ctest -R 'input-plan|cli-aot-flags' --output-on-failure
node tools/mutate-check.mjs --match 'input-plan-shape'    # 守卫自检（⛔ 不要跑全量）
node --test --test-isolation=none tools/test/input-plan-shape.test.mjs
```
屏幕级对照怎么取（用哪个读数、为什么不能用 `stop @` / `lastHash`）⇒ `observations.md` 的 `OBS-20261116-04`。
