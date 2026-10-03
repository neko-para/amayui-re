# 04 · agent 基建（技能 + DSH 插件）

> ★ 本目录下**没有**任何从旧仓复制的知识文档 —— **有意为之**（§1.10）。
> 旧仓的 `.agents/skills/`(8) 与 `plugins/`(5) **都不搬**，只登记为 `rebuild`：重写时读得到旧实现。

## 1. 来源与处置（旧仓实测）

| 旧仓位置 | 实测 | 处置 |
|---|---|---|
| `.agents/skills/` | **8** 个技能（立项原文写 10；全盘只有 8 个 `SKILL.md`，另 2 个在旧仓 `.tmp/` 里） | **不迁移**，内容从零重写 |
| `plugins/` | **5** 个 DSH 插件（`amayui-emulator` / `amayui-inspector` / `htmlcard` / `ticket-board` / `uimap`） | **不迁移**，从零重建 + 重新注册 |

用户口径：**几乎所有技能与工具都需要重新适配** ⇒ 全部按 `rebuild` 处理，只留"重写时读得到旧实现"的指针
（`corpus/assets.json` 的 `agent/skills` 与 `agent/plugins`）。

## 2. 两条落点规则必须分开

| | 落点 | 原因 |
|---|---|---|
| **技能** | **固定 `.agents/skills/<名字>/SKILL.md`** —— 仓库根，**不可改名 / 移位** | DSH 按特定路径发现技能 ⇒ 这是**硬要求**。新仓里该目录必须存在，但内容从零重写 |
| **DSH 插件** | 位置自由（本仓用 `plugins/` 只作**源码落点**） | 插件是注册到 DSH 的**软链接**，本来也要重新安装 ⇒ 不为它保留固定顶层目录 |

⇒ **不要为了迎合注册方式去扭曲仓库结构**：插件的注册是**环境级动作**；技能则必须遵守固定路径。

## 3. 怎么注册（环境级动作，步骤也写在 `AGENTS.md` §7）

* **技能**：DSH 按 `.agents/skills/<名字>/SKILL.md` 固定路径发现，**无需注册**。
* **插件**：通过 DSH 的插件安装机制以软链接注册 ——
  **确切命令留待 M6 重建第一个插件时补进 `AGENTS.md` §7**（标 `TBD-M6`）；
  本仓**不凭记忆编命令**（编错了比空着更坏）。

## 4. 批次

见 `../00-origin/knowledge-rebuild.md` §3 的路线表：**M6 = agent 基建 · 重建**（按新体系从零重写技能与插件并重新注册），
前置是 M4（域边界要先稳定，否则技能会再次把跨域关注点焊在一起）。

## 5. 本轮明确不做

* ❌ 不迁任何插件源码（`plugins/` 下**只有说明性 README**）；
* ❌ 不注册 / 安装任何东西。

## 6. 已重建的第一个技能

`.agents/skills/amayui-translate/` —— **翻译 / 更新译文**（批 M6 · agent 基建重建的第一件，
提前于整批落地：它是翻译域数据模型（patch 叠加层）定案后的直接消费方）。

* 它是**重写**，不是迁移：机制完全按新模型（`pnpm tools patch view/edit/verify`），
  旧仓 `amayui-script-translate` / `amayui-script-update` 只作"当时怎么约定"的参照
  （纯约定被继承进 `references/conventions.md`，与旧仓工具/路径绑死的部分**全部丢弃**）。
* 技能消费的参考资产是 `docs/01-translation/ref/`（只读快照）—— 技能里写死了
  「旧文档的结论与计数一律先核实再用」，见 `docs/01-translation/README.md` §5。
* **其余技能仍按 `rebuild` 处理**：`.agents/skills/` 下除这一个之外不放内容，直到各自的前置稳定。
