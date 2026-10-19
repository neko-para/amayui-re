# `data/spec/audit.json` —— 说明书

> 同名说明书（约定见 `../../docs/00-origin/decisions.md` §6，由 `tools/test/json-docs.test.mjs` 守）。
> 字段 / 枚举 / 不变量 / 操作的唯一真源是 `pnpm tools spec describe` —— 本文件不复述 schema。

## 这个文件是什么

**范围审计**：本轮验收域（`pilot`）、已写章节的成熟度与残差（`chapters[]`）、未写章节（`planned[]`）、
每条规则的处理状态 / effect 选择器 / 残差 / 可证伪预言（`rules[]`）、以及**尚无规范条目的义务**（`gaps[]`）。
它回答"处理/替换/舍弃了哪些机器行为、哪些还没处理、下一步问什么"。

★ **候选是机器块，覆盖是 effect 级**：
- `pilot.sites` 是块入口 EA（本仓当前试点：五个操作数原语的 case / default 块）；
- `effect` 由 `tools/lib/x86-effects.mjs` 从 `.lst` **机械枚举**（内存读/写、地址、算术、寄存器、栈、控制、调用…），
  带 `block`（块入口）与 `site`（指令 EA）；
- 规则的 `covers` 是**选择器**（`{blocks?, kinds?, ops?, targets?}`），不是 effect 实例，也不是"整条指令"；
  调用只记 `kind=call` + `target=<实际符号>`（例如 `_atoi`、`_atof`、`__ftol2_sse`、`__CxxThrowException@8`），
  不另起 `parse-int` / `throw` 之类的名字；
- `pilot.internal` 逐类给出"实现内部、不作为领域行为"的理由；`pilot.residuals` 是带 `next` 的显式残差；
- 每个 effect 必须落在 **covered / internal / residual** 之一；**未登记 = 0**（assets 档现算）。

⛔ 它**不是**规范正文：可读正文只从 `chapters/*.md` 来；审计字段不进入分发产物。

## 怎么查

```bash
pnpm tools spec report                   # effect 覆盖 / 内部 / 残差 / 未登记（不给百分数）
pnpm tools spec effects --pilot          # 从语料机械枚举试点块的 effect（--json 给机器读）
pnpm tools spec effects sub_41BF50       # 一个函数的全部 effect
pnpm tools spec chapters                 # 章节成熟度 + 每章残差
pnpm tools spec validate                 # 处理状态凭据 / effect 闭合 / 跨文件一一对应
pnpm tools spec describe                 # 字段 / 枚举 / 不变量 / 操作（schema 真源）
```

## 怎么改

1. 加规则时在 `rules[]` 加同一条 `id`：`status/draft|reviewed`、`disposition`、`covers`（选择器）、`residual`、`falsifiers`。
   还没有规范条目、但已经识别出来的义务 ⇒ 在 `pilot.residuals` 里加带 `next` 的选择器；
   还没写的章节 ⇒ 放进 `planned[]`（不要塞进 manifest）。
2. `disposition=replaced` 要写 `replacement`（preserves/discards/chosen/scope）；`omitted` 要写 `omission`；
   `discarded` 要写 `discard`（reason/range/impact）。判据见 `pnpm tools spec describe`。
3. **未登记义务 = 0**：新增试点块 / 改变 effect 枚举口径后，跑 `pnpm tools spec report`；
   未被 `covers` / `internal` / `residuals` 归属的 effect 会逐条列出。
4. 改完跑 `pnpm tools spec validate` + `report`；assets 档再跑 `pnpm test:assets`。
