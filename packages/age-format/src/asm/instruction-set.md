# packages/age-format/src/asm/instruction-set.md — `packages/age-format/src/asm/instruction-set.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段 / 不变量 / 怎么查 / 怎么改**一律由控制脚本自描述**：
> **`pnpm tools opcodes describe`**（机器可读加 `--json`）。本文件不复述 schema，只写"它是什么 + 非显然口径 + 指向"。
> ★ **不要手改** `instruction-set.json`：唯一写入口是 `pnpm tools opcodes derive`。
> ★ **文件名有意不叫 `opcodes.json`**：那是旧仓**原表**的名字，容易被误读成"这份就是那份"；
> 这份是**派生**出来的格式层四列。

## 它是什么

AGE 脚本（`.BIN`）的**格式层**指令表：每条 = `{ opcode, argc, name, aliases }`（**只有这四列**）。
`argc` 是**唯一决定指令边界的字段**（一条指令占 `4 + argc*8` 字节）；
`name` / `aliases` 是反汇编输出与重汇编解析共用的助记符。

它是**从旧仓旧表机械派生**的，不是复制品：
`pnpm tools opcodes derive` 读旧仓 `scripts/asm/opcodes.json`，**只抽格式层四列**。

## ★ 非显然的口径：判据是「**能不能再观测一次**」，不是"格式层 vs 知识层"

旧表里混了三类东西：

| 字段 | 类别 | 复核路径 | 处置 |
|---|---|---|---|
| `opcode` / `argc` | **观测**：引擎实际怎么切字节流 | 正在用：104/106 个真实 `.BIN` 往返逐字节相同 | 派生表保留 |
| `handler`（574 条、543 个不同函数） | ★ **观测**：引擎分派表把 opcode 映射到哪个处理函数 | 引擎分派表 → 函数 **EA**（**当前未知入口，但路径存在**） | ★ **继承**：登记 `knowledge/opcode-handlers`。格式层不需要它，**但它不该被丢** |
| `status`（`已核对` / `仅映射` / `已核对（2026-09）`） | **人工自述标签** | **无** —— 没有机械复核路径 | **丢弃**（留痕 `knowledge/opcode-status-labels`） |
| `name` / `aliases`（87 条） | 三方工具旧资产，**零独特信息** | 后续逆向更可靠 | 保留作**文本层兼容层**（只决定输出什么 token，不影响怎么切） |

★ **`handler` 与 `argc` 是同类，不该一个留一个丢**：两者都是从引擎读出来的观测，
只是格式层现在只用得到 `argc`。所以 `handler` 走**继承**（登记条目），而不是"当结论丢掉"。
我此前把它称作"知识结论"是错的 —— 它是**数据**，缺的只是"分派表在哪"这条知识。

★ **为什么 `handler` 还是不能进格式层**：它不满足准入 —— 值只有 IDA 函数名 `sub_XXXXXXXX`、
**没有 EA**，按「锚点锚 EA」根本重校验不了。所以它 `external-only`、`blocks: [K1,K2,K3]`：
**K 线第一件事就是把分派表重挂成 EA**，挂上之前不得进台账。

★ 另外，本表**不入库**是守卫 #8 钉着的：`kind=knowledge-source` 只能 `external-only`/`deferred`
（口径见 `docs/00-origin/knowledge-rebuild.md` §3 的"顺序纪律"）。

### ⚠ 被留下但**同样属于语义**的东西：助记符（`name` / `aliases`）

本表里有 **87 条带助记符**（`abort` / `ret` / `set-render-target` …），其余 487 条为空串。

**它们没有任何独特信息**：那是旧仓从**旧第三方工具**继承来的命名，与上游工具同源；
真要知道某条指令干什么，**后续逆向的结论更可靠**（而不是这种继承来的名字）。
留下的理由只有一条、且是可执行的：反汇编要**输出 token**、重汇编要把它**读回来**，
"输出什么 token"必须有单一来源 —— 而助记符的取值**不影响"这四个字节怎么切"**（那是 `argc` 的事），
所以它只是**文本层的兼容层**，不是"这条指令干什么"的结论。

★ 将来若 K 线给出更可靠的命名，**直接替换这 87 条并重新派生**即可：判据是"反汇编→重汇编仍逐字节相同"，
与名字叫什么无关（未命名条目本来就输出规范名 `iXXX`）。

## ★ 已登记的三条：来源、继承、留痕

| 条目 | 登记什么 | 为什么 |
|---|---|---|
| `knowledge/opcode-table-source` | 旧仓 `scripts/asm/opcodes.json`（574 条、76574 B、sha256 `4735cbd0…41355`） | **派生器的来源**：`pnpm tools opcodes` **按条目解析路径**，不硬编码旧仓位置 ⇒ 换机器 / 旧仓搬家 / 旧仓移除都只改登记一处，派生链**不断** |
| `knowledge/opcode-handlers` | 同一份文件里的 `handler` 一列（574 条 / 543 个函数） | **继承观测**：它与 `argc` 同类，只是格式层不用；**K 线最该先救的就是它**（复核 `argc` 的唯一入口） |
| `knowledge/opcode-status-labels` | 同一份文件里的 `status` 一列 | **留痕**：说明"为什么单单它被丢掉" —— 人工自述标签，没有机械复核路径 |

★ 三条都是 `kind: knowledge-source` + `storage: external-only` ⇒ 守卫 **#8** 钉住它们
**不得被搬进新仓**，直到 K 线处理（`docs/00-origin/knowledge-rebuild.md` §3 的"顺序纪律"）。

## ★ 权威性说明：`argc` 的权威来自**引擎**，不是这份表

`argc` 不是"三方工具说了算"的字段：它**可以从引擎机械算出来** ——
拿 `handler` 指到的那个函数（引擎的指令处理器），数出它**推进字节偏移的位置**，就得到每条指令的长度。
⇒ 本表里的 `argc` 是**待复核的副本**，不是权威。

* **复核的入口就是 `handler`** ⇒ 它是知识层里**最该先救**的东西（丢了它，机械复核就没有起点）。
  它现在**不在新仓**（只有 `sub_XXXXXXXX` 这样的 IDA 函数名、没有任何 EA）⇒ 归 K 线（本条目 `blocks: [K1,K2,K3]`）。
* 在 K 线复核之前，"这份 `argc` 对不对"**不是靠信任、而是靠可执行观测**：
  `pnpm tools opcodes derive` 出的表能让 **106 个真实 `.BIN` 里 104 个**往返逐字节相同
  （另 2 个不是 AGE 脚本）⇒ `argc` 至少与引擎的实际切分**一致**。
  这条观测就是本包 `argc` 当前的凭据（与知识层要求的"绑定可再校验的观察"同一口径）。

## 怎么查

```bash
pnpm tools opcodes describe     # ★ 字段 / 不变量 / 有意丢弃的字段 / 操作（本文件不含这些）
pnpm tools opcodes report       # 对账：旧表条目数、字段计数、**将丢弃哪些字段与取值**
pnpm tools opcodes derive       # 看派生结果（dry-run，不落盘）
# 格式层自身的守卫（四列齐备 / opcode 唯一 / argc 合法 / 不含知识层字段）
node --test --test-isolation=none packages/age-format/test/asm.test.mjs
```

## 怎么改

* **唯一写入口**：`pnpm tools opcodes derive --write`（缺省 dry-run；写后回读复验，不绿回滚）。
* 要更新指令集：**先在旧仓/K 线把旧表改对**，再跑一次派生；**不要**在这里手工加字段。
* 派生是确定性的（一个 opcode 一行、键序固定）⇒ `git diff` 只会显示真正变了的那些指令。
* ★ **绝不要**把 `handler` / `status` 加回来 —— 那会让格式层携带未过准入的知识；
  真要登记这类结论，走知识线（K1 清理 → K2 校验 → K3 重分类）。
