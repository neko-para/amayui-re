# frame+0x70/+0x74 与 age-format 的 layout.mts 记法冲突（3-dword 容器 vs 两个指针）

- id: REQ-01M7HFWQ989XPPBVMMTJD9298R
- type: bug
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- repro: packages/age-format/src/engine/layout.mts#arrayContainer
- severity: S3
- tags: [emulator-lift, shape, age-format]

## 一句话

`frame+0x70/+0x74` 的实际形态与 `packages/age-format` 的 `layout.mts` 记法**冲突**：

| 来源 | 说法 |
|---|---|
| `layout.mts`（`arrayContainer: 0x70`） | `+0x70` 起是 **3 个 dword = begin/end/cap**（一个容器结构体） |
| 运行时观察（`REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK` 取证） | `+0x70` 与 `+0x74` 各收一个 `operator new(0x10)` 的**指针**（`0x40F60C`→`0x40F63B`、`0x40F642`→`0x40F678`）；被指对象才是 3 dword |

⇒ 两者只有一个对，或者"指针 → 对象里 3 dword"（那么 `layout.mts` 的偏移记法少了间接层）。

## 影响

`layout.mts` 是被 `apps/workbench` / 工具链消费的 shape 声明 ⇒ 记法错会让人按错的偏移读。

## 判据（收口时）

- 给出 `+0x70/+0x74` 的定性（指针 / 结构体 / 数组容器），带 **EA 锚**（⛔ 不锚 `.c` 行号）。
- 若观察正确 ⇒ 改 `layout.mts` 并给一条能红的守卫；若 `layout.mts` 正确 ⇒ 说明观察里的 `operator new(0x10)` 是什么。
- ⛔ 不许"两边都留着，写成未知"结单。
