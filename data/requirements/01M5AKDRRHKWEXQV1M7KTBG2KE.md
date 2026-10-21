# spec 文档与需求树治理二批：planned 对齐 / 键名复述 / 路径标记

- id: REQ-01M5AKDRRHKWEXQV1M7KTBG2KE
- type: req
- status: done
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- order: 65
- verify: tools/test/spec-mechanisms.test.mjs#★ 机制表是闭集：planned[].mechanism 只能是表里的 id 或 core-out
- tags: [engine spec docs infra]

## 六条各自的处置（2026-10-21/22）

| # | 处置 | 怎么验 |
|---|---|---|
| 1 planned ↔ 机制对齐 | **已落地**：`audit.planned[]` 每项加 `mechanism` 字段（`containers→CORE-POOL`、`objects→CORE-OBJECTS`、`save→FMT-SAVE`、`input`/`presentation→core-out`、`ops-unwritten→CORE-OPS`）；`spec.mjs` 的 describe/validate 同步；新增 `repo:tools/test/spec-mechanisms.test.mjs` 守"闭集 / 每项有归属 / 每个机制至少一条需求单在跟踪"。 | `node --test --test-isolation=none tools/test/spec-mechanisms.test.mjs`；变异：删掉 `objects` 的 mechanism ⇒ 守卫红（`--match spec-mechanisms` 实测 1/1）。 |
| 2 键名复述 | **已完成（选 a）**：`repo:data/spec/README.md` 与 `audit.md` 不再枚举 `bindings[]` / `rules[]` / `gaps[]` / `pilot.sites` / `covers` / `disposition=…`，一律"字段见 `pnpm tools spec describe`"。 | `node --test --test-isolation=none tools/test/json-docs.test.mjs` · `pnpm tools spec validate` |
| 3 路径前缀 | **已完成**：`repo:data/requirements/README.md` §7 写清 `repo:` / `old:`；`validateAll` 新增不变量 #6（`repo:` 必须存在、支持 glob；`old:` 只标记）；describe 同源。 | `pnpm tools requirements validate`（#6）；`repo:tools/test/requirements.test.mjs` 的 #6 用例；变异 `--match requirements.test.mjs` 实测会红。 |
| 4 开单口径 | **已完成**：本目录 §8 写"文件:行 + 现查命令"，不写"当前状态"式断言；已漂实例只在被引用时订正。 | `repo:data/requirements/README.md` §8 |
| 5 分发边界 | **已满足**：`repo:data/spec/chapters/README.md` 无 core/README 指针与仓内路径；"机制地图是否进分发清单"是产品取舍，等用户裁决。 | `rg -e core/README -e data/spec/ data/spec/chapters/` |
| 6 机制表机器形态 | **明确暂缓**：§2 仍是散文表（只把 `planned[].mechanism` 做成了机器字段）；等工具真要读 `dependsOn` 时再落 JSON。 | — |

## 边界

* 第 1 条的"每个机制有需求子单"是**存在性**判据（需求单可以仍 open），不是"机制已做完"。
* 第 2 条选 (a) 删第二份 schema，没有新增字段一致性守卫 —— 因为"文档里出现某字段名"的守卫会随措辞变红，收益低。

## 怎么查

`pnpm tools spec describe` · `pnpm tools spec report` · `pnpm tools requirements validate`。
