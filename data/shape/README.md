# `data/shape/` —— 对象 shape 的**声明**（模型只写判断；工具给坐标与判定）

> 本目录是**汇编层（A 层）**的产物：回答"这个对象在内存里长什么样"（有哪些槽、各多宽、哪里是数组、
> 元素多大、步长多少、索引从哪个槽来）。⛔ **不含**名字 / 用途 / opcode 语义 —— 那些是引擎语义层（B 层），
> 走 `AGENTS.md` §6 的准入门、进 `data/ledger/`。
> 口径与判据的真源是 `docs/02-engine/shape-census.md`；**字段 / 义务 / 守卫 / 诚实项**的真源是
> `pnpm tools shape describe`（本目录**不复述** schema）。

## 1. 为什么这么分（两张表，不是一张）

| 落点 | 是什么 | 谁写 |
|---|---|---|
| `types.json` | **类型真源**：每个类型**按 id 共享一份**成员表（少数定义、多数载体） | 只有**一处**：加成员就是改这一份 |
| `carriers.json` | **载体真源（唯一一个）**：只放 `carriers`（机器槽 → 类型），`typesFrom: "types.json"` | ⛔ **不写 types**；话题不是存储轴（见 `carriers.md`） |

★ **为什么类型不放在族文件里**：类型是**中心化**数据。把它写进某个族文件，会让**其它族文件**被迫
`typesFrom` 引用那一个族（"数值族的文件成了全仓的类型真源"）—— 组织上完全说不通，而且改一处类型要动
无关文件。⇒ 类型独立成 `types.json`；族文件之间的引用关系**为零**。
（`typesFrom` 的合并语义仍保留：同名类型按**成员**增量合并、同 `off` 本地优先 —— 它是**合并机制**，不是组织方式。）

## 2. 三层不许混（判据可执行）

* **本目录 = A 层**：身份只许用 **EA · 地址表达式 · 位次名（`obj0`）**；语义名（`cur` / `frames` / 帧记录…）
  ⛔ 不许当键、不许进产物。
* **类型 ↔ 载体的分工**：字段**不是**载体 —— 它是容器的成员（写在 `types.json` 的成员表里）；
  `carriers` 只放**机器槽**（`arg` / `local` / `global`）。
* **opcode ↔ handler 的桥**不属于 A 层、也不属于 B 层：它是可机械核的**语料事实**，用
  `pnpm tools opcodes handlers` 现查，⛔ 不要写进本目录的 JSON。

## 3. 怎么查

```bash
pnpm tools shape describe                       # 分节 / 义务 / 两道守卫 / 诚实项（schema 真源）
pnpm tools shape claims data/shape/types.json   # 类型真源自校验（G1/G4/G6/G8）
pnpm tools shape claims data/shape/carriers.json    # 载体 × 类型 ⇒ 解释 / 残差 / 守恒①②③④ + 闭合度
pnpm tools shape access  sub_42C5E0             # 某个函数的逐 site 访问记录（EA 锚）
pnpm tools shape census                         # ★ 全表普查（有 handler 的 opcode）：偏移聚类 / 索引簇 / 未解析报数
pnpm tools shape bodies                         # ★ handler 体骨架：记录槽写法 + 调用序列 ⇒ 按骨架分组（族边界的依据）
pnpm tools shape regions sub_40ED40 edi         # 机械导出的段落（`roles.span` 只能引用它）
pnpm tools shape verify  0x42C5F1               # ★ 独立复核器：按 EA 回语料重解一条（不信模型转述）
pnpm tools mutate check --match shape           # 守卫自检：改坏一处 ⇒ 对应守卫必须当场红
```

`pnpm tools shape claims --sample` 是**内置样本**（13 个函数的载体 + 两个共享类型）：改了校验器先跑它。

## 4. 怎么改

* **加/改一个类型成员** ⇒ 只动 `types.json`，然后**跑一遍所有族文件的 claims**（类型是共享的，
  改一处会影响每一个引用它的族：G1 重叠 / G2 覆盖 / G3 步长 / G4 边界都会重新判）。
* **加一个载体** ⇒ 只动对应族文件；`id` 用中性编号，⛔ 不要写助记符 / opcode / EA / 行号。
* **只为准而编成员**是这个目录里**最严重的错误**：判据是"这条成员有观察撑着吗"，
  报不出来就**如实报残差**（残差是清单，不是失败）。
* **改了校验器** ⇒ 往 `tools/mutate-check.mjs` 的清单里加一条（否则"它会红"只是个声称），
  并同步 `pnpm tools shape describe` 的自描述（那是 schema 真源）与 `docs/02-engine/shape-census.md`。

⛔ 不许在文档里复述 schema / 字段表 / 计数（会随下一轮干活而变错）—— 指向 `--describe` 与现跑命令。
