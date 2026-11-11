# 交互模型：任务 = 节点图，节点 = when → action → next（失败全部响亮）

> ★ **本文件是交互模型的唯一真源**：节点 schema、三种失败、与 Maa 的差异、确定性口径、实现边界、分期。
> 需求单 `REQ-01M6XVYY5TCKSAS80WS7PADF8P` 只留"判据 + 进度"，`apps/emulator-lift/README.md` 只留
> "现在的时间线是过渡形态"的指向 —— 两处都**不复述 schema**（两份 schema 必漂，见 `AGENTS.md` §10）。
> 本文只写**接口与判据**：EA / 偏移 / 内部函数名属于 `design-notes.md` 与需求单，不进本文。

## 0. 一句话

**任务 = 节点图**；**节点 = `when`（条件）→ `action`（动作）→ `next`**；失败**响亮**。

## 1. 节点 schema（唯一真源）

```json
{
  "entry": "title",
  "nodes": [
    { "id": "title", "when": { "screen_hash": "9F3C1A7D…" }, "action": { "click": "944 562" },
      "next": "room", "timeout": 3000, "expect_change": { "screen_changed": true } },
    { "id": "room", "when": { "screen_rect": { "x": 944, "y": 562, "w": 120, "h": 40,
                                               "avg": "0x1B2B3C" } },
      "action": { "click": "center", "button": "right" }, "next": "", "timeout": 3000 }
  ]
}
```

JSON 与行写法一一对应（`screen hash` ↔ `"screen_hash"`、`click center` ↔ `"click": "center"`、
`click center right` ↔ `"click": "center", "button": "right"`、
`click 1240 687 right` ↔ `"click": "1240 687", "button": "right"`）：
同一份节点，人读写行、生成器读写 JSON。

★ **解析入口只有一份：行写法**（`--interaction-graph <file>`）。上面的 JSON 是**同一份节点的等价展示**
（给人读 / 给生成器当中间模型），**不是**第二个文件格式 —— 第二份 schema 必漂（`AGENTS.md` §10）。
JSON 文件入口留到"生成器落地"时再加，重开条件写在同一张单里。

| 字段 | 必填 | 取值 | 语义 |
|---|---|---|---|
| `when` | ✅ | 条件项（闭集合；行写法 `script <名字\|id>` / `screen hash <hex>` / `screen rect x y w h avg <0xRRGGBB>` / `screen changed` / `always`） | 成立才轮到本节点。一条节点 = **一个**条件项：不做 AND/OR 组合、不做条件分支图。`screen rect …` 命中时**产出框**（= 该 rect），这是 `click center` 唯一合法的坐标来源；`always` 是纯推进。`script` 是**电平**（见下） |
| `action` | ✅ | `click [x y] [left\|right]` / `click center [left\|right]` / `key <VK>` / `wait <n> frames\|ms\|wall_ms` / `profile_begin` \| `profile_end` `<label>` / `quit` | 与 `when` **解耦**：命中才执行。`click center` 打**识别到的框**中心；`click x y` 打固定点。按钮词只有 `left` / `right`，省略 = `left`（⛔ 不收 `middle` / `hold`：按钮是输入目标，`hold` 是时序语义） |
| `next` | ✅（可空） | **单个**节点 id / 空 | 前进到哪一个；空 = 任务结束。⛔ 不是候选列表。id 必须已定义，否则解析期失败（带行号） |
| `timeout` | ✅ | ms | 等到 `when` 成立的上限；到点 = `timeout` 失败。必填；解析器给一个**固定缺省值**（生成器应显式写出来，缺省在 trace 里标出） |
| `expect_change` | ⬜ | 条件项（同上闭集合） | 动作执行后**必须**观测到的变化；到点没变 = `expect_change` 失败 |
| `enabled-if` 之类 | — | — | **不需要**：不做条件分支图 |

`wait <n>` 的三个单位：`frames` = Present 计数（guest 帧）· `ms` = guest 虚拟毫秒 · `wall_ms` = **宿主墙钟**
（自 run 起算；纯等待、不碰输入）。`wall_ms` 让完成时刻取决于宿主调度 ⇒ 该图的 trace 一律标
`# NON-DIFFABLE`（见 §4）。

`when` 的五个源：`script <名字|id>` = 引擎帧身份（来自 marker 事件源，见 §5）· `screen hash <hex>` = 整帧指纹相等 ·
`screen rect x y w h avg <0xRRGGBB>` = 该矩形区域均值命中（并**产出框**）· `screen changed` = 与上一次采样不同 ·
`always` = 恒成立（纯推进）。

`script` 是**电平**条件：判据 = `当前身份 == X`，读的是**最近一次** VM 循环头采样（`0x4123F0` / `0x4123E5`），
所以"**已经**在那一屏"也成立（这正是"等一屏 → 点一下"能自足写出来的前提）。匹配规则：
能解析成 `u32`（十进制 / `0x`）⇒ 按 **id**；否则按**名字**（ASCII 大小写不敏感、**允许省略 `.BIN` 后缀**
——`TITLE` 命中 `TITLE.BIN`；`id:0x%08X` 退化名**始终接受**）。名字表与 `id:0x…` 退化名规则**与时间线
`at marker` 相同**；但 `script` **更宽松**：允许省略 `.BIN` 后缀（`at marker` 不接受省略 ——
`at marker TITLE` 今天**不**命中 `TITLE.BIN`）。
⚠ **边沿**语义不属于本条件：`at marker <名字>` 仍是"进入才发事件、首个样本只播种"（§6 / §7），两者互不改写。

### 1.1 线性写法（解析期展开成同一份节点）

一条近似线性的任务可以直接写成"等一屏 → 点一下"：

```
await script TITLE
click 1174 377
await script GAMESTART
click 715 576
await script SN0000
click 100 100
```

- `await <cond>` 开一个 step（= 一个节点），`when` = `await` 后面的那个条件；条件闭集合与节点形式**完全相同**
  （`script` / `screen hash` / `screen rect … avg` / `screen changed` / `always`）。
- `await` 之后的**第一个**动作行（`click` / `key` / `wait` / `profile_begin` / `profile_end` / `quit`）属于
  当前 step；**再写一个动作行 = 新的 `always` step**（是有意的，⛔ 不是解析错误 —— 一个 step 恰好一个动作）。
  没有 `await` 的动作行同样是 `always` step。要"连点两次、等的是同一屏"就再写一行 `await …`。
- 每个 step 可再跟 `timeout <ms>` / `expect <cond>`（**作用域 = 当前 step**，语义与节点形式完全相同）；
  缺省 `timeout` 仍用固定缺省值（trace 标 `(default)`）。⛔ **不默认** `expect`
  （线性程序里"下一个 `await`"就是期望；默认加会让 no-op 动作误判）。
- 隐式 `next` = 下一个 step；最后一个 step 的 `next` 为空（任务结束）。`next` / `when` 这类键在 step 里
  不合法（条件来自 `await`、连边是隐式的）⇒ 解析期失败。
- step id = `step1` / `step2` …（稳定、进 trace）；与已有 `node` id 冲突 ⇒ 解析期失败（带行号）。
- 线性写法下不写 `entry` ⇒ entry = 第一个 step；写了 `entry` 就照旧。与 `node` 块**可混用**。
- 解析失败一律 `origin:行: 原因`。


## 2. 三种失败，全部响亮

| 失败 | 判据 | 为什么要单独一类 |
|---|---|---|
| `timeout` | `when` 在 `timeout` 内一次都没成立 | "等的那一屏没出现"（画面不对 / 动作没生效） |
| `no-sample` | 窗口内该识别源**一次样本都没有** | 识别源**失明**（帧停住 / 源没接上）—— 与"画面不对"分开，修法完全不同 |
| `expect_change` | 动作已应用，但 `expect_change` 到点仍不成立 | 动作没生效；正是"没点中「戻る」却继续跑"那一坑 |

★ 还有一条**不在上表里、但同样是失败**：**任务没走完**。三种失败都是"任务还在走、但走不动了"；
而 guest 侧硬停（`unimplemented-slot` = 已注册的槽 / 形态没实现、`resource-exhausted` = 我方容量不够、
`ExitProcess`）会让任务停在半路。⇒ 判据只有一条：**没有到达 `task done` 就不是成功**（同一个非 0 退出码 + 同样的现场输出）。
★ 退出码口径（`REQ-01M6WW9R6SJ7P8XA4522XCR48A` 落地后）：`unimplemented-slot` / `resource-exhausted`
**不是**正常结局 ⇒ 没有交互图时 exit 2（`--gpu-strict` 也先判 2）；`ExitProcess` 仍是正常结局
（`Stopped` ⇒ `ok()` 真 ⇒ 0）。所以这条必须**自己判**：只看退出码时，`ExitProcess` 停在任务半路仍然看起来成功。

三种一律：**非 0 退出** + 存最后画面（`--interaction-fail-dump`，缺省 `<graph>.fail.bmp`；⛔ 不是
`--dump-frame` —— 那条是"每次 Present 导出一张"）+ stderr 打出**最后状态与全部候选**（哪个节点、等的是什么、
等了多久、窗口与累计采样数、**每个受监视矩形的最后一次采样值**、整帧 hash 的最后一帧）。⛔ 不许 warning-only、⛔ 不许当 0 点继续跑。

**判定口径（可复现；第一期按这几条实现）**

- **采样点 = Present**（`HeadlessPresenter::present` 是唯一呈现汇点）：一次 Present = 一个样本。
  采样量 = 整帧 hash（已有）+ **图里引用到的每个矩形**的均值与区域 hash。
  ⛔ 判定侧不读像素：它只读两个**单调计数**（样本数 / "与上一次采样不同"的次数）。
- `screen hash` = 该样本的整帧 hash 相等；`screen changed` = 自进入这次等待以来
  **"与上一次采样不同"发生过至少一次**（不是"当前这帧与上一帧不同" —— 两次判定之间帧会推进好几帧）。
- `screen rect … avg` = 该矩形**各通道算术均值**（整数截断）与目标值每通道相差 ≤ **8**。
  为什么要容差：淡入 / 抗锯齿 / 16bpp 展开会让均值差 1~2，逐位相等等于"一个像素都不许动"，那不叫识别。
- 矩形越界 ⇒ **夹到画面边界**再算（识别框常常贴边）；夹完为空 ⇒ 该次采样对这个矩形**无效**
  （无效 ≠ 命中，也 ≠ 一次有效样本）。
- `no-sample` 的判据 = 这次等待里**样本数一次都没增长**。它与 `timeout` 分开报，因为修法完全不同。
- 到期有两把尺：① `timeout` 的 **guest 毫秒**；② **墙钟停滞兜底** —— guest 指令与帧都不再推进
  超过 `timeout` 墙钟毫秒 ⇒ 直接判到期（guest 可能正阻塞在 `GetMessageA`，那时 guest 时钟是冻住的，
  光看它会把整轮拖死）。


## 3. 与 Maa 的差异（逐条：不抄什么）

背景：Maa 的用例是"**很多节点 + 很保守的策略干一个任务**"；我们是"**成批生成任务**"，每条节点少、路径近似线性。
所以参考思路、**不照抄**：

| Maa 有 | 我们 |
|---|---|
| `next` = 有序候选列表，按序识别、第一个命中即前进 | **不做候选轮询**：`next` = 单个 id |
| `on_error` 图（错误分支跳转） | **不做**：任何失败 = 整条任务失败 |
| `rate_limit` / 轮询节奏 | **不做**：只在样本边界采样，策略固定、可解释 |
| 点击抖动（框中心 + 高斯采样） | **不做**：要确定性 —— 框中心或固定点 |
| `wait_freezes` 失败被静默放过 | **反着做**：动作后没变就是失败 |
| `[JumpBack]` 回跳 | **不做**：`next` 只向前 |
| 为"人手工调一张保守图"设计 | **为生成而设计**：结构化、无自由文本、id 稳定、可程序批量生成 |

## 4. 确定性

- **同一模拟器行为 ⇒ 同一节点序列**：判定是"观测值"的纯函数（采样在判定之外）。
- **trace 可 diff**：一行一事件，字段只用确定性量 —— 节点 id + 进入时刻（frame / insn / guest_ms）+
  识别结果 + 动作 + 结果；同一路径两次运行逐行相同（时间戳除外）。
- `--present window` 是 real-time ⇒ trace 的**头部**（第 2 行，含在 `# interaction trace v1` 之后）标
  `# NON-DIFFABLE`，该档 trace 不参与 diff。
- 图里**出现过** `wait <n> wall_ms`（宿主墙钟）⇒ 完成时刻取决于宿主调度，同一输入两次跑的时序不同 ⇒
  该图的 trace 头部同样标 `# NON-DIFFABLE`（与呈现档位无关）。

## 5. 两种识别源为什么都要（互补，不是二选一）

- 房间空闲时**帧会停**（不再 Present）⇒ 纯画面等待会**卡住**；`script` 源（引擎帧身份，来自 marker
  事件源）**不依赖 Present**。
- 反过来，画面能确认"**确实是那一屏**"；脚本身份不能（画面可以不换脚本）。
- ⇒ 两个源都在 `when` 的闭集合里，按节点各选各的；⛔ 不把任一源当"主源"。

## 6. 实现边界（现在没有的）

| 缺什么 | 状态 / 补它大概要改哪里 |
|---|---|
| 区域 hash / 区域均值 / 单点取色 / **模板匹配** | ✅ 已实现区域均值 + 区域 hash（采样点 = Present，`runtime/screen_sample.h`）；单点取色 = 1×1 矩形 ⇒ 不再单列；⚠ 区域 hash 目前**只采样、没有条件消费它**（闭集合里只有 `avg`）—— 要加 `screen rect … hash` 时再接上；**模板匹配不做** |
| presenter 保留"最后呈现那帧"的句柄 | ✅ 已保留（描述符 + 帧号；失败时补写一张，并注明像素是否仍是**被采样到的那一帧**） |
| `script` 条件 | ✅ 已实现，且是**电平**：`script <名字\|id>` = "当前身份 == X"（读最近一次 VM 循环头采样）；采样门控由"图里有 `script` 条件"打开，失败报告写**最后一次身份**（`cur` / `id` / `name`，降级标出）。**边沿**仍是时间线的 `at marker <名字>`，语义不同、互不改写。身份发现路径 = `LIFT_LOG_MARKERS`（环境变量，存在即开）打每次身份变化 |


## 7. 分期（每步独立可验收）

1. **`screen` 源 + 节点图 + 三种响亮失败 + trace** —— 今天就要覆盖**进 / 出 Eushully-chan Room**
   （这一期**不依赖** `script` 源）。
2. **`script` 源**（引擎帧身份采样器；帧停住时仍看得见场景切换）—— ✅ **已落地**：`when script <名字|id>`
   是**电平**条件（另有线性写法 `await script …` 与身份发现日志 `LIFT_LOG_MARKERS`）；时间线
   `at marker` 的**边沿**语义不变。需求单：脚本身份 marker 事件源。
3. **把现有 `--input-script` 时间线并入**：`at frame N` / `wait N frames` 折成节点图里的等待条件
   （`wait n frames`）；**旧解析器保留**，另加一个**机械转换器**（不改旧语法、不改旧守卫）。

## 8. 外部文档

**不依赖外部文档**：交互模型不是 Win32 合同，没有微软文档定义它；Maa 只是**思路来源**，接口形状由本文定义。
