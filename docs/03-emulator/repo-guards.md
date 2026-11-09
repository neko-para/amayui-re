# 仓自身工具链 / 台账的合同（KEEP-T）

> 这份**不是** Win32 合同（那在 `contracts-win32.md`），也**不是**"跑游戏能看到的行为"（那是端到端）。
> 它记的是**这个仓库自己的机械装置**：素材清单、语料定位链、知识台账、shape/spec 的算术、写路径、插件闭接口。
> 判据：这些断言**变了就是 bug**，而且变了之后**没有人会看见** —— 台账会静默腐烂、清单会静默说谎、写路径会静默写坏数据。
> 所以它们配一条守卫；其余"我们测到过 X / 曾经有 bug Y / 为什么这么实现"一律进本目录的其它几份，**不进 test**。
>
> 逐条依据来自 `.tmp/test-audit/inventory.tsv` 的 `KEEP-T` 行；每个用例文件首行的 pragma（`@env/@kind/@why`）
> 就是它自己写下的"红了意味着什么"，本文件只做归类与去重。

## T-1 素材清单：**身份只由 sha256 与唯一写入口决定**

- **合同**：`corpus/assets.json` 里每条素材的身份由 **sha256** 钉住（`pnpm tools corpus scan --write` 是唯一写入口）；
  清单里**不许**出现 git / LFS / 文件系统已经是权威的那些字段（体积 / 入库件校验和 / LFS oid / `status` / `generatedAt`）、
  不许跟踪 `*.sqlite`；LFS 规则、忽略规则、同名说明书三者与清单一致。
- **兜它的用例**：`tools/test/corpus-manifest.test.mjs`（合成清单 #1–#9 的不变量本身）·
  `tools/test/corpus-manifest.assets.test.mjs`（真实清单全量：git 检查 / 哈希复核 / recipe）·
  `tools/test/corpus-assets.test.mjs`（LFS 规则 / 忽略规则 / 说明书约定）·
  `tools/test/corpus-cli-write.assets.test.mjs`（`--scan` / `--normalize` 写路径，含 Windows 上不许吐 CR）·
  `tools/test/age-sectfix-artifact.test.mjs`（入库拆壳件 sha256 = `.lst` 头部那份）·
  `tools/test/agerc-artifact.test.mjs`（AGERC 可信产物身份）· `tools/test/fonts.test.mjs`（字体链落点 / 版本）
- **失效后果**：执行源与"用来看的语料"分家（sectfix 那份 sha256 对不上 ⇒ 跑的不是分析的字节）；换机器红、或本机覆盖悄悄改写仓库事实。

## T-2 反汇编语料：**锚只能是指令行首，定位链不能"看起来对"**

- **合同**：`EA → 行号 → 所属函数` 这条链可机械重算；`.lst` 文法（行首地址、`@` 修饰名算函数头）
  与两侧配对（原型/定义）判据稳定；`EA` 落在函数体内时必须归**该**函数（不是后一个、不是前一个）。
- **兜它的用例**：`tools/test/disasm.test.mjs`（EA→行索引 + opcode 分派表提取）·
  `tools/test/disasm-extract.test.mjs`（合成语料上的段名 / 表体 / 连续性）·
  `tools/test/disasm-fn-format.test.mjs`（`.lst` 行与 `.c` 定义判定、两侧配对）·
  `tools/test/disasm-fn.assets.test.mjs`（真语料的 item 目录契约、覆盖率前提）·
  `tools/test/disasm-enclosing.assets.test.mjs`（"EA → 所属函数"）· `tools/test/manifest-local.test.mjs`（本机私有覆盖）
- **失效后果**：静默给出"看起来对"的行区间与参数偏移；`EA` 指到指令中间 / 指到数据行，而一切仍然绿。

## T-3 知识台账：**每条结论都绑一个可解析的锚**

- **合同**：`data/ledger/` 的每条记录必须带可解析锚（`ea` / `bin` / 守卫用例 id）与合法分类轴；
  `bin` 锚必须带 `len` + `sha256`（否则"这条观察没被换掉"只剩一句声称）；DB 永远可由文本真源确定性重建；
  覆盖度查询算的"锚属于哪个函数 / 收没收口"必须与台账一致。
- **兜它的用例**：`tools/test/ledger.test.mjs`（schema / 锚点 / 分类轴 / DB 可重建）·
  `tools/test/ledger-reference-bin-digest.test.mjs`（`repo=reference` 的 bin 锚必须带摘要）·
  `tools/test/ledger.anchor-lst.assets.test.mjs`（锚必须是 `.lst` 行首、不许落在指令中间）·
  `tools/test/ledger-coverage.test.mjs`（覆盖度 / 前沿归属）· `tools/test/ledger.pe.external.test.mjs`（EA→文件偏移的 VA/RVA 口径）
- **失效后果**：把没分析过的说成收口、把前沿指错人 —— 而这是"下一步该做什么"的唯一机械依据。

## T-4 shape：**"判不了"不许被读成"已覆盖"**

- **合同**：claims 校验器（覆盖 / 步长 / 边界 / 下钻 / 两道守卫）、普查口径（分桶闭合 / 偏移聚类 / 过滤器）、
  欠账三档（已声明 / `count:null` / 未声明）、段落划分（init 开段、类别切换、caller-saved）各自稳定；
  `lea reg,[一跳基址-K]` 的负位移**不许**被折叠成"二级偏移"（那是假缺口）。
- **兜它的用例**：`tools/test/shape-claims.assets.test.mjs`（claims 校验器的义务）·
  `shape-census.assets.test.mjs`（全表普查口径）· `shape-census-debt.test.mjs`（欠账三档）·
  `shape-access.test.mjs`（负位移不许当二级偏移）· `shape-regions.test.mjs`（段落划分）·
  `shape-coverage.test.mjs`（归档算术）· `shape-need.test.mjs`（按需补齐的提案）·
  `shape-bodies.assets.test.mjs`（handler 体骨架）
- **失效后果**：覆盖率把"判不了"读成"已覆盖"，下一轮补的是**不存在的成员**；或把有直接观察的数组判成"条数无依据"（假红）。

## T-5 spec：**未知不许被洗成省略**

- **合同**：规范自包含；章节规则与 `bindings` / `audit` 一一对应；未登记义务 = 0；
  证据锚指向的用例必须**真的列出**该 rule（"用例存在"≠"用例覆盖该 rule"）；
  代码注释里引用的规范条目 id 必须存在（否则"注释说有这条规范"会被当证据）；残差地图两轴与明细同源。
- **兜它的用例**：`tools/test/spec.test.mjs`（不变量本身）· `spec-corpus.test.mjs`（effect 枚举 / 覆盖与语料）·
  `spec-format-table.test.mjs`（指令表形态 / 待核验状态）· `spec-gaps.test.mjs`（未成篇义务）·
  `spec-isa.assets.test.mjs` · `spec-agf.assets.test.mjs` · `spec-alf.assets.test.mjs` ·
  `spec-pool.assets.test.mjs` · `spec-save-sth.assets.test.mjs`（协议章与真实语料/样本）
  · `spec-mechanisms.test.mjs` · `spec-rule-refs.test.mjs`（注释引用的规范 id 必须存在）·
  `spec-residual-footprint.test.mjs` · `spec-residuals.test.mjs`（残差两轴）
- **失效后果**：未知被洗成"明确舍弃"、未成篇义务变回"缺口 0"的假象；读者按错的字节结构写解码器。
- **不在这一档的**：站点/绑定类（`spec-*-sites`、`spec-evidence-covers-rule`、`spec-containers-*`）在审计里是 `MERGE`/`DOC` ——
  它们校验的东西（EA 必须对上 `.lst` 指令行首）**属于 §T-2 那条合同**，合并后由那一处的判据承担。

## T-6 写路径：**要么落盘、要么红，不许写坏数据**

- **合同**：`fixtures` / `patch` / `requirements` / `release` / `translation-ref` 的写入是原子的、可重入的、
  与只读路径分开；报告 / 台账的落点**必须真在**（说落盘就得查得到）；样本台账与它自己的判据不许脱节；
  发布链不许在缺浏览器时偷偷改走"重烧"，也不许把"这次烧出来的"当成进包字节。
- **兜它的用例**：`tools/test/fixtures.test.mjs` · `fixtures-cli-write.test.mjs` · `patch.test.mjs` ·
  `requirements.test.mjs` · `release.test.mjs` · `release-agf-source.test.mjs` · `translation-ref.test.mjs` ·
  `tools/test/mutate-journal.test.mjs`（变异被中断后工作树自愈；否则下一次"跑绿"可能是假绿）
- **失效后果**：真实存档 / 译文 / 需求单被写到别的文件或写坏；半坏的工作树留在磁盘上而没人知道。

## T-7 工具链自描述与分层：**真源指针不许指空**

- **合同**：`pnpm tools <域> <动作>` 的域地图由各工具自我声明派生（有工具消失 / 转发坏 ⇒ 红）；
  分层不许被破坏（agent 基建不 import 业务、冻结件不 import 分析产物）；每个 `*.test.mjs` 首行必须有合法
  `@env/@kind/@why` pragma；每个自有 JSON 旁必须有同名 `.md` 且指向 `--describe`；
  覆盖清单与 C++ 注册表不许漂；`pl_mpeg` 三处副本不许漂。
- **兜它的用例**：`tools/test/cli.test.mjs` · `layering.test.mjs`（含"分级 pragma 必须存在"）· `json-docs.test.mjs` ·
  `skills.test.mjs`（技能里引用的 `pnpm tools ...` 必须真存在）· `routes.test.mjs` · `platform.test.mjs` ·
  `lift.test.mjs` · `inventory.test.mjs` · `opcodes.test.mjs` · `ui-bake.test.mjs` ·
  `age-format-types.test.mjs` · `emulator-cxx-pl-mpeg-sync.test.mjs`
- **失效后果**：技能与文档指向不存在的动作；同一部影片在三条实现线上解出不同像素而三边测试都绿。

## T-8 插件（agent 基建）：**op 是闭集合，写类默认 dry-run**

- **合同**：`plugins/` 的每个 op 是闭集合、argv 数组不经 shell、参数受校验、写类 op 缺 `write:true` 只出计划；
  提权插件不许变成"能跑任意命令"；`wait_subagent` 的等待 / 插话 / 竞态语义稳定。
- **兜它的用例**：`plugins/pnpm-priv/test/ops.test.mjs` · `plugins/pnpm-priv/test/plugin.test.mjs` ·
  `plugins/deploy/test/ops.test.mjs` · `plugins/deploy/test/plugin.test.mjs` ·
  `plugins/sandbox-nomic/test/confine.test.mjs` · `plugins/sandbox-nomic/test/provider.test.mjs` ·
  `plugins/wait-subagent/test/wait.test.mjs` · `plugins/wait-subagent/test/plugin.test.mjs`
- **失效后果**：宿主替调用方真的跑构建 / 起窗口 / 越界写仓库外的路径，而调用方以为只是"出了个计划"。

## T-9 附：**spec 的证据锚**（`apps/emulator-cxx` 的 CTest）

它们不在上面那批 JS 文件的账里，但在 inventory 里同属 `KEEP-T`：这批 CTest **就是**
`data/spec/bindings.json` 与已 `done` 需求单的 `verify:` 指向的证据锚（**逐条名单以 inventory 为准，这里不抄数**）。
⛔ 不是一个"没用就删"的集合：按 `AGENTS.md` §11，动它们 = **单独一条迁移单**（先把证据锚迁到
`apps/emulator-lift` 的顶层 CTest，才能谈退役 `apps/emulator-cxx`）。

## 去向（已落地：`pnpm verify`）

**这批用例已经从 `pnpm test` 移到 `pnpm verify`**：职责分开 —— `pnpm test` 只留对外边界合同 / 基建契约，
`pnpm verify` 承担"仓库自己的机械装置还成立吗"。判据**仍然写在每个文件首行的 pragma 里**（`@env` 决定"这台机器上有没有意义"，
`@suite` 决定"属哪一档"），**不建清单文件**。怎么跑：

```bash
pnpm test        # 合同档：@env pure ∧ @suite contract（快、常跑）
pnpm verify      # 工具链档：@env pure ∧ @suite toolchain（台账 / 写路径 / 知识层）
pnpm verify:all  # 工具链档的**全部**：再加 @env assets / external 那些文件（缺旧仓 / 游戏安装时会 SKIP，如实读输出）
pnpm test:list   # 两档各选了哪些文件（带 ·toolchain 标记的就是本页 T-1…T-9 点名的这批；少数换档例外见下）
```

- **两档之和 = 改动前的默认档**：两档都只跑 `@env pure`，`@suite` 恰好互补 ⇒ **没有用例从默认门禁里消失**；
  `@env assets` / `external` 的分级判据**一字未动**，仍由 `pnpm test:assets` / `pnpm test:all` 承担
  （要连工具链档的这两部分一起跑：`pnpm verify:all`）。
- **`layering.test.mjs` 在合同档**：它守的是"**测试套件自己是否卫生**"（pragma 合法性 + `t.diagnostic` 裸 `return`），
  是**套件的元守卫** ⇒ 必须每次 `pnpm test` 都跑；上面 T-7 点到它，点的是"它守的那条分层"，不是"它自己该在工具链档"。
- **两个写路径边界用例在工具链档**：`fixtures-edit.test.mjs` / `requirements-add-lands.test.mjs` 测的是
  **台账的写路径会不会写坏**（它属于本页 T-6 那类"仓库自己的机械装置"），不是对外边界合同 ⇒ 跟 T-6 同档。
- **两档都必须绿**（口径已登记进 `AGENTS.md` §5「测试分级」）。
- 声明格式：`/** @env <env> @kind <kind> [@suite <contract|toolchain>] @why <一句话> */`；`@suite` **省略 = `contract`**。
- 选中集合的实现是 `tools/test-run.mjs`（`--suite`）；它**仍然扫全部 `*.test.mjs`** 校验 pragma，
  声明不齐时**两档都红**；分层 / pragma / 测试卫生这三条守卫本身也只看文件内容，与档位无关。
