# `tools/test-frozen.json` —— 永久不跑的测试清单

> 冻结清单：这些用例永久不跑（只保留 `data/spec` 的 evidence 锚）；要跑用 `--include-frozen`。
> 字段只有 `frozen[]`；schema describe 以 `tools/test-run.mjs` 为真源（无 `--describe`）。
## 怎么查
```bash
node tools/test-run.mjs --list                     # 末行「冻结（永久不跑）N 个」
node tools/test-run.mjs --list --include-frozen    # 冻结文件计入 picked（不再被去掉）
```
## 怎么改
* 加 / 删条目只改 `frozen[]` 并保持排序；坏 JSON ⇒ `tools/test-run.mjs` **报错退出 2**（不静默当空表）。
* 文件不存在 ⇒ 只是没被扫到、不报错；`--list` 的 N 是**实际存在的**冻结文件数。
* ⛔ 不要用 `pnpm test` 的「绿」证明这些用例还能跑：冻结 = 文件在、证据锚在、默认不跑。
