# 原创决策（本仓的设计决定）

> **本文件只记跨域 / 提纲式的决定与"为什么"**；域内细节在各自文档（§8 有去处索引）。
> 硬性规则条目（怎么写 / 不许怎么做）在 `AGENTS.md`；知识层重建计划在 `knowledge-rebuild.md`；立项原文 `init-prompt.md`。
> 标注：`[§x.y]` = 立项原文的小节；`[本轮]` = 本仓初始化轮新定的口径。

---

## 1. 为什么重建 `[§1]`

旧仓的实测症状：`tickets/` 每个文件承载不到 1/3 条记录；`.tmp/` 里几百个一次性脚本；`scripts/` 一个目录混了 6 类关注点；
台账生成器住在名义上的翻译包里；10 个跨域守卫寄生在模拟器的 `test/` 下；真源 / 派生 / 叙述不分区；
根目录没有 `package.json`（不存在 workspace）；`.gitattributes` 只声明 LFS，零 `text`/`eol` 规则。
（实测数字随时可重测：`pnpm tools old-repo inventory`。）

用户口径：

> 目前为了允许 git 管理几乎都是纯文本 + 文件管理，但带来大量零散文件；当前项目本质上是多个子项目混合
> （翻译项目、模拟器项目、一个 wiki 网页），整个体系缺乏设计。所以直接 archive 历史数据，重新搭建一个更合理的环境。

⇒ **旧仓保持原样可归档；新仓分域、只登记指向；历史知识待清理 / 校验 / 重分类后再逐批进来**。

## 2. 存储纪律：介质按「查询方式」三分，不按体积 `[§1.6]`

| 介质 | 放什么 | 判据 |
|---|---|---|
| **git 文本** | 代码、配置、手写叙述、**append-only 的台账日志**、素材清单 | 需要 diff / review；人改；小 |
| **LFS 入库** | 反编译语料（只读、不需 diff）、基线二进制与脱壳件、fixtures、原生 prebuilds、字体、UI 图片 | 大 / 二进制 / 只读 ⇒ 显式 `git lfs track` 后提交 |
| **单文件结构化库（SQLite）= 派生只读查询层** | 各类台账的**查询视图** | 需要跨表 join / 反向索引 ⇒ **永不入库** |
| **不入库（留在仓库外，只登记）** | 真游戏安装、外部素材、发行 zip、`.tmp/`、`node_modules/` | 太大或本就在仓库外 ⇒ 符号链接或路径登记引用 |

### 五条硬纪律

1. **仓库不得跟踪任何 `*.sqlite` / `*.sqlite3` / `*.db`**：二进制库在两台机器之间**无法合并**，提交它等于制造"你覆盖我"。
2. **需要入库的大二进制 / 只读语料一律走 LFS**：显式声明并提交，不要靠本地忽略凑合。
   ★ LFS 只解决"大"，**不解决合并** —— 所以它救不了 DB（见第 1 条）。
3. **DB 永远可删**：删掉本地 DB 后，一条命令必须能从 git 文本真源**确定性重建**（同输入同字节）。
4. **台账真源是 append-only 文本日志**：一条记录一行，自带 ULID 或 `时间戳+计数器`，顺序不依赖文件位置；
   按 key 前缀 / 按月分片把并发追加的冲突窗口压到接近 0；周期性 `compact` 出 canonical 排序的 snapshot（**仍是文本**）。
   理由：两台设备各自追加的是**不同行** ⇒ git 三路合并天然干净，且每条结论都有 `git log -L` 可追的问责链。
5. **原始语料逐字节忠实**：任何"解析友好化"只能是派生的内存视图，**不得落回语料文件**（实证见 §4）。

**反模式**：把 `.sqlite` 提交进 git / 用 LFS 存 DB / 把 DB 当唯一存储 / 用 `sqlite3 .dump` 当文本真源 /
让 DB 参与写事务再"导出"成文本。

## 3. 跨平台纪律 `[§1.7]`

| # | 项 | 动作 | 为什么 |
|---|---|---|---|
| 1 | 换行 | `.gitattributes` 首行 `* text=auto eol=lf`；二进制 / LFS 加 `-text` | 旧仓零规则、`core.autocrlf=false`，实测确有混合行尾文件（当前数字见 inventory） |
| 2 | 生成物 | 生成视图与 DB 全部 `.gitignore` | 避免两台机器各自重建时互相打架 |
| 3 | 大小写 | 保持**零大小写冲突**（旧仓实测 0 组，见 inventory） | win/mac 不敏感、Linux CI 敏感 |
| 4 | 符号链接 | **可以用**：外部素材就用它引用；只要不 track | ★ 用户已明确排除的顾虑 |
| 5 | LFS | **直接用**：两台机器都 `git lfs install` | ★ 用户已明确排除的顾虑（不为"不支持 LFS"设计降级） |
| 6 | 非 ASCII 路径 | **可以用**，不构成问题 | ★ 用户已明确排除的顾虑（别为它改名） |

★ 第 4/5/6 条**不是限制**：`AGENTS.md` 里不得把它们写成限制，也不得"顺手"改既有路径名或把符号链接换成配置索引。

## 4. 语料与知识的两条原则 `[§1.8]` `[§1.10]`

**① 语料逐字节忠实 —— 为什么非有不可（实证）**：旧仓 `sanitize_symbols.py` 会把字符串字面量里含 `::`/空格/`<>`/`*`
的整段折叠成 `_`。同一份语料实测：raw 的 `::` **4754** / `this` **36753**，其 `_utf8.c` 只剩 **3** / **1**
⇒ **那份 `_utf8.c` 不是忠实导出，基于字符串层的结论全不可信**。
⇒ 转写**不许做任何符号改写**，且**"忠实"必须自证**（把结果逐行反解回字节、与源逐字节相同）。
具体逐行规则 / 宽串判定 / 编解码差异 / 断言清单都在域内文档（§8 索引），本文件不重复。

**② 锚点锚 EA，不锚语料行号**：IDA 输出是**一次性产物、不具可复现性** ⇒ 整体当只读资源保存；
语料只提供 `EA → 当前这份导出里的行号` 映射；换一次反汇编**只重建映射，不改任何锚**。
（旧仓 `raw N` 行号锚全废就是这个教训。）

## 5. 清单是 lockfile，不是约束 `[§3.1]` `[§3.2]`

| | 是什么 | 该放哪 |
|---|---|---|
| **约束 / 纪律** | "什么能进 git"、"语料不得改写"、"锚点锚 EA"、"禁 `.sqlite`" | **`AGENTS.md` 的规则条目 + 可执行守卫** |
| **素材清单** | 每个只读素材：从哪来、去哪、走哪种存储、怎么消费 | `corpus/assets.json` 的 `entries[]` |
| **派生数据** | 大小、入库件的校验和、LFS oid、计数 | **不写进来**（git / LFS / 文件系统已是权威） |

**唯一能把它变成约束的是守卫**：`pnpm tools corpus validate` 必须能红。**没有守卫的清单等于一份 Markdown。**

★ 由此推出两条**跨域**写清单纪律：
1. **不写可从磁盘 / git 推导的东西**（大小、入库件 sha256、LFS oid、`status`、`generatedAt`、计数）——
   这类数字属于生成物（如 `old-repo-inventory.md`）。
2. **`sha256` 只写"不入库件"**：那是"转码 / 搬运之前它长这样"的唯一证据；入库件的校验和交给 git/LFS。

## 6. 文档纪律：文档只写「不变的东西」+ 指向 `[本轮]`

用户口径：**过多 README 记录只是在制造污染与不一致**；**JSON 是不透明数据**；**这里搭的是基建不是业务**。

1. **不写状态**。自检一句话：**"这句话会不会因为下次干活而变错？"** 会 ⇒ 不写，改成**"怎么查"**。
   真源：**进度与"还要做什么" = `pnpm tools requirements plan`（需求树 `data/requirements/`）**；
   入库进度 = 清单的 `storage`/`dest`；条目与去向 = `pnpm tools corpus list`；
   语料保真 = `pnpm tools disasm verify`；旧仓数字 = `pnpm tools old-repo inventory`；变更历史 = `git log`。
   **不开内部变更记录 / 进度表**。例外**有两处**：
   ① **生成物**（脚本产出 + 文件头写明"别手改"，范例 `old-repo-inventory.md`）；
   ② **面向用户的发行文本**（`release/CHANGELOG.md` · `release/安装说明.md`）——
   它们是**产品的一部分**（随补丁包发给玩家），与"内部沿革只有 `git log`"是两件事。
   ★ 这条②判过一次**改判**：批 M1b 曾以"CHANGELOG 是手写状态而 `git log` 已足够"为由把 `CHANGELOG.md`
   连产物一起跳过；2026-10-03 用户口径推翻该理由（原结论在 M1b 节点里**保留可见**，未静默改写）。
   规格真源 = `release/README.md` §4；工作项 = `REQ-01M40RP2BN9S86K6SGEW671PYA`。
   ★ 进度原先是根目录一份手写的批次表（`PLAN.md`），现已被**需求树**取代并删除；
   因此本仓没有"唯一允许手写状态的文件"，两处例外**也都不是进度表**。
2. **结构化数据不进散文**：文件清单 / 用途 / 定位 / mtime / 哈希**只留一份结构化真源**，README 不列表、不抄数。
   ★ 确实需要两处都出现时 ⇒ **先问"能不能只留一处"**；真需要就**钉住**（测试断言两处一致）。
   ❌ 不加"不许出现某字符串"这类脆弱守卫 —— 守卫要**红得有意义**。
3. **JSON 是不透明数据，schema 由控制脚本自描述**：字段 / 枚举 / 不变量 / 怎么查 / 怎么改一律看 `--describe`，
   文档只写"它是什么 + 非显然口径 + 指向"（实现上枚举与不变量标题**只有一份真源**，所以不可能两处漂移）。
   每个**自有**的结构化数据文件旁边必须有**同名说明书**（`foo.json` ↔ `foo.md`，含「怎么查」「怎么改」并指向 `--describe`）；
   生态文件（`package.json` 等）不在范围内。
4. **测试只测基建契约**：
   * 测：守卫能不能红 · 写路径会不会写坏 · 发布物是否满足不变量 · 约定是否齐全。
   * 不测：业务结论 · 复述代码逻辑 · **数据的当前取值**（那是状态的活，交给 `validate` 当哨兵、`--list` 当查询）。
   * 判据一句话：**"这条断言红的时候，指向的是一处真实的不一致吗？"** 不是 ⇒ 别写。
   * 推论：**生成器宁可失败，也不许写出错文件**（拿不到事实就抛，不"读不到就当空"）。

## 7. 工具层的四条决定 `[本轮]`
1. **脚本优先，工具只在真能带来新能力时才存在**：判据是"生命周期 / 人的参与 / 输出形态 / 调用频次 / 发现方式"
   —— 短命、无句柄、有 `--json`、低频、`pnpm` 足够 ⇒ 脚本（corpus 操作五项全落在脚本列）。
   工具 = 插件 = 环境级安装 × 每台机器 × DSH 升级要重装；**最大风险是写路径分裂**。
   将来若做（M6 评估），契约写死：**只转发脚本的 `--json` 与退出码，不实现任何规则**。
2. **一个入口，不堆 `package.json`**：`pnpm tools`（域地图）+ `pnpm tools <域> <动作> [args…]`（薄转发），
   根 `package.json` 只留 `test` 与 `tools`；地图由各工具的自我声明**派生**（新增工具只改它自己，也漏不进地图）。
3. **工具目录三层，依赖方向单向**：纯工具 → 领域模型 → CLI；
   `lib/**` 不得 import CLI、CLI 之间不得互相 import、纯工具不得反向依赖领域模型。
   判据一句话：**"这个函数认识『素材/槽/语料』吗？"** 不认识 ⇒ 纯工具；只是"数据怎么读怎么写" ⇒ 领域模型；
   只有"怎么从命令行调、怎么打印" ⇒ CLI。
4. **写路径只有一份，前端只是调用方**（`[2026-10]`，网页建单时定）：**规则住在领域模型里，不住在任何一个前端里**。
   判据：**"第二个前端出现时，要不要抄一遍规则？"** 要抄 ⇒ 规则放错地方了，先挪进模型（`planAdd` 就是这么来的：
   CLI `--add` 与工作台 `POST /api/nodes` 共用它）。推论：**有第二个消费者才提取**，不预支
   （`--set` 至今只有一个调用方，就留在 CLI）；CLI 缺省 dry-run、写后回读复验、**不绿回滚**，
   网页那边照抄同一条纪律（HTTP 层只把"不合格"翻成 400、"守卫不绿"翻成 422，自己不做任何裁决）。

> 四条的实现细节 / 全图 / 命令表都在 `tools/README.md`（§0 分层、§0.1 入口、§2 常用命令），本文件只留决定本身。

## 7.1 依赖布局与 store：为什么是 `isolated` + 仓库内 store `[本轮]`

`AGENTS.md` §3 只留"写什么"，这一节留"为什么"。

**(1) 为什么 `.npmrc` 里不放结构设置。** 官方口径（<https://pnpm.io/settings>）：`.npmrc`
**只读 auth 与 registry**；定义 `node_modules` 结构的设置（`nodeLinker` / `shamefullyHoist`）**只能**写在
`pnpm-workspace.yaml`。pnpm 10 还认 `.npmrc` 的老写法、**pnpm 11 起忽略** ⇒ 写错地方会在某次升级后
**静默失效**。所以本仓 `.npmrc` 现在没有任何有效键，项目级设置全在 `pnpm-workspace.yaml`。

**(2) 为什么不设 `nodeLinker`（用默认 `isolated`）。** 起因是仓库里曾写的一条病因：
"本机 `fs.realpathSync` 不解析 junction ⇒ `vite` 的 `import 'rolldown'` 会 `ERR_MODULE_NOT_FOUND`"。
**这条病因已被直接实验推翻**：在 `.tmp` 里复刻 pnpm 的 `isolated` 布局（顶层 junction → `.pnpm/…` 实体），
从 junction 路径 `import` 那个包自己的嵌套依赖，**成功**（Node 的 ESM 解析走真实路径；
`realpathSync` 确实不解析 junction，但它不挡模块解析）。
随后做了**干净对照**（副本里，同一份真实依赖，只差这一个设置）：

| 配置 | 根 `node_modules` | `tools/node_modules` | `.modules.yaml` 记的 |
|---|---|---|---|
| **只有 `storeDir`（采用）** | **0 个** | 1 个 junction（`jimp`） | `nodeLinker: "isolated"` |
| 加 `nodeLinker: hoisted` | **91 个实体目录** | **不存在** | `nodeLinker: "hoisted"` |

两者**都能正常解析依赖**（`tools` 解析 `jimp` 均 OK）⇒ `hoisted` **不是必须的**；
而它把依赖全摊到**根**上（91 个实体目录）⇒ **遮蔽风险更大**。⇒ 用默认 `isolated`。

**(3) 为什么不写 `shamefullyHoist`。** 它默认就是 `true`，显式写是冗余；且实测上述两种形态下
`.modules.yaml` 记的都是 `publicHoistPattern: []` ⇒ 那一行**不生效**。

**(4) 为什么 store 用仓库内的相对路径。** `storeDir: '.pnpm-store'` 相对 workspace 根 ⇒ **可移植**
（换机器、换盘符都不用改），且**落在工作区内** ⇒ 受限沙箱下 `pnpm install` **不需要提权**。
放 `.npmrc` 不行：那里不做变量展开，只能写**绝对路径** ⇒ 机器相关。
背景：pnpm 默认 store 是 `$PNPM_HOME/store`，而**若工作区所在盘没有 home、或沙箱只允许写工作区，
它会回退到 `<workspace>/.pnpm-store`**（[pnpm#13525](https://github.com/pnpm/pnpm/issues/13525) 的标题
就是"AI agent 沙箱里出现项目内 `.pnpm-store`"）。显式钉死是为了**沙箱内外落点一致**。

**(5) 为什么"删不掉 `node_modules`"不是 bug。** `isolated` 布局下包文件是**硬链接**回 store
（实测 `nlink>1`），而受限沙箱只允许写工作区 ⇒ `Remove-Item node_modules` 会报成千上万条
`Access denied`（实测 23772 条）且**删不干净**。⇒ 换布局/重装一律**改名让开**（`Rename-Item`），
**不要**指望删除；也**别删 store**（它是 `node_modules` 的支撑）。
附带一条运营事实：改名后的遗留树若叫 `node_modules-xxx`，会被 `json-docs` 守卫当成"自有 JSON"而变红
⇒ 让开的树请挪进 `.tmp/`。

**(6) 换 npm 解决不了。** npm 的 cache 在 `%LOCALAPPDATA%\npm-cache`，沙箱同样拒写
（实测 `npm install` 直接失败：`lack permissions to access it` / 日志目录写不了）。
区别只是 npm **硬失败**、pnpm 能**降级**到项目内 store。

**(7) 提权只发生在两步。** "装依赖"（写工作区外的 store）与"headless 渲染"（Chrome 的 mojo IPC
要开命名管道）。`pnpm test` / `pnpm tools` / vite 构建都只读 `node_modules`，**不提权照跑**。
第三条环境口径见 `AGENTS.md` §5。

## 8. 域内决定的去处（索引）

| 主题 | 权威文档 |
|---|---|
| 反汇编语料：逐行转写规则、宽串判定、保真断言、`.staging/` 可弃 | `corpus/disasm/README.md` + `pnpm tools disasm describe` |
| 语料的编解码差异（Node vs Windows CP932 的 8 个单字节等） | `tools/lib/cp932.mjs` 头注释 |
| 素材清单的字段 / 9 条不变量 / 怎么查怎么改 | `corpus/assets.md` + `pnpm tools corpus describe` |
| 存档样本的槽 / 定位 / mtime 判据 / 唯一编辑入口 | `corpus/fixtures/samples.md` + `pnpm tools fixtures describe` |
| 素材总则：只读消费规则、LFS 口径、按需迁移、`.staging/` 定位 | `corpus/README.md` |
| **需求台账**：需求/缺陷的流程差异、父子树、一屏预算、`pnpm tools requirements` 命令表 | `data/requirements/README.md` + `pnpm tools requirements describe` |
| **知识台账**：append-only 文本真源、记录 schema（`id`/`at`/`kind`/`subject`/`claim`/`anchor[]`/`status`）、**锚点两形态**（二进制 EA / 可执行守卫用例 `路径#用例名`）、`repo=self/reference` 与"参考仓不在场只 warn"、`status`（声称）vs `effective`（投影）、冲突显式化、DB 可重建的判据 | `data/ledger/README.md` + `packages/ledger/README.md` + **`pnpm tools ledger describe`** |
| 引擎域：基线二进制与哈希、节表修补口径、AGERC 三份二进制与作废的旧语料 | `docs/02-engine/README.md` |
| 格式层：ALF / AGF / ASM 三套容器的盘上事实、"解包→重打包逐字节相同"判据 | `packages/age-format/README.md` + `node packages/age-format/cli.mjs verify` |
| ASM 指令集表（`opcodes.json`）的身份与改法 | `packages/age-format/src/asm/opcodes.md` |
| **AGE 脚本文本层**：谁负责解析 / 组装 / **reflow**（原 `packages/script-dsl` 占位包已删） | `packages/age-format/README.md` §1 / §3.4 + 跟踪单 `pnpm tools requirements show 7SGPFRBWPEQA8AF1WYQ8KP` |
| UI 图片与字体：落点、版本表、7z 解压产物不入库 | `corpus/README.md` §1、`corpus/assets/ui-images/versions.md`、`pnpm tools corpus describe` |
| 简→日写法占位字典（cp932 编码方案的一半） | `data/translations/README.md`、`data/translations/subs-cn-jp.md` |
| 翻译域：现状、数据模型（patch 叠加层）、**旧仓翻译文档快照 `ref/` 的地位与"先核实"口径** | `docs/01-translation/README.md` |
| **翻译 patch 方案**：真源 = 叠加层，`data`/`src` 都是视图；锚定规则与支撑观测 | `docs/01-translation/patch-design.md` + `pnpm tools patch describe` + `pnpm tools requirements show 8SNRXKV` |
| **翻译参考资产（只读快照）**：`assets/`（还能用的语言资产）vs `archive/`（历史工作单）的二分判据、来源、映射规则、为什么翻译资产可整棵迁而引擎知识不行 | `docs/01-translation/ref/README.md` + `tools/test/translation-ref.test.mjs` |
| **AGERC.DLL 的汉化**：旧做法实测（两条链叠加、整份改动 < 2.5 KB）、占位码位与字典/字体的隐藏耦合、六个方案的评估、待做的五个实验 | `docs/01-translation/agerc-design.md` + `pnpm tools requirements show 1ENS6BN3090X8M` |
| **发行（给玩家那一包）**：落点、包里每件的真源、CHANGELOG 规格、**三处缺口**（AGF 烘焙 / AGERC 链 / 打包动作）、有意不迁的件 | `release/README.md` + `pnpm tools requirements show 0RP2BN9S86K6SGEW671PYA` |
| **翻译 / 更新译文的作业流程**（技能） | `.agents/skills/amayui-translate/SKILL.md`（+ `references/conventions.md` · `references/lookup.md`） |
| 模拟器 / 探针域：按新结构重写、工具链、跨域守卫要拆回各域 | `docs/03-emulator/README.md`、`apps/*/README.md` |
| **项目工作台（网页）**：需求 + AGE 脚本反汇编；Vue 3 + Vite + TS；服务端 `server.ts` 由 Node 原生 type stripping 直跑；正文 Markdown 渲染（`markdown-it`）+ 通用控件用 `naive-ui`；**唯一写端点** `POST /api/nodes`（建单，规则 = 模型的 `planAdd`，只在监听回环时开） | `apps/workbench/README.md` + `pnpm tools requirements show 1M3XWRXVB04WQ4J0MA6AXZY9D` |
| agent 基建：技能固定路径、插件软链接注册、按重建处理 | `docs/04-agent/README.md`、`AGENTS.md` §7 |
| 知识层：清理起点清单、A/B/C 分级、准入规则、`callers/callees` 数据源缺失 | `knowledge-rebuild.md` |
| 环境与权限：五条硬纪律、跨平台六条、语言口径、怎么跑、三个沙箱口径 | `AGENTS.md` |
| **依赖布局与 store**：为什么不设 `nodeLinker`（`isolated` vs `hoisted` 的实测对照）、为什么不写 `shamefullyHoist`、为什么 store 用仓库内相对路径、硬链接删不掉的处置 | §7.1；纪律在 `AGENTS.md` §3 |
| 旧仓盘点（实测数字、跨域引用、混合行尾） | `docs/00-origin/old-repo-inventory.md`（生成物） |

★ 用户已定、但**不属于本仓设计**的口径（保持旧仓原样不导出 bundle/tag、技能与插件不迁移只重建、
平台优先 win32、包管理用 pnpm、只有 emulator 用 TS）分别记在上面这些文档里；本文件不重复。

### 8.1 一行口径（用户已定；详情在各域文档）

| 口径 | 详情 |
|---|---|
| 平台优先级：**win32 优先**，macOS 只做兼容性验证 | `AGENTS.md` §2 末 |
| 包管理 **pnpm**；**TS 只用在自带工具链的 app 上**（`apps/emulator`、`apps/workbench`），其余一律 `.mjs` | `AGENTS.md` §3 |
| 旧仓保持原样可归档，**不要求**导出 bundle / tag | §1 的结论；只读边界见 `AGENTS.md` §0 |
| 技能与插件**不迁移、只重建**（技能路径固定） | `docs/04-agent/README.md`、`AGENTS.md` §7 |
| `install/` **先保留**、后续重新设计 | `docs/01-translation/README.md` |
| 工作台可以**建单**（HTTP 写端点），但建单规则只有模型那一份；**改已有节点仍然只有 CLI** | §7 第 4 条、`apps/workbench/README.md` |
| 需求正文按 **Markdown 渲染**（`markdown-it`，`html: false`）；通用控件用 `naive-ui` | `apps/workbench/README.md`、`apps/workbench/src/markdown.ts` |
| `raw/` 继续用**符号链接**引用（不 track） | `corpus/README.md` §3 |
| 知识线（K1/K2/K3）**先不启动**；`callers/callees` 数据源缺失，先不做 | `knowledge-rebuild.md` |
| **翻译文档按「只读快照 + 先核实再用」迁回**（旧仓 `docs/translate/**` + `docs/translation/README.md`），并按**还能不能用**二分：`ref/assets/**`（语言资产，**散文件**要能 rg）/ `ref/archive.zip`（历史工作单，**打包**只要内容还在）；引擎知识仍必须过准入门 | `docs/01-translation/README.md` §5、`ref/README.md`、`pnpm tools old-repo translate-ref` |
| **CHANGELOG 与安装说明随包保留**（产品文本，不是内部变更记录）⇒ §6 的第②处例外；**发行目录叫 `release/` 不叫 `patch/`**（"patch" 已被变更叠加层占用） | `release/README.md`、`AGENTS.md` §10 |
| **删掉 `packages/script-dsl/` 占位包**（[本轮]）：它无代码、无人依赖；声称的"解析 / 组装"本来就在 `packages/age-format/src/asm/`（旧仓模拟器也只自带一份 `src/script/bin.ts`，`dependencies` 只有 `pixi.js`）⇒ **AGE 脚本文本层的落点 = `age-format`**，"排版 / reflow 要不要机械化"另立跟踪单（挂在「翻译更新」下，下游依赖是软的：翻译域现在接受手工折行） | `packages/age-format/README.md` §3.4；`pnpm tools requirements show 7SGPFRBWPEQA8AF1WYQ8KP` |
| **模拟器分两层**（[本轮]）：**M4-1 无头核心**（是知识线 K2/K3 的前置）与 **M4-2 表现层与窗口**（不是）—— 依据：79 条 `modeled-verified` 的守卫**全在 T0/T1 档**，T2 全仓只有 1 个文件且不进 `verify` ⇒ 复核不需要窗口、原生输入、真游戏安装 | `docs/03-emulator/README.md` §4.1；`pnpm tools requirements show 7SGXYWPQ1KWET2J49TJQST` |
| **`apps/inspector` ≠ 模拟器**（[本轮]）：前者是**真机探针**（C#/.NET 10 + WPF，读真进程内存，M7），后者自己的窗口壳是 `app/amayui-emulator/electron/`（8 文件，M4-2）；"都按新结构重写"不等于它们是同一件事 | `packages/host-input/README.md` §4、`apps/emulator/README.md` §5.1 |
| **节点预算只数活节点**（[本轮]）：`done`/`dropped`/`superseded` **不占** `maxNodes`。预算是给"还要人读它才能推进"的待办表设的；否则树会因为"做过的事变多"而爆预算、逼人删掉**已经交付的**记录，而拆一个节点出来（本该鼓励的动作）反而要先删两个已完成节点 | `tools/lib/requirements.mjs` 的 `BUDGET` 注释 + `data/requirements/README.md` §6 |
| **动作名与字段名不许同名**（[本轮]，工具层通则）：`pnpm tools ledger` 的概览动作叫 **`report`** 而不是 `status` —— 因为 `--status` 是**字段**（`--add … --status accepted`）。同名会让参数被**静默吃掉**（实测踩过：`--write` 被忽略、什么都没写）；判据 = "把动作名当字段名再用一次，会不会有歧义？" | `tools/ledger.mjs` 的 `parseArgs` 注释 |
| 技能「不迁移、只重建」的**第一个实例**：`amayui-translate`（继承纯约定、丢弃旧工具绑定） | `docs/04-agent/README.md` §6、`.agents/skills/amayui-translate/SKILL.md` |
