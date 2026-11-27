# 组装输入：外部数据投递到 shim 输入分发点

> 状态口径：本文只写**不变量 / 落点 / 怎么查**。读数与历史进 `observations.md`；
> 缺陷进 `defects.md`（`BUG-26`）；进度真源 `pnpm tools requirements plan`。
> ★ **设备模型在 `input-device.md`**（`InputDevice` 的字段 / 转移表 / 租约 / 坐标链 / S9 注入路由）。
> 本文仍是**配方 / 投递**的入口：只写「一条指令怎么到达 guest 读点、完成度怎么判」。
> ★ **配方语法 / 闭集合 / 能力面的真源 = `apps/emulator-lift/src/runtime/recipe.h`**
> （`--input-plan` 与机器内驱动器 `InteractionGraph` **共用同一个解析器与同一个 step 模型**；
> ★ **能力面只有一份**：九条指令 + 行尾 `timeout` 每个驱动器都认，⛔ 没有 `kPlanFeatures` / `kGraphFeatures` 这种按入口
> 分叉的表，也没有"这个入口不认"的解析期硬错 —— 入口只决定**推进点**与**动作落在哪个宿主对象**上。
> ★ **2026-11-16：`--interaction-graph` 用户入口退役**（配方只经本臂）⇒ 本文的 `--input-plan` 就是
> 唯一入口；解释器臂只剩机器内驱动器（没有 CLI 参数能装载它）。
> ★ **2026-11-16（同日第二轮）：解释器侧的旧输入驱动 `--input-script` 与旧 bench 一并退役** ——
> 输入的用户面**只剩**本臂的 `--input-plan`。退役面 / 为什么 `InputScript` 类仍保留为机器内
> 动作落地面 / 重开条件见本文 **§7**。
> 本文只写"落到哪、怎么查"，⛔ 不在这里维护第二份语法表）。
> 现行口径由 `REQ-01M7HBG0NYH957M9EK3JVMF5CC` + 位置门裁决（设计经独立复审；早期的本地设计稿 /
> 复审稿在 `.tmp/aot/` 下，已删除，⛔ 不要照旧稿实现）。

## 0. 为什么这么做（口径来源）

用户的判断：**"最核心的错误是在解释循环里面迭代判断"**。输入是**外部数据**，应该类似**独立推进的线程**，
往 **shim 层的输入分发点投递**；内部判定点（等 script 之类）**由内部在对应位置显式向外投递事件**。
输入**不需要"特定 frame"、"到脚本后立即"**这种人工都做不到的精度 —— **只需要粗糙**。

## 1. 今天错在哪（改前，带落点）

- `src/runtime/machine.cpp` 的 `harness_script` 闸门 + 两处 `tick`：**每个基本块**都过一次；有脚本时再付一对 `GuestToken::YieldGuard`。
- ★ **错不在"扫得慢"**：`InputScript::tick` 本身已是 O(1)（几个聚合比较）。**错在调用点还在循环里。**
- ⛔ **不要用"那一段占墙钟 9–16%"当本单收益**：那是 `REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9` 里
  "窗口 profiler 开着 + 过渡帧 + host 桶占比"的口径，且只存在于**解释器 + `--input-script`**（该入口
  2026-11-16 已退役，见 §7；读数保留为历史口径）；**AOT 臂为 0**。

## 2. 设计（已裁决）

### 2.1 状态源 = `InputDevice`（`WindowManager` 成员）

唯一输入状态在 **`InputDevice`**（`runtime/input_device.{h,cpp}`；`WindowManager` 的私有成员
`device_`）。`InputPlan` **只经 `src/runtime/input.h` 的四个投递入口**到达 `WindowManager`，
由它经 `input::*` 施加到设备（转移表 / 租约 / 写后回读见 `input-device.md`）。落点
`src/runtime/input_plan.{h,cpp}`。

★ 不再有 v1 的三套 owner（`key_owner_` / `button_owner_` / `cursor_script_owned_`）与
`mouse_buttons_`：设备只有 `key_level_[256]` + `level_owner_[256]` + 单一 `lease_`
（S4）。`WindowManager` 保留消息队列 / 窗口表 / 计时器 / 无效区 / 采样计数 / 计划与 router 的
非拥有指针；⛔ 它不再持有第二份电平。

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

★ **写入点是另一张账**（别把「读点」与「写点」混成一个数）：当前是 **17 个 `input::*` 事件落点 +
5 个会话写**。计数口径（排除注释与 `input.h/.cpp` 自身的转发 / 定义）：

```bash
rg -n -U 'input::(mouseMove|mouseButton|mouseWheel|key)\(' apps/emulator-lift/src
```

- **事件落点**：SDL `present_sdl.cpp` 的键 / move / 按钮 / 滚轮 4 处；配方 / 控制层的
  `input_plan.cpp` 4 处（`armMoveAt` 的 move、`applyMouseButton` 的 down/up、`pressDue` 的 down、
  `emitRelease` 的 up —— 同一段核心被 click 与三条鼠标原语共用）；`InputScript` 的
  `apply` / `emitRelease` 9 处（`releaseOwnedInputs` 不直接调事件入口，它是会话写）。
- **会话写**：失焦（`present_sdl.cpp` → `input::dropOsLevels`）· 控制层恢复
  （`aot_host.cpp` 的 `controlOnResume` → `input::dropOsLevels`）· 配方收口
  （`closeSessionIfDone` / `releaseAll` → `input::releaseProgram`）· `InputScript` 收口
  （`releaseOwnedInputs` → `input::releaseProgram`，`tick` / `abandon` 共用这一处）。
- ⚠ **历史口径**：设计稿 §1.2 记的「18 个 `input::*` 落点」是 S4 之前的读数；
  `InputScript::releaseOwnedInputs` 里原有两个 `input::*` 已被 `input::releaseProgram` 取代 ⇒
  按当前代码是 17。⛔ 不要把 18 当现状写进新文档。

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

- ★ **一个读点最多一条写动作**（S7）：`releaseDue` → `pressDue` → 立即队列 → 配方当步四段
  共用 `writes_this_point_`；本读点已经写过一条，后面的**写** step 留在 `next_` / 队首，但门 /
  `wait` / `profile_*` / `quit` 仍可继续消费（它们不是写）。**同一按钮还按着**时后一条
  `click` / `mouse_down` 不消费（连点串行化，⛔ 不再「同按钮先抬起」）。细节与代价见
  `input-device.md` §5。

### 2.6 确定性：门只用 guest 可见状态；**`wait` 是唯一的墙钟面**

- 门（`await script`）只读**引擎脚本身份**（`MarkerSampler` 纯读 guest 内存；`base==0` / `valid=false`
  ⇒ 门不成立、继续等，⛔ 不报错）。
- ★ **`wait <ms>` 例外且显式**：它用**宿主单调时钟**（`std::chrono::steady_clock`；⛔ 不是
  `Clock` = guest 墙钟的读数口径）。**非阻塞**：只记 deadline，到期由**下一个读点**消费
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
  时钟唯一档 = 墙钟 ⇒ `wait` 是配方里唯一读时间的面（其余门都只读 guest 状态）。
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

**只有 `await script` 与 `await position`（都是电平门）** —— 这个臂能表达的门就这两条。
画面源（`screen hash` / `screen rect … avg` / `screen changed` / `always`）与旧节点模型
（`node` / `when` / `action` / `next` / `expect`）在 2026-11-16 随顺序模型收敛一起退役
（理由与判据 = `interaction-model.md` §5）；现在写进配方 = **未知指令**（解析期硬错）。
`await presents <n>` / `key <VK>` 同样是硬错（下面两条是"为什么不做"的记账）。
- 为什么删 `Presents`：它是**帧号门**，人无法评估；同一个 N 在两臂不对应同一 guest 时刻（`BUG-26` 候选①）。
- ★ 陷阱（留下当"为什么不做"）：`Reads` 的刻度**不是"读点次数"** —— `inputSampleCount` 只在
  `GetCursorPos` / `GetAsyncKeyState` 自增，后者在这条 UI 路径上一次也没被调用 ⇒ 实际是
  `GetCursorPos` 的 20 Hz；且同一次迭代常有多个读点 ⇒ "after 2 reads" 可能只是**一次**迭代。

### 2.8 配方格式：共享语法的**一份**（真源 = `runtime/recipe.h`）

> 本节只列**落到哪**；⛔ 不要把 `recipe.h` 的词表抄成第二份真源。
> 图那套词（`screen …` / `node` / `when` / `next` / `expect`）已退役 ⇒ 现在写进配方是未知指令。
> ★ **九条指令 + 行尾 `timeout` 本臂都认、都有落点**（2026-11-16 能力面统一；S6 追加三条鼠标原语；
> `timeout` / `profile_begin` / `profile_end` / `quit` 不再是"这个入口不认"）：落点表见下面那段，
> 语义真源仍是 `recipe.h`。

```
# 注释（整行 / 行尾都吃）；空行忽略
await script TITLE timeout 30000      # 电平门：身份 == TITLE；行尾 `timeout` = 这个门的守卫
await position TITLE 0x039C 0x0460 timeout 5000
                        # 电平门：script == TITLE **且** FROM <= pos < TO（半开区间；FROM/TO 是带 0x 的 hex）
wait 500                # 宿主单调时钟毫秒；正整数。★ 固定时长 ⇒ ⛔ 不带 timeout
click 1070 480 right    # x/y = 1280×720 参考系；按钮词裸写（省略 = left；⛔ 不收 =left）
mouse_move 1070 480     # 只写光标（与 click 的第一段同一段代码 / 同一参考系 / 同一缩放）
mouse_down left         # 用**当前设备光标位置**按下；必须有一条更早的 mouse_move；
                        # 轻闸门 = 那条 move 之后完成过一次 CursorPos 读点，或墙钟兜底
mouse_up left           # 用**当前设备位置**抬起（可与 down 不同点 ⇒ 拖拽 up 不回跳）
```

- **按文件顺序执行**；`click` 分三段（arm → press → release），**一个读点最多一条写动作**；
  同一按钮还按着时后一条 `click` / `mouse_down` 不消费（连点串行化；见 §2.5 与
  `input-device.md` §5）。
- **`mouse_move` / `mouse_down` / `mouse_up` 是写动作**：一条一次、在读点消费时直写；`click`
  仍是「写动作 + 四条闸门」的复合（① KeyLevel 读点 · ② 墙钟兜底 · ③ 按下侧 CursorPos 读点 ·
  ②' **回调边界** `frames >= press_frames + 2`，见 `input-device.md` §5）。拖拽 = `mouse_move` → `mouse_down` → `mouse_move` × N →
  `mouse_up`，每条一个读点 ⇒ guest 每一拍看到新的 `WM_MOUSEMOVE`（MK 位 = 设备电平的投影）。
- **`wait` 的起点** = 上一条指令生效的那个读点 ⇒ 有 0..一个迭代周期的偏差（**不可评估**）。
- ★ **`timeout <ms>` 是行尾可选项，而且只允许出现在 `await script` / `await position` 上**：
  它守的是**同一行**那个门（这些门可能永远等不到）。本臂**只有一把尺 = 宿主墙钟**
  （`stepTimeoutMs` 给宽度、`stepWindowExpired` 判到期；"刚好等满"还不算到期 = 严格大于）。
  到期 ⇒ 记下是哪一条 ⇒ **exit 5** + stderr 点名（见 §2.9）。
  · ⛔ **单独一行写 `timeout`**（旧的跨行写法）⇒ 解析期硬错，文案直接说"写到它守的那一行末尾"；
  · ⛔ **`wait` / `click` / `profile_*` / `quit` 带 `timeout`** ⇒ 解析期硬错（"没有可等待的东西"）：
    `wait` 是固定时长、到点就走，`click` 等一到就施加 —— 它们**不需要**守卫。
  ★ **`wait` 没有守卫**（`stepTimeoutMs` 对它恒返回 0）：2026-11-16 删掉了旧的 `ms + 缺省` 补丁
  （那份补丁是图节点上的 timeout 被拍平到 `wait` 上时拖进来的；固定等待不需要"等它成立"）。
  卡住由 `--time-limit` / `--max-frames` 这层看门狗报。
  ★ **本臂不给缺省窗口**（0 = 没写 `timeout` 就没有守卫）：本臂的看门狗是 `--time-limit` /
  `--max-frames`，而且"等 TITLE"这种门的耗时在 headless 下与解释器臂不是一个量级
  ⇒ 一个固定缺省值在两臂含义不同。解释器臂的缺省是 5000 ms（它有两把尺）。
  ⛔ 这不是能力面分叉：**显式 `timeout` 在两臂语义相同**，差别只在入口给的缺省策略
  （`recipe.h` 的 `stepTimeoutMs` 是唯一一份公式）。
- ★ **`quit` 在本臂的落点**：`WindowManager::postQuit(0)`（与解释器臂**同一个**交付点 ——
  `Machine::pumpScript` 消费 `quit` 时也是投 WM_QUIT）+ **宿主停止请求**
  （`AotRunOptions::stop_requested`，run 循环在下一个块边界收口成 `Stopped`/`detail=input-plan: quit`）。
  ⛔ 不等 guest 自己退出：实测 WM_QUIT 之后 guest 会走进**闭包外**的停机路径（停在 `00513B90`
  ⇒ `decode-error`、exit 2），那与"quit 是正常结局"冲突。`quit` 之后的指令**不再推进** ⇒
  末尾不是 `quit` 的配方会留下 `pending != 0`（= 任务没走完 ⇒ 5）。
- ★ **`profile_begin|end <label>` 在本臂的落点**：两件事，同一次收口里出。
  1. **宿主墙钟区间读数**（既有）：begin / end 各一行（`label` + `wall_ms` + `reads`）。
  2. **窗口桶表**（2026-11-17 起，`REQ-01M7JNX5MR7RWTBNBAE70PBCQP`）：`end` 之后打一张
     `=== profile window: <label> ===` 的表（`wall / lift / boundary / shim（+ present / shim:名 子行） /
     partial / instrument / unaccounted / coverage / lift_dyn / verdict`）。口径见 `measurement.md` **M-12**。
     ★ 它**不依赖 `--profile`**（窗口自己驱动）⇒ 不开窗 = 一行不打、一次取时都不做。
  ⛔ 两件事都**不是**解释器 `WindowProfiler` 的分项（那套来自 `BlockCache` / `Iterator` / 派发循环，
  AOT 一个都没有）—— AOT 的桶是**重新定义**的（`interp → boundary`、**没有** `native`）。
  `end` 没有配对的 `begin` ⇒ 一行 stderr 提示、**不判失败**（仪器问题不是任务失败）。
- ⛔ **解析期硬错**（`exit 2` + `文件:行号: 原因`，**只剩两类**）：
  · **两臂都不认**：`await always` · `await presents <n>` · `key <VK>` · 旧节点模型的 `node / when /
    action / next / expect` · 画面源 `screen …` · 任何别的词（报"未知指令"并回显那行）；
  · **`timeout` 写错位置**：单独成行（旧跨行写法）· 挂在 `wait` / `click` / `mouse_move` /
    `mouse_down` / `mouse_up` / `profile_*` / `quit` 上 · 不在行尾 · 缺毫秒数 / 非数字 / 0
    （文案点名行号并给出修法）。
  缺参数 / 负毫秒 / `=left` / `mouse_down`·`mouse_up` 前没有 `mouse_move`（或未知按钮）同样硬错。
- ★ **只写门 / 只写 `wait` 是合法的**（线性指令流）⇒ 旧的"同一份计划去掉动作表达不了"
  这一限制**不再成立**：现在可以直接给一份"只等门"的配方当对照。
- ⛔ 解释器臂**不接** `--input-plan`（给它 = `未知参数` + exit 2）。
  两个驱动器**共用同一份解析器与同一个 step 模型**（`runtime/recipe.{h,cpp}`），差别只有**入口**与**推进点**：
  本臂在 guest **读点**推进（`InputPlan::atPoint`），机器内驱动器在**块边界**推进（`InteractionGraph::tick`）。
  ★ 2026-11-16：`--interaction-graph` 已退役 ⇒ **本臂是唯一的配方入口**。
  ★ **同一个文件两臂都能跑**（能力面只有一份）：差别只剩推进点 / 动作落点 / 缺省 `timeout` 策略
  （本臂不给缺省，解释器臂 5000 ms）。

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

### 2.9 **完成度是判据**（`pending != 0` / `timeout` 到期 ⇒ exit 5）

- 跑完打一行**完成度**读数（一次性，位置在 `input-plan:` 前缀后）：
  `input-plan: steps=N fired=M pending=K presents=… released=… held=…`
  ⇒ 读这一行的最短形式就是 `steps= fired= pending=`（旧字段一个都没动）。
- ★ **S9**：只有**真的吞过**注入 click 时才在这一行末尾追加 ` ui_consumed=N`
  （= `InputPlan::uiConsumed()`；一条 click 的 Down 那一瞬 +1，up 不重复数）。没有叠层 /
  没有命中 / `--ui-input=pass` ⇒ **这一行逐字不变**（默认档仍是既有判据面）。
  ⛔ 控制层协议里的 `consumed_by_ui` 字段**未做**（见 §6）；这里只有 `InputPlan` 的摘要字段。
- ★ **判据**（`recipe.h` 的 `RecipeOutcome` / `recipeOutcomeExitCode`，**唯一一份**：
  `--input-plan` 与机器内驱动器都对同一个 5 口径）：
  · `pending == 0` ⇒ `Done` ⇒ **不覆盖**运行结论（run 自己失败仍然 2）；
  · `pending != 0`（跑完了还有指令没消费）⇒ **exit 5** + stderr 点名**第一条没走完的**指令；
  · 某步 `timeout` 到期 ⇒ **exit 5** + stderr 点名**到期的那一条**。
  ⛔ 不许只打一行警告仍 exit 0 —— 那正是"图 → 配方"改写会把 CTest 变成假绿的那条缝
  （ADR `REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的**前置**）。
- 报错形状（`InputPlan::failureReport()`；`plan:` 行 = `文件:行号`）：

  ```text
  input-plan: FAIL timeout
    plan     : /tmp/feat/to.plan:1
    step     : step1（行 1，门）
    waiting  : await script NOSUCHSCRIPT
    window   : 守卫 2000 ms（宿主墙钟；本臂只有这一把尺）已等 2001 ms ⇒ 到期
    progress : steps=2 applied=0 pending=2 fired=0 reads=1898273
  ```

  `FAIL incomplete（配方没有走完）` 是同一条报告的另一个头（没有 `window` 行，`说明` 换成
  "跑完时这条还没有成立 / 没有轮到它"）。
- ★ **反例/正例读数**（2026-11-16，macos-arm64 RelWithDebInfo，`--present headless --skip-logo
  --region-log --max-frames 400`，每次全新 `--appdata`；命令见"怎么查"）：

  | 配方 | 改前 | 改后 |
  |---|---|---|
  | `await position TITLE 0x388C 0x38A4` + `click 1070 480` | `fired=0` + **exit 0** | `steps=2 fired=0 pending=2` + **exit 5** + 点名 `plan:1` / `step1（行 1，门）` / `await position TITLE 0x0000388C 0x000038A4` |
  | `await position TITLE 0x0460 0x0520` + `click 1070 480 left` | exit 0 · `fired=1`（`region in_use=117490928 / surfaces=25`） | **不变**：exit 0 · `steps=2 fired=1 pending=0` · 同上 region（`heap_blocks=6821`） |
  | `await script NOSUCHSCRIPT timeout 2000` + `click 1070 480` | （改前 `timeout` = **exit 2**「这个入口不认」） | **exit 5** · `FAIL timeout` · 守卫 2000 ms / 已等 2001 ms · 点名 `step1（行 1，门）`（⛔ 不是靠 `--time-limit`） |

## 3. 落点地图

| 件 | 位置 |
|---|---|
| **设备实体**（唯一电平 / 单一租约 / 转移表 / 坐标链；`WindowManager` 私有成员） | `src/runtime/input_device.{h,cpp}`（`InputDevice`；合同见 **`input-device.md`**） |
| **配方解析器 + step 模型 + 门判定**（唯一一份，两臂共用；语法真源） | `src/runtime/recipe.{h,cpp}`（`Recipe` / `Step` / `stepGateHolds` / `scriptNameMatches`） |
| **AOT 臂的读点驱动**（顺序推进 + 施加 click / 鼠标原语 + 待抬起 + 计数 + 摘要） | `src/runtime/input_plan.{h,cpp}`（`atPoint` 是唯一施加口） |
| **解释器臂的块边界驱动**（同一份 `Recipe`；动作经 `InputScript` 追加；**没有 CLI 入口**，只剩 pure 夹具） | `src/runtime/interaction_graph.{h,cpp}` |
| **注入 click 的叠层路由**（S9；`UiInputRouter` / `--ui-input` 值域） | `src/runtime/ui_input.h`；实现 = `SdlWindow::Impl`（`present_sdl.cpp`） |
| 参考系 → 客户区的**唯一**映射 | `src/runtime/cursor_map.{h,cpp}`（`referenceToClientPixel`） |
| 名字口径（大小写 / 两侧 `.bin` / 退化名） | `src/runtime/recipe.{h,cpp}`（`scriptNameMatches`） |
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
| ④ | CLI 面：`--input-plan` 缺文件 / **旧指令硬错** / 缺参数 / 负毫秒 / `=left` / `mouse_down`·`mouse_up` 前没有 `mouse_move` / **`timeout` 写错位置**（跨行 · 挂在 `wait`/`click`/`mouse_*`/`quit` 上 · 不在行尾 · 缺毫秒数/非数字）⇒ `exit 2` + 点名行号 + 文案指向修法；缺省档不出现 `input-plan:` 行 | 强（⚠ `cli-aot-flags` 在非 Windows 是 SKIP） |
| ⑤ | 语义面（CTest `input-plan-read-points`）：`await script` 电平门 + 身份读不到继续等 + `wait` 注入时钟（未到期不施加 / 到期施加，不需要真睡）+ click 抬起的**两条腿**（trace 的 `reason=keylevel` / `wallclock` / `keylevel+wallclock`）+ 1280×720 → 客户区缩放且 move/down/up 同一结果；S6 的子用例 `input-plan/mouse-primitives` · `input-plan/drag` · `input-plan/drag-repeat` | 强：改语义就红；`reason=` 口径与 `aot_main` 的 `--help` 必须一致 |
| ⑥ | 位置门（同一 CTest 的 `input-plan/position-gate`）：`pos == FROM` 命中 / `pos == TO` 不命中 / 区间外继续等 / `base==0`·`pc<base`·`pc` 未提交·`pc−base` 越上界 ⇒ 不满足不崩 / `FROM>=TO`·缺参数·非 hex ⇒ 解析期硬错 / 未知脚本名不在解析期校验 | 强：改半开区间或哨兵就红（`tools/mutate-check.mjs` 有一条第 ⑥ 条的代表变异） |
| ⑦ | **完成度判据**（同一 CTest 的 `input-plan/completion`）：门永不成立 ⇒ `outcome()==Running` + `pending==2` + `recipeOutcomeExitCode()==5`；全走完 ⇒ `Done` + 0 + 空报告；`Done` 时 `failureReport()` 为空串 | 强：**这就是"图 → 配方"改写会不会把 CTest 变成假绿的唯一闸门**（`tools/mutate-check.mjs` 有一条"完成度 ⇒ 恒 0"的代表变异） |
| ⑧ | **能力面只有一份**（同一 CTest 的 `input-plan/step-timeout` + `input-plan/profile-and-quit`）：行尾 `timeout` 差 1 ms 不判 / 超 1 ms 判 + 报告点名 `mem:<行>` + 门；`wait` **没有守卫**（超长 `wait` 冲过 guest 时钟也不判 timeout）；`quit` ⇒ 队列尾 WM_QUIT + 不再推进；`profile_begin|end` ⇒ 收口一次、孤儿 `end` 不失败；**九条指令** + 行尾 `timeout` 在**本臂**解析器上都被接受 | 强：谁把某条指令写成"这个入口不认"就红（含 `timeout` 的落点与 `wait` 无守卫两条代表变异） |
| ⑨ | **S7 写预算 + 连点串行化**（CTest `input-plan-read-points` 的 `input-plan/one-write-per-read-point` · `input-plan/control-queue-one-write` · `input-plan/two-clicks-serialized`；回归 `input-plan/gates-and-releases` / `input-plan/completion`；控制层 `lift-control-endpoint` 的 `click-enqueue-at-read-point`） | 强：同一读点出现两条写 / 两条 click 被合并成 `DOWN,DOWN,UP,UP` 就红 |
| ⑩ | **S9 注入路由**（CTest `input-plan-read-points` 的 `input-plan/click-consumed-by-ui` / `input-plan/click-router-miss-identical`；解析面 `overlay` 的 `overlay/ui-input-parse`；CLI 面 `cli-aot-flags` 的 `ui-input-values`，⚠ 非 Windows SKIP 不作唯一判据） | 强：吞掉却仍调用 `input::*` / down 吞 up 不吞 / 未命中却改消息 / `pass` 不是逐位相同 / 非法值静默成 consume ⇒ 红 |

★ **一条要记住的分级**：如果一条守卫的"红"只是因为**编译不过**（断言用到新增 API），
它并没有证明**运行期行为** ⇒ 必须在报告里标成"证明力弱"，⛔ 不许与运行时红并列。

## 6. 已做 / 未做

- ★ **输入重设计的 S1–S9 都已落**（实现判据以代码 / CTest 为准，逐条见 `input-device.md`）：
  - **S1 设备实体**：`runtime/input_device.{h,cpp}`；`InputDevice` 是 `WindowManager` 的私有成员，
    `WindowManager` 保留同名 setter / getter 与 `checkMainThread` 站点名；死状态
    `setCapture/capture` 删除。
  - **S2 消息 = 设备的投影**：`input::*` 写后回读 `wParam/lParam`；`Script` 分支检查写返回值。
  - **S3 按钮位图 = VK 电平的投影**：`mouse_buttons_` 已删，`mouseButtons()` / `buttonDown()`
    从 `key_level_` 派生。
  - **S4 单一租约 + `Driver::Guest` + 转移投影**：三套 owner 删除；`releaseScriptOwnership` /
    `clearOsInput` 删除；收口改 `input::releaseProgram` / `input::dropOsLevels`，**逐条 post up**；
    OS up 的 #8/#9/#10 与 Program up 的 #15/#18 按转移表；键盘 repeat 是显式例外。
  - **S4 follow-up**：`Script` 的 **up 不再先移动光标**（否则 Program up 的 #15/#18 不可达）；
    up 用**设备当前位置**，见 `input.cpp` 的 `mouseButton`。
  - **S5 overlay 指针改读设备**：hover 走 `device client → imageToWindowPixel → window 点 →
    pointerWindowPixels → window 像素` 的 SDL-free 纯函数；`SDL_GetMouseState` 不再参与。
  - **S6 配方原语**：`mouse_move <x> <y>` / `mouse_down [left|right]` / `mouse_up [left|right]`
    （能力面仍是 `recipe.h` 一份；`InteractionGraph` 同步执行）；`--help` 同一张表。
  - **S7 一个读点最多一条写动作 + 连点串行化**：`writes_this_point_` 覆盖
    `releaseDue` / `pressDue` / `drainImmediate` / flush；`armClickAt` 不再强制抬起；
    同一按钮还在 `pending_` 时后一条不消费。
  - **S8 文档切片**：`input-device.md`（设备合同）+ 本文 + `interaction-model.md` +
    `overlay.md` + `contracts-win32.md` 的已实施口径。
  - **S9 `--ui-input=consume|pass`**：注入 click 的 down/up 与 OS 入口问**同一个**
    `UiClickRouter`（实现 = `SdlWindow::Impl`）；缺省 consume；摘要只在非 0 时追加
    `ui_consumed=N`。
- **S9 residual（明确未做，⛔ 不是"顺手会做"）**：
  - `mouse_move` / `mouse_down` / `mouse_up` **原语不过 router**：它们是"手"而不是"一次点击"；
    接上去会改拖拽 / 显式 hold 的语义（`input_plan.h` 头注也写了这条）。
  - 控制层回执里的 `consumed_by_ui` **未做**：当前只有 `InputPlan::uiConsumed()` 与摘要行
    `ui_consumed=N`（控制层协议 `consumed_by_ui` 仍在 `overlay.md` 的 M3 待办里）。
- **未做**（其余）：`GetMessageA` 空队列由计划供给 · 两臂共有的事件 emit 点（`ScreenSample` 等）·
  旧路径删除 · `ScreenChanged` / `ScreenRect` / `click center` · 键盘 `key`（已删，需要时再加）·
  解释器臂接 `--input-plan` · **`await position` 的区间选取**（哪些脚本的哪一段"点击会被消费"
  只解了 TITLE；其余脚本要按 §2 的方法逐支解）。
- **两臂分歧**（打开中）：`defects.md#BUG-26` —— 同一点击在 AOT 生效、在解释器无反应。
  ★ 这条不查清，所有"解释器能跑通 / AOT 跑不通"的结论都要重打折扣。

## 怎么查

```bash
node tools/requirements.mjs --plan                 # 进度真源（父单 REQ-01M7G7EJF062NKQ0D40RF3BZ4Z）
cd apps/emulator-lift && ctest -R 'input-plan|cli-aot-flags' --output-on-failure
cd apps/emulator-lift && ctest -R 'input-injection|input-inject-visible|overlay|ui-click-router|lift-control-endpoint|cursor-warp' --output-on-failure
node tools/mutate-check.mjs --match 'input-plan-read-points'   # 变异自检（wait deadline 那两条）
node tools/mutate-check.mjs --match '完成度'                    # 变异自检（pending != 0 ⇒ exit 5 那条）
node tools/mutate-check.mjs --match '行尾 `timeout` 的落点'      # 变异自检（行内 timeout 被忽略）
node tools/mutate-check.mjs --match 'wait` 恒 0'                # 变异自检（wait 又拿到缺省守卫）
node --test --test-isolation=none tools/test/input-plan-shape.test.mjs
```

**机械复核（不靠肉眼）**：

```bash
# 设备状态只剩一份（S1/S3/S4）：实现文件里 0 命中（头注释提到旧名不算）
rg -n 'cursor_x_|key_down_|mouse_buttons_|key_owner_|button_owner_|cursor_script_owned_' \
  apps/emulator-lift/src/runtime/host_window.cpp

# 旧收口入口已删（S4）：0 个**调用点**（注释里提到旧名不算）
rg -n '\.releaseScriptOwnership\(|->releaseScriptOwnership\(|\.clearOsInput\(|->clearOsInput\(' \
  apps/emulator-lift/src apps/emulator-lift/tests

# 写入点两张账（口径见 §2.3）：看实际落点，不要手抄过期计数
rg -n -U 'input::(mouseMove|mouseButton|mouseWheel|key)\(' apps/emulator-lift/src

# S9：注入 click 的两端都在 `InputPlan` 里问 router；摘要只在非 0 时追加
rg -n 'routeInjectedPointer|uiConsumed|ui_consumed' apps/emulator-lift/src/runtime/input_plan.cpp
```

★ **完成度判据的端到端读数**（下面三段命令的实测输出见 §2.9 的表；`--max-frames 400` 只是让 run
自己停下 —— 判据是 `pending` / `timeout`，⛔ 不是 `--time-limit`）：

```bash
cd apps/emulator-lift
# 反例：永不执行的区间 ⇒ pending=2 ⇒ exit 5 + 点名（改前是 fired=0 + exit 0）
printf 'await position TITLE 0x388C 0x38A4\nclick 1070 480\n' > /tmp/neg.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --region-log --max-frames 400 --input-plan /tmp/neg.plan
# input-plan: steps=2 fired=0 pending=2 presents=400 released=0 held=0
# stderr: input-plan: FAIL incomplete（配方没有走完） / plan: …/neg.plan:1 / step: step1（行 1，门）
#         waiting: await position TITLE 0x0000388C 0x000038A4        ⇒ exit 5

# 每步超时：`timeout` 写在它守的那条 `await` 的**行尾**（改前这里连解析都不过 = exit 2「这个入口不认」）
printf 'await script NOSUCHSCRIPT timeout 2000\nclick 1070 480\n' > /tmp/to.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --max-frames 400 --input-plan /tmp/to.plan
# input-plan: FAIL timeout / window: 守卫 2000 ms…已等 2001 ms ⇒ 到期 / step: step1（行 1，门） ⇒ exit 5

# 位置门 + 行内 timeout（同一个门两种条件：script + 区间）
printf 'await position TITLE 0x0460 0x0520 timeout 5000\nclick 1070 480\n' > /tmp/pos.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --max-frames 400 --input-plan /tmp/pos.plan

# `wait` 是固定时长、**没有守卫** ⇒ 超长 wait 不会被判 timeout
printf 'wait 20000\nquit\n' > /tmp/longwait.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --max-frames 400 --input-plan /tmp/longwait.plan

# 能力面：`quit`（末尾 ⇒ Done，进程按预期提前收口）与 `profile_begin|end`
printf 'await script TITLE timeout 30000\nwait 3000\nquit\n' > /tmp/quit.plan
./out/emulator-lift-aot --present headless --skip-logo --game-dir ../../links/install \
  --appdata "$(mktemp -d)" --max-frames 400 --input-plan /tmp/quit.plan
# [input-plan] quit：已投 WM_QUIT … / input-plan: steps=3 fired=0 pending=0
# stop: stopped … — input-plan: quit     ⇒ exit 0（帧号停在 197/400 ⇒ 真的提前收口）
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

## 7. 退役：解释器侧旧输入驱动 + 旧 bench（2026-11-16）

**裁决（用户）**：旧 bench（解释器侧）**不需要了**；真要做测试也**重新写**。⛔ **不要为旧 bench 给配方补
`at frame/insn/marker`**。登记单：`REQ-01M7HQ57XV66FZQA5ECGHWD71N`（+ 旧 bench 的裁决单
`REQ-01M7HSXV7NP1ANJP5Q5TSM78A1` 的 `dropped_reason`）。

**退役面（已删，逐条可查 `git log -S`）**：

| 面 | 处置 |
|---|---|
| `emulator-lift --input-script <path>` + 时间线整族 CLI（`--anchor-script` / `--input-stall-ms` / `--input-stall-boot-ms` / `--input-max-hold` / `--input-marker-timeout-ms` / `--expect-change-after` / `--expect-change-window`） | **删**（`src/main.cpp`）；给了 ⇒ 一行"已退役" + exit 2（⛔ 不静默忽略） |
| `Machine::loadInputScript(path)`（文件装载入口） | **删**（CLI/旧 bench 是唯一调用者） |
| `pnpm tools lift bench`（`tools/lift.mjs` 的 op）+ `apps/emulator-lift/scripts/bench/*.txt` + `tools/test/lift-bench.test.mjs` | **删**（旧 bench 只驱动解释器臂；`report --bench N` 的合成循环探针**不是**它，保留） |
| `docs` / `README` §4.2c 的"怎么用 `--input-script`" | 改成"已退役 + 重开条件"（本节） |

**保留的最小面（⛔ 不是"兼容层"）**：`runtime/input_script.{h,cpp}` + `Machine::input_script_` 的
tick/pump/expect 机械**没有删**，因为两处**保留消费者**结构上依赖它：

1. **交互配方驱动器的 actuator**（`InteractionGraph::actuator()`——`recipe.h` §"入口只管推进点"）：
   配方动作经 `InputScript::appendText` 追加成 `at frame <当前+1>`，由 `InputScript` 保证
   "一次按下一定被 guest 观察到"（自动 hold / 延迟抬起 / profiler 窗口请求 / `quit` 唤醒）。
   删它 = 把自动 hold 与 profiler 边界重写一遍（本轮边界外）。
2. **pure CTest 夹具**（`input-tick` / `input-injection` / `input-expect` / `input-unfinished` /
   `input-marker` / `input-anchor` / `profile-window` / `insn-cost` / `messagebox`）：它们用
   `Machine::loadInputScriptText` 造确定性动作序列（`at insn N` 可在帧内切窗口），
   这是裸 `Machine` 上唯一的时间锚。逐个依赖见 `.tmp/graphfix/report-idescript.md`。

⇒ **没有 CLI 入口**：本类的任何能力都不能再从命令行触达；`tickInputScript` 只在
`Machine::run` 内部（夹具跑到装载过的脚本时）生效。⛔ 新需求不许把它挂回 CLI。

**重开条件（写在这里，不写进 data/）**：

1. 性能测量只在 **AOT 侧**重写（`REQ-01M7HQJEEENYDJ91FB5DAC4K23`）：boot→TITLE 稳定 / 打开 FELLOW /
   加载存档 三档，驱动用**配方的确定性门**（`recipe.h`），⛔ 不用 `at frame/insn/marker`。
2. 如果将来 AOT 压力档确实需要"帧内时间戳"（如逐段吞吐），先立**新单**并给出"为什么配方门不够"的
   判据 —— ⛔ 不复活 `--input-script`。
3. 解释器臂本身在退役排期里（同单 §退役顺序）：删解释器时这段最小面随之删，届时连 CTest 一起收口。
