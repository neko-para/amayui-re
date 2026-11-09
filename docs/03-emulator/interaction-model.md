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
    { "id": "title", "when": { "screen_hash": "9F3C1A7D…" }, "action": { "click": "center" },
      "next": "room", "timeout": 3000, "expect_change": { "screen_changed": true } },
    { "id": "room", "when": { "screen_rect": { "x": 944, "y": 562, "w": 120, "h": 40,
                                               "avg": "0x1B2B3C" } },
      "action": { "click": "center" }, "next": "", "timeout": 3000 }
  ]
}
```

JSON 与行写法一一对应（`screen hash` ↔ `"screen_hash"`、`click center` ↔ `"click": "center"`）：
同一份节点，人读写行、生成器读写 JSON。

| 字段 | 必填 | 取值 | 语义 |
|---|---|---|---|
| `when` | ✅ | 条件项（闭集合；行写法 `script <名字>` / `screen hash <hex>` / `screen rect x y w h avg <0xRRGGBB>` / `screen changed` / `always`） | 成立才轮到本节点。一条节点 = **一个**条件项：不做 AND/OR 组合、不做条件分支图。`screen rect …` 命中时**产出框**（= 该 rect），这是 `click center` 唯一合法的坐标来源；`always` 是纯推进 |
| `action` | ✅ | `click [x y]` / `click center` / `key <VK>` / `wait <n> frames\|ms` / `profile_begin` \| `profile_end` `<label>` / `quit` | 与 `when` **解耦**：命中才执行。`click center` 打**识别到的框**中心；`click x y` 打固定点 |
| `next` | ✅（可空） | **单个**节点 id / 空 | 前进到哪一个；空 = 任务结束。⛔ 不是候选列表。id 必须已定义，否则解析期失败（带行号） |
| `timeout` | ✅ | ms | 等到 `when` 成立的上限；到点 = `timeout` 失败。必填；解析器给一个**固定缺省值**（生成器应显式写出来，缺省在 trace 里标出） |
| `expect_change` | ⬜ | 条件项（同上闭集合） | 动作执行后**必须**观测到的变化；到点没变 = `expect_change` 失败 |
| `enabled-if` 之类 | — | — | **不需要**：不做条件分支图 |

`when` 的五个源：`script <名字>` = 引擎帧身份（来自 marker 事件源，见 §5）· `screen hash <hex>` = 整帧指纹相等 ·
`screen rect x y w h avg <0xRRGGBB>` = 该矩形区域均值命中（并**产出框**）· `screen changed` = 与上一次采样不同 ·
`always` = 恒成立（纯推进）。

## 2. 三种失败，全部响亮

| 失败 | 判据 | 为什么要单独一类 |
|---|---|---|
| `timeout` | `when` 在 `timeout` 内一次都没成立 | "等的那一屏没出现"（画面不对 / 动作没生效） |
| `no-sample` | 窗口内该识别源**一次样本都没有** | 识别源**失明**（帧停住 / 源没接上）—— 与"画面不对"分开，修法完全不同 |
| `expect_change` | 动作已应用，但 `expect_change` 到点仍不成立 | 动作没生效；正是"没点中「戻る」却继续跑"那一坑 |

三种一律：**非 0 退出** + 存最后画面（`--dump-frame`）+ stderr 打出**最后状态与全部候选**
（哪个节点、等的是什么、等了多久、每个候选最后一次采样值）。⛔ 不许 warning-only、⛔ 不许当 0 点继续跑。

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
- `--present window` 是 real-time ⇒ trace 首行标 **`NON-DIFFABLE`**，该档 trace 不参与 diff。

## 5. 两种识别源为什么都要（互补，不是二选一）

- 房间空闲时**帧会停**（不再 Present）⇒ 纯画面等待会**卡住**；`script` 源（引擎帧身份，来自 marker
  事件源）**不依赖 Present**。
- 反过来，画面能确认"**确实是那一屏**"；脚本身份不能（画面可以不换脚本）。
- ⇒ 两个源都在 `when` 的闭集合里，按节点各选各的；⛔ 不把任一源当"主源"。

## 6. 实现边界（现在没有的）

| 缺什么 | 补它大概要改哪里 |
|---|---|
| 区域 hash / 区域均值 / 单点取色 / **模板匹配** | 现在只有**整帧 hash**；`screen rect … avg` 与区域 hash 要新写"按矩形取像素"的路径（presenter / 面读取那一侧）。模板匹配**不做** |
| presenter **不保留**"最后呈现那帧"的句柄 | 于是块边界拿不到像素；要加"最后帧的句柄 + 格式 + 尺寸"（presenter 侧） |
| `script` 事件源**未实现** | `when script <名字>` 今天不可用：解析期**拒绝**并报错（⛔ 不静默退化成 `always`）；实现落点是另一张单（"脚本身份 marker 事件源"） |

## 7. 分期（每步独立可验收）

1. **`screen` 源 + 节点图 + 三种响亮失败 + trace** —— 今天就要覆盖**进 / 出 Eushully-chan Room**
   （不依赖未实现的 `script` 源）。
2. **`script` 源**（引擎帧身份采样器；帧停住时仍看得见场景切换）—— 需求单：脚本身份 marker 事件源。
3. **把现有 `--input-script` 时间线并入**：`at frame N` / `wait N frames` 折成节点图里的等待条件
   （`wait n frames`）；**旧解析器保留**，另加一个**机械转换器**（不改旧语法、不改旧守卫）。

## 8. 外部文档

**不依赖外部文档**：交互模型不是 Win32 合同，没有微软文档定义它；Maa 只是**思路来源**，接口形状由本文定义。
