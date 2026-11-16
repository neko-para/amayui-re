# 脚本内执行位置识别：sample 能直接返回 <script>:<offset>（替掉不可读的 ScreenHash/ScreenRect）

- id: REQ-01M7HC1Z1RD4DNH25BN5ZE1HQK
- type: spike
- status: done
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- done_reason: 位置今天就在 guest 内存里：pos = headerLen + (frame[cur].+0x04 - frame[cur].+0x00)，+0x04 是 PC（指进脚本体缓冲的 guest 指针，指向当前 opcode dword）、+0x00 是脚本体基址、headerLen 从装载头读（本语料 0x3C）。纯读 6 个 dword，与 MarkerSampler 同一函数 ⇒ 不需新采、不需钩子、不撞块边界 pump。坐标机械核验 0 反例（TITLE 622 指令/106 label；SYSTEM4 545/62）。TITLE 输入循环 = [0x039C,0x0460)（清单 56-64 行），退出条件唯一：local-int 3fc 属 {1,2,3}，其全部写点只在菜单 handler。前提：位置只对同一份脚本字节成立（汉化 insert-after 会移偏移）⇒ sample 必须带 script_sha。
- tags: [emulator-lift, reverse, script-pos]

## 一句话

搞清楚"**引擎现在在哪个脚本的哪条指令**"，让 `sample` 能直接返回 **`<脚本名>:<脚本内偏移>`**（例 `TITLE.BIN:0x1fc0`），
替掉不可读的 `ScreenHash` / `ScreenRect`（它们只判"像不像那一屏"，判不了"引擎在哪"）。

★ 坐标系已定（不用再研究）：偏移 = **绝对字节偏移、4 字节对齐**，与 `dist/views/data/TITLE.BIN.txt` 的 `label_%08x` 同一坐标系
（`packages/age-format/src/asm/assemble.mts:16`：`label = headerLen + 指令序号*4`）⇒ 指令序号 = `(offset-headerLen)/4`，
`.txt` 由同一解码器产出（`tools/patch.mjs`）⇒ 偏移 ↔ 清单行可机械映射（这一点请去**确认并给命令**）。

## 为什么（用户口径，2026-11-16）

> `ScreenHash` 和 `ScreenRect` 本质上依然是不可阅读的，但更细致的判定确实是需要的。
> 需要研究如何识别到 script 中具体的执行位置：像 TITLE 中的输入循环，会在 `TITLE.BIN` 中的特定的一个指令区间内。

## 要回答的问题

1. ★ **这个偏移在运行时存在哪里**（唯一的核心问题）：是 `frame[cur]`（步长 0x78）里的某一格？
   是 VM 循环（`0x4123F0`）里的某个寄存器/全局？还是脚本体在 guest 内存里的指针（那要先减基址）？
   请**逐字段定性** `+0x00…+0x78`（未知写未知，锚 EA）。
   若今天根本没有可读的偏移（引擎只保留"下一条要执行的指令"而无回写点）⇒ **如实写"没有"**，并给"最小可加入的记录点"
   （只能落在 Present / shim 回调这类**宿主侧汇点**，⛔ 不许块边界 pump）。
2. **TITLE 的输入循环区间**：给**偏移区间**（例 `TITLE.BIN:0x1f00–0x2050`）+ 清单行区间（两者用上面的机械映射对上）+ 退出条件。
3. **判定原语**：结论落到"`sample` 能不能直接返回它"：能 / 要新采 / 不建议，各一句（含稳定性：重解码或脚本改动后是否失效）。
4. **暴露面**：只在 guest 读点/宿主汇点纯读，还是加钩子（⛔ 后者=块边界 pump）。推荐 + 成本上界。
5. **替代关系**：hash/矩形 为什么不可读（现有 `.graph` 夹具举 1–2 例），位置判定怎么替代它（至少一条等价改写）。

## 判据（本节点收口时）

- §1 的字段表逐项带 **EA 锚**（⛔ 不锚 `.c` 行号）；"没有回写点"也是一个合法结论，但要说清缺什么。
- TITLE 输入循环给出**偏移区间 + 清单行区间 + 退出条件**，附一条可复跑命令。
- 候选谓词 ≥3 条，每条：判定式（含偏移区间）/ 取证锚 / 能不能做成守卫。

## 未做

不实现、不改产品代码、不进 DSL；"位置从哪读"若需要新增记录点，那是后续单。

## 结论（收口，2026-11-16）

```
pos(label 坐标) = headerLen + ( frame[cur].+0x04 − frame[cur].+0x00 )
```

- `+0x04`（`Engine+0x5D898`）= **PC**：指进脚本体缓冲的 **guest 指针**，指向**当前正在执行那条指令的 opcode dword**
  （`0x412825` 取 / `0x41282C` 读 opcode / `0x412843` 派发 / `0x413487` 每条步进 `4*operandCount`）。
- `+0x00`（`Engine+0x5D894`）= 脚本体缓冲基址（`0x40EE44 GlobalAlloc` → `0x40EE57` 存 → `0x40F6DB` PC 初值）。
- `headerLen = 0x20 + sub_header_length`（本语料 941/941 视图全 `SYS4450` ⇒ 0x3C）；★ 实现要**从装载头读**，0x3C 只作回退。
- **纯读 guest 内存**：与 `MarkerSampler::sampleIdentity()` 同一函数、多读 **2 个 dword** ⇒ 不需要新采、不需要钩子、不撞块边界 pump。
- ⛔ **不要用 `frame[cur].+0x58`**：算式确实是 `(pc−base)>>2`，但只有 9 处写、全在装载/正文/存档路径 ⇒ 快照会**陈旧**。`+0x5C` 全语料 **0 处读**。
- 坐标机械核验 **0 反例**：TITLE 622 指令 / 106 label、SYSTEM4 545 / 62（`node .tmp/script-pos/verify-listing-offsets.mjs …`）。
- ★★ **TITLE 输入循环 = `TITLE.BIN:[0x039C, 0x0460)`（清单 56–64 行）**，退出条件唯一：`local-int 3fc ∈ {1,2,3}`
  （`3fc` 的 12 个写点全在菜单 handler 里）⇒ 光标移动/按键**不会**离开循环。
- ★ **前提：位置只对同一份脚本字节成立** —— 汉化补丁的 `insert-after` 会插指令 ⇒ `sample` 必须带 `script_sha`，
  否则位置谓词在打了补丁的脚本上**静默失效**。

## 未验证（跟进项）

- opcode `0x0CD` 的助记符 `get-input-type` 来自 `instruction-set.json`（**命名层**，不是观察）；其 handler `sub_41ACD0`
  与 `0x8F call` 同形（压返回位 `(pc−base)>>2 + 1`）⇒ **语义未登记**（§6 准入门）。
- `frame+0x70/+0x74` 与 `packages/age-format` 的 `layout.mts` 里 `arrayContainer: 0x70`（3 dword）**记法冲突**：
  观察是两格各收一个 `operator new(0x10)` **指针**（`0x40F60C`→`0x40F63B`、`0x40F642`→`0x40F678`）⇒ 待裁决。
- 结论目前只在 `.tmp/script-pos/research.md`（scratch）⇒ 进 `data/ledger` / `docs` 另开单。
