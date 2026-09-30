# PLAN.md — 迁移进度（多轮执行用）

> **本文件只记批次级进度**：哪些批次、什么状态、卡在哪、下一步从哪进。
> ❌ **不写细节**（文件名 / 数量 / 哈希 / 命令输出 / 设计理由）—— 那些要么**已经在仓库里**，要么**在旧仓**
> （见 `corpus/assets.json` 的登记）。
> ★ 这是本仓**唯一允许手写状态**的文件（`docs/00-origin/decisions.md` §6 的例外之一）⇒ 它必须保持**批次级**。
> 真源：入库进度 = 清单的 `storage`/`dest`（`pnpm tools corpus list`）·语料保真 = `pnpm tools disasm verify`·
> 旧仓数字 = `pnpm tools old-repo inventory`·变更历史 = `git log`。
> 状态取值只有四种：`未开始` / `进行中` / `阻塞` / `已完成`。**每轮结束只改状态**（必要时改"下一步"），不动结构。

## 素材线（M）— 搬工件

| # | 内容 | 状态 | 依赖 | 入口 |
|---|---|---|---|---|
| M0 | 骨架与纪律（目录 / 根配置 / 清单守卫 / 旧仓盘点） | 已完成 | — | `AGENTS.md`、`docs/00-origin/` |
| M1 | 只读语料入位（反汇编语料 + 存档样本） | 已完成 | M0 | `corpus/README.md`、`pnpm tools disasm verify` |
| M1b | 纯资源第二批（脱壳件与节表修补件、UI 图片、字体、补丁产物） | 未开始 | M1 | `pnpm tools corpus list`（登记为 deferred 的那些） |
| M2 | 格式层（AGE 容器格式：ALF / AGF / ASM / uimap） | 未开始 | M1 | `packages/age-format/README.md` |
| M3 | 台账层（append-only 文本格式 + 派生查询层 + "可删可重建"守卫） | 未开始 | M1 | `packages/ledger/README.md`、`data/ledger/README.md` |
| M4 | 模拟器与原生输入（**按新结构重写**；把跨域守卫拆回各域） | 未开始 | M2 / M3 | `apps/emulator/README.md`、`packages/host-input/README.md` |
| M5 | 翻译域（按定案后的数据模型） | 未开始 | 数据模型定案 | `docs/01-translation/README.md` |
| M6 | agent 基建重建（技能 + DSH 插件；顺带评估工具是否上 DSH） | 未开始 | M4 | `docs/04-agent/README.md`、`AGENTS.md` §7 |
| M7 | 真机探针（.NET 10，**按新结构重写**） | 未开始 | — | `apps/inspector/README.md` |
| M8 | 旧仓收尾（只补归档说明，不删内容） | 未开始 | 全部 | `docs/00-origin/old-repo-inventory.md` |

## 知识线（K）— 清理 / 校验 / 重分类

| # | 内容 | 状态 | 依赖 | 入口 |
|---|---|---|---|---|
| K1 | 清理（只读旧仓）+ 产出候选分级清单供裁决 | 未开始 | 用户裁决 | `docs/00-origin/knowledge-rebuild.md` |
| K2 | 校验（锚点重挂 EA + 重跑幸存守卫 + B 类抢救） | 阻塞 | K1 + M4 | 同上 |
| K3 | 重分类（A/B/C 落位，逐条过准入规则） | 未开始 | K2 | 同上 |

## 下一步

素材线：**M1b 或 M2**（二选一，见上表入口）。知识线：按用户口径**不启动**（K2 另需等 M4）。
