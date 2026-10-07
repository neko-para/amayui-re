# 冲突 · local 池基址：新取证（+0x3C..+0x54，池序不同）vs 已登记守卫（+0x34..+0x48，来源=旧仓 fields.json）

- id: REQ-01M4B49MK0HMJ7NK00PN4SMDHB
- type: bug
- status: open
- parent: REQ-01M4B248NB8FWDN61E2NMK8N6V
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 计数 6 个 + 基址 6 个
- severity: S3

## 冲突是什么

**同一件事有两份互斥的登记**（局部池的基址）：

| 来源 | local 池基址（按池名） |
|---|---|
| 新取证（`sub_40ED40`：分配紧随其后的那次 store；EA `0x40F3B1/0x2E9/0x31B/0x3F0/0x42F/0x471`） | string `+0x3C` · int `+0x40` · float `+0x44` · ptr `+0x48` · floatPtr `+0x50` · stringPtr `+0x54` |
| 已登记守卫 `tools/test/emulator-model.test.mjs`「local 池的 slot 几何」（注释写明来源 = **旧仓 `fields.json`**） | int `+0x34` · float `+0x38` · string `+0x3C` · ptr `+0x40` · floatPtr `+0x44` · stringPtr `+0x48` |

**两者只在 `string = 0x3C` 上一致** —— 而 `0x3C` 恰好是本仓**自己独立验证过**的那一格：
`set-string` 对 type 11（局部串）的目标 = `[Engine+0x5D8BC] + 28*idx`，`0x5D8BC − 0x5D880 = 0x3C`。

## 为什么现在**不**改

* 新证据自己标注了两处不确定：「float/string 与 `+0x50`/`+0x54` 与头 dword 的**二元归属**只有"读取顺序 + 元素宽"的交叉，
  **没有**"读字段→传参"的显式指令」。
* 而改 `LOCAL_POOL_SLOTS` 会让那条守卫变红 —— 那需要**先裁决**，不是先改表（§6.4：冲突显式化，不就地改写）。

## 对运行中的模型**零影响**（这点很关键）

那条守卫**自己**就断言了：`LOCAL_POOLS`（语义层）**不带** `base` / `count` 字段。
⇒ 这两张表只在**布局知识**层，前沿与语义都不依赖它们。
⇒ 所以这条冲突**不阻塞**本目标的下一步（地址空间），但**必须裁**，因为地址空间的
`capacity` 最终要从"池容量字段"来，而"哪个字段是哪个池"正是这条冲突的核心。

## 收口判据

裁出来之后：
1. `LOCAL_POOL_SLOTS` 按裁定的值改（或确认旧表对）；
2. 那条守卫的断言与注释同步（它现在写着"与旧仓一致"—— 那是**来源**，不是**判据**）；
3. 在本条写 `done_reason`（说明依据哪一份取证）。
