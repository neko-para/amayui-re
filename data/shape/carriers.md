# `carriers.json` —— 唯一的**载体**真源（机器槽 → 类型）

> 同名数据文件的说明书。字段 / 枚举 / 义务 / 守卫的真源是 `pnpm tools shape describe`（本文件**不复述 schema**）。
> 类型不在这里：`types.json` 是**中心类型表**，本文件一律 `typesFrom: "types.json"` 引用它。

## 它是什么

每个 `carrier` 声明**一个机器槽装的是哪个类型**：

* `kind: "arg"` —— 函数的某个入参槽（`__thiscall` 的对象根 `ecx` 就是它）；
* `kind: "local"` —— **某一段**里某个寄存器装的是某个类型（`span` 必须逐字等于 `regions` 的活窗口）；
* `kind: "global"` —— 数据段符号。

⛔ 字段**不是**载体：它是**容器的成员**，写在 `types.json` 的成员表里。

## 为什么是**一个**文件（而不是按 topic 分族）

早期按"族"分（`numeric-handlers.json` / `control-flow-handlers.json` / …），但实测：
函数总量以千计，而**大多数函数根本没有 topic** —— 分族会逼着人给每个函数编一个族名，
于是"这个载体该放哪个文件"变成没有答案的组织问题；同一类型的载体还会被摊到多个文件里，
改一处要动无关文件。

⇒ 现在：**载体只有一个文件**，放置取决于"有没有被观察撑着"，不取决于"属于哪个话题"。
**话题（族 / 骨架组）不是存储轴，是查询**：现跑 `pnpm tools shape bodies`（骨架分组）、
`pnpm tools shape census`（偏移聚类）就是"族"这一层。

★ 数据是**直接更新**的（发现错误就改对），⛔ 不记历史版本 —— 历史看 `git log -L`。

## 怎么查

```bash
pnpm tools shape describe                    # schema 真源：字段 / 三种载体形态 / 义务 / 守卫 / 诚实项
pnpm tools shape claims data/shape/carriers.json    # 载体 × 类型 ⇒ 解释 / 残差 / 守恒①②③④ + 闭合度
pnpm tools shape claims data/shape/types.json       # 类型表自校验
pnpm tools shape calls                       # ★ 调用链对账：调用方实参类型 ↔ 被调方载体声明（跨函数）
pnpm tools shape coverage <sub_XXXXXX>       # ★ 闭合度：这个函数"分析完了吗"（L0 判据）
pnpm tools shape access <sub_XXXXXX>         # 逐 site 的访问记录（EA 锚）
```

## 怎么改

1. **加载体**：先跑 `--access <fn>` 拿站点（`base=` / `obj=` / `abs=` / `w=`），
   载体只写**机器槽 + 类型**，⛔ 不写助记符 / opcode / EA / 行号（身份只许 EA / 地址表达式 / `obj0`）。
2. **`kind:"local"` 的 `span`** ⛔ 不许自己写：必须逐字等于 `--regions <fn> <reg>` 导出的**活窗口** `[from..liveTo]`。
3. **类型**改在 `types.json`（共享 ⇒ 改完跑一遍 `claims` 全部；G1 重叠 / G2 覆盖 / G3 步长 / G4 边界都会重判）。
4. **改了校验器** ⇒ 往 `tools/mutate-check.mjs` 加一条变异（否则"它会红"只是声称），
   并同步 `pnpm tools shape describe` 与 `docs/02-engine/shape-census.md`。
5. **只为准而编载体 / 成员**是本目录最严重的错误：判据是"有观察撑着吗"，报不出来就**如实报残差**。

⛔ 文档里不复述 schema / 字段表 / 计数（会随下一轮干活而变错）—— 指向 `--describe` 与现跑命令。
