# `data/spec/audit.json` —— 说明书

> 同名说明书（约定见 `../../docs/00-origin/decisions.md` §6，由 `tools/test/json-docs.test.mjs` 守）。
> 字段 / 枚举 / 不变量 / 操作的唯一真源是 `pnpm tools spec describe` —— 本文件不复述 schema。

## 这个文件是什么

**范围审计**：本轮验收域、已写章节的成熟度与残差、未写章节、每条规则的处理状态 / effect 选择器 / 残差 /
可证伪预言，以及**尚无规范条目的义务**。字段名与枚举只看 `pnpm tools spec describe`。
它回答"处理/替换/舍弃了哪些机器行为、哪些还没处理、下一步问什么"。

★ **候选是机器块，覆盖是 effect 级**：
- 试点块是**块入口 EA**（本仓当前试点：五个操作数原语的 case / default 块）；
- effect 由 `tools/lib/x86-effects.mjs` 从 `.lst` **机械枚举**（内存读/写、地址、算术、寄存器、栈、控制、调用…），
  每条带块入口与站点指令 EA（字段名见 `pnpm tools spec describe`）；
- 规则的覆盖声明是**选择器**（按块 / 类别 / opcode / 调用目标筛），不是 effect 实例，也不是"整条指令"；
  调用只记"调用" + 实际符号名（例如 `_atoi`、`_atof`、`__ftol2_sse`、`__CxxThrowException@8`），
  不另起 `parse-int` / `throw` 之类的名字；
- 内部省略逐类给出"实现内部、不作为领域行为"的理由；残差带**下一步问题**；
- 每个 effect 必须落在 **covered / internal / residual** 之一；**未登记 = 0**（assets 档现算）。

⛔ 它**不是**规范正文：可读正文只从 `chapters/*.md` 来；审计字段不进入分发产物。

## 章级残差与规则残差的形态**不同**

- **规则残差**（`rules[].residual`）是**某条规则的未处理项**：`id` / `kind` **必填**，`kind` 决定要不要额外的
  依据 / 范围 / 影响（由 `validate` 的第 4、5 条不变量强制）。
- **章级残差**（`chapters[].residual`、`planned[].residual`）是**域级待办**（这个域还缺什么），不是某条规则的义务：
  `id` / `kind` **可选**，但每项**必须**有 `what` + `next`。不强制 `kind` 的理由：章级残差常常跨多条规则、
  甚至跨未来才成篇的规则，强行给它一个处理状态会把"整个域未成篇"误标成某一类已定性的处理结果；
  已能定性的项仍会写 `kind`（如 `boot` 的几条）。
- 两者**不共用一个强制形态**，但都不许沉默：没有 `next` 的章级残差同样会被 `validate` 拦下。

## 未成篇义务的唯一落点：`gaps[]`

- **未成篇的 opcode 族/段**（有处理函数、但还没有任何规则**标题**覆盖它）与**按结构位置排除的机器分支**
  （如容器句柄的四个类型标签分支）都登记在 **`gaps[]`**；`ops` 列出该 gap 覆盖的 opcode
  （可选：非 opcode 级的 gap 不填）。
- `pilot.residuals` 只装**试点域内**已枚举 effect 的归属残差；`chapters[].residual` / `planned[].residual`
  只是**域级待办**。两者都**不是**"未成篇"的落点 —— 未成篇的东西不许只留在那里。
- `ops` 由 `format/opcode-table.json` 的 `hasHandler` 减去规则 `###` 标题里的 opcode **机械派生**
  （报告里「未成篇 opcode N（gaps 覆盖 M；已写成 S）」就是这份账）；守卫
  `tools/test/spec-gaps.test.mjs` 断言 `hasHandler = 已写 + gaps`，不重不漏。
- ★ **判据是标题级的**：派生只扫**带 `<!-- rule: -->` 的 `###` 标题**里的 `0x…`。
  ⇒ 一条规则若在正文里点名了 opcode（例如「配置」的「脚本访问」写了四条），**必须把它们也写进该规则的 `###` 标题**，
  否则会被算成未成篇；反过来 `##` 分组标题里的 opcode **不算**已写。
  ⇒ 同一 opcode 不许既被规则标题覆盖、又出现在某个 gap 的 `ops` 里（守卫会点名报红）。
- 字段 / 形状 / 派生口径只看 `pnpm tools spec describe`（本文件不复述 schema）。

## 怎么查

```bash
pnpm tools spec report                   # effect 覆盖 / 内部 / 残差 / 未登记 / 未成篇 opcode 计数（不给百分数）
pnpm tools spec effects --pilot          # 从语料机械枚举试点块的 effect（--json 给机器读）
pnpm tools spec effects sub_41BF50       # 一个函数的全部 effect
pnpm tools spec chapters                 # 章节成熟度 + 每章残差
pnpm tools spec validate                 # 处理状态凭据 / effect 闭合 / 跨文件一一对应
pnpm tools spec describe                 # 字段 / 枚举 / 不变量 / 操作（schema 真源）
```

## 怎么改

1. 加规则时在本文件里登记同一条规则（规则 `id` 与章节里的 `<!-- rule: … -->` 对齐）：处理状态、
   effect 选择器、残差、可证伪预言各自**要写什么、哪些必填、枚举有哪些** —— 只看
   `pnpm tools spec describe`（本文件**不复述 schema**）。
   还没有规范条目、但已经识别出来的义务 ⇒ 登记进 **`gaps[]`**（未成篇的唯一落点；`ops` 的覆盖由指令表
   机械派生），**不要**写成规则；还没成篇的域 ⇒ 登记进**待办章节清单**（不要塞进 manifest；可以是"部分"状态：规则已落在某章、但该域还没有独立章节）。
2. 处理状态各有**不同的必填子结构**（替换要说明保留 / 丢弃什么，省略要写理由，舍弃要写理由 / 范围 / 影响）——
   字段名与必填条件以 `pnpm tools spec describe` 为准。
3. **未登记义务 = 0**：新增试点块 / 改变 effect 枚举口径后，跑 `pnpm tools spec report`；
   未被选择器 / 内部省略 / 残差归属的 effect 会逐条列出。
4. 改完跑 `pnpm tools spec validate` + `report`；assets 档再跑 `pnpm test:assets`。
