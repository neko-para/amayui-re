# corpus/assets.md — `corpus/assets.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段语义 / 枚举 / 不变量 / 怎么查 / 怎么改**全部由控制脚本自描述**：
> **`pnpm tools corpus describe`**（机器可读加 `--json`，源码就是那份 schema）。
> 本文件**不复述 schema**，只写"它是什么 + 非显然的口径 + 指向"。
> ★ **不要手改 `assets.json`**：唯一写入口是 `tools/corpus.mjs`。

## 它是什么

**lockfile** 性质的素材清单：记「来源与去向」。它**不是**约束（约束在 `AGENTS.md` 的规则条目 + 可执行守卫里），
也**不重复**校验和。**唯一能把它变成约束的是 `pnpm tools corpus validate` 必须能红** —— 没有守卫的清单等于一份 Markdown。

## 非显然的口径（脚本里学不到、必须写下来的那种）

* **每批入库件都配两条记录**：**载荷**（`storage: lfs` + `dest`）说"仓库里有什么"；
  **来源记录**（`external-only` + 逐件 sha256）说"从哪来、没被改过"。两者用 `derivedFrom` 相连
  （范例：`disasm/bundle` ↔ `disasm/raw-source-*`、`fixtures/save-samples` ↔ `fixtures/raw-source-save-samples`）。
* **入库件的 `origin` 不许带 `sha256`**（校验和交给 git/LFS）；翻牌 `deferred → lfs` 时工具**会自动剔除**残留的。
* **目录型 `dest` 的副本必须与来源逐字节相同**（每次 `validate` 现算 sha256 比对，不写进清单）。
* **`root=staging` 是唯一的例外**：它的 origin 允许缺席（中转区，不是长期位置）—— 只 warning。
* **守卫 #7 / #9 只对"真正入库的那一份"强制**（`storage ∈ {lfs,git}` 的 `disasm-corpus` 才必须有 `recipe`、
  必须有一条指向 `kind=binary` 的前身）；**"转码前的原件"与"明确不带的清单"豁免** —— 它们是参照件 / 清单，
  不是被加工的语料。
* **刻意不写**：体积、入库件 `sha256`、LFS oid、`status`、`generatedAt`、任何可从磁盘 / git 推导的计数。

## 怎么查

```bash
pnpm tools corpus describe        # ★ 字段 / 枚举 / 不变量 / 操作表（本文件不含这些）
pnpm tools corpus validate                           # 逐条断言（红/绿 + 详情）
pnpm tools corpus list                   # 条目一览
pnpm tools corpus validate --json # 机器可读
```

> ⚠ `docs/00-origin/init-prompt.md`（立项原文）里也有一份当年的字段表 —— 那是**历史记录，不是现行 schema**；
> 现行 schema 永远以 `--describe` 为准。

## 怎么改

**唯一写入口**：`tools/corpus.mjs`（`--add` / `--set` / `--set-root` / `--scan --write` / `--normalize --write`）。
缺省 **dry-run**；`--write` 时**写前先在内存里复验**（不绿一个字都不写）+ **写后回读复验**（不绿自动回滚）。

完整命令表见 `--describe` 的「怎么查 / 怎么改」一节；`deferred → lfs` 的翻牌示例也写在那里。

## 与 `corpus/fixtures/samples.json` 的分工

| | 本清单 | `samples.json`（说明书：`fixtures/samples.md`） |
|---|---|---|
| 管什么 | **来源 / sha256 / 存储去向**（跨域统一） | **文件级事实**：槽定位、源 mtime（git 存不下） |
| 谁读 | 守卫、迁移轮 | 未来的模拟器测试 |

两者**只重叠一个"文件集合"**，由 `tools/test/fixtures.test.mjs` 断言一致（各管一摊，不许互相抄）。
