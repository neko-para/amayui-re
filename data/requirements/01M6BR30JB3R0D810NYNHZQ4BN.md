# T1 · 类型可证伪性清单：零成员 / 尾部缺口 / 无人引用（shape debt，⛔ 不判红）

- id: REQ-01M6BR30JB3R0D810NYNHZQ4BN
- type: req
- status: done
- parent: REQ-01M6BPB07YQ73NYVMYNFPQ1X26
- order: 1
- verify: tools/test/shape-claims-guards.test.mjs#★㉞ 类型欠账
- tags: [shape tooling]

## 一句话

`census --debt` 的分母是"有载体的观察集" ⇒ **零成员 / 无载体的类型它看不见**（`T_FadeTimer` 因此能长期全绿）。
本单给每个已声明类型报"它有没有被任何东西约束"，把三类可疑声明单列出来。

## 判据（做完的样子）

1. `pnpm tools shape debt` 能跑：每个类型报 `{成员数, size, 成员跨度, 载体数, 被引用次数, 观察档站点}`。
2. **零成员**（有 `size` 却零成员）与 **无人引用**（无载体且无人引用）必须单列；
   `count:null` / 元素 `size` 未知时**不判"尾部缺口"**（跨度算不出来就不许下结论）。
3. 三档**不判红**（exit 0）：零成员可能只是还没补 —— 判红会逼出"为了变绿编成员"。
4. 入库模型上跑一次必须点名 `T_FadeTimer` / `T_FileDB_424_10` / `T_Sound_690`，并顺带暴露
   `T_Scene_elem14` 的 4 字节尾部缺口。
5. 守卫：`tools/test/shape-claims-guards.test.mjs#★㉞`（合成 types/carriers：三档 + 互斥 + `spanUnknown`）；
   变异：把 `flags.push('zero')` 拆掉 ⇒ 该守卫当场红。

## 不做什么

- ⛔ 不把"零成员"写成硬红；⛔ 不为了清空报表编成员（报表只点名，不裁决）。
