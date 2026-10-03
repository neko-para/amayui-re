# 发行打包（release 落点 + CHANGELOG 随包；AGF / AGERC 链缺失）

- id: REQ-01M40RP2BN9S86K6SGEW671PYA
- type: req
- status: open
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 50
- tags: [translation, release]

## 范围
把 `data/translations/patch.json` + 字体 + 面向用户的文本，打成**发给玩家的那一包**，
落到 `dist/patch/<版本>/`（生成物、不入库）。设计（含缺口与"有意不迁"的清单）见 `release/README.md`。

## 已完成
* **落点定了**：入库的只有三份文本 —— `release/README.md`（设计 / 配方 / CHANGELOG 规格）·
  `release/CHANGELOG.md`（★ 随包发给玩家）· `release/安装说明.md`（随包）；
  产物一律 `dist/patch/<版本>/`（`.gitignore` 的 `dist/` 已命中）。
* **CHANGELOG 已迁回**：旧仓 `patch/CHANGELOG.md` 的 15 个版本节逐字迁入（v1.14 为「开发中」节）。
  ★ 它是**唯一允许手写的状态类文件** —— 因为它是**产品的一部分**，不是内部变更记录（内部沿革仍只有 `git log`）。
* **有意不迁 `patch/patch.config.json`**（30 KB）：BIN 清单可由 `patch.json` 的键派生 ⇒ 再存一份就是第二真源。

## 缺（**留空并记录** —— 不许在缺口上"先对付一下"）
1. **AGF 烘焙链**：已汉化 PNG（`corpus/assets/ui-images/`，已入库）→ **有头注入** AGF。
   旧仓走 `scripts/agf/` 的 Node 版或 `tools/Eushully_AGF_TooL`，本仓没有。
   进包清单旧仓是 **10 个**：`SO001` `SO002` `SO009A` `SO009B` `SO017` `SO020` `SO021` `SO025` `SO030` `SO039`。
   ★ 不要直接拿旧仓 `patch/AGF/` —— 那是**产物**不是来源。
2. **AGERC.DLL 构建链**（`rc → 编译 → 注入`）：归子节点 `REQ-01M3SVH5F3VJ1ENS6BN3090X8M`（windows 资源重建）。
3. **打包动作本身**：`patch.json → BIN` 的能力已在（`pnpm tools patch`），
   缺的是"按发行包形状铺开 + 自检"那一步。

## 判据
* 一条命令能从**文本真源**（`patch.json` + 入库 PNG / 字体 + rc 系列）打出完整的包，
  且在干净目录里**可复现**（同输入同字节）；
* 包里每一件都能指回它的真源；三处缺口要么补齐、要么在包说明里**显式声明"本版不含"**；
* 打包自检**能红**：缺件 / 多出件 / 与 `patch.json` 的键集不一致 / CHANGELOG 里没有当前版本节
  ⇒ 失败，而不是出个残包。

## 非目标
* 不发版、不打 tag、不建 CI（提交与发布时机由用户决定）；
* **不把产物入库**（含 LFS）：产物可由文本真源重建 ⇒ 入库只会制造"你覆盖我"。
