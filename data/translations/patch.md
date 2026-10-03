# data/translations/patch.md — `data/translations/patch.json` 的说明书

> ★ 本仓约定（`docs/00-origin/decisions.md` §6）：**JSON 是不透明数据** ——
> 字段 / 枚举 / 不变量 / 怎么查 / 怎么改**全部由控制脚本自描述**。
> 本文件**不复述 schema**（那是第二份 schema，必然漂），只写"它是什么 + 非显然口径 + 指向"。
>
> 控制脚本：**`pnpm tools patch`**（模型在 `tools/lib/patch.mjs`）—— 字段表与不变量看 **`pnpm tools patch describe`**。

## 它是什么

**唯一入库的翻译资产**：相对**原版 BIN**的**变更叠加层**。

* `data` 视图 = 原版 BIN 反汇编出来的文本；`src` 视图 = 原版 BIN 打上 patch 之后重放出来的文本。
  **两者都不入库**，随时可重算。
* 旧仓那棵 941×2 文件的文本树、以及 `install/` 汉化产物树，因此都**不再是真源**。

设计与支撑观测（为什么锚"行序 + 摘要"、为什么存中文、为什么不用旧仓 `raw-parts/`）：
`../../docs/01-translation/patch-design.md`。

## 非显然的口径（改它之前必须知道）

1. **`line` / `instr` 存的是中文**，不是 BIN 里的写法。
   BIN 走 cp932，简体字多半编不进去、要用同码位的日文写法占位（`subs-cn-jp.json`）——
   于是**同一个码位既可能是"本来就编得进去的汉字"，也可能是占位**，BIN **无法反推回中文**。
   ⇒ 中文才是真数据；落到 BIN 的写法是**构建时**按 `canEncodeCp932 优先 → 字典 → 全角空格` 派生的。
2. **顶层 `subsSha` 是构建的一环**：换字典 ⇒ 重建结果会变。`--verify` 会把它与当前字典对一下。
3. **`i` 是"基线反汇编的行序"**，不是字节偏移、不是 label、不是字符串槽——那三者都会随插入而漂。
   `sha8` 是那一行的内容摘要：基线一换就**报冲突**，不会按位置硬套。
4. **`resultSha` 是判据的证人，不是"上一次重建的结果"**：它由 `extract` 从旧仓产物写入，
   `verify` **只读不写**（否则就是自证循环）。
5. **没有变更的脚本不进 patch**：`ops` 空数组在 schema 里仍然合法（手写 / 分片场景），但
   `extract` **不会**为空 patch 建条目 —— "没改"不需要记录，空条目只会让 diff 变大。
   ⇒ "一共有多少脚本 / 其中多少有译文"**不能从 patch 反推**；产物根在场时由 `--verify` 现算并复核
   「没有条目的那些确实与基线逐字节相同」（少了这条，"少建条目"就会变成一条静默丢改动的路）。
6. **范围 = 全部能反汇编的脚本，不按名字筛**（只有产物与基线不同的才进 patch）。
   ★ 别把"旧管线**标注过**"（`SPEAKER_FILTER` = `SC*`/`SP*`，含 `$N$`）当成"有译文的脚本"：
   实测旧仓 `src/` 覆盖 **941** 个文本，其中非 SC/SP 的 **247 支同样有译文** ——
   按那个正则筛范围曾漏掉它们（详见 `../../docs/01-translation/patch-design.md` §2.3）。
7. **op 上可能有一个 `def`**：产物新增的跳转目标若落在"插入出来的行"上，基线里没有身份，
   只能给它一个 **patch 局部 label 符号**（`def`）—— 基线那边已有的定义**不重复放**。
8. **条目上可能有一个 `header`**：产物反汇编的**头部 4 行**与基线不同时才出现。重放时用它替掉基线那 4 行。
9. **旧仓有一处坏文件，迁移时没取它**：`install/$1$IMINIT.BIN` 是个 64 B 空壳（基线 10 184 B / 367 条指令
   → 只剩 `exit`），而它的文本树一行中文都没有、那 64 B 与游戏自带的 `$4$IMINIT.BIN` 逐字节相同 ⇒
   它是**旧工具链的事故**而不是译文，所以 `patch.json` 里**没有**这一条（构建保留游戏原版）。
   ★ 这件事**不在数据模型里**：跳过是迁移时用命令行 `--skip '<名字>'` 做的，理由见
   `../requirements/01M3YF5120V9WXB7NF36GVP52G.md`。与第 5 条成对：**"没进 patch"有三种原因** ——
   没变更 / 旧仓的坏文件被显式跳过 / 漏了；用 `--verify --target <产物根>` 可以把三者分开看。

## 怎么查

```bash
pnpm tools patch describe        # ★ 字段 / 不变量（含"谁在守它"）/ 操作 —— schema 的唯一真源
pnpm tools patch status          # 规模与分布：脚本数 / 操作数 / 体积 / 基线解析来源 / 字典指纹
pnpm tools patch baseline        # 每个脚本的**基线**与**产物**分别从哪来（散装 / 哪个 ALF 的哪一段）
pnpm tools patch verify          # 判据：基线 + patch ⇒ **逐字节**相同
pnpm tools patch verify SC0000.BIN   # 只核一个（逐条打印）
pnpm tools patch view --kind src --name SC0000.BIN --stdout   # 看 `src` 视图（生成物，不入库）
pnpm tools patch view            # 生成全部 data + src 视图 → dist/views/（已被 .gitignore 命中）
pnpm tools patch edit            # ★ 改完视图之后：把**改过的 src 视图**反解回 patch（dry-run；--write 落盘）
```

★ **`data` 与 `src` 视图是同构的两份文本**（同样的行数口径、同样的**真实地址**）：
`data` 是基线的反汇编，`src` 是"重建出来的 BIN 的反汇编，但字符串取 patch 里的**中文**"。
不让 `src` 直接显示 BIN 的原因：BIN 里存的是**占位写法**，中文只存在于 patch 里（见上 §非显然的口径 1）。

## 改文案的标准回路

```bash
pnpm tools patch view --kind src                # ① 生成 src 视图（生成物；改它是**正常操作**）
#  ② 用编辑器改 dist/views/src/<脚本>.txt 里的**中文**（不要动头部 4 行、不要动 label 与操作数形态）
pnpm tools patch edit                           # ③ dry-run：报出每个脚本的操作数变化
pnpm tools patch edit --write                   # ④ 落盘（写后回读复验，不绿回滚）
pnpm tools patch verify                         # ⑤ 复核：基线 + patch ⇒ 逐字节
```

* 反解**只处理与当前重建结果不同**的视图文件 ⇒ 全量扫描也不会误伤；
* 反解出来的 ops 是**全量替换**（永远相对**基线**提取，不在旧 ops 上叠加）；
* 有任何一个脚本反解不过 ⇒ **一个字都不写**（宁可没改，也不要一份半对的 patch）；
* 实测往返性：改一处中文 → 反解 → 重新生成 `src` 视图 ⇒ **与改过的那份逐字节相同**；
  再改回原样 ⇒ 操作集与 `resultSha` 都回到原值。

## 怎么改

* ❌ **不要手改 `patch.json`**：它的唯一写入口是控制脚本，且只有 **`extract`**（重取）与 **`edit`**（改过的视图反解）
  两条写路径（两者都缺省 dry-run，写前自证，写后回读复验，不绿回滚）。
* ✅ **改文案**：走上面「改文案的标准回路」（`view --kind src` → 编辑 → `edit --write`）。
  反解永远相对**基线**提取，所以改错了只要把视图改回去、再 `edit --write` 就回到原值。
* ✅ **重取**（旧仓在场时）：`pnpm tools patch extract`（dry-run 看会写什么）→ `--write`。
  提取是**迁移期一次性**的：patch 落库之后，旧仓 `install/` 与 `raw-parts/` 都不再被运行时引用。
* ✅ **基线换了**（游戏版本升级）：`pnpm tools patch verify` 会逐条报出 `sha8` / `baseSha` 冲突 ——
  **不要**放宽检查，那是"显式暴露冲突"的设计。
