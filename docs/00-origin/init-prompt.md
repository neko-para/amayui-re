# 任务：初始化 `E:\Projects\amayui-re` 的 monorepo 骨架，并建立「新仓 ↔ 旧仓」引用关系

> 本文件就是本轮提示词。开始前请完整读完 §0–§1。
> 完成后把本文件移到 `docs/00-origin/init-prompt.md`（保留为立项记录），不要留在仓库根。

---

## §0 本轮范围（**严格限定，别越界**）

**只做三件事：**

1. 建目录骨架 + 根配置（`package.json` workspaces / `.gitattributes` / `.gitignore` / `AGENTS.md` / `README.md`）。
2. 建 **只读素材登记表** `corpus/assets.json` —— 这就是"新旧引用关系"的机器真源：每条素材登记
   「在旧仓/外部的哪、多大、sha256、该不该进 git、怎么消费、**是否待重分类**」。
3. 生成 `docs/00-origin/old-repo-inventory.md`（旧仓盘点快照，用脚本**重新实测**，供后续迁移轮按图索骥）。

> ★★ **本轮不复制任何历史知识**（硬约束，理由见 §1.10）：旧仓的全部知识资产都**待清理 / 校验 / 重分类**，
> 在 `corpus/assets.json` 里**只登记指向**（记旧仓路径 + `storage: external-only` + `role: archived` + 禁止直接引用的消费规则），
> **一个文件都不复制**、一个字都不摘抄。**新仓的知识层本轮是空的，这是有意为之。**

**不做（留给后续交互式迁移轮）：**

* ❌ 不搬大数据：旧仓 `raw/`(8.3 GB) `raw-parts/`(8.3 GB) `install/`(7.7 GB) `engine/`(118 MB) `res/`(772 MB) 一个字节都不复制。
* ❌ 不迁移代码：`app/amayui-emulator`(589 文件) / `app/amayui-inspector` / `native/host-input` 都不搬。
* ❌ **不迁移任何历史知识与台账**：`analysis/*.json`(7) / `tickets/`(208 张，735 文件) / `docs-new/`(122 份)，
  以及任何由它们派生或复述它们的文档，一律**只登记、不复制**（理由见 §1.10）。
* ❌ 不建数据库、不写任何业务代码、不装依赖（`npm install` 可选）。
* ❌ 不写任何**游戏/引擎语义结论**（字段含义、函数用途、opcode 语义、脚本角色……）—— 那些属于待重建的知识层。
* ❌ **不改动、不删除旧仓任何东西**。旧仓 `E:\Games\Eushully\天結` 要保持可归档的完整状态。

**开工前先向用户确认 §5 的四件事**，确认完再动手。

---

## §1 背景（新会话无上下文，全部事实在此）

### 1.1 项目

《天結いキャッスルマイスター》(Amayui Castle Meister, Eushully) 的老游戏逆向工程 / 重实现 / 汉化工程。

* 引擎二进制约 1.3 MB；游戏业务数据 > 2 GB；IDA 反编译 C 约 19.6 万行 / 5 MB。
* VM 约 1000 条指令；典型路径每秒执行 5000+ 条 VM 指令；业务逻辑主要在外部数据里。
* 真正困难的逆向对象是 **engine 的内部 object model**（`this` + 大量字段、内部对象、状态机）。
* 交付目标：理解 engine 对象模型 / VM 指令与引擎状态的关系 / 辅助重实现 / mismatch 时能定位原实现。
* 开发环境：**win32 与 macOS 两台设备**，macOS 侧只做兼容性验证（不关键但必须有）。纯文本依赖就是为此。
* 最终工作流（长期目标，数千次增量分析）：

```text
mismatch → 稳定实体 target → 自动 context packet（已有事实+原始观察+局部代码+runtime evidence+显式冲突）
         → 局部分析 → 人工裁决 → 单写者更新知识与实现
```

### 1.2 为什么重建（用户原话的要点）

> 目前为了允许 git 管理几乎都是纯文本 + 文件管理，但带来大量零散文件；当前项目本质上是多个子项目混合
> （翻译项目、模拟器项目、一个 wiki 网页），整个体系缺乏设计。所以直接 archive 历史数据，重新搭建一个更合理的环境。

实测症状（旧仓）：`tickets/` 735 个文件承载 208 条记录（3.5 文件/条，最多 22 个）；`.tmp/` 10210 个文件里含
724 个 `.mjs` + 431 个 `.py` 一次性脚本；`scripts/` 一个目录混了 6 类关注点；台账生成器住在名义上的翻译包里；
10 个跨域守卫寄生在模拟器的 `test/` 下；`analysis/`(真源) 与 `output/`(派生) 与 `docs-new/`(叙述) 不分区。

### 1.3 涉及的全部路径（绝对）

| 名称 | 路径 |
|---|---|
| **新仓（本轮目标）** | `E:\Projects\amayui-re`（已 `git init`，尚无提交） |
| **旧仓（只读来源，勿改）** | `E:\Games\Eushully\天結` |
| 外部游戏安装目录（旧仓 `raw` 符号链接的目标） | `E:\Games\Eushully\天結いキャッスルマイスター` |
| **临时投递区**（仓库内、**gitignore**） | `<repo>\.staging\` —— 出仓带回来的文件先丢这里，**它不是"来源"，只是中转** |

> ★ **桌面不是来源**：目前那 4 个 `.c`/`.lst` 只是**临时躺在** `C:\Users\liaoh\Desktop\`，用户随时会清掉。
> 所以 `corpus/assets.json` 的 `roots` 里**不写桌面路径**。反汇编的**真前身是它导出自哪个二进制**
> （见 §3.2 的 `derivedFrom`），而"出仓再带回来"这一步走**仓库内的 `.staging/`**（用完即弃、不进 git）。

### 1.4 域划分（调研得出的结论，按产物与依赖而非目录名）

| 域 | 旧仓位置 | 性质 | 新仓落点 |
|---|---|---|---|
| **翻译** | `data/`(941, 日文只读基线) + `src/`(941, 译文) + `scripts/` 一半 + `res/fonts` + `patch/` | 交付 | `data/translations/` |
| **AGE 二进制格式** | `scripts/{asm,alf,agf,uimap}/` | 交付 | `packages/age-format/` |
| **模拟器** | `app/amayui-emulator`(589 跟踪 + 268 测试) + `native/host-input` + `plugins/amayui-emulator` | 交付 | `apps/emulator/` + `packages/host-input/` |
| **真机探针** | `app/amayui-inspector`（.NET 10 / C# / WPF / CLI / Core） | 交付 | `apps/inspector/` |
| **wiki** | `app/amayui-toolkit`（Vite+React+MUI，GitHub Pages 部署） | 交付 | **不迁移**，archive 在旧仓 |
| 知识台账 | `analysis/*.json`(7) + `scripts/build-*.mjs`(8) | 横切 | **都不带**（知识待重分类、工具待重建） |
| 票据台账 | `tickets/`(735) + `.agents/skills/amayui-ticket-ledger` | 横切 | 只带技能 |
| agent 基建 | `.agents/skills/`(10 技能) + `plugins/`(5 个 DSH 插件) | 横切 | **不迁移**：几乎都要重新适配 ⇒ **标记为后续重建**（技能路径固定，见下） |
| 素材/语料 | `engine/` `install/` `raw/` `raw-parts/` `cache/` `tools/` | 横切 | `corpus/`（只登记，不入 git） |

**agent 基建：不迁移，只重建。** 用户口径：**几乎所有技能与工具都需要重新适配** ⇒ 旧仓的 `.agents/skills/`(10) 与
`plugins/`(5) **都不搬**，只在 `corpus/assets.json` 里登记为 `rebuild`（留一个"重写时读得到旧实现"的指针）。

两条落点规则必须分开：

| | 落点 | 原因 |
|---|---|---|
| **技能** | **固定 `.agents/skills/`**（仓库根，**不可改名/移位**） | DSH 按特定路径发现技能 —— 这是**硬要求**。新仓里该目录必须存在，但**内容从零重写** |
| **DSH 插件** | **位置自由** | 插件只是注册到 DSH 的软连接，本来也要重新安装 ⇒ 新仓不为它保留固定顶层目录 |

⇒ **不要为了迎合注册方式去扭曲仓库结构**：插件的注册是环境级动作（步骤写进 `AGENTS.md`），技能则必须遵守固定路径。

### 1.5 旧仓盘点（实测，2026-09-30）

**git**：3677 个跟踪文件 / 555 次提交 / pack 170 MiB。**没有根 `package.json`**（不存在 workspace）。
`.gitattributes` 只声明了 Git LFS（149 个文件），**零 `text`/`eol` 规则**。

| 目录 | 跟踪 | 全部文件 | 体积 | 备注 |
|---|---:|---:|---:|---|
| `src/` | 941 | 941 | 91.8 MB | 译文（LF，297/300 抽样纯 LF、3 个混合行尾） |
| `data/` | 941 | 941 | 82.9 MB | 日文只读基线；与 `src/` 同构（370 相同 / 571 不同） |
| `tickets/` | 735 | 735 | 103.7 MB | 354 md / 220 json / 64 png / 37 log / 29 txt / **14 mjs / 5 ps1** / 1 zip |
| `app/` | 589 | 47300 | 793.9 MB | 含 node_modules |
| `docs-new/` | 122 | 122 | 6.1 MB | front-matter(`kind`/`state`) + 生成器 |
| `scripts/` | 115 | 184 | 6.1 MB | god dir：翻译管线 + 格式工具 + 逆向脚本(35) + 台账生成器(8) + 数据导出 + 引擎精化 |
| `res/` | 78 | 100 | 772.4 MB | LFS（images/fonts） |
| `plugins/` | 47 | 621 | 4.7 MB | 5 个 DSH 插件 |
| `.agents/` | 32 | 32 | 0.4 MB | 10 个技能 |
| `native/` | 15 | 864 | 6.9 MB | N-API + CMake + LFS prebuilds(darwin/win32) |
| `output/` | 10 | 10 | 3.2 MB | 派生（含 2 MB 的 html） |
| `analysis/` | 7 | 7 | 2.5 MB | 机器真源（fields/functions/opcodes/capabilities/scripts/opcode-gaps/journal.jsonl） |
| `cache/` | 7 | 7 | 3.4 MB | LFS：真玩家存档样本 SAVE76/78/79 + README |
| `engine/` | 11 | 11 | 118.2 MB | **未走 LFS**，是 git 普通 blob |
| `patch/` | 5 | 469 | 128.4 MB | LFS + 忽略 |
| `tools/` | 5 | 3946 | 490.9 MB | 第三方工具链，大多已 ignore |
| `install/` | 0 | 548 | **7677 MB** | 忽略；翻译产物编译后合并的目录（用户：**先保留**，后续重新设计） |
| `raw/` | 0 | 1033 | 7973.7 MB | **目录符号链接 → 外部游戏目录**（符号链接可用；只要不 track 即可） |
| `raw-parts/` | 0 | 27636 | 8269.4 MB | 忽略，在盘 |
| `.tmp/` | 0 | 10210 | 2773.4 MB | 忽略；含 724 `.mjs` + 431 `.py` 一次性脚本 |
| 根目录 | — | — | — | 另有 16 个发行 zip（`v1.0`…`v1.13`，合计约 1.4 GB，被 `/v*.zip` 忽略） |

**旧仓耦合实测**（说明为什么不能简单按目录切）：`tickets/` → 模拟器 **378 文件 / 2176 行**；模拟器 `src`+`test` →
`tickets/T-` **334 文件 / 1497 行**；`docs-new/` → 模拟器 75 / 805；`plugins/` → `tickets/`+`src/` 26 / 133；
`.agents/` → `tickets/` 9 / 62；`scripts/` → `analysis/` 12 / 51。

### 1.6 存储纪律（本轮要写进 `docs/00-origin/decisions.md` 与 `AGENTS.md`）

**介质按「查询方式」三分，不按体积：**

| 介质 | 放什么 | 判据 |
|---|---|---|
| **git 文本** | 代码、配置、手写叙述、**append-only 的台账日志**、素材清单 | 需要 diff / review；人改；小 |
| **LFS 入库** | **反编译语料**（只读、不需 diff）、基线二进制与脱壳件、fixtures（存档样本/截图）、原生 prebuilds、字体、UI 图片 | 大 / 二进制 / 只读 —— **需要入库就 `git lfs track <pattern>` 后提交** |
| **单文件结构化库（SQLite）= 派生只读查询层** | 票据台账、知识台账、脚本台账的**查询视图** | 需要跨表 join / 反向索引 —— 但**永不入库** |
| **不入库（留在仓库外，只登记）** | 真游戏安装 `install/`(7.7 GB)、`raw/`(8.3 GB，外部)、发行 zip(1.4 GB)、`.tmp/`、`node_modules/` | 太大或本就在仓库外 ⇒ 符号链接或路径登记引用 |

**五条硬纪律（写进 `AGENTS.md`）：**

1. **仓库不得跟踪任何 `*.sqlite` / `*.sqlite3` / `*.db`**。二进制库在两台机器之间**无法合并**，提交它等于制造"你覆盖我"。
2. **需要入库的大二进制 / 只读语料一律走 LFS**：用显式 `git lfs track <pattern>` 声明并提交，不要靠本地忽略凑合。
   ★ LFS 只解决"大"，**不解决合并** —— 所以它救不了 DB（见第 1 条）。
3. **DB 永远可删**：删掉本地 DB 后，一条命令必须能从 git 文本真源**确定性重建**（同输入同字节）。
4. **台账真源是 append-only 文本日志**（一条记录一行，自带 ULID 或 `时间戳+计数器` 以保证顺序不依赖文件位置；
   按 key 前缀或按月分片以把并发追加的冲突窗口降到接近 0；周期性 `compact` 出 canonical 排序的 snapshot，**仍是文本**）。
   理由：两台设备各自追加是**不同行** ⇒ git 三路合并天然干净；而且每条结论都有 `git log -L` 可追的问责链。
5. **原始语料逐字节忠实**，任何"解析友好化"只能是派生的内存视图，不得落回语料文件（见 §1.8 的实证）。

**明确的反模式**（写进 decisions）：把 `.sqlite` 提交进 git / 用 LFS 存 DB（LFS 不解决合并）/ 把 DB 当唯一存储 /
用 `sqlite3 .dump` 当文本真源 / 让 DB 参与写事务再"导出"成文本。

### 1.7 跨平台纪律（实测得出；逐条写进 `.gitattributes` / `AGENTS.md`，并写明**哪些不是**限制）

| # | 项 | 动作 | 为什么 |
|---|---|---|---|
| 1 | 换行 | `.gitattributes` 首行 `* text=auto eol=lf`；二进制/LFS 加 `-text` | 旧仓零规则、`core.autocrlf=false`，已实测出 3 个混合行尾文件 |
| 2 | 生成物 | 生成视图与 DB 全部 `.gitignore` | 避免两台机器各自重建时互相打架 |
| 3 | 大小写 | 保持零大小写冲突（旧仓实测 0 组） | win/mac 不敏感、Linux CI 敏感 |
| 4 | 符号链接 | **可以用**：外部素材就用它引用；只要**不 track** | 用户口径：win32 侧不存在使用障碍 |
| 5 | LFS | **直接用**：大二进制走 LFS；两台机器都 `git lfs install` | 用户口径：无需为"不支持 LFS"设计降级 |
| 6 | 非 ASCII 路径 | **可以用**，不构成问题 | 用户口径：中/日文件名照旧，别为它改名 |

> ★ 第 4/5/6 条是**用户已明确排除的顾虑**：不要在 `AGENTS.md` 里把它们写成限制，
> 也不要"顺手"去改既有路径名或把符号链接换成配置索引。

### 1.8 反汇编基线（**本轮必须知道的工件与语料事实**，非游戏语义）

新基线 = **原版 `AGE.exe` 的脱壳件 + PE 节表修补**。旧基线是汉化组的改写版，两者是父子关系（地址空间一致，
`.lst` 函数定义集交集 **3758/3797 = 99.0%**，大小同为 1,746,944 B）。

| 文件 | 路径 | MD5 | 说明 |
|---|---|---|---|
| 原版脱壳件（原件） | `E:\Games\Eushully\天結\raw\AGE.EXE__dumped.EXE`（经符号链接，实为外部游戏目录） | `F8A4BC5962CF2BC598DE2FF83B1ABE9E` | PE 节表**未还原**，直接分析会丢数据段 |
| **节表修补副本（= 基线二进制）** | `E:\Games\Eushully\天結\raw-parts\AGE.EXE__dumped.sectfix.EXE` | `50ECBA9B35F0886031B2752B5D8F73CE` | 与原文件**仅节表 24 字节不同** |
| **基线反编译导出** | `C:\Users\liaoh\Desktop\AGE.EXE__dumped.sectfix.EXE.c`（5.17 MB）/ `.lst`（18.04 MB） | | CP932 + 全 CRLF + 无 BOM |
| 汉化改写版导出（对照用） | `E:\Games\Eushully\天結\engine\天结_unpacked.exe_utf8.c` / `.lst` | | ⚠ **非忠实**（见下） |

> ★ **两个 exe 后续再处理**：本轮不带进新仓，M1 也不含（用户口径）。它们暂时留在外部游戏目录 / 旧仓 `raw-parts/`。
> 上表列它们只是为了让新会话理解"基线二进制长什么样、哈希是多少"。

**节表修补做了什么**（24 字节）：节 0 补 `IMAGE_SCN_CNT_CODE(0x20)` 并命名 `.text`；节 1/2/5 去掉 `MEM_EXECUTE`
并命名 `.data`/空/`.edata`；节 6 `.idata` 去 `CNT_CODE`；节 7 `poly` 保持 `CODE`。
OptionalHeader / DataDirectory / 其余字节一律不动。

**验收口径（修补后实测）**：`.lst` 段声明 `.text`/`poly` = `'CODE'`、`.data`/`seg002`/`seg003`/`.idata` = `'DATA'`；
`assume ds:` = `_data`；`#error` **0 条**；`char aXXX[] =` 声明 **782**；distinct 字符串字面量 **1357**；
`0x51D738` 显示为 `asc_51D738 db '…',0`（修补前是 `dword_51D738 dd …`）。

**原版与汉化改写版的实质差异只有两类：**

1. **唯一确证的指令级补丁 `sub_41A6C0` = ASCII→全角映射表**：原版 CP932（前导 `0x82`，偏移 `+0x1F`/`+0x20`，
   符号位 `817B/817C/8194`）；汉化版 CP936/GBK（前导 `0xA3`，偏移 `+0x80`，`'#'→A3A3`/`'-'→A3AD`/`'+'→A3AB`）。
2. **导入调用"拉平"**：原版 `call ds:` 1470 处（163 个具名 Win32 API），汉化版仅 13 处 + 56 处 `jmp ds:` 跳板。
   残差法实测 **3189/3758（84.9%）函数残差为 0** ⇒ 体长差异由此解释，语义未变。

**⚠ 语料污染实证（写进纪律）**：旧仓 `scripts/re/sanitize_symbols.py` 的 `QUAL_PAT` 字符类**包含空格**，
且**只跳反引号区段、不跳 `"..."` 字符串**，会把字符串字面量里含 `::`/空格/`<>`/`*` 的整段折叠成 `_`
（`"Live2D::init()…"` → `"Live2D__init()…"`）。同一份语料：raw `engine/天结_unpacked.exe.c` 里 `::` 有 **4754** 处、
`this` 有 **36753** 处，其 `_utf8.c` 只剩 `::` **3** 处、`this` **1** 处 ⇒ **旧仓 `engine/天结_unpacked.exe_utf8.c` 不是忠实导出**。

**关于反汇编语料的定位（用户已定）**：IDA 输出是**一次性产物、不具可复现性**，整体作为**只读资源**保存；
**锚点必须锚二进制里的 EA**，语料只提供 `EA → 当前这份导出里的行号` 映射（换一次反汇编只需重建映射，不改任何锚）。

**旧仓里与之相关的文档（本轮只读、不复制；后续按 §1.10 重分类）**：

* `E:\Games\Eushully\天結\docs-new\99-records\2026-09-disasm-baseline\disasm-baseline.md` —— 节表修补的完整前后对照表
* `E:\Games\Eushully\天結\docs-new\03-engine\unpacking.md` —— 拆壳与反汇编管线机制页
* `E:\Games\Eushully\天結\DESIGN.md` —— 工作流与知识消费的设计讨论
* `E:\Games\Eushully\天結\AGENTS.md` —— 旧仓的环境/权限须知

> 列它们只是为了让你**理解背景**；§1.8 的**事实**（路径 / 哈希 / 验收口径 / 补丁点）已足够本轮使用。
> 需要更细的内容时**去旧仓读**，**不要复制进新仓**。

#### 1.8.1 ★反汇编语料：只带 4 个文件，整体一个 zip 入库

**决定（已回退"3 对 × 2 视图"的方案）**：新仓只带下面 **4 个文件** ——
**出仓带回来的 sectfix 两个 + 旧仓内的 `engine/AGERC.DLL_utf8.{c,lst}`**：

| # | 文件 | 来历 | 角色 |
|---|---|---|---|
| 1 | `AGE.EXE__dumped.sectfix.EXE.c` | **`binary/age-sectfix` 的 Hex-Rays 导出**（出仓 → 丢进 `<repo>/.staging/`） | **基线** |
| 2 | `AGE.EXE__dumped.sectfix.EXE.lst` | 同上 | 同上 |
| 3 | `AGERC.DLL_utf8.c` | 旧仓 `engine/`（真前身：`engine/AGERC.DLL` 的转写） | 配套模块 AGERC.DLL |
| 4 | `AGERC.DLL_utf8.lst` | 同上 | 同上 |

**已废弃的另外那两个（`AGE.EXE__dumped.EXE.{c,lst}`）不要带** —— 它们是节表修补前的导出，已被 sectfix 取代；
它们若还躺在桌面，直接删掉即可。

**为什么不带旧仓那一对 exe**（`天结_unpacked.exe`）：即使把旧的迁进来，**其余文档仍要到旧仓原位置查**，
并不能自包含 ⇒ 半迁移只增加"重复且会不一致"的风险，换不来可用性。旧仓那几对留在旧仓，随时可按需再导出。

**入库形态：zip + 忽略的解压产物**

```text
corpus/disasm/
  disasm-20260930.zip      # ★ 入库（走 LFS）：内含上面 4 个文件（**UTF-8**）
  README.md                 # 清单 + 来源 + 每个文件的 sha256 + 解压/转码/断言口径
  files/                    # ← **gitignore**：解压出的 4 个文件；agent 直接读这里的
```

为什么 zip：这 4 个文件是**只读、基本不会再改的归档工件**，合成一个 blob 既不产生逐文件 diff 噪声，
文本压缩率又高（原始合计约 49 MB）。**入库的是 zip；解压产物 ignore**（本地解一次即可）。

**装进 zip 前的内容口径（写进 `AGENTS.md` 与 `corpus/disasm/README.md`）**

* 目标是 **4 个 UTF-8 文件**，好让 agent 直接读。
* **sectfix 那 2 个需要转码**（原为 CP932 + 全 CRLF，投递原件 5.17 MB + 18.04 MB）。
  转码**只允许两步**：① CP932 → UTF-8；② CRLF → LF。**除此之外一个字符都不许动。**
  * ❌ 禁止 `::` → `__`、❌ 禁止 `this` → `_this`、❌ 禁止折叠空格或重命名符号。
  * 转码脚本必须自带断言：与原始导出**逐行全等**、行数相同、`::` 计数相同、字符串字面量集合相同、`this` 计数相同。
* **AGERC 那 2 个已经是 UTF-8、原样收**（已实测忠实：`::` 127 = raw 的 127、字面量 119 = 119、
  `this` 151 = 151、0 处 `U+FFFD`）。⚠ **不要**改用 `AGERC.DLL.{c,lst}`（raw 是**混编**：
  CP932 + GBK 文件名行 + UTF-16LE 宽字符串，裸按 CP932 解码会掉 6 处；转写规则见旧仓
  `docs-new/03-engine/agerc-module.md` §4）。
* **sectfix 投递原件的 sha256 记进 `corpus/assets.json`**（原件只在 `.staging/` 过一手，不入库）——
  这样"没被改过"这件事仍然可复核。

**为什么这条纪律非有不可**（旧仓实测）：旧仓的 `sanitize_symbols.py` 与 `hexrays_prep.py` 就是这么干的 ——
同一份语料 raw `engine/天结_unpacked.exe.c` 里 `::` **4754**、`this` **36753**，其 `_utf8.c` 只剩 `::` **3**、`this` **1**。
语料一旦被这样改过，**基于字符串层的结论就都不可信**。

**体积预算（LFS）**：4 个文件原始合计约 **49 MB**（`.lst` 占大头：sectfix 18.04 MB + AGERC 25.61 MB），
压成一个 zip 后文本压缩率很高 ⇒ 实际入库大约 **10 MB 量级**。

### 1.9 其它已定决策（用户口径）

* `install/`：**先保留**（翻译产物编译后合并的目录），后续重新设计。
* `raw/`：本来就是外部的，**继续用符号链接引用即可**（符号链接可用，不必改造成配置索引），
  只需把链接本身 **gitignore 掉、不 track**；另在 `corpus/assets.json` 里登记它指向哪。
* `engine/`：**只保留 utf8 版本**（但也见 §1.8 —— 新基线是 sectfix 导出）。
* wiki(`app/amayui-toolkit`)：**不迁移**，archive 在旧仓。
* `.NET`(inspector) 与 CMake(native)：**也作为 repo 的一部分**。
* **agent 基建（技能 + 插件）**：**不迁移，标记为后续重建**（几乎都要重新适配）。技能**发现路径固定 `.agents/skills/`**
  （DSH 硬要求：新仓该目录必须在，但内容从零重写）；DSH 插件是软连接注册、**位置自由**、也要重装。
  二者的注册/安装步骤都写进 `AGENTS.md`（环境级动作）。
* **旧仓工具**（`scripts/` 的台账生成器/校验器、各技能内的脚本、`.tmp/` 里那些一次性脚本）：同样**按重建处理**，
  只登记为 `rebuild`，作为重写时读旧实现的参考。
* 历史数据：**一律不带进新仓**，只在 `corpus/assets.json` 里登记指向；未来源头必须先进 §1.10 的清理 / 校验 / 重分类。
  "配方"（生成器 / 守卫 / 流程脚本 / 技能 / 插件）**同样不迁移** —— 见上面两条：工具与 agent 基建一律按 `rebuild` 处理。
* 归档方式：用户会保持旧仓原样，本轮**不要求**导出 bundle 或 tag。
* `callers/callees` 数据源：旧仓不存在引擎函数级调用图，**先不做，仅记录缺失**。

### 1.10 ★历史知识一律待清理/校验/重分类（本轮不复制任何一条）

用户的明确口径：**现在所有历史知识都需要必要的清理、校验、重分类**，因此本轮**不把它们搬进新仓**。

**为什么不能直接搬**（旧仓实测，作为清理工作的起点清单）：

| 缺陷类 | 实测 |
|---|---|
| 非法枚举 | `analysis/fields.json` 有 3 条 `status: "ANALYZED"`（那是 `functions.json` 的枚举，写串了） |
| 主键冲突 | `fields.json` 里 **2 组同 `scope+offset`** 且两边都标 `confirmed`（`Engine/0x5D880` = `cur_script` vs `frames`；`Engine/0x5D88C` = `dispatch_saved_effect_flags` vs `dispatch_saved_flags`） |
| 重名 | 9 组同名跨 scope（`flags`/`scene`/`width`/`height`……）⇒ **键必须是 `scope+offset`，绝不能是 name** |
| 写入缺陷 | `functions.json` 有 14 条 `fields_used` 是**字符串化的 JSON 数组** |
| 无 schema | `fields.json` / `functions.json` 是**裸数组**，没有 `_doc`/`statusEnum`/`counts`，也没有 `--validate` |
| 记法混乱 | 字段引用至少 **6 种记法**并存（`Engine+0x5D8C8` / `Engine+82876` 十进制 / `Engine[93384]` / `this->cur_script` / `_this+4*cur+388612` / `MeshEntry.vbuffer`）；一条 `fields_used` 归一化后只有约 **1/4** 能回连 `fields.json` |
| 锚点失效 | 换反汇编后所有 `raw N` 行号锚全废（旧仓 611/625 个函数区间、139 条票据证据、180 条 `handlerBodyLine`） |
| 语料污染 | 旧仓 `engine/天结_unpacked.exe_utf8.c` 被 `sanitize_symbols.py` + `hexrays_prep.py` 改过（同一语料 raw 的 `::` 4754 / `this` 36753 → `_utf8` 只剩 **3** / **1**），据此得出的**字符串层结论**都不可信 |
| 自述标签不可信 | `confirmed`/`tentative` 都是人工自述；`capabilities` 146 条里只有 79 条 `modeled-verified`（且这 79 条**都带可执行守卫**） |

**建议的重分类框架**（写进 `docs/00-origin/knowledge-rebuild.md`，供后续批次执行；本轮**不执行**）：

| 类 | 判据 | 处置 |
|---|---|---|
| **A 可机械再生成** | opcode 派发表、`raw_name → 起止行`、调用图、脚本骨架、各类生成视图 | 不是知识 ⇒ **数据丢弃**；生成器本身按 §1.9 一并**重建**，不迁旧脚本 |
| **B 被可执行守卫验证** | `capabilities` 的 `modeled-verified` + `guard`；票据的 `tests[]`；缺口台账的 `implemented` | **优先抢救**（这是旧仓唯一真正被验证过的部分） |
| **C 纯人工散文** | 字段语义 / 函数用途 / opcode 散文 / 脚本角色 | 默认**不采信**，逐条重做或按新准入规则重新登记 |

**知识层的新准入规则（写进 `AGENTS.md`，本轮先立规矩不落数据）**：

1. 每条语义结论必须**绑定至少一条可再校验的观察**（二进制 EA / 语料行区间 + 内容摘要 / 可执行守卫用例 id）。
2. **锚点锚 EA，不锚语料行号**；换反汇编只重建 `EA → 行号` 映射，不改任何锚。
3. 读取时**重校验**绑定：观察失效（行内容变了、守卫用例没了）⇒ 该条自动降级为 `stale`，**不得再进 accepted**，但**不删除**。
4. 冲突**显式化**为产物，不允许静默改写已有结论。
5. **不需要人工审核全部历史结论** —— 靠上面的机械失效暴露问题，而不是靠人逐条看。

---

## §2 本轮要创建的骨架（逐项，含每个文件写什么）

```text
E:\Projects\amayui-re\
├─ README.md                    # 项目定位 / 域地图 / 怎么跑 / 指向 docs 与 corpus
├─ AGENTS.md                    # 环境与权限须知（旧仓同名文件的角色）：§1.6 五条硬纪律 + §1.7 跨平台六条 + 搜索约定(rg) + 怎么跑
├─ .gitattributes               # 首行 `* text=auto eol=lf`；二进制 `-text`；LFS **直接启用并显式 track**
│                               #   例：corpus/disasm/**  *.BIN  *.DAT  *.STH  *.png  *.ttf  *.node  *.DLL  *.exe
├─ .gitignore                   # 见下
├─ .editorconfig                # eol=lf / utf-8 / indent
├─ package.json                 # 根；private；workspaces
├─ docs/
│  ├─ 00-origin/
│  │  ├─ init-prompt.md         # 本文件（完成后移来）
│  │  ├─ decisions.md           # 新项目的**原创决策**：§1.6 五条硬纪律 + 反模式清单 + §1.7 跨平台六条 + §1.9 已定口径
│  │  ├─ knowledge-rebuild.md   # 知识层重建计划：§1.10 的清理起点清单 + A/B/C 重分类框架 + 准入规则（**只有计划，无任何结论**）
│  │  └─ old-repo-inventory.md  # §1.5 盘点（本轮由脚本重新实测生成，别手抄）
│  ├─ 01-translation/README.md  # 翻译域：现状与待定（双份合并 / 机读修改点语法 / BIN 内嵌中日文的前瞻）
│  ├─ 02-engine/README.md       # 引擎域：**只写语料与工件事实**（§1.8 的路径/哈希/节表修补口径/编码），不写任何游戏语义结论
│  ├─ 03-emulator/README.md     # 模拟器域：来自旧仓哪些目录 / 工具链
│  └─ 04-agent/README.md        # agent 基建：来源（旧仓哪些技能/插件）+ **怎么注册到 DSH**（环境级步骤，与目录无关）
│                               # ★本目录下**没有**任何从旧仓复制的知识文档 —— 有意为之（§1.10）
├─ corpus/
│  ├─ assets.json               # ★ 素材清单（lockfile 性质，**不是**约束）—— 见 §3
│  ├─ README.md                 # 素材总则：只读件的消费规则、LFS 口径、按需迁移
│  ├─ disasm/                   # ★ 反汇编语料（见 §1.8.1）—— 本轮只建目录与 README
│  │  ├─ README.md              #   4 文件清单 + 来源 + sha256 + 解压/转码/断言口径
│  │  ├─ disasm-20260930.zip   #   入库（LFS）：sectfix 两个 + AGERC 两个（均为 UTF-8）
│  │  └─ files/                 #   ← gitignore：解压出的 4 个文件，agent 直接读这里的
│  ├─ binaries/   (.gitkeep)    # 脱壳件 / 节表修补件 —— **后续再处理**（本轮为空、M1 也不含）
│  ├─ fixtures/   (.gitkeep)    # 玩家存档样本（cache/）落点（本轮为空）
│  └─ game/       (.gitkeep)    # 外部游戏目录 / 安装目录的**指针**（不搬数据）
├─ data/
│  ├─ translations/README.md    # 译文真源落点（本轮只写模型说明，不搬 941×2 个文件）
│  └─ ledger/README.md          # 台账真源（append-only 文本日志）落点与格式草案
│                               # ★本轮**没有任何台账条目** —— 旧知识待重分类，不迁移（§1.10）
├─ packages/
│  ├─ age-format/{package.json,README.md}   # ALF/AGF/ASM 二进制格式
│  ├─ script-dsl/{package.json,README.md}   # AGE 脚本解析/组装/reflow
│  ├─ ledger/{package.json,README.md}       # 台账：文本日志 + 派生 SQLite 查询层
│  └─ host-input/README.md                  # N-API/CMake（.NET/CMake 各自工具链，不进 npm workspaces）
├─ apps/
│  ├─ emulator/README.md        # 迁移轮再入 workspaces
│  └─ inspector/README.md       # .NET 10，独立工具链
├─ tools/                       # 跨域 CLI（生成视图 / 校验 / context builder）—— 从零写，不迁旧脚本
│  ├─ package.json / README.md
│  ├─ corpus.mjs                # ★ 素材清单的**唯一写入口**：--scan / --validate / --add / --set（§3.3）
│  ├─ disasm-recode.mjs         # CP932→UTF-8 无损转码 + 逐行全等断言（§1.8.1）
│  └─ test/corpus-manifest.test.ts   # ★ 守卫：跑 --validate，红了就拦（"约束"落在代码里，不在 JSON 里）
├─ .agents/skills/README.md     # ★ 发现路径**固定**（DSH 硬要求），不可改名/移位；本轮只放说明，技能内容后续重建
└─ plugins/README.md            # DSH 插件的**源码**落点（位置自由；插件走软连接注册）—— 后续重建，不迁旧源码
```

**根 `package.json` 建议**（本轮可 `npm install` 通过，无新依赖）：

```json
{
  "name": "amayui-re",
  "private": true,
  "type": "module",
  "workspaces": ["packages/*", "tools"]
}
```

`packages/*/package.json` 与 `tools/package.json` 写最小合法内容（`{"name":"@amayui/<dir>","private":true,"type":"module"}`），
`apps/*` 本轮**不进** workspaces（代码还没搬）。

**`.gitignore` 建议**：

```gitignore
node_modules/
# 派生：DB 与生成视图永不入库（硬纪律 1）
*.sqlite
*.sqlite3
*.db
.cache/
dist/
tmp/
# 仓库外的大素材：用符号链接 / 路径登记引用，不入库（§1.6 第 4 类）
/raw
/install
/raw-parts
/.tmp
# 出仓带回来的临时投递区（不是"来源"，只是中转）
/.staging/
# 反汇编语料的解压产物（入库的是 zip，见 §1.8.1）
/corpus/disasm/files/
# 本机私有配置
*.local.json
```

> ★ `corpus/` **不再整体忽略**：需要入库的语料 / 二进制 / fixtures 一律走 `git lfs track` 后提交（见 `.gitattributes`）。
> `corpus/assets.json`、`corpus/README.md` 与各载荷目录的 `.gitkeep` 必须入库。

---

## §3 `corpus/assets.json` —— 素材清单（lockfile 性质，**不是**约束）

### 3.1 先分清三样东西（上一版我把它们混在一个 JSON 里了）

| | 是什么 | 该放哪 | 为什么 |
|---|---|---|---|
| **约束 / 纪律** | "什么能进 git"、"语料不得改写"、"锚点锚 EA"、"禁 `.sqlite`" | **`AGENTS.md` 的规则条目 + 可执行守卫** | 约束只有**可执行**才算约束。写成 JSON 里的 `policy: "…"` 散文，机器执行不了、agent 也不会去读它 ⇒ 那是**伪装成数据的文档** |
| **素材清单** | 每个只读素材：从哪来、去哪、走哪种存储、怎么消费 | 本文件的 `entries[]` | 只有**从磁盘/git 推导不出来**的信息才需要人写 |
| **派生数据** | 大小、入库件的校验和、LFS oid、解压产物 | **不写进来** | git / LFS / 文件系统已经是权威；手抄一份必然漂 |

**一句话**：`corpus/assets.json` 是 **lockfile**（同 `package-lock.json` 的角色）——记录"来源与去向"，
不记录"规则"，也不重复"校验和"。**唯一能把它变成约束的是守卫**：`tools/corpus.mjs --validate` 必须红。
**没有守卫的清单等于一份 Markdown。**

### 3.2 完整字段（上一版缺的就是这些）

顶层：

```jsonc
{
  "schemaVersion": 1,
  "roots": {                                   // ★ 绝对路径只写在这里；条目里一律相对
    "oldRepo":     "E:\\Games\\Eushully\\天結",
    "gameInstall": "E:\\Games\\Eushully\\天結いキャッスルマイスター",
    "staging":     ".staging"                  // 仓库内的临时投递区（gitignore）；不是"来源"，只是中转
  },
  "entries": [ /* 见下 */ ]
}
```

每个 `entry` —— **所有类别共用同一形状**，用 `kind` / `storage` 区分
（上一版把 `knowledge` / `rebuild` 做成两个游离顶层块、字段与 `entries` 又不一样，是错的：那样校验要写三套）：

| 字段 | 必填 | 类型 / 枚举 | 说明 |
|---|---|---|---|
| `id` | ✅ | string，全局唯一，形如 `域/名` | 稳定键，其它文档引用它 |
| `kind` | ✅ | `disasm-corpus` \| `binary` \| `fixture` \| `asset` \| `knowledge-source` \| `tooling` \| `agent-infra` | 类别 |
| `role` | ✅ | `baseline` \| `reference` \| `module` \| `fixture` \| `archived` \| `rebuild` \| `deferred` | 人类语义角色 |
| `storage` | ✅ | `lfs` \| `git` \| `external-only` \| `deferred` | ★ **存储去向** —— 取代上一版含糊的 `status` |
| `origin` | ✅ | `[{ root, path, sha256? }]`，**非空** | 来源。`root` ∈ `oldRepo`/`gameInstall`/`staging`/`abs` + 相对 `path`；`sha256` **仅当该件不入库或来自 `staging` 时**才写 |
| `derivedFrom` | 条件 | `[{ ref, tool?, note? }]`，`ref` = 另一个 `id` | ★ **真前身**：这份东西是从哪个件、用什么工具产生的（例：反汇编 ↔ 二进制）。`kind=disasm-corpus` 时**必填** |
| `dest` | ✅ | string \| null | 入库路径；`storage ∈ {external-only, deferred}` ⇒ **必须 `null`** |
| `readOnly` | ✅ | boolean | 只读件禁止就地修改 |
| `recipe` | 条件 | string | 生成 / 转码脚本路径；`kind=disasm-corpus` 或"入库件经过加工"时**必填** |
| `consume` | ✅ | string | 一句话消费规则（禁止改写 / 锚 EA / 只提供 EA→行号映射 …） |
| `blocks` | ⬜ | `["M1","K3"]` | 哪些迁移批次依赖它（给 §6 用） |
| `note` | ⬜ | string | 补充（例如"该件的 raw 是混编、不可裸转"） |

**刻意不写的字段**（写了就是重复真源）：`bytes`、入库件的 `sha256`、LFS oid、`status`（用 `storage` + 磁盘是否存在表示）、
`generatedAt`（只会让每次扫描都产生 diff）。

### 3.3 守卫（`tools/corpus.mjs --validate`；红了才算约束生效）

| # | 断言 |
|---|---|
| 1 | schema：必填齐全、枚举合法、`id` 全局唯一 |
| 2 | 自洽：`storage ∈ {external-only,deferred}` ⇔ `dest === null`；`storage ∈ {lfs,git}` ⇔ `dest` 非空 |
| 3 | 存在性：`storage ∈ {lfs,git}` ⇒ `dest` 在盘上；`origin[].path` 按 `roots[root]` 解析后必须存在。★ **`root=staging` 例外**：可缺失，只报 warning（它是用完即弃的中转，不是长期位置） |
| 4 | **不重复校验和**：入库件**不得**写 `origin[].sha256`；不入库件与 `root=staging` 的**必须**写 |
| 5 | 忽略一致性：`external-only` 且 `dest` 在仓内 ⇒ 必须被 `.gitignore` 命中 |
| 6 | LFS 一致性：`storage=lfs` ⇒ `git check-attr filter -- <dest>` 必须是 `lfs` |
| 7 | 语料保真：`kind=disasm-corpus` ⇒ `recipe` 必填，且跑一次 recipe 的断言（与原始导出**逐行全等**、行数 / `::` / 字面量 / `this` 计数相同） |
| 8 | 知识准入门：`kind=knowledge-source` ⇒ `storage` 只能是 `external-only`/`deferred`（K3 通过前一律不得入库） |
| 9 | **真前身可解析**：`derivedFrom[].ref` 必须是另一个存在的 `id`；`kind=disasm-corpus` ⇒ `derivedFrom` 非空且指向 `kind=binary` 的条目 |

`tools/corpus.mjs` 同时是**唯一写入口**：`--add` / `--set` / `--scan`（补 `origin[].sha256`）/ `--validate`（缺省 dry-run、写后回读）。

### 3.4 必须登记的条目

| id | kind | role | storage | origin（相对 `roots`） | dest | 备注 |
|---|---|---|---|---|---|---|
| `disasm/bundle` | `disasm-corpus` | `baseline` | **`lfs`** | `staging:AGE.EXE__dumped.sectfix.EXE.{c,lst}`（带 sha256）+ `oldRepo:engine/AGERC.DLL_utf8.{c,lst}` | `corpus/disasm/disasm-20260930.zip` | ★ **只带这 4 个**（§1.8.1）。`derivedFrom=[{ref:binary/age-sectfix, tool:"Hex-Rays 8.3.0.230608"}]`；`recipe=tools/disasm-recode.mjs` |
| `disasm/raw-source-sectfix` | `disasm-corpus` | `reference` | `external-only` | `staging:AGE.EXE__dumped.sectfix.EXE.{c,lst}`（CP932+CRLF，**带 sha256**） | `null` | 转码前的忠实参照 ⇒ "没被改过"可复核（原件是 `.staging/` 里过一手的中转件） |
| `disasm/raw-source-agerc` | `disasm-corpus` | `reference` | `external-only` | `oldRepo:engine/AGERC.DLL.{c,lst}`（**混编**，带 sha256） | `null` | 入库的是其 `_utf8` 版 |
| `disasm/not-included` | `disasm-corpus` | `deferred` | `deferred` | `staging:AGE.EXE__dumped.EXE.{c,lst}`、`oldRepo:天结_unpacked.exe*.{c,lst}`、`oldRepo:engine/{defs.h,engine.hpp,hxclang_prelude.h}` | `null` | 明确**不带**：修补前导出已被 sectfix 取代（桌面那两个可直接删）；旧 exe 那对与三个头文件留在旧仓 |
| `binary/age-original` | `binary` | `deferred` | `deferred` | `gameInstall:AGE.EXE__dumped.EXE` | `null` | **后续再处理**（本轮与 M1 都不入仓） |
| `binary/age-sectfix` | `binary` | `deferred` | `deferred` | `oldRepo:raw-parts/AGE.EXE__dumped.sectfix.EXE` | `null` | 同上 |
| `game/install-dir` | `asset` | `reference` | `external-only` | `gameInstall:.`（156 项） | `null` | 只登记路径，**不搬** |
| `game/translated-install` | `asset` | `reference` | `external-only` | `oldRepo:install/`（548 文件 / 7.7 GB） | `null` | 用户口径：先保留、后续重新设计 |
| `game/raw-parts` | `asset` | `reference` | `external-only` | `oldRepo:raw-parts/`（27636 文件 / 8.3 GB） | `null` | 外部索引改造后可能不再需要 |
| `fixtures/save-samples` | `fixture` | `fixture` | `lfs` | `oldRepo:cache/`（7 文件） | 待定 | 需带 **mtime 侧车**（存档头记录 mtime，测试断言依赖它） |
| `translation/baseline` | `asset` | `reference` | `external-only` | `oldRepo:data/`（941 文件） | `null` | 日文只读基线 |
| `translation/translated` | `asset` | `reference` | `external-only` | `oldRepo:src/`（941 文件） | `null` | 译文 |
| `assets/ui-images` | `asset` | `reference` | `external-only` | `oldRepo:res/images/`（62 跟踪） | `null` | 旧仓走 LFS |
| `tools/thirdparty` | `tooling` | `reference` | `external-only` | `oldRepo:tools/`（3946 文件 / 491 MB） | `null` | 第三方工具链 |
| `knowledge/analysis` | `knowledge-source` | `archived` | `external-only` | `oldRepo:analysis/`（7 文件 / 2.5 MB） | `null` | `consume: 禁止直接引用，见 §1.10` |
| `knowledge/tickets` | `knowledge-source` | `archived` | `external-only` | `oldRepo:tickets/`（208 条 / 735 文件） | `null` | 178 条带 `tests[]` 者属 B 类、优先抢救 |
| `knowledge/docs` | `knowledge-source` | `archived` | `external-only` | `oldRepo:docs-new/`（122 份） | `null` | 含 10 份跨域活文档 |
| `knowledge/re-scripts` | `knowledge-source` | `archived` | `external-only` | `oldRepo:scripts/re/`(35) + `scripts/*.py` | `null` | ⚠ 含会改坏语料的 `sanitize_symbols.py` |
| `reference/design-discussion` | `knowledge-source` | `archived` | `external-only` | `oldRepo:DESIGN.md` | `null` | 工作流与知识消费的设计讨论 |
| `agent/skills` | `agent-infra` | `rebuild` | `deferred` | `oldRepo:.agents/skills/`（10 技能） | `null` | **不迁移**；技能发现路径固定，重写时必须落 `.agents/skills/` |
| `agent/plugins` | `agent-infra` | `rebuild` | `deferred` | `oldRepo:plugins/`（5 个 DSH 插件） | `null` | **不迁移**；位置自由，靠软连接重新注册 |
| `tooling/ledger` | `tooling` | `rebuild` | `deferred` | `oldRepo:scripts/build-*.mjs`(8)、`scripts/journal.js` | `null` | **不迁移**；台账形态要换，旧生成器只作参考 |
| `tooling/re` | `tooling` | `rebuild` | `deferred` | `oldRepo:scripts/re/`(35) + `scripts/*.py` | `null` | **不迁移**；⚠ 含 `sanitize_symbols.py` |

> **去重**：`knowledge/re-scripts` 与 `tooling/re` 指向同一批脚本 —— 只留一条（建议留 `tooling/re`，
> `knowledge/` 只放真正产出"结论"的东西）。动手时按 `id` 唯一性守卫发现重复就合并。

**同时生成 `docs/00-origin/old-repo-inventory.md`**：用脚本**重新实测**旧仓（别手抄 §1.5），输出
「目录 / 跟踪数 / 全部文件数 / 体积 / 是否 gitignore」表 + `git log -1` 的 HEAD sha，作为迁移轮的定位基线。

---

## §4 本轮验收（做完请自检并回报）

1. `git status --porcelain` 只列出本轮新建的骨架文件；**旧仓路径下无任何变更**（在旧仓跑一次 `git status --porcelain` 证明）。
2. **`corpus/assets.json` 过守卫**：`node tools/corpus.mjs --validate` **绿**（§3.3 的 8 条断言全过）。
   特别是第 4 条：**入库件没写 `sha256`**（校验和交给 git/LFS），不入库件**必须写**；
   第 8 条：`kind=knowledge-source` 的条目 `storage` 只能是 `external-only`。
   报告里贴出 `--validate` 的完整输出。
3. ★**零复制自检**（本轮最关键的一条）：新仓里**没有** `analysis/`、`tickets/`、`tools/` 下的旧脚本；
   `docs/**` 下每一份文件都能说明「本文件为本轮新写」，**没有任何一份是旧仓内容**；`data/ledger/` 下**没有条目**；
   `.agents/skills/` 与 `plugins/` 下**只有说明性 README**（技能与插件都待重建）。报告里要写明你是怎么验的。
4. `npm install` 在根目录可成功（或明确报告失败原因与阻塞项）。
5. `.gitattributes` 里 `* text=auto eol=lf` 在首行；`git check-attr text eol -- <某 .md>` 返回 `text: auto` / `eol: lf`。
6. **忽略规则对**（本轮就能验）：`git check-ignore -v corpus/disasm/files/x.c` 命中（解压产物不入库，§1.8.1）；
   `git check-ignore -v install/xxx` 命中（仓库外大素材不入库）；`git check-ignore corpus/assets.json` **不**命中。
   LFS 属性等 M1 产出 zip 后再验：`git check-attr filter -- corpus/disasm/disasm-20260930.zip` 应为 `filter: lfs`。
7. `docs/00-origin/decisions.md` 覆盖 §1.6 五条硬纪律 + 反模式清单 + §1.7 六条跨平台纪律 + §1.9 已定口径；
   `docs/00-origin/knowledge-rebuild.md` 覆盖 §1.10 的清理起点清单 + A/B/C 重分类框架 + 准入规则，
   且**不得含任何游戏/引擎语义结论**（那属于待重建的知识层）。
8. 报告里明确列出**本轮没做的事**（§0 的六条 ❌）与**下一步建议的前三个迁移批次**。
9. 不要 `git add`、不要 `git commit`（沿用旧仓的长期要求：由用户决定提交时机）。

---

## §5 开工前必须向用户确认的四件事

> **已定案、不要问**：旧仓全部知识资产（`analysis/` `tickets/` `docs-new/` 等）**一律只登记、不复制**，
> 因为它们需要清理 / 校验 / 重分类（§0、§1.10）。`docs-new/03-engine/` 里那几份"活文档"也在这个范围内。

1. **`apps/emulator` 与 `apps/inspector` 的代码怎么进新仓**：整体搬迁后再改结构，还是按新结构重写？
   （用户此前说"逐步重建"，含义需明确；这决定骨架里 `apps/*` 是占位还是迁移目标。）
2. **翻译数据模型要不要现在就定**：`data/`(日文基线) + `src/`(译文) 双份是否合并为
   **单份 + 机读修改点标记语法**（替代现在用注释承担、不稳定的做法）？这直接决定 `data/translations/` 的形态。
   用户的前瞻意向是"在 BIN 里同时嵌入中日文，让 emulator 切换"。
3. **重分类从哪一批开始**：知识层重建的第一批，是先做「A 类可机械再生成」的清点（低风险、纯机械推导），
   还是先做「B 类被可执行守卫验证」的抢救（高风险高价值）？要不要**先产出一份只读的候选分级清单**（不改旧仓）供用户逐条裁决？
4. **平台优先级**：win32 优先，还是要求 macOS 侧当天就能 clone + 跑？
   （决定本轮要不要顺带写 macOS 的 bootstrap 说明。）

---

## §6 后续迁移路线（不在本轮执行，供排序参考）

按依赖顺序，逐批交互式推进（每批都要可独立验证）。
**两条线并行、互不阻塞**：素材线（M*，搬工件）与知识线（K*，清理/校验/重分类旧知识）。

| 批次 | 内容 | 前置 |
|---|---|---|
| M1 | **只读语料入位（素材，非知识）**：产出 `corpus/disasm/disasm-20260930.zip`（sectfix 两个转 UTF-8 + AGERC 两个，共 4 文件，必须过 §1.8.1 的断言）+ 复制 fixtures 7 个；按 `.gitattributes` 的 LFS 规则显式 track 后提交。★ 两个 exe **不在本批**（后续再处理） | §2 骨架就位 |
| M2 | **格式层**：`packages/age-format`（搬 `scripts/{asm,alf,agf,uimap}`）+ 其守卫 | M1 |
| M3 | **台账层**：`packages/ledger` 的文本日志格式落地 + 派生 SQLite 构建 + "DB 可删可重建"守卫。**只落格式与守卫，不含任何旧条目** | §2 的 `data/ledger/` 草案定稿 |
| **K1** | **知识线 · 清理**（只读旧仓）：去沿革/修非法枚举（3 条 `ANALYZED`）/修主键冲突（2 组同 offset 双 `confirmed`）/统一 6 种字段记法/剔出 14 条字符串化 JSON。产出**候选分级清单**供用户裁决 | 用户裁决 §5.3 |
| **K2** | **知识线 · 校验**：锚点从行号重挂到 **EA**；重跑幸存守卫；**B 类抢救**（79 条 `modeled-verified` + 178 张带 `tests[]` 的票 + 74 条 `implemented`） | K1 + M4（守卫得能跑） |
| **K3** | **知识线 · 重分类**：按 A/B/C 落位，逐条过准入规则（必须绑定可再校验观察），只有通过者进新仓台账 | K2 |
| M4 | **模拟器搬迁**：`apps/emulator` + `packages/host-input` + `plugins/amayui-emulator`；把 10 个跨域守卫从模拟器 `test/` 里拆出来 | M2/M3 |
| M5 | **翻译域**：按确认后的数据模型搬 `data/`+`src/` | §5.2 定案 |
| M6 | **agent 基建 · 重建**：按新体系**从零重写**技能（必须落在固定的 `.agents/skills/`）与 DSH 插件（位置自由），并重新注册/安装 —— **不迁旧源码** | M4 |
| M7 | **真机探针**：`apps/inspector`（.NET 10，独立工具链文档化） | 无强依赖 |
| M8 | **旧仓收尾**：只补归档说明（tag 或 README 指向新仓），不删内容 | 全部迁移完成 |

> **顺序纪律**：K1 可以立刻开始（纯静态、只读旧仓）；但 **K2/K3 必须等 M4**——因为"被守卫验证过"这一类
> 要靠真正跑起来的测试来复核，而守卫现在还寄生在模拟器的 `test/` 里。**任何知识条目在 K3 通过前，不得进新仓台账。**
