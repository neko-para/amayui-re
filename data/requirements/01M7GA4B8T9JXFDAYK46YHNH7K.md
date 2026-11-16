# AOT 自动到加载存档屏：需要一份经验证的按键/坐标配方 + 人手对照读数

- id: REQ-01M7GA4B8T9JXFDAYK46YHNH7K
- type: req
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- tags: [emulator-lift, input, aot]

## 缺什么

能力面已交付（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z` 的 S1 + S4 + S2′，单 `REQ-01M7G7EMX3JR272D8FVM7JFY9D` 已收口）：
AOT 臂现在有 `--input-plan <file>`，格式是 `interaction_graph` 线性写法子集
（`await always` / `await presents <n>` + `click x y [left|right]` / `key <VK>`），
门只有 `{Always, Presents}`，flush-before-read，跑完打一行
`input-plan: steps=N fired=M presents=P released=R held=H`。

**缺的是"配方"**，不是机制。

## 2026-11-16 探测结果（已做 13 次 run，每次全新空 appdata）

### 两条会改变所有后续命令的事实

1. **到 TITLE 的任何配方必须带 `--skip-logo`。** 不带时逐帧 hash 在 **frame 64** 就冻死
   （`0xBE8BCF5F5F55D3F5` 连续 337 帧）⇒ 那之后的按键**打在静止的 boot 帧上，当然没反应**。
   ★ 上一轮 S4 报的"端到端"其实就是这个情形（当时没带 `--skip-logo`），已被本轮纠正。
2. **判据不能用 `stop @` 也不能用 `lastHash`**：
   - `stop @` 两侧**恒同** = `00498AE4`（`--max-frames` 停在 Present 桩，与画面无关）；
   - `lastHash` 每次 run 都不同（real-time 时钟 + TITLE 是动画屏）。
   ⇒ **用收尾 `region` 那一行做屏幕指纹**（同一命令多次 run **逐位相同**，且随屏而变；
   需要 `--region-log` 才打印 —— 见 `OBS-20261116-02/03`）。

### 候选配方（仓内既有图文件，坐标原义搬运）

`graphs/title-loaddata.graph` 自带「TITLE → 点 LoadData（**用户给的客户区坐标 1070,480**）」。
换算成 plan（`.tmp/aot/recipe/title-loaddata.plan`）：

```
await presents 150
click 1070 480
```

⚠ `await script TITLE` → `await presents 150` 是**就近替换、不是等价**：plan 的门只有 `{Always, Presents}`，
script 门在 plan 文件里被解析期硬拒。

### 对照读数（region 三数 = committed / surfaces / heap_blocks）

| 跑法 | region 三数 | 判定 |
|---|---|---|
| 不给 plan（3 次） | `108595152 / 21 / 6674` | 基准 |
| 中性点击 `640 360`（= 特异性对照） | `108595152 / 21 / 6676` | 只多 2 个 block ⇒ **不算反应** |
| **`click 1070 480`（2 次逐位相同）** | **`117490928 / 25 / 6821`** | 与对照**不同**（+8.9 MB、**+4 surfaces**） |
| `click 1186 373`（GameStart） | `112791848 / 24 / 6747` | 第三种结局 |

## 仍然没证明的（这是本单不能收口的原因）

**没证明那副新画面就是「加载存档」屏。** 已证的是：换上了一副新画面（+4 纹理 / +8.9 MB）、
**可复现**（两次逐位相同）、且**不是随便点**（中性点击不产生变化）。

### 一条更值得先查的矛盾（另开单：`REQ-01M7GAMJQ26DHZSPJQTXZ88S7D`）

**同一点击在解释器臂上"没有反应"**：1 条动作全部应用，但 arena / surfaces 与无动作**逐位相同**，
`engine_marker` 停在 `TITLE.BIN` 且无新 identity（`frame=` 全是 0）。
⇒ 两臂对**同一个输入**给出不同结果。可能原因（未定）：两臂 guest 时间步长差 ~15 倍 + 解释器 `bounded-no-sample=1`。

## 判据（收口时）

- 一份 `--input-plan` 文件，用 AOT 跑出的 **`region` 三数**（或 `ScreenSample`）与"**人手点一次**"的 run **逐位相同**；
- 且"没人点"的对照**不同**（这一半已经做到了）；
- `input-plan:` 摘要行的 `fired` / `released` 符合预期，`held=0`。
- ⛔ 不许用"看起来进了读档界面"当判据。

## 已知的相邻缺口（不在本单）

- AOT 真出声：`REQ-01M7G7GPBG9BK2360WSG6DBFNP`。
- `GetMessageA` 空队列由计划供给（S5）、旧路径删除（S6）、`Reads`/`Insns`/`ScreenSample` 门 —— 都还没做，
  但对"到加载存档"不是必需（复审裁定）。
- **plan 表达不了"同一份计划去掉动作"**：只有门没有动作时解析报 `线性 step step1 缺动作行` ⇒ exit 2。
  对照只能用"不给 plan"或"惰性点击"两档（本轮就是这么做的）。若将来要更干净的对照，给 plan 加一个
  `noop` 动作（**未做，也未立单**）。
