# release/ —— 发行物（**发给玩家的那一包**）

> 本目录只放**面向用户的文本**与**打包配方**；**打出来的包不入库**（生成物，见 §2）。
> 缺口如实留空并记录（§5）—— 不在缺口上"先对付一下"，那会把"缺一条链"变成"有一条看起来能用的链"。

## 1. 为什么单开一处（旧仓 `patch/` 的问题）

旧仓的 `patch/` **同时**装了三种东西：① 要发给玩家的文本（`CHANGELOG.md`、`README-测试版说明.md`）、
② 构建产物（`BIN/` 400+ 支、`AGF/` 10 张、`AGERC.DLL`、字体）、③ 一份同步清单（`patch.config.json`）。
三者混在一处 ⇒ 分不清"哪个是来源、哪个是产物、哪个是第二真源"。

本仓按介质分开：**文本真源入库**，**产物进 `dist/`**，**清单能派生就不存**。

## 2. 落点

| 件 | 入库 | 说明 |
|---|---|---|
| `release/README.md` | ✅ | 本文件：设计 / 配方 / 缺口 |
| `release/CHANGELOG.md` | ✅ | ★ **随包发给玩家的更新记录**（规格见 §4；它决定 zip 叫什么版本） |
| `release/安装说明.md` | ✅ | **随包发给玩家的安装步骤**（逐字迁自旧仓 `patch/README-测试版说明.md`） |
| `dist/patch/<版本>.zip` | ❌ | 打出来的包（`BIN/` `AGF/` `AGERC.DLL` 字体 两份文本），**一个 zip**；旁边落一份同名 `.manifest.json`（每一件指回真源） |
| `dist/install/` | ❌ | **测试安装树**（旧仓 `install/` 的等价物：同一批字节 + 游戏本体，ALF 走硬链接） |

★ **名字为什么不叫 `patch/`**：`pnpm tools patch` 与 `data/translations/patch.json` 已经占用了 "patch" 这个词，
而那是**变更叠加层**（数据，不是发行物）。两个意思混用，会让每条命令、每句文档都要先猜是哪个。
★ **生成物一律不入库**（含 LFS）：它们可由文本真源重建 ⇒ 入库只会制造"你覆盖我"。
★ 打出来的东西**长什么样**由 `pnpm tools release describe` 给（形状 / 顺序 / 文件名），本文件不复述。

## 3. 包里有什么 · 每件的来源

> **集合 / 顺序 / 文件名**的机器真源是 `pnpm tools release describe`（**不要**在这里再抄一份清单：
> 抄的那一刻起就注定与真源漂）。本节只讲"每件的真源与为什么"，以及**有意不放**的东西。

| 包内件 | 来源（真源） | 为什么是它 |
|---|---|---|
| `BIN\`（已汉化脚本） | `data/translations/patch.json` 的**键** + 基线（`gameInstall` 的散装 / ALF） | 范围与"有没有变更"由 patch 说了算（旧仓那份手维护的 `patch.config.json` 实测与它对不齐） |
| `AGF\`（已汉化 UI 图） | `tools/ui-bake/recipes/*.json` 的**配方集合** → `pnpm tools ui-bake build` 的产物 | 配方就是"我们改过哪些图"的真源；烧图归 ui-bake（要 Chrome 的光栅化） |
| `AGERC.DLL`（主菜单汉化） | 入库的可信产物 `corpus/assets/agerc/AGERC.DLL`（条目 `binary/agerc-dist`） | 本版直接用它（旧仓随包发布的那一份，sha256 由 `tools/test/agerc-artifact.test.mjs` 复核）；**自建链待重建**（§5.2） |
| `Amayui-CN_cnjp{,-Bold}.ttf` | `corpus/assets/fonts/`（条目 `assets/fonts-dist`） | 现成入库件，直接复制；**两份都要装**（族名同、按 Regular/Bold 配对） |
| `安装说明.md` · `CHANGELOG.md` | 本目录 | 逐字节复制（不是生成物）；CHANGELOG 的版本节决定 zip 名 |

**有意不放**：`AGE-EXTEND.TTF`（要玩家自己移出，见安装说明第 2 步）· Sarasa 上游基底（那是构建链的输入，
不是成品）· 旧仓 `patch/patch.config.json`（第二真源，见 §6）。

## 4. CHANGELOG 规格（★ 唯一真源；技能只引用、不复述）

**这是唯一允许手写的"状态类"文件** —— 因为它**是产品的一部分**（随包发布给玩家），
不是仓库的内部变更记录。内部沿革仍然只有一处：`git log`。

* **版本节**：新版本在前。当前开发版写作 `## vX.Y（开发中）`（**不带日期**），发布后补上发布日期
  （`## vX.Y（YYYY-MM-DD）`）；若不存在「开发中」节（刚发过版），**先新建** `## vX.(Y+1)（开发中）`，
  不许直接往已发布的节里加。
* **追加位置**：新条目加到最新一个「开发中」节的**最上方**（最新在前），不从下方追加。
* **条目间必须空一行**（每条 bullet 独占一段，不得粘连）。
* **条目形态**：`- [类型][脚本] 改动说明`
  * **类型**：`新翻译` / `修改` / `术语统一` / `文档` / `发布`；
  * **脚本**：BIN 脚本名（如 `SC0560`、`SKINIT`、`SG5744`），多个用「、」；纯文档条目可省；
  * **改动说明**：写清「旧 → 新」或行为变化，**连同理由**（旧条目的价值正在理由里）。
  * ❌ **不写仓库内的东西**：仓库路径（`docs/…`、`data/…`、`.tmp/…`）、需求节点 id、工具命令、
    文件名/MD5/像素坐标这类工程细节 —— 玩家读不懂；"当时依据了什么"的真源是 `git log`。
* ❌ **不许记录技术校验信息**：重建字节数、`sha256`、工具调用细节、回读比 —— 那些属工程内部校验。
* ❌ **不许把 `git log` 的提交信息抄进来**：CHANGELOG 面向玩家，一条 = 一次对玩家可见的改动。

## 5. 缺口（**留空 + 记录**）

### 5.1 AGF 烘焙链 —— **已交付；残差归 ui-bake 的节点**

* 链在 **`pnpm tools ui-bake`**：原始 ALF/AGF（只读）→ 配方（`tools/ui-bake/recipes/*.json`）→ headless 渲染 → 合成 →
  **注回 AGF**；产物落 `dist/ui-bake/`（`build`），进包集合 = **有配方的那些块**。
* **判据是像素**（`pnpm tools ui-bake verify` / `build` / `agf`）：与 `corpus/assets/ui-images/` 的生效版逐像素、
  并与来源件解码后比对。★ 烧图要 headless Chrome ⇒ 这一步在受限沙箱里要提权（判据见 `tools/ui-bake.md` §5.1）。
* **进包清单不在这里维护**：`release pack` 按配方集合取，缺哪张就报"先跑 `ui-bake build`"。
* **已知残差**：`SO001` 的 AGF 层重放有偏差 —— 那是 ui-bake 链上的**未收口缺陷**
  （需求节点 `REQ-01M42S9QSMTPCYTHDEDCHWX3R5`），**不是**打包链的问题；打包只如实搬运它的产物。
* **为什么不直接拿旧仓 `patch/AGF/`**：那是**产物**不是来源（来源是原始 ALF + 配方），且两边字节必然不同
  （8bpp 调色板领域只认解码后的像素）。

### 5.2 AGERC.DLL —— **本版用「可信产物」；自建链待重建**

**已定案（2026-10-03）**：菜单使用面小 + 16 个对话框基本没汉化（只 `DIALOG 3`）⇒ 自建链收益有限，
**先把旧仓随补丁发布的那一份入库当可信产物直接进包**：

* **件**：`corpus/assets/agerc/AGERC.DLL`（= 旧仓 `patch/AGERC.DLL`，848,896 B，sha256 `6241de66…`）；
  清单条目 `binary/agerc-dist`（`storage: lfs` · `readOnly` · `derivedFrom: binary/agerc-debug-unpacked`）。
* **"可信"是可机械复核的**：`tools/test/agerc-artifact.test.mjs` 的基准**不是另写一份 sha**，
  而是清单里 `binary/agerc-modified-install` 已记的那个（**一处真源**），并在旧仓在机时逐字节比对。
* **待重建**：`rc → 编译 → 注入` 的链（还要摆脱 Resource Hacker 这条 Windows GUI 依赖）
  ⇒ 归需求节点 **「AGERC.DLL 二进制重建（把可信产物换成自建链）」**；
  评估与判据见 `docs/01-translation/agerc-design.md`（含"两条链叠加"的实测与六方案对比）。
* 旧仓 `res/` 的 rc 系列与三条 AGERC 二进制**仍只登记**（`corpus/assets.json` 的 `binary/agerc-*`）；
  注意 `binary/agerc-packed`（带壳根目录那份）**不可作可编辑素材** —— 它的资源字节不在盘上。

### 5.3 打包动作 —— **已交付**

* **一条命令**：`pnpm tools release pack --write` ⇒ `dist/patch/<版本>.zip`（+ 同名 `.manifest.json`）。
  形状 / 变更集从哪来 / 不变量见 `pnpm tools release describe` 与 `tools/release.md`。
* **另一条命令**：`pnpm tools release install --write` ⇒ `dist/install/`（可运行测试树 = 《安装说明》第 1–5 步；
  ALF 走硬链接 —— 7.4 GiB 不占额外空间，代价是那 13 个 ALF 与本体**同一个 inode**）。
* **自检**（都写成了守卫，不是"看一眼"）：缺件 / 与 `patch.json` 的键集不一致 / 与 `resultSha` 不符 /
  CHANGELOG 没有当前版本节 ⇒ 退出码 1 且**一个文件都不落**；打包后**回读逐条目复验**、同输入同字节。
* **仍未收口的两件事**（各自有节点，不阻塞出包）：① §5.1 的 `SO001` 残差；
  ② **AGF 产物新鲜度没有守卫** —— `pack` 只在清单里记 sha256，不判断 `dist/ui-bake/` 里那批图是不是按**当前**配方烧的
  （要确认就重跑 `pnpm tools ui-bake build` 再 `pack`；`build` 是确定性的，重跑产物逐字节相同）。
* **测试树与 Locale Emulator**（两条环境口径，都会伪装成"包坏了"）：
  ① 树里的 `启动游戏-LE.cmd`（`--le-cmd` 生成）负责 `cd /d "%~dp0"` —— 引擎按当前目录找自己的件，
     从 shell 菜单起时当前目录未必是游戏目录；
  ② **完整性标签**：受限沙箱工作区里建出来的树会带 `Mandatory Label\Low`，
     而**从带 Low 标签的 EXE 起的进程本身就是 Low 完整性** ⇒ LE 那一步失败（现象：**进程起来了、窗口没建出来**；
     直接双击却能看到窗口只是没转码）。处置：`--relabel-medium` 或建到受限工作区之外。详见 `tools/release.md` §3.1。

## 6. 有意不迁的东西

| 旧仓件 | 为什么不迁 |
|---|---|
| `patch/patch.config.json`（30 KB） | BIN 清单**可由 `data/translations/patch.json` 的键派生** ⇒ 再存一份就是**第二真源**。（它那 400 条与 patch 的 453 条键本身就对不齐，正是"同一份信息两处写"的样本。） |
| `patch/BIN` · `patch/AGF` · 字体 | 都是**产物**；来源在本仓（patch.json / UI PNG / `corpus/assets/fonts/`） |
| `patch/AGERC.DLL` | ★ **只取了这一个** —— 它是本版要用的**可信产物**（入库为 `corpus/assets/agerc/AGERC.DLL`）；其余 `patch/` 内容仍不迁 |
| `install-manifest.json` · `raw-manifest.json` | 旧仓那两份是"安装树 / 解包树自检"用的清单。本仓的测试树清单一律由 `pnpm tools release install` **现写**（生成物、不存第二份）；发行包不需要它们 |

## 7. 怎么查 / 怎么改

* **要发一版**：`pnpm tools release plan pack` 先看计划（缺件 / 键集 / 版本节）⇒ 绿了再 `--write` 出包；
  要试玩就 `pnpm tools release install --write` 铺出测试树。形状与不变量看 `pnpm tools release describe`。
* **要发一版之前**：先看需求树（`pnpm tools requirements plan`）里「发行打包」那条节点，以及 §5 里还挂着的东西。
* **改 CHANGELOG**：按 §4 规格加到「开发中」节最上方；改完不要动历史版本节（zip 名由这一节决定）。
* **看某一版包是怎么来的**：`dist/patch/<版本>.manifest.json`（每一件指回真源 + sha256）·
  `git log`（配置与配方的沿革）· 本条目的 §3 表（每件的真源）。
* ★ 历史条目里的 `关联：docs/translate/...` 是**迁移前的旧路径**（现在在 `docs/01-translation/ref/` 下）；
  **历史条目是记录，不回改**。
