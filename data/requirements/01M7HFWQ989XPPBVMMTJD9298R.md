# frame+0x70/+0x74 与 age-format 的 layout.mts 记法冲突（3-dword 容器 vs 两个指针）

- id: REQ-01M7HFWQ989XPPBVMMTJD9298R
- type: spike
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- tags: [emulator-lift, shape, age-format]

## 一句话

`frame+0x70/+0x74` 的实际形态与 `repo:packages/age-format/src/engine/layout.mts` 的记法**对不上**：

| 来源 | 说法 |
|---|---|
| `layout.mts`（`arrayContainer: 0x70`） | `+0x70` 起是 **3 个 dword = begin/end/cap**（一个容器结构体） |
| 运行时观察（`REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK` 取证） | `+0x70` 与 `+0x74` 各收一个 `operator new(0x10)` 的**指针**（`0x40F60C`→`0x40F63B`、`0x40F642`→`0x40F678`）；被指对象才是 3 dword |

⇒ 两者只有一个对，或者"指针 → 对象里 3 dword"（那么 `layout.mts` 的写法**少了间接层**）。

## 为什么是 spike 而不是 bug（2026-11-17 改类型）

它**不是可观测的行为分歧**（没有"两臂跑同一段输入得出不同结果"），而是**两份文档的记法冲突**：
一份是 shape 声明、一份是取证观察。缺陷的判据是"分歧还在不在"，而这里的产出是**一个定性结论**
⇒ `type: spike`（限时设问，产出是结论）。
⚠ 随之撤掉 `severity: S3` 与 `repro:`：那是缺陷专属字段，留着会把"修复"变成"跑一遍 repro 看看"这种没意义的动作。

## 影响

`layout.mts` 被 `apps/workbench` / 工具链消费 ⇒ 记法错会让人按错的偏移读。

## 判据（收口时）

- 给出 `+0x70/+0x74` 的定性（指针 / 结构体 / 数组容器），带 **EA 锚**（⛔ 不锚 `.c` 行号）。
- 若观察正确 ⇒ 改 `layout.mts`（改动随结论一起落；**有守卫更好、没有也行** —— 但"没有守卫"不能成为不收口的理由）；
  若 `layout.mts` 正确 ⇒ 说明观察里的 `operator new(0x10)` 是什么。
- ⛔ 不许"两边都留着，写成未知"结单。
