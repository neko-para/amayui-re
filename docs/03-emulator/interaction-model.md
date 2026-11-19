# 交互模型：任务 = 一串**顺序**配方（门 → 动作 → 下一个门；失败全部响亮）

> ★ **真源分工**：**行写法 / 闭集合 / 能力面** = `apps/emulator-lift/src/runtime/recipe.h`
> （本文⛔ 不复述语法 —— 两份 schema 必漂，见 `AGENTS.md` §10）。本文只写**模型 / 判据 / 确定性口径**。
> 需求单 `REQ-01M6XVYY5TCKSAS80WS7PADF8P` 只留"判据 + 进度"，`apps/emulator-lift/README.md` §4.2d
> 只留入口与退出码。
> ★ 本文不写 EA / 偏移 / 内部函数名（那些属于 `design-notes.md` 与需求单）：位置读数的 EA 锚在
> `runtime/engine_marker.h` 的头注里。

## 0. 一句话

**任务 = 一串按文件顺序推进的指令**。两条指令是**门**（`await script` / `await position`），其余是动作
（`click` / `mouse_move` / `mouse_down` / `mouse_up` / `wait` / `profile_*` / `quit`）；失败**响亮**。

## 1. 模型（唯一一套）

- **顺序**：从第一条未消费的指令开始，遇到不成立的指令就停（⛔ 不越过它）。
  ⇒ 没有 `next` / 条件分支 / 候选轮询 / 回跳：**"线性"不是语法糖，而是模型本身**。
- **门（两个，都是电平）**：
  - `await script <名字|id>` = **引擎脚本身份**（marker，纯读 guest 内存）。判据是"当前身份 == X"，
    读**最近一次** VM 循环头采样 ⇒ "已经在这一屏"也成立（这就是"等一屏 → 点一下"能自足写出来的前提）。
    名字口径：能解析成 `u32`（十进制 / `0x`）⇒ 按 id；否则按名字（ASCII 大小写不敏感、**两侧**都忽略
    `.BIN` 后缀、`id:0x%08X` 退化名始终接受）。
  - `await position <名字|id> <FROM> <TO>` = 上面那条 **且** `FROM <= pos < TO`（**半开区间**，TO 不含）。
    `pos` = 脚本体内**绝对字节偏移**（`header_len + (pc − base)`），与 `dist/views/data/*.txt` 的
    `label_%08x` 同一坐标系；读数只有一份（`runtime/engine_marker.h` 的 `readScriptPositionSample`）。
- **为什么需要位置门**：`await script TITLE` 只保证"当前帧是 TITLE 这个脚本"，**不保证它的输入回调已就绪**。
  实测（`observations.md` 的 `OBS-20261116-05`）：TITLE 上"点得进去"的窗口落在回调体的输入端，
  而轮询体里同一个门也命中、点击却**不被消费** ⇒ "输入循环在跑" ≠ "点击会被消费"。
- **动作**：`click <x> <y> [left|right]`（1280×720 参考系，投递前过 `cursor_map.h` 的唯一映射）·
  `mouse_move <x> <y>`（同一参考系；只写光标，与 `click` 的第一段同一段代码）·
  `mouse_down [left|right]` / `mouse_up [left|right]`（坐标 = **设备当前位置**，⛔ 不带 x/y；
  拖拽 up 不回跳；`mouse_down` 有轻闸门：上一条 `mouse_move` 之后完成过一次位置读点，或墙钟兜底）·
  `wait <ms>`（**宿主墙钟**，非阻塞 deadline）· `profile_begin|end <label>` · `quit`（立即收口）。
  ★ **配方能写拖拽，但 HUD 路由只接 `click`**：`mouse_move` / `mouse_down` / `mouse_up` 原语
  **不过** `UiClickRouter`（S9 residual；接上去会改拖拽 / 显式 hold 的语义）。HUD 的命中与捕获
  只对 `click` 和 OS 事件生效。
- **`timeout <ms>`**：**行尾可选项**，只允许出现在 `await script` / `await position` 那一行的末尾
  （它守的就是**同一行**那个门；⛔ 旧的"守上一条指令"跨行写法已删除，单独一行写 `timeout` = 解析期硬错）。
  缺省由**入口**给（`recipe.h` 的 `stepTimeoutMs` 是唯一一份宽度公式）：解释器臂 **5000 ms**（它的
  "到期"有两把尺）；AOT 臂**不给缺省**（它只有宿主墙钟一把尺，看门狗是 `--time-limit` /
  `--max-frames`）⇒ 没写 `timeout` 就没有守卫。
  ★ **`wait <ms>` 没有守卫**（`stepTimeoutMs` 对它恒返回 0）：它是**固定时长**、到点就走，
  不需要"等它成立"（旧的 `ms + 缺省` 补丁已删）。`click` / `profile_*` / `quit` 也没有可等待的
  东西 ⇒ 给它们写 `timeout` = 解析期硬错。⚠ 缺省值不同是**入口策略**，不是能力面分叉：
  显式 `timeout` 在两臂语义相同。到期 = **已等的严格超过**窗口（`>` 不是 `>=`，同一份判据）。
- **一个用户入口 + 一个机器内驱动器，同一份语义**：
  - `--input-plan`（AOT 臂，**唯一**的用户入口）：在 guest **读点**推进，动作直接写 `WindowManager`；
  - `InteractionGraph`（机器内驱动器，**2026-11-16 起没有 CLI 入口**：历史入口 `--interaction-graph`
    已退役）：在**块边界**推进，动作经 `InputScript` 追加（`at frame <当前+1>`，由注入层保证
    "一次按下一定被 guest 观察到"）。
  ⇒ 差别只有**推进点**与**动作怎么落地**；顺序推进、两个门的判定、每步守卫的宽度 / 到期判据、
  完成度的结论与退出码**都是同一份实现**（`runtime/recipe.cpp` 的解析 + `stepGateHolds` +
  `stepTimeoutMs` / `stepWindowExpired` + `RecipeOutcome`）。
  ★ **能力面只有一份**（2026-11-16 统一；S6 追加三条鼠标原语）：九条指令 + 行尾 `timeout` 每个驱动器都认，⛔ 没有 `kPlanFeatures` /
  `kGraphFeatures` 这种按入口分叉的表，也⛔ 没有"这个入口不认"的解析期硬错。

## 2. 失败（全部响亮）

| 失败 | 判据 | 为什么会发生 |
|---|---|---|
| `timeout` | 某条指令的守卫到期（窗口一次都没成立） | 等的那一屏没出现 / 门写错了区间 / 动作一直没能施加 |
| **任务没走完** | 每个驱动器各有一把量尺，结论**同一个**：机器内驱动器 = 没到 `done()`；AOT 臂 = 跑完了还有指令没消费（`pending != 0`） | 实现缺口 / 引擎自己退出 / 门永不成立 —— ⛔ 不是"成功"，哪怕退出码看起来是 0 |

- 一律：**非 0 退出（5）** + stderr 打失败报告并**点名 `文件:行` + 门/动作**（解释器臂：哪一条指令 /
  等的是什么 / 等了多久 / 最后一次身份 / 位置读数 / actuator 账；AOT 臂：`FAIL timeout` 或
  `FAIL incomplete` + 那条指令 + 进度 `steps= applied= pending= fired= reads=`）。
  ⛔ 不许 warning-only。★ 解释器臂"补写最后画面 BMP"随 `--expect-change-*` 退役删掉了
  （2026-11-16：那条分流没有 CLI 生产者了；片面对照改看 `--dump-frame`）。
- ★ **退出码 5 只有一处口径**（`recipe.h` 的 `recipeOutcomeExitCode`）：`Done` ⇒ 0（**不覆盖**运行结论）；
  `Running`（还有指令没消费）/ `TimedOut` ⇒ 5。⛔ 不许只打警告仍 exit 0 —— 那是"图 → 配方"改写
  会把 CTest 变成假绿的缝（ADR `REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置）。
- 到期有两把尺：① `timeout` 的 **guest 毫秒**；② **墙钟停滞兜底** —— guest 指令与帧都不再推进超过
  `timeout` 墙钟毫秒 ⇒ 直接判到期（guest 可能正阻塞在 `GetMessageA`，那时 guest 时钟是冻住的）。
- 退出码口径：`unimplemented-slot` / `resource-exhausted` **不是**正常结局；`ExitProcess` 仍是正常结局
  （`Stopped` ⇒ `ok()` 真 ⇒ 0）⇒ **"任务没走完"必须自己判**：只看退出码时，引擎停在半路仍然看起来成功。

## 3. 与 Maa 的差异（逐条：不抄什么）

| Maa 有 | 我们 |
|---|---|
| 节点 + `next` 有序候选列表，按序识别、第一个命中即前进 | **没有图**：一串顺序指令，⛔ 不做候选轮询 |
| `on_error` 图（错误分支跳转） | **不做**：任何失败 = 整条任务失败 |
| `rate_limit` / 轮询节奏 | **不做**：门是电平，策略固定、可解释 |
| 点击抖动（框中心 + 高斯采样） | **不做**：要确定性 —— 固定点 |
| `wait_freezes` 失败被静默放过 | **反着做**：守卫到点没成立就是失败 |
| `[JumpBack]` 回跳 | **不做**：只向前 |
| 为"人手工调一张保守图"设计 | **为生成而设计**：结构化、无自由文本、指令 id 稳定（`step1`…）、可程序批量生成 |

## 4. 确定性

- **门只看 guest 可见状态**（脚本身份 + 脚本内 PC）：同一份观测 ⇒ 同一串判定。
- **`wait <ms>` 是唯一的墙钟面** ⇒ 同一份配方两次跑的完成时刻不同 ⇒ 该 trace 头部一律
  `# NON-DIFFABLE`（与呈现档位无关）。其余字段（指令 id / frame / insn / guest_ms / 命中 / 动作）是确定性的，
  可逐行 diff。
- ⛔ **时间门不能当判据**：`wait` 是宿主墙钟，而 guest 推进不是墙钟（headless 下尤其不成比例，
  见 `docs/03-emulator/input-plan.md` §2.6 与 `OBS-20261116-05`）⇒ 要"这一屏就绪了没有"用
  **位置谓词**，不要用 `wait` 去凑。
- **trace 一行一事件**：`task start` / `step <id> enter` / `when <指令> hit` / `action <指令> applied` /
  `step <id> done` / `task done steps=…` / `FAIL …`。`origin=` 只写文件名（⛔ 不写宿主路径）。
  ⚠ **2026-11-16：`--interaction-trace` 已退役** ⇒ 这份 trace 今天只能由程序内的
  `InteractionGraph::setTrace` 产出（没有 CLI 入口）；上面的确定性口径仍是驱动器的口径，
  而**用户面**的配方完成度看的是 AOT 臂的 `input-plan: … pending=` 那一行。

## 5. 画面源**已退役**（历史）

旧模型有五个源（`screen hash` / `screen rect … avg` / `screen changed` / `script` / `always`）与
`when → action → next` 的节点结构。2026-11-16 收敛成顺序模型时删掉：

- 画面三源的判据依赖 **Present 采样**：房间空闲时帧会停（~0.5 s 一帧）⇒ "永远等不到"；
- `screen rect … avg` 命中会**产出框**，而 `click center` 又把"识别框"与"点击坐标"耦合在一起 ——
  一个识别失败会静默改掉点击位置；
- 需要"这一屏 + 就绪"时，`await position` 是**不依赖 Present** 的可判定谓词（PC 在脚本体内）。

⇒ `runtime/screen_sample.*` **不是**为配方服务的：它服务**画面采样**（`FrameSampler` /
`ScreenSample`）与 `--overlay` 的每帧读数。★ 2026-11-16：效果级判据的 CLI 已随 `--input-script`
退役（`--expect-change-after` / `--expect-change-window` 不再是参数），但采样器本身仍有消费者
（`--overlay` 与 pure CTest 夹具）⇒ ⛔ 不要因为"配方不用它"或"CLI 没了"就删掉。

## 6. 实现边界（现在没有的）

| 缺什么 | 状态 |
|---|---|
| 模板匹配 / 单点取色 / 区域均值作为**门** | **不做**（画面采样只服务 `--overlay` 与夹具的采样计数） |
| 条件组合（AND/OR）、错误分支、回跳 | **不做**：顺序模型没有图 |
| JSON 文件入口 / 生成器 | **没有**：行写法是唯一入口（第二份 schema 必漂） |
| 鼠标拖拽 | **配方有**（S6：`mouse_move` / `mouse_down` / `mouse_up`；坐标 = 设备当前位置，拖拽 up 不回跳）**但 HUD 路由不接**（只接 `click`，S9 residual） |
| 滚轮 | **配方没有原语**：`SDL_EVENT_MOUSE_WHEEL` 经 `input::mouseWheel` 直接投给 guest；`InputScript` 也不建模滚轮。它不是电平（没有 `key_level_` 项） |
| `hold` 时序 | **不是配方概念**：`click` 的抬起由注入层闸门（KeyLevel 读点 + 墙钟兜底）决定；`mouse_down/up` 的释放由配方自己控制。**`InputScript` 仍保留自己的 hold 策略**（`inputSampleCount` 闸门 + 上限；Q4 取消，不统一） |
| `key <VK>` 动作 | **已退役**（没有生产者） |

## 7. 分期（历史与未做）

1. `screen` 源 + 节点图 + 三种响亮失败 —— **已删**（§5）。
2. 脚本身份（marker）源 —— ✅ 保留，且是现在两个门之一；身份发现路径 = `LIFT_LOG_MARKERS`。
3. `await position`（脚本内位置门）—— ✅ 保留；AOT 臂"到加载存档屏"的配方靠它确定。
4. **顺序模型收敛（2026-11-16）**：两臂共用 `runtime/recipe.{h,cpp}`；节点结构、画面三源、
   `expect`、`key` 一并退役。`graphs/` 与 `scripts/interaction/` 的存量文件见 `graphs/README.md`。
5. **已退役（2026-11-16 用户裁决）**：`--input-script` 时间线（`at frame` / `at insn` / `at marker`）
   **不并入配方** —— 旧 bench 不需要了，"真要做测试也是重新写"；⛔ 不为它给配方补细粒度触发。
   断面的依赖清单见 `.tmp/graphfix/report-idescript.md`，重开条件见 `input-plan.md` §7
   （AOT 压力档按 `REQ-01M7HQJEEENYDJ91FB5DAC4K23` 重写）。

## 8. 外部文档

**不依赖外部文档**：交互模型不是 Win32 合同，没有微软文档定义它；Maa 只是**思路来源**，接口形状由本文定义。
