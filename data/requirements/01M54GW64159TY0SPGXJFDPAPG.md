# 批 S1k · 调用链类型一致性：给被调方补载体（传播推断 + 逐条核调用点 witness）

- id: REQ-01M54GW64159TY0SPGXJFDPAPG
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 51
- verify: tools/test/shape-claims-guards.test.mjs#★⑭
- tags: [engine shape]

## 背景（用户的问题）

能不能检查"A 把 reg/local 传给 B 的 reg/arg"两侧引用的是不是同一个类型？只有一侧有类型时能不能发现？

## 结论：能判（三态），瓶颈（被调方没声明）已按**机械依据**批量解决

判据三步（真源：`pnpm tools shape describe` 的 `calls` 一节）：
①调用点上该寄存器的**来源**；②用 `types` 沿路径解析成类型；③取被调方在该槽位的 `kind:"arg"` 载体声明；
两者比 ⇒ `一致` / **`不一致`** / `单侧缺声明`（+ `适配器相容`：被调方是 `adapter:true` 的布局约束）。

## 做法：传播推断 + 逐条核 witness（⛔ 不按偏移命中率猜类，也不照抄提案）

1. `pnpm tools shape propagate --json` 的 `next` = "已被传播确定类型、尚无载体"的候选（本次 296 个）；
2. 每个候选的 `via` 里带一条 EA ⇒ **逐条回语料核**它是不是逐字的 `call <该函数>`
   （这正是 G16 的硬要求："至少一条 witness 落在调用点上"，⛔ 只凭"某行提到过 ecx"不给背书）；
3. 核过的一个候选写一条 `arg` 载体（`type` 由传播给出、`witness` 就是那条调用点）。
   一次性脚本 `.tmp/shape/s1k-add-carriers.mjs`（**不是**真源：产物仍只有 `data/shape/carriers.json`）。

* 可写 296 个；其中 **7 个**一加就红（把成员表缺口暴露出来）⇒ **撤回并逐条登记**在 `S1n`（带 id / fn / witness / EA）；
  剩下 **289 条**留下。

## 实测（改前 → 改后）

| | 槽位对账 | ✅ 一致 | ❌ 不一致 | 🔗 适配器 | ⚪ 被调方无类型 | ⚪ 调用方给不出 |
|---|---|---|---|---|---|---|
| 改前（233 载体） | 2912 对 | 447 | 0 | 107 | 2206 | 152 |
| 改后（522 载体） | **4520 对** | **1359** | **0** | 191 | 2624（对账基数同时变大） | 346 |

* `claims`：**全绿**（G1..G9 / J3 / 守恒①~④）；载体级闭合度 519 个载体函数 **L0 闭合 159 · 未闭合 360**；
* 残差 153（守恒① `9547 = 解释 9394 + 残差 153`）：新载体把"成员表解释不了的访问"**如实暴露**出来
  ⇒ 那是下一轮的清单，不是失败（⛔ 不许为了让残差归零收紧/放开声明）。

## 不覆盖（已在 `S1o` 跟踪，且 `calls` 自描述里写着）

栈上传参（要 ABI 证据）· 间接调用（目标未知，见 S1j/S1l）· `local` 载体当实参来源。

## 判据

* `tools/test/shape-claims-guards.test.mjs` 的 `★⑭`：断言 same / mismatch / 单侧**三态**，
  且"把被调方声明改一下就要求判定翻转"；
* G16（witness 必须落在调用点）与变异清单（对账不再区分不一致 ⇒ 当场红）。

## 怎么查

```bash
pnpm tools shape calls                # 三档 + 五类计数
pnpm tools shape propagate --json     # 还有哪些函数"已推断但没载体"
pnpm tools shape claims data/shape/carriers.json    # 载体改动后必须全绿
```
