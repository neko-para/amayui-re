# tools/patch.md — `tools/patch.mjs`（翻译 patch 控制脚本）的**落点说明**

> 它**不是**工具台账（那个要 append-only 文本 + `--describe`）；这里只写"它动哪片数据、为什么这么动"。
> 命令与自描述：**`pnpm tools patch describe`** / `status` / `baseline` / `extract` / `verify`。
> 设计与支撑观测：`../docs/01-translation/patch-design.md`。

## 它动哪片数据

| | 落点 | 读写 |
|---|---|---|
| **产物（唯一入库物）** | `data/translations/patch.json` | **唯一写入口 = `extract` / `set` / `edit`**；`verify` / `find` 只读 |
| **基线索引**（生成物，**不入库**） | `dist/index/base.json`（`--index` / 全量 `--view`） | 只依赖**不可变**的东西（基线 BIN + codec 指纹）⇒ **永不陈旧**；名单与逐支指纹的唯一来源 |
| **base 视图**（生成物，**不入库**） | `dist/views/data/<脚本>.txt`（`--view` 的**缺省**） | 由基线算出 ⇒ 同样不陈旧；给人读、给全库检索供文本 |
| **src 草稿账本**（生成物，**不入库**） | `dist/views/manifest.json` | 只记"哪几支 `src` 被物化过"（`baseSha`/`resultSha`/`subsSha`/`codecSha`）⇒ 供 `edit` 验来源；**`find` 不需要它** |
| 输入（只读） | `data/translations/subs-cn-jp.json`（简→日写法字典） | 只读；指纹写进 patch 的 `subsSha` |
| 输入（只读） | `gameInstall/`（清单 `roots.gameInstall`） | 只读：基线由「散装优先 → ALF」实时解析 |
| 输入（只读，**仅提取期**） | 旧仓 `install/` | 只读一次，用来把汉化产物提取成 patch |
| 不引用 | 旧仓 `raw-parts/`、`patch/BIN`、`gameInstall/_extracted/` | ❌ 都是派生物 / overlay |

★ **绝对路径只写在 `corpus/assets.json` 的 `roots` 里**：本工具按清单解析根，不硬编码旧仓路径
（换机器 / 旧仓搬家都只改登记一处）——与 `tools/opcodes.mjs` 同一口径。

## 三件东西的**寿命**不一样（这是整套设计的地基）

| | 依赖什么 | 陈旧吗 | 落盘吗 |
|---|---|---|---|
| **基线索引** | 基线 BIN + **codec 指纹** | ❌ **永不陈旧** | ✅ 建一次（`patch index --write`，约 0.2 s） |
| **base 文本** | 基线 BIN + codec | ❌ 永不陈旧 | ✅ `patch view`（941 支 ≈ 3.3 s / 83 MB） |
| **src 投影** | 基线 + **可变的 patch** | ✅ 每次改文案就旧 | ❌ **不常驻**：查询按锚**现算**（merge on read），只在"要用编辑器整篇改"时物化一支 |

* **检索**（`find`）：**不物化投影** —— 日文侧扫 base 文本、中文侧扫 **op 载荷**（patch 里存的就是中文），
  命中之后才按**锚**配对。于是它既不依赖 `src` 缓存、也不会因为缓存陈旧而少报（实测全库 1.4 s）。
* ★ **`dist/views/src/**` 里出现的文件 = 你物化过的「草稿」**（`dist/views/manifest.json` 记着来源）：
  它**随时可删** —— 删了 `edit` 会要求重新物化，别的什么都不受影响（`find` / `set` / `verify` 都不看它）；
  `patch status` 会报"src 草稿账本 N 支物化过"。
* **编辑**（`set`）：**按锚直改 op**（改字面量 / 插一行 / 删一行），不渲染视图、不重跑 diff；
  判据是"重建出来的行空间 == 独立算出的期望行空间"（逐行比）。
* **`edit`**：唯一的例外 —— 它的输入**就是**磁盘上那份 `src` 草稿，所以它必须先验来源（陈旧 ⇒ 拒绝）。
* ⇒ **`src` 视图不该常驻**：它只是 base 与 patch 的 join 结果。留 `dist/views/data` 的理由是
  ① 全库检索要文本 ② 人直接读；`src` 只在用编辑器整篇翻译时按支物化。

## **codec 指纹**：可逆性这句话的证人

本方案的主张是"**BIN 是文本的可逆像**"（`age-format` 的汇编器 / 反汇编器 / 指令表是这一对函数）。
主张要有证人：`patch.json` 钉了字典（`subsSha`），**codec 得另有指纹** ——
`codecContext()` 把 `packages/age-format/src/asm/**` 的内容哈希成 `codecSha`，写进基线索引与草稿账本。
它一变，旧索引里的文本就**不再担保**能重建出同样字节（与"字典换了"同一个道理）。

## 视图的三个范围（★ 混过一次，代价是检索静默少报）

| scope | = 哪些脚本 | 什么时候用 |
|---|---|---|
| `all`（**缺省**） | 基线根里**全部**能反汇编的脚本 | ★ 全库检索 / 并排读：`SG`/`SN`/`CONFIG`/物品表… 都得搜得到 |
| `patch` | patch 里有条目的（= 有变更的） | 只看"动过什么" |
| `annotated` | `SPEAKER_FILTER` 那 223 支（旧管线做过页 / 说话人**标注**的） | **只是那个任务的口径** —— 不是"一类脚本"，更不是视图范围 |

* 实测踩过：`data` 视图停在 `annotated` 的 223 支，而 patch 早已是 453 支 ⇒
  `rg dist/views/data` 得到的是"当年的 223 支上的答案"，**少报 718 支**且**不报错**。
* 基线索引干脆把这个风险从源头上掐掉：**名单与逐支指纹取自它**，而它的键是（基线指纹，codec 指纹）——
  与"某一批脚本"无关。它不新鲜就整份作废（`patch index --write`）。

## 为什么这么动

* **为什么 patch 是唯一资产**：`data`（原版）与 `src`（原版 + patch）都是**实时视图**。
  实测 `src` 里的信息 **100% 可再算**（页来源、输入原文、原文存档块、页边界都能从基线 BIN 重算），
  所以"没有文本树会丢东西"这个顾虑不成立；真正要小心的是**工具算错**。
* **为什么锚"行序 + 摘要"**：插入一条指令会改掉后续所有字节偏移，而 `label_XXXXXXXX` 本身就是绝对偏移
  ⇒ 偏移与 label 都是**派生量**，当锚就会一插就错位。
* **为什么 patch 里**不记** label 地址**：地址随偏移漂；patch 只保留"这里是个 label 引用"这一结构事实，
  重建时由汇编器按符号重算。
* **为什么存中文**：见 `../data/translations/patch.md` §非显然的口径 第 1 条。
* **为什么判据是"逐字节"**：它比"指令流逐条相同"更强，而且能一次性抓出
  `comment` 被当注释滤掉、label 定义没跟着搬这类**不报错的错**。
* **为什么 `verify` 不许写**：`resultSha` 若能在 verify 里回写，判据就退化成自证循环。
* **为什么视图范围缺省是"全部脚本"**：视图是"把整个语料摆成可检索 / 可并排读的文本"，
  而"有没有译文"只能由 patch 有没有条目判 ⇒ 按"有变更"来建视图，就等于**把那 488 支没变更的脚本藏起来**
  （它们同样有原文、同样要被引用为先例）。三个范围的区别见上一节。
* **为什么检索要配对而不是单侧 rg**：人问的是"这个日文词现在译成什么"。
  `data` 与 `src` 的行数会因 `insert-after` / `delete` 不同，所以配对锚在**基线行序**上（与 patch 同一个锚），
  不是行号。
* **为什么查询不物化投影，编辑不重跑 diff**（merge on read）：
  `src` = `base 行 ∖ {被 replace/delete} ∪ op 载荷` 是**一条谓词**，而 `patch.json` 里存的**就是中文** ——
  于是"查中文"根本不需要投影，"查日文"只需要 base。同理，编辑落在**锚上的 op**（改载荷 / 插一条 / 删一条）
  就够了，不需要"渲染整支 → 反解 → 整支重跑对齐"。这两条一起把全库检索从 10 s（甚至 57 s）压到 ~1.4 s，
  并且让单处修改只动 `patch.json` 的**一行**（git blame 与两端合并都干净）。
* **为什么 `src` 不常驻**：它只是 base 与 patch 的 join 结果，依赖可变的那一侧 ⇒ 每次改文案都会旧。
  只有"用编辑器整篇翻译"需要一份 `src` **文件**当工作面 ⇒ 那时按支物化（`--kind src --name X --out`），
  并记进草稿账本供 `edit` 验来源。
* **为什么需要 codec 指纹**：见上一节 —— 它是"BIN 是文本的可逆像"这句话的证人。
* **为什么没有"无条件全库替换"这条写路径**（用户口径）：替换**必须逐条确认**。
  所以机械的部分只做**生成清单**（`find --edits`，一条记录一处，带"期望的当前内容"），
  落笔一律是 `set` **逐条应用**：清单里每条都要与现场逐字相符，不合就报错 —— 这就是"逐个确认"的机械形式。
  （实测反面教材：拿 `赫塔` 直接换成 `废柴雷斯` 会得到"废柴雷斯雷斯"，机械替换会误伤同形词。）

## 两条编辑路径（都缺省 dry-run；`--write` 才落盘；同一次原子写盘）

```
① pnpm tools patch find <字串> --edits e.txt [--to <新串>]   # 生成清单（只写 e.txt，不碰 patch）
   pnpm tools patch set --edits e.txt                        # dry-run：逐条打「- 现在 / + 改后」
   pnpm tools patch set --edits e.txt --write                # 一次写盘进 patch
② pnpm tools patch view --kind src --name <脚本> --out dist/views   # 物化一支草稿，用编辑器整篇改
   pnpm tools patch edit --name <脚本> --write                        # 反解（来源对不上会拒绝）
```

**编辑清单的形态**（一条记录 = 一个 hunk，头行用**锚**）：

```
$1$ITINIT.BIN 316                 ← 头行：锚 = **基线行序**（= patch 自己的键空间）；`316+1` = 挂在它后面的第 1 条插入行
- set-string (global-string 19964) "赫塔雷斯之戒"    ← 期望的当前内容（必须与当前 src 视图那一行逐字相同）
+ set-string (global-string 19964) "废柴雷斯之戒"    ← 换成什么
```

| 形态 | 语义（映到哪个 op） |
|---|---|
| `-` 1 / `+` 1（**只有字面量不同**） | 改这一行：改 `replace-line` 的载荷；还没翻译就**新建**一条（`sha8` 由基线现算） |
| 只有 `+` | 在这一行**之后**插一条 `insert-after`（`+k` 指定顺序） |
| 只有 `-` | `delete` 掉这一行（它本来是 `replace-line` 就先撤掉） |
| `- N / + M`（行数变化） | ❌ **不在 `set` 的词汇里**：那需要重新对齐整支脚本 ⇒ 请走 ②（`view --out` + `edit`） |

* ★ 锚与 `-` 行是**约束**：`sha8` 不动、内容对不上就报错（清单是照着当时那份状态写的）；
* **判据**：把清单独立算成一份"期望的行空间"，再与"新条目重建出来的 src 视图"**逐行比**（label 遮蔽后）——
  行数或内容有一处不符 ⇒ **一个字都不写**；
* 只有**改字面量**这一种会被 `literalShape` 挡在门外（动了结构会明确报错并指路）；
* ★ 编辑会**更新** `resultSha`（= 新产物 BIN 的 sha）：从那以后它与"旧仓当年那份产物"不再相等 ——
  这是"译文变了"的必然结果，所以 `verify --target <旧产物>` 对**改过**的脚本不再适用（它只对没改过的脚本有意义）；
* ★ 写盘**不需要刷新任何缓存**：`data` 视图与基线索引只依赖基线（没变），`src` 投影不落盘。

## 怎么改

```bash
pnpm tools patch describe         # ★ 先看：字段 / 不变量 / 操作 / 视图范围 / 基线索引
pnpm tools patch status           # 规模 + 字典指纹 + 基线索引/草稿账本 新不新
pnpm tools patch index --write    # 建/刷新**基线索引**（名单 + 逐支指纹 + codec/基线指纹；约 0.2 s）
pnpm tools patch view             # 建/刷新 **base 文本**（缺省只写 data；941 支 ≈ 3.3 s，顺手写索引）
pnpm tools patch extract          # dry-run：提取并**逐条自证**，报告会写什么
pnpm tools patch extract --write  # 落盘（写后回读复验，不绿回滚）
pnpm tools patch verify           # 判据：基线 + patch ⇒ 逐字节相同
pnpm tools patch find 'ヘタレ'     # ★ 检索：日文查基线、中文查 op 载荷，按锚配对（不物化投影）
pnpm tools patch find --regex 'ヘタレ|へタレ' --count   # 只要分布
pnpm tools patch find '赫塔' --edits e.txt --to '废柴'   # ★ 生成编辑清单（机械填充，仍要逐条看）
pnpm tools patch set --edits e.txt          # ★ 逐条应用（dry-run：逐条打「- 现在 / + 改后」）
pnpm tools patch set --edits e.txt --write  # 一次写盘进 patch
pnpm tools patch view --kind src --name <脚本>   # 只有要用编辑器整篇改时才物化这一支的 src
pnpm tools patch edit --name <脚本> --write      # …改完反解（来源对不上会拒绝）
pnpm test                         # 基建契约：tools/test/patch.test.mjs
```

* ❌ **不要手改** `patch.json`（写入口只有 `extract` / `set` / `edit`；`verify` / `find` 只读）；
* ❌ **不要**把 `--extract` 的部分结果写盘（`--name` / `--limit` 只能是 dry-run —— 否则会删掉其余条目）；
* ❌ **不要**在 patch 里存 BIN 里的日文写法（那是派生物，反推回中文不可逆）；
* ❌ **不要**把 `src` 当常驻工作面：它是 merge 结果，会被下一次 `patch view` 覆盖；要整篇改就物化一支 + `edit`；
* ❌ **不要**指望"一条命令全库替换"：`find --edits` 只**生成清单**，落笔一律 `set` 逐条应用（用户口径）；
* ❌ **不要**在改了汇编器 / 指令表之后继续信旧索引：`patch status` 会报 codec 指纹变了 ⇒ `patch index --write`；
* ✅ 基线换了 ⇒ 让 `verify` **报冲突**，不要放宽检查。
