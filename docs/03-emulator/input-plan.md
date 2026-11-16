# 组装输入：外部数据投递到 shim 输入分发点

> 状态口径：本文只写**不变量 / 落点 / 怎么查**。读数与历史进 `observations.md`；
> 缺陷进 `defects.md`（`BUG-26`）；进度真源 `pnpm tools requirements plan`。
> 配方语法（三条指令）由 `REQ-01M7HBG0NYH957M9EK3JVMF5CC` 裁决；本文是**修正后的口径**
> （设计经独立复审；早期的本地设计稿 / 复审稿在 `.tmp/aot/` 下，已删除，⛔ 不要照旧稿实现）。

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
- **不保证**：脚本进入后立即（门是**电平**：身份变化后还要等**下一个读点**才被消费；
  两次读点之间可能隔一个 guest 迭代周期）。
- 配方里⛔ **没有 frame 单位、没有帧号门**（用户口径："不依赖特定 frame，这个本身人工都无法评估"）：
  `await presents <n>` 这类帧号门在 `--input-plan` 里是**解析期硬错**。
- ★ **`await script` 是"当前帧是哪个脚本"的电平判据，不保证该脚本已进入自己的输入循环**：
  `--skip-logo` 下**第一个读点**的身份就可能是 `TITLE.BIN`（此时还没到第一帧）⇒ 光靠身份门
  表达不了"标题菜单已就绪"，只写 `await script TITLE` + `click` 会把点击投在 boot 上
  （本轮实测：`fired=1` 但收尾 `region` 与无输入基线相同）。需要"菜单就绪"级判定时**只能等
  位置谓词**（另一单在取证）；`wait` 也**不可靠**（它是宿主墙钟、与 guest 推进不同步 ⇒ 见 §2.6 的边界）。
  ⛔ 别把身份门当成"到标题后立即"（那正是旧 `presents` 帧号门想解决、而本单裁决**不再**用帧号解决的事）。

### 2.6 确定性：门只用 guest 可见状态；**`wait` 是唯一的墙钟面**

- 门（`await script`）只读**引擎脚本身份**（`MarkerSampler` 纯读 guest 内存；`base==0` / `valid=false`
  ⇒ 门不成立、继续等，⛔ 不报错）。
- ★ **`wait <ms>` 例外且显式**：它用**宿主单调时钟**（`std::chrono::steady_clock`；⛔ 不是 `clock.h`
  的 `VirtualClock` = guest 虚拟时钟）。**非阻塞**：只记 deadline，到期由**下一个读点**消费
  （`atPoint` 在 shim 栈里 ⇒ ⛔ 不许 sleep）。
- ⇒ **带 `wait` 的配方不可复现、不可跨臂比、不作语义指纹**：墙钟就是那一步生效时刻的一部分，
  这正是本设计**接受**的代价（用户要的是"到 TITLE 后等半秒再点"这种**粗糙**推进）。
- ★★ **`wait` 的边界（实测，父级 2026-11-16）**：`wait` 是**宿主墙钟**，而 guest 推进**不是墙钟**
  （帧生产慢于 60/s 时，同样的 2 s 墙钟对应的 guest 时刻会漂）⇒ 点击落在"菜单就绪前"还是"就绪后"
  是**随机的**：同一 `await script TITLE` + `wait 2000` + `click 1070 480` 的 `region` 级对照
  **12 跑 4 成**；bare（省略按钮词）/ 显式 `left` / `right` 之间**没有稳定差别**
  （`right` 2 跑没成，样本不足以单独定罪）。
  ⇒ ⛔ **时间门不能当判据**、`wait` 只作尽力而为的粗推进；**`region` 级正例只能等位置谓词**
  —— `TITLE.BIN:[0x039C,0x0460)`（另一单）。
- 没有 `wait` 的配方仍然只受 guest 可见状态驱动 ⇒ 与旧的"墙钟只当看门狗"结论不冲突。
  AOT 的 real-time 时钟档只在**有 `wait`** 的配方上改变语义指纹。

### 2.7 门的**最小可用集**

**只有 `{Script}`（电平）**。`ScreenChanged` / `ScreenRect` / `ScreenHash` / `Presents` / `Reads` /
`Insns` / `MessageTaken` / `FileReadDone` / `GuestBlocked` 在 `--input-plan` 里都**不可表达**
（写进配方 = 解析期硬错）。
- 为什么删 `Presents`：它是**帧号门**，人无法评估；同一个 N 在两臂不对应同一 guest 时刻（`BUG-26` 候选①）。
- ★ 陷阱（留下当"为什么不做"）：`Reads` 的刻度**不是"读点次数"** —— `inputSampleCount` 只在
  `GetCursorPos` / `GetAsyncKeyState` 自增，后者在这条 UI 路径上一次也没被调用 ⇒ 实际是
  `GetCursorPos` 的 20 Hz；且同一次迭代常有多个读点 ⇒ "after 2 reads" 可能只是**一次**迭代。

### 2.8 配方格式：**三条指令**（平铺；⛔ 不是图语法的子集）

```
# 注释（整行 / 行尾都吃）；空行忽略
await script TITLE      # 电平门：当前身份 == TITLE（ASCII 大小写不敏感、两侧都忽略 .bin）
wait 500                # 宿主单调时钟毫秒；正整数
click 1070 480 right    # x/y = 1280×720 参考系；按钮词裸写（省略 = left；⛔ 不收 =left）
```

- **按文件顺序执行**；`click` 在同一个读点施加（flush-before-read 口径不变）。
- **`wait` 的起点** = 上一条指令生效的那个读点 ⇒ 有 0..一个迭代周期的偏差（**不可评估**）。
- ⛔ **没有每步超时**：卡住由 `--time-limit` / `--max-frames` 这层看门狗报。
- ⛔ **解析期硬错**（`exit 2` + `文件:行号: 原因`，点名"未知指令"）：
  `await always` · `await presents <n>` · `key <VK>` · 图那套 `node / when / action / next /
  expect / timeout` · 任何别的词。缺参数 / 负毫秒 / `=left` 同样硬错。
- ★ **只写门 / 只写 `wait` 是合法的**（线性指令流）⇒ 旧的"同一份计划去掉动作表达不了"
  这一限制**不再成立**：现在可以直接给一份"只等门"的配方当对照。
- ⛔ 解释器臂**不接** `--input-plan`（给它 = `未知参数` + exit 2）；"两臂同一份配方"要等解释器接线。

#### 坐标合同

- `click` 的 `(x,y)` 属于**标准 1280×720 参考系**（人手在标准画面上量出来的数）。
- 投递前必须缩放：**唯一公式来源**是 `src/runtime/cursor_map.h` 的
  `referenceToClientPixel()`（纯函数、SDL-free；`input_plan.cpp` 里⛔ 不许出现 1280/720 的除法）。
  目标尺寸 = 主窗口客户区 `HostWindow::client_w/h`（headless 也有；0 ⇒ 退回 1280×720）。
- `mouseMove` / `mouseButton(down)` / `mouseButton(up)` 用**同一个**映射结果
  （`applyStep` 算一次，写进 `PendingRelease`；⛔ 两处各算一次 = 静默漂移）。
- ★ **非 16:9 时按各轴独立缩放**（拉伸参考系；⛔ 不 letterbox、⛔ 不 clamp 到边缘）。
  ⚠ **这一条没有取证**：仓里没有"非 1280×720 时点击命中"的观测；已观测的 run 里目标都是
  1280×720（恒等）。⇒ 它是一条**显式裁决**，不是已验证结论。

## 3. 落点地图

| 件 | 位置 |
|---|---|
| 计划对象 + 三条指令 + 待抬起 + 摘要 | `src/runtime/input_plan.{h,cpp}` |
| 参考系 → 客户区的**唯一**映射 | `src/runtime/cursor_map.{h,cpp}`（`referenceToClientPixel`） |
| 名字口径（大小写 / 两侧 `.bin` / 退化名） | `src/runtime/interaction_graph.{h,cpp}`（`scriptNameMatches`） |
| 四个投递入口（inline，无计划时**一条 null 检查**） | `src/runtime/input.h` |
| 装配点 | `WindowManager::setInputPlan()` + `InputPlan::attachGuestMemory(&host.mem())`（AOT 臂；`atPoint()` 签名不变） |
| AOT CLI | `src/aot/aot_main.cpp` 的 `--input-plan`（解析在 **GPU bring-up 之前** ⇒ 文件错不需要 GPU） |
| 逐帧计数（**只是摘要读数**） | `src/runtime/present.cpp`（Present 成功那一刻 `note(Frame)`；`presents=` 不再是门） |

★ **`src/runtime/machine.cpp` 与 `src/aot/aot_run.cpp` 里不许出现**：
`InputPlan` / `input_plan` / `input::` / `deliver(` / `atPoint(` / `note(`（守卫钉住，0 命中）。
★ 旧 `harness_script` 闸门**故意保留**（删它是后续单）⇒ 守卫把"新层不许进循环"与"旧层还在"分开判：
新层 6 个词必须 0，旧 tick 只许钉在已知的两个位置。删旧路径后自然退化成"那组词全 0"。

## 4. 性能判据（可判，别写成数字）

- **静态**：`llvm-objdump -d` 的指令行数**增量**（`OBS-20261116-03`：解释器 +152 / AOT +169）。
- **动态**：300 帧交错 A/B ≥3 对取中位；本机分辨不了 <5% ⇒ 结论只许写"在噪声内"或"显著"。
- **成本上界**：无计划时每个读点**一条 null 检查**；频次上界 = guest 读输入次数。
- ★ `await script` 的采样有缓存（同 `(cur,id)` + 名字表签名不变 ⇒ 不重解析名字）；
  它只在**下一个待判指令是 `await script`** 时才采。

## 5. 守卫（三种，**证明力不同，别混着说**）

| # | 形状 | 证明力 |
|---|---|---|
| ① | **源码形态**：执行循环文件里那 6 个词命中 0（`tools/test/input-plan-shape.test.mjs`） | 强：谁把推进塞回循环就红 |
| ② | **读点闭环（按谓词）**：每个已注册 shim"是否读输入态"的判定 ⟺ "函数体里调了投递入口" | 强：两边都不许单边为真 |
| ③ | **关系断言**：`plan.deliverCalls(…)` ⟺ `shims.callCount(…)`（两个**独立来源**，不是取值断言） | 中：无计划时两侧同为 0，需挂计划才有效 |
| ④ | CLI 面：`--input-plan` 缺文件 / **旧指令硬错** / 缺参数 / 负毫秒 / `=left` ⇒ `exit 2` + 点名行号；缺省档不出现 `input-plan:` 行 | 强（⚠ `cli-aot-flags` 在非 Windows 是 SKIP） |
| ⑤ | 语义面（CTest `input-plan-read-points`）：`await script` 电平门 + 身份读不到继续等 + `wait` 注入时钟（未到期不施加 / 到期施加，不需要真睡）+ click 的 sampled / forced 抬起 + 1280×720 → 客户区缩放且 move/down/up 同一结果 | 强：改语义就红 |

★ **一条要记住的分级**：如果一条守卫的"红"只是因为**编译不过**（断言用到新增 API），
它并没有证明**运行期行为** ⇒ 必须在报告里标成"证明力弱"，⛔ 不许与运行时红并列。

## 6. 已做 / 未做

- **已做**：S1（投递层 + 11 读点，行为逐字节不变）· S4（AOT `--input-plan` + 一行摘要）·
  S2′（flush-before-read + 待抬起 + `releaseAll`）· **三条指令**（`await script` 电平门 +
  `wait` 宿主单调毫秒 + `click` 1280×720 参考系缩放；删 `key` / `presents` / 图语法）。
- **未做**：`GetMessageA` 空队列由计划供给 · 两臂共有的事件 emit 点（`ScreenSample` 等）· 旧路径删除 ·
  `ScreenChanged` / `ScreenRect` / `click center` · 键盘（`key` 已删，需要时再加） · 解释器臂接 `--input-plan`。
- **两臂分歧**（打开中）：`defects.md#BUG-26` —— 同一点击在 AOT 生效、在解释器无反应。
  ★ 这条不查清，所有"解释器能跑通 / AOT 跑不通"的结论都要重打折扣。

## 怎么查

```bash
node tools/requirements.mjs --plan                 # 进度真源（父单 REQ-01M7G7EJF062NKQ0D40RF3BZ4Z）
cd apps/emulator-lift && ctest -R 'input-plan|cli-aot-flags' --output-on-failure
node tools/mutate-check.mjs --match 'input-plan-read-points'   # 变异自检（wait deadline 那条）
node --test --test-isolation=none tools/test/input-plan-shape.test.mjs
```

端到端（AOT，**唯一可复现的 `region` 级对照 = 负对照**；每次全新空 `--appdata`）：

```bash
cd apps/emulator-lift
printf 'await script NOSUCHSCRIPT\nclick 1070 480 right\n' > /tmp/neg.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/neg.plan
# input-plan: steps=2 fired=0 presents=400 released=0 held=0
# region    : arena in_use=108595152 peak=108595152 surfaces=21 heap_blocks=6674
# ⛔ 正例（TITLE 之后点击）不要用 `wait` 凑：同一 `wait` 12 跑 4 成；等位置谓词（另一单）。
```

★ `region` 三数的稳定面：`in_use` / `surfaces` 逐位稳定；**`heap_blocks` 长跑会漂**
（短跑 6674 / 6675、长跑 6821 / 6822 都出现过，原因未查）⇒ 判据给它 **±1 容差**，⛔ 别把某一跑的
`heap_blocks` 当指纹。

读数口径（为什么用收尾 `region` 三数、⛔ 不用 `stop @` / `lastHash`）⇒ `observations.md` 的 `OBS-20261116-04`。
