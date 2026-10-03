---
name: amayui-translate
description: 在《天結いキャッスルマイスター》中文汉化工程（本仓）里翻译或更新 AGE 脚本译文。以 data/translations/patch.json（唯一入库的变更叠加层）为真源：用 `pnpm tools patch view` 生成 src 视图 → 在视图上按约定落笔 → 用 `pnpm tools patch edit` 反解回 patch → 用 `pnpm tools patch verify` 过判据。当用户要求：翻译或重译某个脚本、润色/修正已有译文、统一术语或角色语气、核对旧文档里的译法断言时使用。整篇重译与局部更新走同一个闭环，只在"要不要先等用户确认"上分岔。
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
| **patch** | `data/translations/patch.json` —— 相对基线的**变更叠加层**，★ **本域唯一入库资产** |
| **视图** | 由基线（±patch）**实时算出**的文本：`data` = 基线的反汇编；`src` = 基线 + patch（**字符串取 patch 里的中文**） |

⇒ 所以「译文真源」是 patch 里的**中文**，不是任何 `.txt`；`src`/`data` 都是**生成物**（`dist/views/`，不入库、可无限重算）。
字段 / 不变量 / 操作的**唯一真源**是 `pnpm tools patch describe`（本技能不复述 schema）。

## 2. 两种作业（机制相同，护栏不同）

| | A · 翻译（新增 / 重译） | B · 更新（评估 / 润色 / 统一） |
|---|---|---|
| 触发 | "翻译 SC0010"、"重译这一段" | "这句不通顺"、"术语统一成 X"、"评估一下这句" |
| 是否先等用户确认 | 不需要（整篇授权即落笔） | ★ **需要**：先给**字母编号候选**（A/B/C…）并标注主推，用户选定后才动 |
| 落笔范围 | 该脚本的全部文本行 | 只改用户点头的那些页/句 |

**B 的护栏（硬）**：① 候选**必须**用字母序号编号且必须标「主推 X」，不许给无编号并列选项；
② 评估阶段**一个字都不许写**（不写视图、不写 patch、不写文档）；③ 说话人只认机械依据（§5），不许凭台词内容猜。

## 3. 命令（本技能只用这些）

```bash
pnpm tools patch status                    # 规模：脚本数 / 操作数 / 体积 / 基线解析来源
pnpm tools patch baseline [<脚本>]          # 该脚本的基线与产物从哪来 + 是否 ≠ 基线（= 有没有译文）
pnpm tools patch view --kind src --name SC0000.BIN --stdout   # 看 src 视图（生成物）
pnpm tools patch view --kind src --out dist/views             # 落到 dist/views/src/<脚本>.BIN.txt（一次全量）
pnpm tools patch view --kind data --name SC0000.BIN --stdout  # 看日文基线（对照用；两视图同构、地址相同）
pnpm tools patch edit [--name <脚本>]       # ★ 改过的 src 视图 ⇒ 反解回 patch（缺省 dry-run）
pnpm tools patch edit --write              # 落盘（写后回读复验，不绿回滚）
pnpm tools patch verify [--name <脚本>]     # 判据：基线 + patch ⇒ 产物逐字节相同
pnpm tools patch describe                  # 字段 / 不变量 / 操作（schema 唯一真源）
```

## 4. 流程

### A · 翻译

1. **定位**：`pnpm tools patch baseline <脚本>` 确认基线与产物来源；`patch view --kind src --name <脚本> --stdout`（或 `--out`）看现状。
2. **落笔**：直接在 `src` 视图上改**中文字串**（`show-text` / `display-furigana` / `set-string` 等文本行的引号内）。
   * 只碰**字符串**；助记符、操作数、label、头部 4 行**一律不动**（`patch edit` 会因头部不同而抛）。
   * 落笔约定（折行 / 注音 / concat / draw-string / 术语 / 角色语气）见 `references/conventions.md`。
   * 版本相关的两个动作：改前 `patch view --kind data --name <脚本> --out <临时目录>` 留一份日文对照（两视图同构，可并排 diff）。
3. **反解**：`pnpm tools patch edit --name <脚本>`（dry-run，看它报"哪些脚本有改动 / 反解出多少 op"）⇒ 核对无误再 `--write`。
4. **过判据**：`pnpm tools patch verify --name <脚本>`。
   * `patch edit` 内部已经断言"反解后重建 == 改过的视图"（逐字节），所以这一步是**第二道**、不是唯一一道。
5. **收口**：见 §7。

### B · 更新（评估 → 确认 → 改）

1. **取证**（只读，不改任何文件）：
   * 在 `src` 视图里定位目标行；
   * 定**说话人**：向上找最近的 `mov (global-int 3f37) <十六进制 id>`（本仓已记录的观测：
     这是旧管线 `// FROM:` 的来源，全量可再算），再按 id 查角色语气文档（§5）；
   * 读 `data` 视图同一地址那一行，逐字对照日文原文；
   * `rg` 在 `dist/views/src` 里检索**同类句式的既有译法**（先例优先）。
2. **评估**：讲清问题（语义/语感/标点/术语/冗余），给**字母编号候选**并标「主推 X」，每个候选附一句取舍。
   ★ 此阶段不写任何文件。
3. **等确认**：用户选定（字母或自拟）后才动。
4. **改 + 反解 + 过判据**：同 A 的 2–4 步。
5. **收口**：见 §7。

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
  * 内含 `archive/prob/**`（逐脚本待办与"不一致"清单）、`archive/legacy-translation-README.md`、
    `archive/问题梳理与整理流程.md`、`archive/menu-patch-notes.md`、`archive/plan-ui-tools.md`；
  * 这些写的就是"当时的状态" ⇒ **状态 / 计数 / 日期 / "已回改"一律不采信**，只用来回答"当时提过什么"；
  * 真要看：摊到本地临时区（`.tmp/` 已 gitignore）而**不要**摊进树里 ——
    `Expand-Archive docs\01-translation\ref\archive.zip -DestinationPath .tmp\ref-archive`（macOS/Linux 用 `unzip -d`）。
* ★ **资产也要核实现状**：`assets/` 说"这个词译作 X"，不等于现在的译文就是 X。
  用它之前**必须**在当前译文上实测：

  ```bash
  pnpm tools patch view --kind src            # 生成当前译文视图
  rg -n --no-ignore '<字串>' dist/views/src   # 实测：这个译法现在还在不在、有几处
  ```

  判读：**同一意思有两种写法**才叫活冲突；只命中一种 ⇒ 那条已闭环或从未成立。
  "文档说已改完"**不算**证据 —— 唯一证据是你刚跑出来的那次检索。
  ★ 两个**实测踩过**的计数陷阱：`comment "…"` 里是日文原文标记**不是译文**；
  `display-furigana "A" "B"` 是**主词+注音**，B 不是竞争译名。
* 已知在跑的活冲突清单挂在需求树（`pnpm tools requirements plan` / `show`），别自己另建一份。

## 6. 判据（收口凭据）

* **机械判据**：`pnpm tools patch verify` —— 基线 + patch ⇒ **逐字节**相同（缺省自证 `resultSha`）。
* **编辑回路自证**：`patch edit` 反解后立刻断言"重建 == 改过的视图"；**有任何一个反解不过 ⇒ 一个字都不写**。
* 改了术语/语气口径要**回改全工程**时：在视图上全量 `rg` 找出现位置，逐个脚本重复 §4 的 2–4 步；
  不要试图直接改 patch 的载荷（那正是唯一写入口要守的东西）。

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
* 视图是**生成物**：`patch view` 会**覆盖** `--out` 下的文件。改过的视图在 `patch edit --write` 成功前都还是"草稿"。

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
| `references/lookup.md` | 参考资产检索与**核实**：可复制的 `rg` 命令、说话人定位、活冲突怎么判 |
| `docs/01-translation/ref/README.md` | 快照的地位、`assets/**` 与 `archive.zip` 的二分、"引用前必须核实"的口径 |
| `docs/01-translation/patch-design.md` | patch 方案的设计与支撑观测（锚定规则、`src` 为何是视图） |
| `release/README.md` | 发行物：包里每件的真源、**CHANGELOG 规格（真源）**、三处缺口 |
| `pnpm tools patch describe` | patch 的字段 / 不变量 / 操作（**schema 唯一真源**） |
