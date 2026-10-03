# docs/01-translation/ref/ —— 旧仓翻译文档的**只读快照**

> **两句话**：① 这里是旧仓 `docs/translate/**` 与 `docs/translation/README.md` 在**被删掉之前**那一版的逐字节副本；
> ② 它按**还能不能用**分成两边 —— `assets/`（还能查的语言资产）与 `archive.zip`（只要内容还在的历史工作单）。
>
> ★ **不要就地改这里的任何东西**：快照唯一的用处就是"当时它长什么样"。
> 由 `tools/test/translation-ref.test.mjs` 判红（逐字节比对旧仓来源 + 条目集合精确相等 + 拒绝清单外的文件）；
> 重建入口只有一个：`pnpm tools old-repo translate-ref --write`。

```text
ref/
  README.md        本文件
  assets/**        语言资产 ⇒ **散文件**（技能要能直接 rg）
  archive.zip      历史工作单 ⇒ **打包**（不预期被阅读，只要内容还在）
```

## 1. 为什么会有这个目录

旧仓在提交 `6cf11a79 chore: remove obsolete files` 里把整个 `docs/` 删掉了；翻译侧那 302 篇
**没有被 `docs-new/01-translation/` 接管**（那边只收了管线 / 编码 / 字体 / 发布几篇），
`keywords-角色语气.md` 这类**语言资产**随之失踪。本目录是删除前那一版的逐字节副本。

来源提交 id 与映射规则只写在 `tools/lib/translate-ref.mjs`（一处真源，本文件不复述）；
映射是**规则**不是清单 ⇒「漏搬了哪一份 / 某一篇被放错边 / zip 里多了什么」都是机械可查的。

## 2. 二分：`assets/` 与 `archive.zip`

| | `assets/`（**还能查**） | `archive.zip`（**只要内容还在**） |
|---|---|---|
| 是什么 | 落笔要查的**语言资产**：术语对照、角色语气、系统提示与剧情的联动规范 | **某时点的工作单**：逐脚本待办、冲突清单、进度、计划、旧管线说明 |
| 形态 | **散文件**（技能按 id / 词条直接 `rg`，这是它唯一的存在意义） | **一个 zip**（253 篇；没人预期去读它） |
| 失效方式 | 内容仍可照用，但**仍须**按 §4 核实现状 | 写的就是"当时的状态" ⇒ 状态、计数、日期、"已回改"一律**不可采信** |
| 有哪些 | `assets/keywords/keywords-*.md`（48 篇）· `assets/glossary-draft.md` · `assets/SG与SC分节对应.md` | `archive/prob/prob-*.md`（249 篇）· `archive/legacy-translation-README.md` · `archive/问题梳理与整理流程.md` · `archive/menu-patch-notes.md` · `archive/plan-ui-tools.md` |

★ 划线的判据是**内容主体**：讲"这个词译作什么 / 这个角色怎么说话" ⇒ 资产（散文件）；
讲"某脚本还有哪几句没定 / 已改了多少处 / 下一步做什么" ⇒ 存档（进 zip）。
★ 这里的 `assets/` 是**翻译语言资产**，与素材清单的 `corpus/assets/`（只读素材、走 LFS）**没有关系**。

### 2.1 为什么 archive 打包而不是散着放

1. **那些文件没有阅读价值**（写的就是"当时的状态"）⇒ 散在树里只是 253 个噪声文件；
2. **zip 里的字节不过 git 的 `text` / `eol` 规范化** ⇒ 作为"历史原件"比散文件**更忠实**
   （`.gitattributes` 首行是 `* text=auto eol=lf`，散文件的换行由 checkout 决定；zip 里是原字节）；
3. `tools/lib/zip.mjs` 的写入是**确定性**的（固定时间戳、无 extra 字段、level 9）
   ⇒ 同输入**同字节**，所以"内容还在"这件事可以机械复核。

★ 代价说清楚：**不能直接 `rg` 里面了**。要读就先摊出来（§3），这也是"内容还在"与"随时能查"的取舍
—— `assets/` 那边选了后者（因为技能每次落笔都要查语气/术语），`archive.zip` 这边选了前者。
★ 存储：它按 `.gitattributes` 的 `*.zip` 规则走 **LFS**（不为一个文件加路径式例外 —— 那条纪律见
`.gitattributes` 里"路径规则会连纯文本侧车一起吞进 LFS"的说明）。⇒ 新机器上先 `git lfs pull`；
**没 smudge 时盘上是 LFS 指针**，守卫会如实 skip 而不是误报红。

## 3. 怎么读（要不是要"当时提过什么"，平时不用管它）

摊到**本地临时区**（`.tmp/` 被 gitignore，摊出来不会脏树；读完随手删）：

```bash
# Windows PowerShell
Expand-Archive docs\01-translation\ref\archive.zip -DestinationPath .tmp\ref-archive
# macOS / Linux
unzip -d .tmp/ref-archive docs/01-translation/ref/archive.zip
```

zip 里的条目名 = 它原本的落点（`archive/prob/prob-SC2180.md` 等），所以摊出来就能照 §5 的路径读。

## 4. 怎么查：**用资产之前必须核实现状**

资产里说"这个词译作 X"，不等于现在的译文就是 X。核实的唯一办法是拿**当前译文**实测：

```bash
pnpm tools patch view --kind src            # 生成 dist/views/src/<脚本>.BIN.txt（基线重放 patch，字符串取 patch 里的中文）
rg -n --no-ignore '<字串>' dist/views/src   # 译法现在还在不在、有几处、在哪些脚本
```

判读：**同一意思有两种写法**才叫活冲突（已登记的以需求树为准，别另建第二份台账）；只命中一种 ⇒ 已闭环或从未成立。

★ 两条**实测踩过**的计数陷阱（不排除会把判断完全带偏）：

1. `comment "…"` 里是**日文原文标记不是译文**（实测 `嵐燐` 13 处命中里 13 处都在 comment）；
2. `display-furigana "A" "B"` 是**主词 + 注音**，B 不是竞争译名
   （实测 `精域神战争` / `菲优希亚圣战` 就是这一对，SC2770 还自己写着「称之为『精域神战争』或『菲优希亚圣战』」）。

## 5. 怎么改

* ★ **本目录不写、不改、不加**（含 zip 内部）—— 守卫会红。
* **要重建快照**（换来源提交、或本地被谁改坏了）：改 `tools/lib/translate-ref.mjs` 里的来源常量，然后
  ```bash
  pnpm tools old-repo translate-ref            # dry-run：逐件对账（资产按 blob sha、zip 按条目内容）
  pnpm tools old-repo translate-ref --write    # 重建（写后回读复验）
  ```
  它**拒绝**在"快照区里有来源清单之外的东西"时写入 —— 那是重建消不掉的，得先人工处理。
* 要写"当前的结论 / 待办"：
  * **可收敛的工作项**（"还有哪些译名没统一"）⇒ 需求树（`pnpm tools requirements`，节点正文写**判据**）；
  * **可再校验的知识**（引擎 / 数据表语义）⇒ `data/ledger/`，且必须过 `AGENTS.md` §6 的准入门；
  * **面向用户的发行文本**（CHANGELOG / 安装说明）⇒ `release/`；
  * 长调查 / 设计 / 沿革 ⇒ `docs/`（`git log` 是沿革真源）。
* 快照里的**引擎语义结论**不构成知识条目 —— 它得按 §6 重新绑观察之后才准登记；快照只降低"重新发现的成本"。

## 6. 消费方

`.agents/skills/amayui-translate/`（翻译 / 更新译文）只把 `assets/` 当**检索源**，
并强制"引用前先在当前译文上核实"（配方见该技能的 `references/lookup.md`）。
