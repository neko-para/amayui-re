# 组装输入：外部数据投递到 shim 输入分发点

> 状态口径：本文只写**不变量 / 落点 / 怎么查**。读数与历史进 `observations.md`；
> 缺陷进 `defects.md`（`BUG-26`）；进度真源 `pnpm tools requirements plan`。
> 配方语法（**四条指令**）由 `REQ-01M7HBG0NYH957M9EK3JVMF5CC` + 位置门裁决；本文是**修正后的口径**
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
  （本轮实测：`fired=1` 但收尾 `region` 与无输入基线相同）。需要"菜单就绪"级判定时用
  **`await position`**（§2.8；它只读 guest 内存里的脚本体 PC）；`wait` 仍**不可靠**
  （它是宿主墙钟、与 guest 推进不同步 ⇒ 见 §2.6 的边界）。
  ⛔ 别把身份门当成"到标题后立即"（那正是旧 `presents` 帧号门想解决、而本单裁决**不再**用帧号解决的事）。
  ★ **区间不是随便挑的**：`[0x039C,0x0460)` 是"输入轮询体在跑"，但**在轮询体里投的点击不保证被回调
  看见**（实测见 §2.6 末段）；要让点击落在"回调正在读输入"那一刻，区间要取回调的输入读段
  （TITLE = `[0x0460,0x0520)`）。

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
- ★ **`await position` 是纯 guest 门**：`pos = header_len + (frame[cur].+0x04 − frame[cur].+0x00)`
  （`+0x00`/`+0x04` = 脚本体基址 / PC；EA `0x40EE57` / `0x412825` / `0x413487`）。读不到
  （`base==0` / `pc < base` / `pc` 未提交 / `pc−base` 越语料上界）⇒ 门**不成立、继续等**，只记内部计数
  `InputPlan::positionMisses()`（缺省不打印）。`header_len` 是**语料常量 `0x3C`**：装载器只把
  `0x20` 头读进栈、把脚本体从文件偏移 `0x3C` 起读进 `frame+0x00`（`0x40EE67` / `0x40EFF2` /
  `0x40F01C` / `0x40F69E`）⇒ 头本身**不在 guest 内存里**，读不到。
- ★★ **e2e 实测（2026-11-16，macos-arm64 RelWithDebInfo，`--present headless --skip-logo
  --max-frames 400 --time-limit 150 --region-log`，每次全新空 `--appdata`）**：
  1. `await position TITLE 0x039C 0x0460` + `click 1070 480 left` 跑 **5 次**：`fired=1` 恒成立
     （门确实在 `pos=0x39C..0x3BC` 命中），但收尾 `region` **5/5 = 基线**
     `in_use=108595152 / surfaces=21 / heap_blocks=6675`（**不是**"点击成功"的
     `117490928/25/6821`）。
  2. 把门区间换成回调的输入读段（`TITLE.BIN:[0x0460,0x0520)`，即 `call 0x47c` → `i2fc` →
     `read-mouse-pos/read-mouse-button` 之前）后同一 click **3/3 = `117490928/25/6821`**。
  3. 区间取回调后端（`[0x05D0,0x06C8)` / `[0x0778,0x083C)`）或轮询体端点
     （`0x39C` / `0x3A0` / `0x3BC` / `0x448` / `0x454`）**全部 = 基线**。
  4. **一个反直觉读数**：把 `[0x0460,0x0500)` 当"负对照"（原设想是"区间外 ⇒ `fired=0` + 基线"）
     **不成立**：它 3/3 `fired=1` 且 region = `117490928/25/6821`。原因同上 —— `0x460..0x4FC`
     是回调体**前半段**（输入读之前），每轮轮询都会执行 ⇒ 它落在"有效区间"一侧。
     真正的负对照要用**永不执行**的区间（如 TITLE 的退出出口 `[0x388C,0x38A4)`）或**名字不匹配**
     （`await position NOSUCHSCRIPT …`）：两者各 3/3 `fired=0` + 基线 `108595152/21/6674`。
  ⇒ **结论**：位置门本身是确定性的（读数逐位稳定），但"轮询体在跑"**不等于**"这一刻投的点击会被
  回调消费"；点击必须在回调**读输入之前**到位。未取证的机制猜测：轮询体里的消息泵会在回调的
  `i2fc` 之前把注入的消息取走/派发掉，回调这一趟于是看不到"新输入"。⛔ 这是**读数**，不是已登记的
  引擎语义（§6 准入门不适用于本条，除非另开取证）。

### 2.7 门的**最小可用集**

**只有 `{Script}` 与 `{Position}`（都是电平）**。`ScreenChanged` / `ScreenRect` / `ScreenHash` /
`Presents` / `Reads` / `Insns` / `MessageTaken` / `FileReadDone` / `GuestBlocked` 在 `--input-plan`
里都**不可表达**
（写进配方 = 解析期硬错）。
- 为什么删 `Presents`：它是**帧号门**，人无法评估；同一个 N 在两臂不对应同一 guest 时刻（`BUG-26` 候选①）。
- ★ 陷阱（留下当"为什么不做"）：`Reads` 的刻度**不是"读点次数"** —— `inputSampleCount` 只在
  `GetCursorPos` / `GetAsyncKeyState` 自增，后者在这条 UI 路径上一次也没被调用 ⇒ 实际是
  `GetCursorPos` 的 20 Hz；且同一次迭代常有多个读点 ⇒ "after 2 reads" 可能只是**一次**迭代。

### 2.8 配方格式：**四条指令**（平铺；⛔ 不是图语法的子集）

```
# 注释（整行 / 行尾都吃）；空行忽略
await script TITLE      # 电平门：当前身份 == TITLE（ASCII 大小写不敏感、两侧都忽略 .bin）
await position TITLE 0x039C 0x0460
                        # 电平门：script == TITLE **且** FROM <= pos < TO（半开区间；FROM/TO 是带 0x 的 hex）
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

#### `await position` 的语法与坐标

- 语法：`await position <SCRIPT> <FROM> <TO>`。`<SCRIPT>` 沿用 `await script` 的**运行期**名字匹配
  （ASCII 大小写不敏感、两侧都忽略 `.bin`）⇒ **未知脚本名不是解析期错误**（运行期永远不匹配 = 一直等）。
- `<FROM>` / `<TO>` **必须是 `0x` 前缀的 32 位 hex**（⛔ 不收十进制 / 负 / 裸 hex `39C`）；
  解析期校验 **`FROM < TO`**；**FROM 含、TO 不含**。
- `pos` 是**脚本体内绝对字节偏移**（**4 字节对齐**），与 `dist/views/data/*.txt` 的 `label_%08x`
  **同一坐标系**：`pos = header_len + (frame[cur].+0x04 − frame[cur].+0x00)`，`header_len = 0x3C`
  （语料 941/941 `SYS4450`；见 §2.6）。⇒ 清单里的 `label_0000039c` 就是 `0x39C`，
  **不需要**任何"+1 / ×4 / 减表头"的换算。
- 语义是**电平门**（不是边沿）：`script` 匹配 **且** `FROM <= pos < TO` 才通过，否则继续等
  （⛔ 不报错、不超时、不施加）。读不到位置（`base==0` / `pc` 越界 / 未提交）⇒ 同样继续等。
- ⚠ **区间要选"能被消费"的那一段**：见 §2.6 末段的 e2e 读数 —— 轮询体 `[0x039C,0x0460)` 里投的点击
  5/5 没有生效；回调输入读段 `[0x0460,0x0520)` 里投的同一 click 3/3 生效。这是**读数**，不是命名。

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
| 计划对象 + 四条指令 + 待抬起 + 摘要 | `src/runtime/input_plan.{h,cpp}` |
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
| ⑥ | 位置门（同一 CTest 的 `input-plan/position-gate`）：`pos == FROM` 命中 / `pos == TO` 不命中 / 区间外继续等 / `base==0`·`pc<base`·`pc` 未提交·`pc−base` 越上界 ⇒ 不满足不崩 / `FROM>=TO`·缺参数·非 hex ⇒ 解析期硬错 / 未知脚本名不在解析期校验 | 强：改半开区间或哨兵就红（`tools/mutate-check.mjs` 有一条第 ⑥ 条的代表变异） |

★ **一条要记住的分级**：如果一条守卫的"红"只是因为**编译不过**（断言用到新增 API），
它并没有证明**运行期行为** ⇒ 必须在报告里标成"证明力弱"，⛔ 不许与运行时红并列。

## 6. 已做 / 未做

- **已做**：S1（投递层 + 11 读点，行为逐字节不变）· S4（AOT `--input-plan` + 一行摘要）·
  S2′（flush-before-read + 待抬起 + `releaseAll`）· **三条指令**（`await script` 电平门 +
  `wait` 宿主单调毫秒 + `click` 1280×720 参考系缩放；删 `key` / `presents` / 图语法）·
  **第四条 `await position`**（脚本内位置电平门，`header_len + (pc−base)`，半开区间；见 §2.6/§2.8）。
- **未做**：`GetMessageA` 空队列由计划供给 · 两臂共有的事件 emit 点（`ScreenSample` 等）· 旧路径删除 ·
  `ScreenChanged` / `ScreenRect` / `click center` · 键盘（`key` 已删，需要时再加） · 解释器臂接 `--input-plan` ·
  **`await position` 的区间选取**（哪些脚本的哪一段"点击会被消费"只解了 TITLE；其余脚本要按 §2 的方法逐支解）。
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
# ⛔ 正例（TITLE 之后点击）不要用 `wait` 凑：同一 `wait` 12 跑 4 成。

# 位置门（2026-11-16 实测）：轮询体区间 ⇒ fired=1 但 region = 基线（3/3 相同）
printf 'await position TITLE 0x039C 0x0460\nclick 1070 480 left\n' > /tmp/pos-loop.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/pos-loop.plan
# input-plan: steps=2 fired=1 presents=400 released=1 held=0
# region    : arena in_use=108595152 peak=108595152 surfaces=21 heap_blocks=6675  ← 基线，不是"点击成功"

# 同一 click，门取回调的输入读段 ⇒ region 变成 117490928/25/6821（3/3 相同）
printf 'await position TITLE 0x0460 0x0520\nclick 1070 480 left\n' > /tmp/pos-cb.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/pos-cb.plan
# input-plan: steps=2 fired=1 presents=400 released=1 held=0
# region    : arena in_use=117490928 peak=117490928 surfaces=25 heap_blocks=6821

# 真正的负对照（区间永不执行 / 名字不匹配）⇒ fired=0 + 基线（3/3 相同）
printf 'await position TITLE 0x388C 0x38A4\nclick 1070 480 left\n' > /tmp/pos-neg.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/pos-neg.plan
# input-plan: steps=2 fired=0 presents=400 released=0 held=0
# region    : arena in_use=108595152 peak=108595152 surfaces=21 heap_blocks=6674
```

★ `region` 三数的稳定面：`in_use` / `surfaces` 逐位稳定；**`heap_blocks` 长跑会漂**
（短跑 6674 / 6675、长跑 6821 / 6822 都出现过，原因未查）⇒ 判据给它 **±1 容差**，⛔ 别把某一跑的
`heap_blocks` 当指纹。

读数口径（为什么用收尾 `region` 三数、⛔ 不用 `stop @` / `lastHash`）⇒ `observations.md` 的 `OBS-20261116-04`。
