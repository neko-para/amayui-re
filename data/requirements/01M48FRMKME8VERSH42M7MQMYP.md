# 指令区之后的三张表 = 0x71 / 0x3 call-script / 0x8f call 的**位置表**（原题「label / message / call」，命名对照见正文）

- id: REQ-01M48FRMKME8VERSH42M7MQMYP
- type: req
- status: done
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 14
- verify: tools/test/emulator-frame-tables.assets.test.mjs#★ `table_i` 逐元素等于该类 opcode 指令的 dword 位置序列、且表长度 == 出现次数（491 脚本逐条核）
- tags: [engine, emulator]

## 结论已落台账（`data/ledger/`；锚 = 二进制 EA + 守卫用例）
| subject | 说的是什么 | status |
|---|---|---|
| `Engine+0x5D880/frame-tables/entry-structure` | 三张表恒相邻且序 t1→t2→t3；都是 u32 数组；`table_i[k]` = 第 k 条该类 opcode 指令的 dword 位置；长度 == 出现次数 | accepted |
| `Engine+0x5D880/header-subheader-length-and-table-fields` | 头 `+32` = 子头块长度（本语料恒 28）；payload = `subLen−4` 字节 = 6 个 dword = 三组 (len,off)；`off` 是 dword 下标 | accepted |
| `Engine+0x5D880/frame-0x54-group1-0x71-table` | `帧+0x54/0x58` = `table_1` = `0x71` 位置表；唯一消费者 `sub_48E870`（键 = 位置，返回**下标**或 −1） | proposed |
| `Engine+0x5D880/frame-0x5C-group2-0x03-table` | `帧+0x5C/0x60` = `table_2` = `0x3 call-script` 位置表；**无 handler 读**，只在恢复路径当代码位置用 | proposed |
| `Engine+0x5D880/frame-0x64-group3-0x8f-table` | `帧+0x64/0x68` = `table_3` = `0x8f call` 位置表；恢复路径把表项 `+3` 压进 `帧+0x5EEA4` | proposed |
| `Engine+0x5D880/script-base-and-position-numbering` | `帧+0x14` = 代码区基址 ⇒ 位置 k ⟺ 字节 `headerLen + 4*k`（与反汇编器 label 同口径） | proposed |
| `Engine+0x5D880/frame-0x6C-0x70-position-not-index` | ★ **订正**：`帧+0x6C/0x70` 存**位置**不是下标（旧 accepted 条已按纪律撤回） | proposed |
| `Engine+0x5D880/frame-tables/batch-id-index` | 本批 id 索引 + 两条 id 引用勘误 | accepted |

* 三张表规模（本安装）：`table_1`(`0x71`) 90703 项 · `table_2`(`0x3`) 7588 项 · `table_3`(`0x8f`) 150781 项，共 **249072** 项；表项严格递增，首项 = 该类第一条指令。
* ★ **指令区终点不只由三张表决定**：type-2（字符串）与 `0x64` 的数组块也会把它前压（口径见 `apps/emulator/src/model/iterate.ts` 与台账 `model/iterate-dynamic-end`）——"三张表紧跟指令区之后"这句要按**前压之后的**终点读。
* ★ R3–R7 标 `proposed` 的理由：它们的"引擎侧"半（槽 → 表、`sub_48E870` 用法、`帧+0x70` 的写点、三处跳转算式）只有 `.lst` 逐字，**没有**可执行守卫。

## 命名对照（★ 显式化，不许静默改名）
原题写「三张表（**label** / message / call）」，实测是 **`0x71` / `0x3 call-script` / `0x8f call` 的位置表**：
* 旧仓那半句（`0x71` 消息表 / `0x3` call-script 表 / `0x8F` call 表）**成立**；
* 「`table_1` = **通用 label 表**」**不成立** —— 它的 90703 项 100% 落在 `0x71` 指令上，语料里**没有**一张"通用 label 表"；
* 「`帧+0x6C` 是**下标**」**不成立** —— 它存位置，下标只是 `sub_48E870` 的返回值。
⇒ 本单标题按上面的对照**显式改名**（理由就是本节，`git log` 可追）；旧题字样仍留在本节里可搜。

## 守卫（判据的可执行形式）
* `tools/test/emulator-frame-tables.assets.test.mjs`（`@env assets`，3 个用例）：三表恒相邻且序 t1→t2→t3 /
  `table_i` 逐元素 == 该类 opcode 的位置序列且长度 == 出现次数 / 头 `+32` 恒 28 且表项落点就是该 opcode。
* 变异自检：`tools/mutate-check.mjs` 的「头部 `table_1_offset` ↔ `table_2_length` 槽互换（40 ↔ 44）」⇒ 该守卫**当场红**（3/3 用例失败）。
* ★ **欠账**：三张表的**消费侧**（handler / 恢复路径）没有可执行守卫 —— 这正是 R3–R7 只能 `proposed` 的原因。

## 仍未查明
1. 存档**写入侧**怎么产生 `table_2`/`table_3` 的下标（没有"位置 → 下标"的查找例程）。
2. `sub_header_length ≠ 28` 的分支（491/491 都是 28，无样本）。
3. `0x71` 的正式语义名（`instruction-set.json` 里 `name` 是空串）。
4. `0x64` 数组块 / type-2 字符串块是否也有同形表；`sub_48F000(消息引擎, 帧+0x50, 下标)` 的后续语义。
5. 恢复路径的调用者与存档字段（`sub_40F750` 的 `0x410xxx` 调用点、`exit` 的 `帧+0x4C == −11/−10` 分支、那族 per-cur 存档数组）。
6. 命名欠账：`packages/age-format/src/engine/layout.mts` 的 `state6C/state70` 与 `tools/test/emulator-model.test.mjs` 那条用例名仍写「`+0x6C` 索引」。

★ 这 6 条**不在本单解决**（本单已收口），由 **`REQ-01M4GW5NTCWG9KQTTHHVYAT1JG`** 跟踪（parent 同为本单的父）。

## 收口凭据
参数区 `verify` 指向本单新建的守卫用例（`tools/test/emulator-frame-tables.assets.test.mjs`），
判据 = 「491 脚本上 `table_i` 序列 == 该类 opcode 位置序列 + 表长度 == 出现次数」。
