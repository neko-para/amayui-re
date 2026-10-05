---
name: amayui-translate
description: 在《天結いキャッスルマイスター》中文汉化工程（本仓）里翻译或更新 AGE 脚本译文。以 data/translations/patch.json（唯一入库的变更叠加层）为真源：`patch find` 检索（日文原文与当前中文配对，范围一定全）→ 改中文 → **生成编辑清单再逐条应用**（`patch find --edits` 生成 → 人逐条审 → `patch set --edits` 一次写盘；整篇翻译走 `patch view --out` + `patch edit`）→ `patch verify` 过判据。当用户要求：翻译或重译某个脚本、润色/修正已有译文、统一术语或角色语气、复核已定稿的命名决策、核对旧文档里的译法断言时使用。整篇重译与局部更新走同一个闭环，只在"要不要先等用户确认"上分岔。
---

# Amayui Translate（天結い脚本：翻译 / 更新译文）

## 0. 三条最常踩的

1. **不许手改 `data/translations/patch.json`** —— 它是不透明数据，唯一写入口是控制脚本（`patch extract` / `patch edit`）。
   手改 JSON 会让规范形态与 `resultSha` 判据同时失效，而且 git blame 会废。
2. **不许 `git add` / `git commit`** —— 提交时机由用户决定（沿用旧仓长期要求）。
3. **旧仓 `E:\Games\Eushully\天結` 是只读来源**：不改、不删、不移动（可读、可 `git status` 证明未变）。
   它**不是**运行时依赖 —— 基线只认安装目录（`pnpm tools patch baseline` 会给出来源）。

## 1. 数据模型（一句话各自）

| 词 | 是什么 |
|---|---|
| **基线** | 安装目录里的**原版日文 BIN**（不可变，用 sha256 钉住） |
| **patch** | `data/translations/patch.json` —— 相对基线的**变更叠加层**，★ **本域唯一入库资产**（**只收有变更的**脚本） |
| **基线索引** | `dist/index/base.json` —— 脚本清单 + 逐支 `baseSha` + **codec 指纹** + 基线指纹。只依赖**不可变**的东西 ⇒ **永不陈旧** |
| **base 视图** | `dist/views/data/<脚本>.txt` —— 基线的反汇编（日文）。同样只依赖基线 ⇒ **永不陈旧** |
| **src 投影** | `base 行 ∖ {被 replace/delete} ∪ op 载荷` —— 依赖**可变的 patch** ⇒ **不常驻**，查询时按**锚**现算 |

★ **`patch.json` 里存的就是中文** ⇒ **查中文不需要投影**：日文查 base、中文查 op 载荷，命中之后按**锚**配对
（`pnpm tools patch find`）。这就是 **merge on read**：不物化 941 支的投影，全库检索 ~1.4 s。
★ 只有"**要用编辑器整篇翻译**"才需要一份 `src` **文件**当工作面 ⇒ 那时按支物化（`--kind src --name X`），
并记进草稿账本供 `edit` 验来源。
★ **codec 指纹**（汇编器 / 反汇编器 / 指令表）= "BIN 是文本的可逆像"这句话的**证人**：它一变，
旧索引/旧草稿里的文本就不再担保能重建出同样字节（与字典的 `subsSha` 同一个道理）。跑 `patch status` 看它。

⇒ 所以「译文真源」是 patch 里的**中文**，不是任何 `.txt`；`data`/`src` 都是**生成物**（不入库）。
字段 / 不变量 / 操作的**唯一真源**是 `pnpm tools patch describe`（本技能不复述 schema）。

## 2. 两种作业（机制相同，护栏不同）

| | A · 翻译（新增 / 重译） | B · 更新（评估 / 润色 / 统一） |
|---|---|---|
| 触发 | "翻译 SC0010"、"重译这一段" | "这句不通顺"、"术语统一成 X"、"评估一下这句" |
| 是否先等用户确认 | 不需要（整篇授权即落笔） | ★ **需要**：先给**字母编号候选**（A/B/C…）并标注主推，用户选定后才动 |
| 落笔范围 | 该脚本的全部文本行 | 只改用户点头的那些页/句 |

**B 的护栏（硬）**：① 候选**必须**用字母序号编号且必须标「主推 X」，不许给无编号并列选项；
② 评估阶段**一个字都不许写**（不写视图、不写 patch、不写文档）；③ 说话人只认机械依据（§5），不许凭台词内容猜。

### C · 复核一个**已经定稿**的命名 / 术语决策（B 的特例，护栏更硬）

**触发**：「XX 这套译法丢了原义，评估能否优化」—— 要推翻的是**既有定稿**（旧仓 `prob-*.md` 里写死过的那种），
不是"这句不顺"。判据与做法见 `references/consistency-review.md`；四条额外护栏：

1. **先取决策史**：谁定的、当时写的理由与备选是什么、有没有留下"待定"尾巴（§5 的 `archive/prob/**`）；
   ★ 旧文档里的「待定 / 已回改 / 出现 N 处」都是**写时状态**，与当前译文不一致是常态，要当场实测。
2. **证据必须是全库机械清单**（`pnpm tools patch find`）：同一词根的**每个词形** `data`/`src` 两侧各几处、
   在哪几支脚本、是 **UI 表项**还是**剧情正文**。不许拿"我记得有几处"当范围。
3. **候选必须带代价**：涉及几支脚本几处、改哪些词形、宽度会不会变（UI 表项尤其）、
   会不会拆散"同根词"（日文同根 ⇒ 中文也该同根）、要不要一并回改 §C2 的清单。
4. **"维持现状"也是合法裁决**（那时按 A 处理：什么都不写，把结论记进节点）；**未裁决前一个字都不动**。

## 3. 命令（本技能只用这些）

```bash
pnpm tools patch status                    # 规模 + 字典指纹 + **基线索引 / 草稿账本** 新不新
pnpm tools patch index --write              # 建/刷新**基线索引**（名单 + 逐支指纹 + codec；约 0.2 s）
pnpm tools patch view                       # 建/刷新 **base 文本**（缺省只写 data；941 支 ≈ 3.3 s，顺手写索引）
pnpm tools patch baseline [<脚本>]          # 该脚本的基线与产物从哪来 + 是否 ≠ 基线（= 有没有译文）
pnpm tools patch find <字串>…               # ★ 检索：日文查 base、中文查 op 载荷，按**锚**配对（不物化投影）
pnpm tools patch find --regex 'A|B' --count  # 只要分布（术语一致性先看这个）
pnpm tools patch find <字串> --edits e.txt [--to <新串>]   # ★ **生成编辑清单**（锚寻址，只写 e.txt）
pnpm tools patch set --edits e.txt          # ★ **逐条应用**（dry-run：逐条打「- 现在 / + 改后」）
pnpm tools patch set --edits e.txt --write  # 按锚直改 op ⇒ 一次写盘（不渲染、不重跑 diff）
pnpm tools patch view --kind src --name SC0000.BIN --stdout   # 看单支 src 投影（等于 merge on read 的结果）
pnpm tools patch view --kind src --name SC0000.BIN --out dist/views   # 物化一支草稿（整篇翻译用编辑器改）
pnpm tools patch edit --name SC0000.BIN --write   # 草稿改完 ⇒ 反解（来源对不上会拒绝）
pnpm tools patch verify [--name <脚本>]      # 判据：基线 + patch ⇒ 产物逐字节相同
pnpm tools patch describe                   # 字段 / 不变量 / 操作 / 视图 / 基线索引（schema 唯一真源）
```

★ **改文案 = 生成清单（机械）+ 逐条应用（人看过的）**，**没有"无条件全库替换"这条写路径**：

```bash
pnpm tools patch find '<旧串>' --edits e.txt [--to '<新串>']   # 机械那一半：一条记录一处，带"期望的当前内容"
# ── 打开 e.txt 逐条看：头行是 `<脚本> <锚>[+<k>]`（锚 = 基线行序），`-` 是现场、`+` 是改后
pnpm tools patch set --edits e.txt          # dry-run：逐条打「- 现在 / + 改后」
pnpm tools patch set --edits e.txt --write  # 一次写盘；判据是"重建行空间 == 期望行空间"（逐行）
```

* **锚**（基线行序）= patch 自己的键空间，**不是文件行号**（文件行号是渲染产物）。
  `i=30` = 锚上那一行；`i=30+1` = 挂在它后面的第 1 条插入行（`insert-after`）；
* `set` 只接受三种形态：**改一行字面量**（`- 1 / + 1` 且只有引号里不同）/ **插一行**（只有 `+`）/ **删一行**（只有 `-`）；
  **行数变化的块替换**（折行拆分、合并）要用"渲染 + 反解"：`view --kind src --name X --out` 改文件 + `patch edit`；
* ★ `--to` 只是**机械填空**，会误伤同形词（实测 `赫塔 → 废柴雷斯` 会得到"废柴雷斯雷斯"）⇒ 逐条看是硬要求。

## 4. 流程

### A · 翻译

**整篇**（新脚本 / 整篇重译）：
1. **定位**：`pnpm tools patch baseline <脚本>` 确认基线与产物来源；`patch view --kind src --name <脚本> --stdout` 看现状。
2. **落笔**：把这一支的 `src` **物化成一份草稿**再改，改完用 `patch edit` 反解（只有这条路依赖磁盘文件）：
   ```bash
   pnpm tools patch view --kind src --name SC0010.BIN --out dist/views   # 物化这一支（并记进草稿账本）
   # …在 dist/views/src/SC0010.BIN.txt 上改中文字串…
   pnpm tools patch edit --name SC0010.BIN            # dry-run：看它反解出多少 op
   pnpm tools patch edit --name SC0010.BIN --write    # 落盘（写后回读复验，不绿回滚）
   ```
   * 只碰**字符串**；助记符、操作数、label、头部 4 行**一律不动**（`patch edit` 会因头部不同而抛）。
   * 落笔约定（折行 / 注音 / concat / draw-string / 术语 / 角色语气）见 `references/conventions.md`。
   * ★ `patch edit` 只用**与当前重建结果不同**的那些文件；它会先验草稿来源（对不上 ⇒ 拒绝，见 §8）。
3. **过判据**：`pnpm tools patch verify --name <脚本>` —— 反解内部已断言"重建 == 改过的草稿"，这是第二道。

**局部**（只改几句 / 几行）——★ 优先这条，它**完全不依赖磁盘文件，也不重跑 diff**：

1. `pnpm tools patch find <关键词>` 定位（行键 `i=<锚>`）；要成批改就用 `--edits` 让工具生成清单（机械的那一半）：
   ```bash
   pnpm tools patch find '赫塔' --edits e.txt            # 生成模板（`+` 与 `-` 相同 ⇒ 什么都没改）
   pnpm tools patch find '赫塔' --edits e.txt --to '废柴' # 或机械填好 `+`（★ 会误伤同形词，仍要逐条看）
   ```
2. **逐条审** `e.txt`（一条记录 = 一个 hunk：头行 `<脚本> <锚>[+<k>]` + `- 现在` + `+ 改后`），把不对的 `+` 改掉。
   ★ 想折行（拆成两条 `show-text`）就**别在清单里硬写**：那是行数变化 ⇒ 走上面的"整篇"路（`view --out` + `edit`）。
3. dry-run 看逐条 diff，确认无误再落盘：
   ```bash
   pnpm tools patch set --edits e.txt          # dry-run：逐条打「- 现在 / + 改后」
   pnpm tools patch set --edits e.txt --write  # 按锚直改 op ⇒ 一次写盘（不刷新任何缓存：它只依赖基线）
   ```
   * 每条 `-` 必须与现场**逐字相同** ⇒ 锚写错、位置漂了当场报错；
   * 判据：**重建出来的行空间 == 独立算出的期望行空间**（逐行，label 遮蔽后）——有一处不符就一个字都不写。
4. **过判据**：`pnpm tools patch verify --name <脚本>`。
5. **收口**：见 §7。

### B · 更新（评估 → 确认 → 改）

1. **取证**（只读，不改任何文件）：
   * 用 `pnpm tools patch find <字串>` 定位目标行（它把日文（base）与当前中文（op 载荷）**按锚配对**打出来，
     行键 `i=<锚>` 就是编辑要用的键）；
   * 定**说话人**：向上找最近的 `mov (global-int 3f37) <十六进制 id>`（本仓已记录的观测：
     这是旧管线 `// FROM:` 的来源，全量可再算），再按 id 查角色语气文档（§5）；
   * **同类句式的既有译法**（先例优先）：`pnpm tools patch find <日文词/句式>` —— 日文侧命中会连中文侧一起打出来。
2. **评估**：讲清问题（语义/语感/标点/术语/冗余），给**字母编号候选**并标「主推 X」，每个候选附一句取舍。
   ★ 此阶段不写任何文件。
3. **等确认**：用户选定（字母或自拟）后才动。
4. **改 + 过判据**：局部改动按 A 的「局部」走（`find --edits` → 逐条审 → `set`），要点见 §4 C。
5. **收口**：见 §7。

### C · 复核已定稿的命名决策（评估 → 确认 → 改，护栏更硬）

**完整配方见 `references/consistency-review.md`**（这一节只给骨架）：

1. **取证（全库、机械）**：把该词根的**每个日语词形**都搜一遍，两侧配对看现在各译成什么 ——
   ```bash
   pnpm tools patch find --regex '<词根>|<变体>' --count     # 先看规模与分布
   pnpm tools patch find --regex '<词根>|<变体>'             # 再看每一处的日文 ↔ 中文
   pnpm tools patch find --regex '<现有译法 A>|<现有译法 B>' # 中文侧的"活冲突"各在哪
   ```
   ★ 全库检索按锚现算、不物化投影（~1.4 s）；没有基线索引时先 `pnpm tools patch index --write`。
2. **取决策史**：摊开 `ref/archive.zip`，读 `archive/prob/**`（尤其 `prob-决策清单.md` 与相关 `prob-*.md`）——
   回答"当时定过什么、理由是什么、留了什么尾巴"。★ 只当工作单，结论与计数不采信。
3. **评估**：给**字母编号候选 + 主推**，每个候选写**代价**（改几支脚本几处 / 涉及哪些语域 / 宽度变化 /
   会不会拆散同根词 / 要不要连带回改）。**此阶段不写任何文件。**
   * ★ 代价可以**量出来**（只读）：`patch find '<旧串>' --edits e.txt --to '<候选>'` 会生成逐条清单与
     "命中 N 条 / M 支脚本"，正好是候选的波及面（`e.txt` 是临时文件，不碰 patch）。
4. **等确认** ⇒ 才改（**逐条确认是硬要求**）：
   ```bash
   pnpm tools patch find '<旧串>' --edits e.txt --to '<新串>'   # 生成清单（机械填充只是省事）
   # ── 逐条审 e.txt：同形词会被误伤（实测 赫塔→废柴雷斯 得到"废柴雷斯雷斯"），把不对的 + 改掉
   pnpm tools patch set --edits e.txt            # dry-run
   pnpm tools patch set --edits e.txt --write    # 一次写盘
   pnpm tools patch verify                       # 判据：逐字节
   ```
   点状的例外（只改某一处）就直接在清单里留下那一条、删掉其余。裁决为"维持现状"时什么都不写，把结论记进需求树节点。

## 5. 参考资产在哪（旧仓文档怎么用）

旧仓翻译文档的逐字节快照在 **`docs/01-translation/ref/`**，按**还能不能用**分成两边
（判据与用法见该目录的 `README.md`）：

* **`ref/assets/` = 还能用的语言资产**（落笔前查这里）
  * `assets/keywords/keywords-角色语气.md` —— 角色语气手册：条目 = `# <十六进制 id> <角色名>（角色语气）`，
    条内固定四节（`角色信息` / `日文表达风格` / `中文译文风格` / `翻译一致性注意事项`）；
  * `assets/keywords/keywords-术语词典.md`（**首列日文**）与 `assets/glossary-draft.md`（**首列中文**）
    —— **首列方向相反，互查要两边都试**；`assets/keywords/keywords-*.md` 其余各对应一张引擎数据表；
  * `assets/SG与SC分节对应.md` —— 系统提示与剧情的联动规范。
* **`ref/archive.zip` = 只能当历史工作单看**（253 篇打成一个 zip；**不预期被读**，要读先摊开）
  * 内含 `archive/prob/**`（逐脚本待办与"不一致"清单）、`archive/prob-决策清单.md`（★ **主决策清单**：
    跨文件翻译决策的单一来源，含"已定稿 / 待定稿 / 活冲突"三态）、`archive/legacy-translation-README.md`、
    `archive/问题梳理与整理流程.md`、`archive/menu-patch-notes.md`、`archive/plan-ui-tools.md`；
  * ★ **要复核"某个译法为什么是现在这样"，先查这里**（C 模式的第一步）；它写的是"当时提过什么"；
  * 这些写的就是"当时的状态" ⇒ **状态 / 计数 / 日期 / "已回改"一律不采信**；
  * 真要看：摊到本地临时区（`.tmp/` 已 gitignore）而**不要**摊进树里 ——
    `Expand-Archive docs\01-translation\ref\archive.zip -DestinationPath .tmp\ref-archive`（macOS/Linux 用 `unzip -d`）。
* ★ **资产也要核实现状**：`assets/` 说"这个词译作 X"、或还标着「待定」，都不等于现在的译文就是那样
  （实测：`keywords-战斗地名.md` / `keywords-装备与物品.md` 至今写着 `ヘタレス 待定`，而译文早已定稿落地）。
  用它之前**必须**在当前译文上实测：

  ```bash
  pnpm tools patch find '<字串>'              # ★ 首选：全库（按锚现算，不物化投影）+ 两侧配对
  pnpm tools patch find '<字串>' --count      # 只要计数
  ```

  （裸 `rg -n --no-ignore '<字串>' dist/views/data` 只覆盖**日文基线**那一侧；要中文就用 `find`。）

  判读：**同一意思有两种写法**才叫活冲突；只命中一种 ⇒ 那条已闭环或从未成立。
  "文档说已改完"**不算**证据 —— 唯一证据是你刚跑出来的那次检索。
  ★ 两个**实测踩过**的计数陷阱：`comment "…"` 里是日文原文标记**不是译文**（两侧都会命中，别当译文）；
  `display-furigana "A" "B"` 是**主词+注音**，B 不是竞争译名。
* 已知在跑的活冲突清单挂在需求树（`pnpm tools requirements plan` / `show`），别自己另建一份。

## 6. 判据（收口凭据）

* **机械判据**：`pnpm tools patch verify` —— 基线 + patch ⇒ **逐字节**相同（缺省自证 `resultSha`）。
  ★ 编辑会更新 `resultSha`（= 新产物 BIN 的 sha）⇒ 与"旧仓当年那份产物"不再相等是**正常的**；
  `verify --target <旧产物>` 只对**没改过**的脚本有意义。
* **编辑回路自证**：`set` 把清单独立算成一份"期望的行空间"，再与"新条目重建出来的 src 视图"**逐行比**
  （label 遮蔽后）；行数或内容有一处不符 ⇒ **一个字都不写**。`edit` 那条路同理（反解内部断言"重建 == 改过的草稿"）。
* **"改完了"的判据（术语 / 命名回改）**：`pnpm tools patch find --regex '<日文词形>|<中文写法 A>|<中文写法 B>'`
  里**同一日文词根只剩一种中文写法**（活冲突清零），且 `patch verify` 全绿。
  ★ **不是**"我在 `src` 里 `rg` 到 0 处"—— `src` 根本不再常驻；`find` 按锚现算，范围一定全。
* 改了术语/语气口径要**回改全工程**时：`find --edits` 生成清单 → **逐条审** → `set --write` 一次改完。
  逐脚本手改是下策（漏改一处就造出新的活冲突）；**无条件替换更不行**（同形词会被误伤）。

## 7. 记录与收口

* **内部沿革真源 = `git log`**：不写内部的改动记录表、不写进度表（见 `AGENTS.md` §10）。
* ★ **CHANGELOG（随包发给玩家，要写）**：每次对玩家可见的改动完成后，在
  **`release/CHANGELOG.md`** 最新一个「开发中」版本节**最上方**加一条 `- [类型][脚本] 说明`。
  * 格式 / 版本节规则 / 什么不许写 —— **真源是 `release/README.md` §4**（本技能不复述，免得两处漂）；
  * 它**不是**内部变更记录：一条 = 一次对玩家可见的改动，**不要**把提交信息抄进去，
    也不许写重建字节数 / `sha256` / 工具细节；
  * 若不存在「开发中」节（刚发过版），**先新建** `## vX.(Y+1)（开发中）` 再写。
* **可收敛的工作项**（"还有哪些译名没统一"这种）⇒ **需求树**：`pnpm tools requirements add/set`
  （「怎么改」见 `data/requirements/README.md`）。节点正文写**判据**；收口写 `verify` 或 `done_reason`。
* **可再校验的知识**（脚本角色 / 字段语义等）⇒ `data/ledger/`，且必须过 `AGENTS.md` §6 的准入门（K1–K3 之前不进台账）。
* 只做评估、没改文件 ⇒ **什么都不用登记**（本来就没产生状态）。

## 8. 能力边界（★ 落笔前先读这一节）

* **现在是「指令行层面」作业**：视图是反汇编（`show-text 0 "…"` 一行一个视觉行），
  **没有**旧仓那套页块注释层（页边界 / `// FROM:` / `/* 原文存档 */` / `// 页面结束`）。
  该层是需求树上的在办节点（`REQ-01M3XP1ZV3YF5EYHTK2A3KV4CE`），**交付前**：
  * **折行要自己数**：现有译文已经是折好行的（一条 `show-text` = 一个视觉行），加字超宽就自己拆成两条 + `end-text-line`；
  * **说话人要自己按 §5 的机械依据推**（视图里没有 `// FROM:` 行）；
  * 不要假定 `reflow` / `assemble` / `find-untranslated` 存在 —— **它们在本仓不存在**，别照旧仓文档敲。
* **不要用 `data` 视图当译文真源**：BIN 里是 cp932 占位写法，**反推不回中文**；中文只在 patch 里。
* **`src` 投影不常驻**（merge on read）：`find` 按锚现算（日文查 base、中文查 op 载荷），`set` 直改 op。
  ⇒ "先 view 再 find/set"**不是**前置条件。只有 `patch edit`（输入就是那份**草稿文件**）必须先验来源。
  ★ 所以 **`dist/views/src/**` 里出现的文件 = 你物化过的草稿**，**随时可删**（删了 `edit` 会要求重新物化）；
  `patch status` 会报"src 草稿账本 N 支物化过"。
* **`data` 文本与基线索引只依赖基线 + codec ⇒ 永不陈旧**；它们可以放心留着、也不必反复重建。
  ★ 但**改了汇编器 / 指令表**就会让 `codec` 指纹变 ⇒ `patch status` 会红 ⇒ `pnpm tools patch index --write`
  （并且这提醒你：旧译文的重建字节可能变了，跑一次 `patch verify` 复核）。
* **锚是编辑的键，不是文件行号**：`i=<基线行序>`；`i=<锚>+k` = 挂在它后面的第 k 条插入行。
  文件行号只属于**某一次渲染**（label 定义行落在哪里由汇编器决定）。
* **改文案 = 生成清单 + 逐条应用**：不要"改草稿文件之后再想办法同步"（`view` 会覆盖它），
  也不要"一条命令全库替换"（同形词会被误伤，且没人看过）。
* **没有第二份术语台账**：定稿术语的**唯一真源就是 patch 里的中文**（+ 需求树节点记录裁决）；
  `docs/01-translation/ref/assets/**` 是**只读快照**（守卫钉字节保真）⇒ **不要回写它**，
  发现它与现状不符时按 §5 实测，并把"待复核"记进需求树。

## 9. 不做什么

* ❌ 不写 `patch/patch.config.json`（BIN 清单可由 `patch.json` 的键派生）、`PROGRESS.md`、`PENDING.md`
  —— 这些机制本仓没有。★ 但 **`release/CHANGELOG.md` 要写**（那是随包发给玩家的产品文本，见 §7）。
* ❌ 不碰 `data/` / `src/` 的 941×2 文本树（它们是 `external-only`，不是本仓真源）。
* ❌ 不引旧仓的 `scripts/*.js`、字体链、载体字机制 —— 编码/字体是 `packages/age-format` 与
  `data/translations/subs-cn-jp.json` 的事（见 `data/translations/README.md`）。
* ❌ 不把游戏 / 引擎语义当结论写进文档（要走 `AGENTS.md` §6）。

## 10. 资源

| 文件 | 用途 |
|---|---|
| `references/conventions.md` | 落笔约定：折行 / 注音 / concat / draw-string / 术语 / 角色语气 / 质量优先级 |
| `references/lookup.md` | 参考资产检索与**核实**：可复制的 `find`/`rg` 命令、说话人定位、活冲突怎么判 |
| `references/consistency-review.md` | ★ **C 模式配方**：全库一致性 / 命名复核（清点 → 决策史 → 候选与代价 → 回改与收口） |
| `docs/01-translation/ref/README.md` | 快照的地位、`assets/**` 与 `archive.zip` 的二分、"引用前必须核实"的口径 |
| `docs/01-translation/patch-design.md` | patch 方案的设计与支撑观测（锚定规则、merge on read、base 索引与 codec 指纹） |
| `tools/patch.md` | patch 工具动哪片数据、三件东西的寿命、两条编辑路径、基线索引与 codec 指纹 |
| `release/README.md` | 发行物：包里每件的真源、**CHANGELOG 规格（真源）**、三处缺口 |
| `pnpm tools patch describe` | patch 的字段 / 不变量 / 操作 / 视图范围 / **基线索引**（**schema 唯一真源**） |
