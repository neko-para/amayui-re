# tools/opcodes.md — `tools/opcodes.mjs`（指令表派生器）的**落点说明**

> 它**不是**工具台账（那个要 append-only 文本 + `--describe`）；这里只写"它动哪片数据、为什么这么动"。
> 命令与自描述：**`pnpm tools opcodes describe`** / `--report` / `--derive`。

## 它动哪片数据

| | 落点 |
|---|---|
| **来源（只读）** | `corpus/assets.json` 的 `knowledge/opcode-table-source` 条目 ⇒ 旧仓 `scripts/asm/opcodes.json`（`external-only`，只登记不入库） |
| **产物** | `packages/age-format/src/asm/instruction-set.json`（**唯一写入口就是本工具**；消费者 `src/asm/opcodes.mts` 的 `OPCODE_TABLE`） |

★ **来源登记在清单里、不硬编码旧仓路径**：换机器 / 旧仓搬家 / 旧仓移除，都只改登记一处，派生链不断。
（这正是清单作为 lockfile 的用法：记「来源与去向」。）

## 为什么是"派生"而不是"复制"

旧表里混着三类东西，判据是「**能不能再观测一次**」（不是"格式层 vs 知识层"）：

| | 字段 | 处置 |
|---|---|---|
| **观测**（格式层必需） | `opcode` / `argc` —— 决定"这四个字节怎么切" | 派生表保留 |
| **观测**（格式层不用） | `handler` —— 引擎分派表把 opcode 映射到哪个处理函数。★ 与 `argc` **同类**，而且它是"从引擎机械复核 `argc`"的唯一入口 | **继承**：登记 `knowledge/opcode-handlers`（不许当结论丢掉） |
| 文本层兼容 | `name` / `aliases`（87 条）—— 三方工具旧资产、**无独特信息** | 派生表保留（只决定输出什么 token，不影响怎么切） |
| **人工自述标签** | `status` —— `已核对`/`仅映射` | **丢弃**：没有机械复核路径；留痕 `knowledge/opcode-status-labels` |

★ `handler` 目前**不满足准入**：值只有 IDA 函数名 `sub_XXXXXXXX`、**没有 EA** ⇒ 无法按「锚点锚 EA」重校验。
所以它 `external-only` + `blocks: [K1,K2,K3]`：**K 线第一件事就是把分派表重挂成 EA**。
依据见 `docs/00-origin/knowledge-rebuild.md`（任何知识条目在 K3 通过前不得进新仓台账；守卫 #8 会红）。

## 怎么改

```bash
pnpm tools opcodes report          # ★ 先看：来源条目 / 条目数 / 将丢弃哪些字段与取值
pnpm tools opcodes derive          # dry-run：看派生结果（条数与字节数）
pnpm tools opcodes derive --write  # 落盘（写后回读复验，不绿回滚）
pnpm test                          # 守卫：tools/test/opcodes.test.mjs + packages/age-format/test/asm.test.mjs
```

* ❌ **不要手改** `instruction-set.json`（唯一写入口是本工具）；
* ❌ **不要**把 `handler` / `status` 加回来 —— 那会让格式层携带未过准入的知识；
* ✅ 来源换了（新版旧表 / 更可靠的命名）⇒ 先更新 `knowledge/opcode-table-source` 的 `sha256`，再重新派生。
