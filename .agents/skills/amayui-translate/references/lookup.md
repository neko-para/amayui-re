# 参考资产：怎么查、怎么核实

> 资产 = 旧仓翻译文档的**只读快照** `docs/01-translation/ref/`，按**还能不能用**二分：
> **`ref/assets/**`**（语言资产，散文件、可直接 rg）与 **`ref/archive.zip`**（历史工作单，打包、不可采信）。
> 判据与地位见该目录 `README.md`；本文件只写**可复制的检索配方**与**核实判读**，不复述快照内容。

## 1. 资产地图（按"要查什么"索引）

| 要查什么 | 查哪（都在 `docs/01-translation/ref/`） |
|---|---|
| 角色语气 / 自称 / 句尾 / 口头禅 / 笑声 | `assets/keywords/keywords-角色语气.md`（★ 最大的一份） |
| 术语、地名、神名、组织 · **首列日文** | `assets/keywords/keywords-术语词典.md` |
| 世界观总览、作品标题、CAST · **首列中文**、覆盖面更广（二手草稿） | `assets/glossary-draft.md` |
| 单位名 / 道具装备 / 技能 / 称号 / 状态 / 地形 / 卡片 … | `assets/keywords/keywords-*.md` 同名主题件 |
| 系统提示与剧情的联动口径（SG 编号 = SC 内 G 分节编号） | `assets/SG与SC分节对应.md` |
| **"当时提过什么问题"** | `archive.zip` 里的 `archive/**`（**只当历史工作单，结论与计数一律不采信**；要读先摊到 `.tmp/`） |

★ **首列方向不一样**：术语词典是 `日文 | 中文 | 备注`，glossary-draft 是 `中文译名 | 日文原名 | …`
⇒ 互查必须**两边都试**，只按一边会漏。
★ `assets/` 里的文件**自己也带**「状态：确认 / 建议 / 待定」头部与「待联合检查」末尾节 ——
那是**写时状态**，不是当前事实（按 §3 核）。

## 2. 说话人怎么定位（视图里**没有** `// FROM:` 行）

本仓已记录的观测（`docs/01-translation/patch-design.md` §2.2，全量 30 271 / 30 271 页）：
页的说话人 = 页前导区间里**最近**的一条

* `mov (global-int 3f37) <十六进制 id>` ⇒ 说话人是该 id；
* `sub (global-int 3f37) …` 或 `call label_00071658` ⇒ `none`（旁白）。

落地到视图上就是「**向上找最近的一条**」：

```bash
f=dist/views/src/SC0000.BIN.txt
rg -n '3f37' "$f"                 # 看所有说话人设置/清除点
sed -n '1685,1696p' "$f"          # 实测：1688 是 mov (global-int 3f37) 1，1694 起是该页的 show-text
```

得到 id（十六进制，如 `1` / `aa` / `3d9`）后查语气条目 —— **id 就写在标题行上**：

```bash
rg -n '^# 1 '     docs/01-translation/ref/assets/keywords/keywords-角色语气.md   # → # 1 アヴァロ（角色语气）
rg -n '^# aa '    docs/01-translation/ref/assets/keywords/keywords-角色语气.md
rg -n '^# [0-9a-f]+ .+（角色语气）' docs/01-translation/ref/assets/keywords/keywords-角色语气.md  # 列全部条目
```

★ 两条纪律：① 落笔前在**同一页**再验一次（id 取的是"最近一条"，跨页块时会变）；
② **不许凭台词内容猜角色**（台词里的称呼可能是在叫别人）。

## 3. 核实旧文档的断言（★ 用之前必做）

快照里每条「不一致 / 待定 / 已回改 / 出现 N 处」都是**旧仓当时的观察**。核实的唯一办法是拿**当前译文**实测：

```bash
pnpm tools patch view --kind src            # 生成 dist/views/src/<脚本>.BIN.txt（= 基线重放 patch，字符串取 patch 里的中文）
rg -n --no-ignore '<字串>' dist/views/src   # 现在还在不在、有几处、在哪些脚本
rg -c --no-ignore '<字串>' dist/views/src   # 只要计数
```

**判读口径**：

| 实测结果 | 含义 |
|---|---|
| 一对写法**都命中** | **活冲突**（要人去裁决）—— 已登记的以需求树为准，别另建第二份台账 |
| 只命中一种（另一种 0） | 那条**已闭环**，或从未成立 |
| 两边都 0 | 相关台词可能已随改稿消失，或旧文档写的是别的对象 ⇒ 标"无法判定" |

★ **"文档说已经改完了"不算证据** —— 唯一证据是你刚跑出来的那次检索。
★ 计数是**当时的下界**（`rg` 命中数随时会变），引用时带上跑它的日期。

## 4. 其他常用配方

```bash
# 先例优先：工程里这个日文词/句式以前怎么译的
rg -n --no-ignore '<日文或中文>' dist/views/src

# 日文对照（data 视图与 src 视图**同构**：同地址、同行形状 ⇒ 可并排 diff）
pnpm tools patch view --kind data --name SC0000.BIN --stdout | less

# 某个脚本到底有没有译文
pnpm tools patch baseline SC0000.BIN        # 产物 = 基线 ⇒ 没变更；≠ ⇒ 有变更

# 繁体/异体字形混入（简繁混用是实测存在的一类残留）
rg -n --no-ignore '[嵐華燐韓]' dist/views/src
```

## 5. 快照里**不能**采信的东西

* `archive/**` 的一切**结论、状态、计数、日期**（"已回改 / 待定稿 / N 处"）—— 它们在写下的那一刻就冻结了；
* 与旧仓工具链绑死的操作说明（`npm run assemble`、`reflow-apply`、`find-untranslated`、
  `patch/patch.config.json`、`PROGRESS.md`、`PENDING.md`）—— **本仓没有这些**；
  ★ 例外：`CHANGELOG` 换名活了下来（`release/CHANGELOG.md`），它是**随包发给玩家的产品文本**，要写；
* 旧仓的**字体载体字表**与码位归属结论（本仓的编码/字体链路不同）；
* 任何**引擎 / 数据表语义结论**（字段含义、opcode 语义、脚本角色）—— 那些要走 `AGENTS.md` §6 的准入门。

可以放心用的只有 `assets/` 那三类：**术语对照表**、**角色语气条目**、**系统提示与剧情的联动规则**
—— 而且它们也仍要按 §3 在当前译文上核实一遍"现在是不是这么用的"。
