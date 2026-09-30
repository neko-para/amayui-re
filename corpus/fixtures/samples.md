# corpus/fixtures/samples.md — `corpus/fixtures/samples.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段 / 不变量（含"谁在守它"）/ 怎么查 / 怎么改**全部由控制脚本自描述**：
> **`pnpm tools fixtures describe`**（机器可读加 `--json`）。
> 本文件**不复述 schema**，只写"它是什么 + 非显然的口径 + 指向"。
> ★ **不要手改 `samples.json`，也不要手工 `touch`**：唯一编辑入口是 `tools/fixtures.mjs`。

## 它是什么

`corpus/fixtures/` 这一批**真存档样本**的**结构化描述**：一个样本 = 一个槽 = 一对文件（`.DAT` + `.STH`）。
它只管**文件级事实**（槽 → 游戏内定位 → 各文件的源 instant）。

**存储去向不在这里**：那是 `corpus/assets.json` 里 `fixtures/save-samples` 一条（说明书 `../assets.md`）。
而**来源不登记在任何地方** —— fixture 是**自足条目**（固化资源，没有加工链，入库的那一份就是原件），
所以清单不记 `origin`；`slot` + `where` + 源 mtime 这三样合起来**就是**它的身份。

## 非显然的口径（脚本里学不到、必须写下来的那种）

* ★ **mtime 是判据的一部分**：槽头 `+264` 起七个 u16（年,月,星期,日,时,分,秒）记的就是**存档时刻**，
  测试断言「至少一个真槽的头 == 文件 mtime」。而 **git 不保存 mtime** ⇒ 必须记下来。
  记的是**源文件的 instant + 采集时区**（不是"本机墙上时间"），所以
  `pnpm tools fixtures restore-mtime --write` 在**任何机器 / 任何时区**上都能拨回正确值（手工 `touch -t` 不能）。
* **`where` 由来源方给出**，机器推不出来 —— 所以加样本时 `--where` 是必填。
* **不加样本就不需要源目录**：本文件是自足的（`restore-mtime` 只需要它自己）；
  只有 `add` / `refresh` 才要源目录，缺省落在清单登记的 `roots.gameSaves`（真游戏存档目录 —— 它是**活目录**，
  被游戏覆盖会让 `refresh` 报 `sync`，这是设计使然）。
* **消费**：只读；环境构造靠复制到 overlay；**不要**伪造样本去骗断言，**不要**给样本改名凑用例
  （完整口径见同目录 `README.md`）。

## 怎么查

```bash
pnpm tools fixtures describe      # ★ 字段 / 不变量（含谁在守）/ 操作表（本文件不含这些）
pnpm tools fixtures list                 # 槽 / 定位 / 每个文件是否与记录的 instant 一致
pnpm tools fixtures list --json
```

## 怎么改

**唯一编辑入口**：`tools/fixtures.mjs`。缺省 **dry-run**，`--write` 才落盘；
写后复验，不绿回滚。给了来源（`--from` / `--root`）时会**顺带**把 origin 写进 `corpus/assets.json`
（经 `corpus.mjs` 的 `saveManifest` ⇒ 清单仍只有一个写入口）；不说来源就**不碰**清单。

```bash
pnpm tools fixtures add <槽> --where "<定位>" [--from <源目录> | --root <roots 名>] --write
pnpm tools fixtures refresh <槽> [--from <源目录>] --write   # 重取（缺省按登记的 origin；没有就如实报出）
pnpm tools fixtures refresh-all [--from <源目录>] --write
pnpm tools fixtures restore-mtime --write     # 刚 clone：按记录的 instant 拨回
pnpm tools fixtures remove <槽> --write
pnpm tools fixtures normalize --write         # schema 变过之后拉回规范形态
```

加 / 删之后跑一次 `pnpm tools corpus validate`（守卫 #3/#6 会核对目录非空与 LFS 属性）。
