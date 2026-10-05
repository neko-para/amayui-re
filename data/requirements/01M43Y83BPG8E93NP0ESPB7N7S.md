# 🐞S2 视图范围曾按「说话人标注集」筛（223 支）⇒ 全库检索静默少报

- id: REQ-01M43Y83BPG8E93NP0ESPB7N7S
- type: bug
- status: done
- parent: REQ-01M3XJXVYBRFW1VT7RD8SNRXKV
- verify: tools/test/patch.test.mjs#★ 视图范围
- repro: tools/test/patch.test.mjs#★ 视图范围
- severity: S2

## 现象（观测到的分歧）
`dist/views/data` 只有 **223** 个文件，而 patch 里的条目是 453 支、基线根里可反汇编的脚本是 941 支。
223 恰好 = `SPEAKER_FILTER`（`/^(\$\d+\$)?(SC|SP)/i`）挑出来的那批 —— 与
`docs/01-translation/patch-design.md` §2.3 记录过的那个错（把"说话人**标注**的口径"当成
"处理过的脚本集合"）**是同一个错**，只是在**视图层**又犯了一次。

## 为什么危险（不是"少几个文件"）
视图是"全库检索 / 并排读"的面：`rg dist/views` 拿到的是 **223 支上的答案**，而且**不报错** ⇒
术语一致性、先例查询、旧文档断言核实都会**静默少报**。
复现：拿一支非 SC/SP 的脚本（如 `$1$ITINIT.BIN`）做单支视图与全量视图对比，后者才含它。

## 收口（已修，且**从源头掐掉**）
* 落盘的东西**只剩依赖不可变输入的那些**：**基线索引**（名单 + 逐支指纹 + **codec 指纹** + 基线指纹）与
  **base 文本**；`src` 投影不常驻（查询按锚现算，见 `REQ-01M45AGK4JAGCX4KMZA6KRGBR1`）；
* ⇒ "名单取自一个可能只覆盖 223 支的清单"这件事**不可能再发生**：名单与逐支指纹来自基线索引，
  而索引的键是（基线指纹，codec 指纹）—— 它新鲜与否与"某一批脚本"无关，陈旧就整份作废（`patch index --write`）；
* `patch status` 报索引/base 视图/草稿账本的状态；`--view` 缺省范围仍是 `all`；
* 判据：`tools/test/patch.test.mjs#★ 视图范围`（三个范围互不相等）+ `#★ 基线索引` + `#★ merge on read`。
