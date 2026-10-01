# corpus/assets/ui-images/versions.md — `corpus/assets/ui-images/versions.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段 / 不变量 / 怎么查 / 怎么改**全部由控制脚本自描述**。
> ⚠ **本文件目前没有控制脚本**：它是一件**只读事实表**（不是由工具生成的台账），`describe` 暂时无处可指。
> 图像烤箱（UI 烘焙链）落地时**必须**补一个控制脚本并让它 `--describe`，届时"怎么改"迁到那里。
> **不要手改**：见下面的「怎么改」。

## 它是什么

`corpus/assets/ui-images/` 这批 UI 图的**版本表**：一张图 = 一个块名（`SO<NNN><后缀>`），
块名的版本号在各机器上**推不出来**（入库的只有生效版那几张），所以必须记下来。

**为什么推不出来**：入仓的 PNG 只有**生效版**（+ 未汉化图的 `-0`），中间版本留在旧仓不入库。
于是"当前是第几版"无法从文件名序列反推 —— 这正是本文件存在的唯一理由。

## 非显然的口径

* **`-0` 恒为日文原图**（从发布 AGF 抽出的那份），**其余版本号都是汉化后的中间/最终产物**；
  版本号**递增 = 修订次序**，语义是"这一版比上一版新"，**不是**内容版本号。
* **`current` 的来源是旧仓仓库外目录**（`res\images\` 里版本号最高的那张）；本文件记的是
  **迁移时的静态快照**，因此**不会**随旧仓变化 —— 旧仓只读，它变了说明有人动过只读边界。
* **`localized=false` 表示"这张只有 `-0`"**：或者从未汉化，或者汉化产物留在旧仓没进仓。
  ⇒ 拿它当"这块 UI 不需要中文化"来用会得出错误结论（见下面的来源事实）。
* **签名 / 哈希不写在这里**：入库件的校验和交给 git/LFS（`docs/00-origin/decisions.md` §5）；
  AGF 产物的旁证记在 `corpus/assets.json` 的 `assets/agf-overlays` 条目里。

## 怎么查

```bash
# 它就是 { 块名: { current, localized } }；没有查询工具，直接读或用 node 数
node -e "const v=require('./corpus/assets/ui-images/versions.json');console.log(Object.keys(v.images).length)"
ls corpus/assets/ui-images/*.png        # 与表对照：块名必须都在，且最高版本号 == current
pnpm tools corpus validate              # 目录型 dest 的载荷与来源逐字节一致（守卫 #4）
```

## 怎么改

* **数据**：新增/替换一张 UI 图 = ①往本目录放新的最高版本 PNG（**只增不改**：文件名带新版本号）
  ②改本文件的 `current`。**不要**覆盖旧文件、**不要**重命名（改名即失去"版本号最高者生效"这条不变量）。
* **字段 / 不变量**：目前没有 schema 真源（见文件头的说明）；烘焙链落地时补控制脚本，
  并把下面是**待实现的守卫**一并落进那个脚本或 `tools/test/`：

  1. `versions.json` 里每个块名在目录里都有对应的 `-{current}.png`；
  2. 目录里每个块名的**最高版本号 == `current`**（用"入库的那一份就是生效版"把不变量钉住）；
  3. `localized=true` 的块名，其 AGF 旁证能在 `corpus/assets.json` 的 `assets/agf-overlays` 里找到。

* ⚠ 旧仓两处记录与本表**不一致**（以"版本号最高者生效"为准，本表已按此取值）：
  旧仓 `docs-new/01-translation/ui-images.md` 的"当前版本"列把 `SO009B` 写成 `-2`（实际 `-4`，见其 CHANGELOG 条目）；
  该文档同时还漏记了 `SO021-3` 的生效状态。
