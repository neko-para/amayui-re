# amayui-re

《天結いキャッスルマイスター》(Amayui Castle Meister, Eushully) 的**逆向工程 / 重实现 / 汉化** monorepo。

本仓是**重建**出来的新环境，取代旧的单堆仓库（`E:\Games\Eushully\天結`，下称**旧仓**）。
旧仓保持原样、可归档、**只读**；新仓分域、只登记指向，历史知识**待清理 / 校验 / 重分类**后再逐批进来。

---

## 1. 这个仓库要交付什么

| 交付 | 落点 | 说明 |
|---|---|---|
| **翻译** | `data/translations/` | 日文只读基线 + 译文（数据模型待定，见 `docs/01-translation/`） |
| **AGE 二进制格式** | `packages/age-format/` | ALF / AGF / ASM 等容器格式；★ **AGE 脚本**的解析 / 组装 / 文本 **reflow** 也在这里（`src/asm/`） |
| **台账** | `packages/ledger/` + `data/ledger/` | append-only 文本真源 + 可删可重建的派生 SQLite 查询层 |
| **需求台账** | `data/requirements/` | ★ **高层次的进度视图**：需求/缺陷 + 父子树（`pnpm tools requirements plan`）；与业务不耦合 |
| **模拟器** | `apps/emulator/` | 按建模语义**重新实现**；`.ts` 无构建步骤（见"语言口径"） |
| **真机探针** | `apps/inspector/` | .NET 10 / C#，独立工具链 |
| **跨域工具** | `tools/` | 素材清单守卫、语料转码、旧仓盘点（**纯 `.mjs`**） |
| **只读素材** | `corpus/` | 反汇编语料 / 二进制 / fixtures 的**登记 + 消费规则**（大件不入 git） |

长期工作流（数千次增量分析）：

```text
mismatch → 稳定实体 target → 自动 context packet（已有事实 + 原始观察 + 局部代码 + runtime evidence + 显式冲突）
         → 局部分析 → 人工裁决 → 单写者更新知识与实现
```

## 2. 域地图（新旧落点对照）

| 域 | 旧仓位置 | 新仓落点 | 本轮状态 |
|---|---|---|---|
| 翻译 | `data/` `src/` `res/fonts` `patch/` | `data/translations/` | 只登记 |
| AGE 格式 | `scripts/{asm,alf,agf,uimap}` | `packages/age-format/` | 只登记（M2 搬）；★ AGE 脚本的解析 / 组装 / **reflow** 同在此域（`src/asm/`） |
| 模拟器 | `app/amayui-emulator` + `native/host-input` + `plugins/amayui-emulator` | `apps/emulator/` + `packages/host-input/` | **按新结构重写**（M4） |
| 真机探针 | `app/amayui-inspector` | `apps/inspector/` | **按新结构重写**（M7） |
| wiki | `app/amayui-toolkit` | **不迁移**（archive 在旧仓） | 只登记 |
| 知识台账 | `analysis/` `tickets/` `docs-new/` | **不带**（见 `docs/00-origin/knowledge-rebuild.md`） | 待清理 / 校验 / 重分类 |
| agent 基建 | `.agents/skills/` `plugins/` | `.agents/skills/`（固定路径）+ `plugins/` | **不迁移、从零重建**（M6） |
| 素材 / 语料 | `engine/` `install/` `raw/` `raw-parts/` `cache/` `tools/` | `corpus/`（只登记；需要入库的走 LFS） | 入库与否看 `corpus/assets.json` 的 `storage`/`dest`（`pnpm tools corpus list`） |

## 3. 怎么跑

```bash
pnpm install                 # workspace 链接 + 根上两个 devDependencies（typescript 只用于 pnpm typecheck）
pnpm tools corpus validate                # ★ 素材清单守卫：corpus/assets.json 必须绿（这是"约束"所在）
pnpm test                    # ★ 测试（默认只跑 @env pure；`pnpm test:list` 看分级，`pnpm test:all` 跑全量）
pnpm test:assets             # 只跑要 LFS / 游戏安装的那档（控制类内容稳定 ⇒ 不值得每次跑）
pnpm tools corpus scan        # 补 origin[].sha256（缺省 dry-run，加 --write 落盘）
pnpm tools old-repo inventory               # 重新实测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm tools disasm verify      # 反汇编语料保真断言（原件不在时由 zip 反解 + 清单 sha256 自证）
pnpm tools disasm restore     # 由 zip 反解回投递原件（.staging/ 可随时丢）
```

**语言口径**：真源是 **`AGENTS.md` §3** —— TypeScript 只出现在两类落点（自带工具链的 app：`apps/emulator`、
`apps/workbench`；以及 `packages/age-format` 的 `.mts` 一份真源），仓库内**其余一切 JS 一律 `.mjs`**；
`.NET`(C#) 与 `native`(C++/CMake) 各自独立工具链，不在此列。★ 这里**不复述**细则（复述必漂）。
根上只有 `typescript` + `@types/node` 两个 devDependencies，且**只用于 `pnpm typecheck`（`noEmit`）**。

## 4. 从哪读起

| 想了解 | 读 |
|---|---|
| **迁移到哪一步了 / 下一步做什么** | **`pnpm tools requirements plan`**（需求树 = 唯一的进度真源；原 `PLAN.md` 的批次表已并入其「仓库迁移与重建」一支，该文件已删除） |
| 需求/缺陷怎么登记、父子怎么归、两类流程差在哪 | `data/requirements/README.md` + `pnpm tools requirements describe` |
| 为什么重建、有哪些**原创决策**（存储纪律 / 跨平台 / 已定口径） | `docs/00-origin/decisions.md` |
| 知识层怎么重建（清理起点清单 + A/B/C 分级 + 准入规则） | `docs/00-origin/knowledge-rebuild.md` |
| 立项原文（本轮提示词） | `docs/00-origin/init-prompt.md` |
| 旧仓实测盘点（给迁移轮按图索骥） | `docs/00-origin/old-repo-inventory.md` |
| 只读素材从哪来、怎么消费、走哪种存储 | `corpus/assets.json` + `corpus/README.md` |
| 反汇编语料（4 文件 zip）的来源 / 哈希 / 转码口径 | `corpus/disasm/README.md` |
| 环境与权限须知（硬纪律 / 跨平台 / 怎么注册 agent 基建） | `AGENTS.md` |

> ★ **本仓的知识层目前是空的，这是有意为之**：旧仓的全部知识资产（`analysis/` `tickets/` `docs-new/` …）
> 都待清理 / 校验 / 重分类，本轮**一个文件都不复制、一个字都不摘抄**，只在 `corpus/assets.json` 里登记指向。
> 因此 `docs/` 下**没有**任何从旧仓复制来的文档；每一份都是本轮新写的。
