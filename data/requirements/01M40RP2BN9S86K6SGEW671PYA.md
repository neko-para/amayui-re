# 发行打包（release 落点 + CHANGELOG 随包；AGF / AGERC 链缺失）

- id: REQ-01M40RP2BN9S86K6SGEW671PYA
- type: req
- status: doing
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 50
- tags: [translation, release]

## 范围
把 `data/translations/patch.json` + 字体 + 面向用户的文本，打成**发给玩家的那一包**（一个 zip），落到
`dist/patch/<版本>.zip`；同一批字节还能铺成**可运行测试树** `dist/install/`。

## 已实施（一条命令一条链）
* **`pnpm tools release pack --write`** ⇒ `dist/patch/<版本>.zip`（+ 同名 `.manifest.json`：每件指回真源 + sha256）。
  变更集 = `patch.json` 的**键**（BIN，逐支复核 `resultSha`）· ui-bake 的**配方集合**（AGF，取 `dist/ui-bake/` 的产物）·
  `corpus/assets/agerc/AGERC.DLL` · 发行字体 · `release/CHANGELOG.md` + `release/安装说明.md`。
  ★ **不存同步清单**：旧仓 `patch/patch.config.json` 那种第二真源不再存在。
* **`pnpm tools release install --write`** ⇒ `dist/install/`（= 《安装说明》第 1–5 步做成一条命令；
  `*.ALF` 走**硬链接**，其余复制 —— 代价是那 13 个 ALF 与游戏本体同 inode）。
* **自检（红 = 退出码 1 且一个字都不写）**：缺件 / 与 patch 的键集不一致 / 与 `resultSha` 不符 /
  CHANGELOG 没有当前版本节；打包后**回读逐条目复验**、同输入同字节。
* **文档**：`release/README.md`（形状 / 每件的真源 / 缺口）· `tools/release.md`（落点 / 禁令 / 踩过的坑）。

## 已完成
* 落点定了（三份文本入库、产物入 `dist/`，不进 LFS）· CHANGELOG 已迁回 · 有意不迁 `patch.config.json`。
* **对账（迁移期实测）**：本仓重建出来的 453 支 BIN 与旧仓 `install/` **逐字节相同**（453/453）；
  其中 451 支也与旧仓 `patch/BIN` 逐字节相同 —— 差的 2 支正是那份手维护清单漏掉的
  （`$1$SCINIT.BIN` / `$1$SCJUMP.BIN` 没进旧包，`$3$DPINIT.BIN` / `$5$AMINIT2.BIN` 多出来）。

## 缺（**留空并记录**）
1. `SO001` 的 AGF 层重放残差：属 ui-bake 链，归 `REQ-01M42S9QSMTPCYTHDEDCHWX3R5`；打包只如实搬运它的产物。
2. **AGF 产物新鲜度没有守卫**：`pack` 只在清单里记 sha256，不判断 `dist/ui-bake/` 那批图是否按**当前**配方烧的
   （确认方式：重跑 `pnpm tools ui-bake build` 再 `pack`；`build` 确定性，重跑产物逐字节相同）。
3. AGERC 自建链：见 `REQ-01M411CSDXDVGTP83BPS6TNV48`；本版仍用入库的可信产物。

## 判据
* `tools/test/release.test.mjs`：条目表**只由真源算出来**（键集 / AGF 配方集 / 字体集合 / 版本节）·
  形状不变量 · 守卫能红（缺件 / 无版本节 ⇒ 退出 1 且不留残件）· 同输入同字节 · 安装树的硬链接与幂等。
* 出包前的查看路径：`pnpm tools release plan pack`（不落盘）→ 绿了再 `--write`。
