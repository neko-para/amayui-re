# AOT 自动到加载存档屏：需要一份经验证的按键/坐标配方 + 人手对照读数

- id: REQ-01M7GA4B8T9JXFDAYK46YHNH7K
- type: req
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- done_reason: 2026-11-16 用户实测：能进入「加载存档」界面并正常操作 ⇒ 能力面成立。原判据（region 与人手 run 逐位相同）撤回：region 是分配形状的代理、不证明是哪一屏，且用户裁决'肉眼可见的事不值得为它造机械判据'；将来若有节点真的依赖'执行到特定位置并确认'再重新设计（候选见正文）。
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

### 候选配方（仓内既有图文件，坐标原义搬运）

`graphs/title-loaddata.graph` 自带「TITLE → 点 LoadData（**用户给的客户区坐标 1070,480**）」。
换算成 plan（`.tmp/aot/recipe/title-loaddata.plan`）：

```
await presents 150
click 1070 480
```

### 对照读数（region = arena 在用 / 峰值 / surfaces / heap_blocks；⛔ 只作**附带读数**，不作判据）

| 跑法 | region | 判定 |
|---|---|---|
| 不给 plan（3 次） | `108595152 / 108595152 / 21 / 6674` | 基准 |
| 中性点击 `640 360`（= 特异性对照） | `108595152 / … / 21 / 6676` | 只多 2 个 block ⇒ **不算反应** |
| **`click 1070 480`（2 次逐位相同）** | `117490928 / … / 25 / 6821` | 与对照**不同**（+8.9 MB、**+4 surfaces**） |
| `click 1186 373`（GameStart） | `112791848 / … / 24 / 6747` | 第三种结局 |

## 收口（2026-11-16，用户裁决）

★ **用户 2026-11-16 实测确认：能进入「加载存档」界面并正常操作** ⇒ 本条的**能力面**（自动化能到那一屏）成立。

- **原判据撤回**：「automated 的 `region` 与人手 run 的 `region` 逐位相同」**不作判据** —— `region`
  （`src/aot/aot_host.cpp:459` 的 arena 在用/峰值/surface 数/堆块数）是**分配形状的代理**：它证明"换了一副画面"，
  **不证明是哪一副**，且会随缓存/实现变化而漂。
- **理由（用户口径）**：这是**肉眼可见**的事，为它造机械判据的成本 > 收益。若将来某个节点真的依赖
  "执行到特定位置并确认"（例如无人值守批量跑到某屏、或要拿它当门），**那时再重新设计**。
- **将来可选的重设计**（登记备查，⛔ 现在都不是判据）：
  ① 门 `await position SAVE 0x1104 0x11C4`（已存在；`docs/03-emulator/scripts/SAVE.md` ③ 门 1/1）；
  ② 控制层 `sample` 的 `script.name == "SAVE.BIN"`；③ `global a9cb`（`REQ-01M7J97QY66KMP5SBPCTR0S3TP`，能区分加载/保存两屏）；
  ④ 修好后的截图 `pixels_sha256`（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ` 收口后才有）。
- ⚠ **`script` / `pos` 也区分不了加载屏与保存屏**（同一脚本、同一区间 `[0x1104,0x11C4)`，`SAVE.md` 的 ★ 裁决）
  ⇒ 真要区分只有 ③（`global a9cb`）。

## 已知的相邻缺口（不在本单）

- AOT 真出声：`REQ-01M7G7GPBG9BK2360WSG6DBFNP`。
- `GetMessageA` 空队列由计划供给（S5）、旧路径删除（S6）、`Reads`/`Insns`/`ScreenSample` 门 —— 都还没做，
  但对"到加载存档"不是必需（复审裁定）。
- **plan 表达不了"同一份计划去掉动作"**：只有门没有动作时解析报 `线性 step step1 缺动作行` ⇒ exit 2。
  对照只能用"不给 plan"或"惰性点击"两档（本轮就是这么做的）。若将来要更干净的对照，给 plan 加一个
  `noop` 动作（**未做，也未立单**）。
- 同一点击在解释器臂上"没有反应"（`REQ-01M7GAMJQ26DHZSPJQTXZ88S7D`）—— 解释器已退役，该单只作历史记录。
