# tools/release.md — `tools/release.mjs`（发行打包控制脚本）的**落点说明**

> 它**不是**工具台账；这里只写"它动哪片数据、为什么这么动、踩过哪些坑"。
> 命令与自描述：**`pnpm tools release describe`**（字段 / 形状 / 不变量 / 操作的真源，本文件不复述）。
> ★ 会随干活而变的数（条目数 / 体积 / sha256）**不写在这里**：那是命令输出与 `dist/` 里的清单回答的。

## 1. 它把"变更集"铺成两种形状

| 命令 | 铺出什么 | 旧仓的对应物 |
|---|---|---|
| `pnpm tools release install --write` | **可运行的测试安装树** `dist/install/`（+ `dist/install-manifest.json`） | 旧仓 `install/` |
| `pnpm tools release pack --write` | **发给玩家的一个 zip** `dist/patch/<版本>.zip`（+ 同名 `.manifest.json`） | 旧仓 `patch/` |

两者**输入完全相同**（同一个变更集），差的只是形状 ⇒ "装进测试树的"与"发给玩家的"必然是同一批字节。
这正是旧仓最容易漂的地方：`patch/patch.config.json` 是一份**手维护**的清单，实测它的键与 `patch.json` 对不齐
（少 `$1$SCINIT.BIN` / `$1$SCJUMP.BIN`、多 `$3$DPINIT.BIN` / `$5$AMINIT2.BIN`）——
所以本工具**不存同步清单**，清单是算出来的。

## 2. 变更集 = 三处真源

| 件 | 集合从哪来 | 字节从哪来 |
|---|---|---|
| `*.BIN` | `data/translations/patch.json` 的**键** | 基线（`gameInstall` 的散装 / ALF）+ patch **重建**，逐支复核 `resultSha` |
| `*.AGF` | `tools/ui-bake/recipes/*.json` 的**配方集合** | `pnpm tools ui-bake build` 的产物 `dist/ui-bake/<块>.AGF` |
| `AGERC.DLL` | 清单条目 `binary/agerc-dist` | 入库的可信产物 `corpus/assets/agerc/AGERC.DLL` |

另有两件**只进发行包、不进测试树**：`corpus/assets/fonts/Amayui-CN_cnjp{,-Bold}.ttf`（字体是装进系统的，
见 `release/安装说明.md` 第 6 步）与 `release/CHANGELOG.md` · `release/安装说明.md`（随包文本）。

★ **为什么 AGF 不由本工具烧**：烧图要 headless Chrome（字形光栅化在 Chrome 里，换光栅化器就换像素），
那是 `tools/ui-bake.mjs` 的活；本工具**只消费它的产物**，缺产物就报"先跑哪条命令"，不自己凑一张图。

## 3. 测试安装树 = 《安装说明》第 1–5 步做成一条命令

```text
<gameInstall> 的顶层文件 ──┬─ *.ALF  ⇒ 硬链接（7.4 GiB 只读大件，不占额外空间）
                           └─ 其余   ⇒ 复制（并保住来源 mtime）
        + 变更集覆盖件（BIN / AGF / AGERC.DLL，真文件，**永不**硬链接）
        − 排除项（天结.exe / AGE-EXTEND.TTF / *.dmp / 本机脱壳分析产物 —— 每条的理由见源码 INSTALL_EXCLUDES）
```

* **子目录不复制**：测试树是**平的**（与旧仓 `install/` 同形）——`_analysis/` `_extracted/` `补丁/` 都不进树。
* ★ **硬链接 = 同一个 inode ⇒ 别往测试树里写东西**：往 `dist/install/DATA1.ALF` 写一个字节，
  就是往**游戏本体**写（也会顺带毁掉本体的只读基线）。本工具只把"我们自己的产物"写成真文件。
* ★ 副作用如实说：建硬链接会**改到源文件的链接数**（元数据，不动字节）⇒ 本体那些 ALF 的 `nlink` 会变成 2。
  这也意味着这一步**不是纯读操作**：受限沙箱 / 只读 ACL 下会被拒（见 §4 第 1 条）。
* **就地同步**：落点里若有本工具写的清单 ⇒ 按清单**清掉上一轮的过时覆盖件**；
  名字若回到基础树里（例如某支脚本这一版没有译文了），会**重铺成基础件**，不留旧译文。
  落点存在但**没有**本工具的清单 ⇒ 缺 `--force` 就拒绝（"我知道里面是什么"是人的判断，不是工具的猜测）。
* **可选：LE 启动器**（`--le-cmd <LEProc.exe> [--le-profile <guid>]`）⇒ 树里多一份 `启动游戏-LE.cmd`
  （旧仓 `install/` 里就有这么一份）。内容全 ASCII（cmd 按 ANSI 解码，中文 `rem` 会变乱码），
  关键是 `cd /d "%~dp0"` + `"%~dp0AGE.EXE"`：**引擎按当前目录找自己的件**，而从 shell 菜单起时当前目录未必是游戏目录。
* **可选：`--relabel-medium`** —— 见 §3.1。

### 3.1 ★ 完整性标签（Windows 特有，实测踩过）

**现象**：用 Locale Emulator 起这棵树 ⇒ **进程起来了但窗口没建出来**；直接双击 AGE.EXE 却能看到窗口（只是没转码）。

**成因（两段，都是实测）**：

1. **标签从哪来**：DSH ≥ 0.2 的 Windows ACL 沙箱（`@deepseek-ai/dsh-sandbox-windows-acl`，本机 0.2.0-rc.2）
   在授权工作区时**一次调用同时**下三样东西：能力 SID 的允许 ACE、对 world 去掉环境性 `FILE_DELETE_CHILD` 的拒绝 ACE、
   以及 **Low 禁止上调标签**。它自己的 README 写明这些改动**按设计常驻**（"工作区的可继承标签在会话结束后依然存在"），
   并且**明确承认**"Low 标签的可执行文件会影响用户在 DSH 外启动程序"。⇒ 这解释了"以前 0.1.x 没这毛病"。
   ★ 本工具只是**在那类会话里跑**，它自己只写字节、不改 ACL。
2. **标签为什么能让游戏起不来**：**Windows 让"从带 Low 标签的 EXE 起的进程"本身就是 Low 完整性**
   （实测 `whoami /groups` = `S-1-16-4096`；把标签改成 Medium 后同一个 exe 起出来就是 `S-1-16-8192`）。
   Low 完整性的游戏：写自己的存档目录（`%LOCALAPPDATA%\Eushully\…`，Medium）会被拒，
   **Locale Emulator 的注入 / 注册表重定向那一步会失败** ⇒ 正是上面的现象。

**处置（推荐 ①；②有代价，实测过）**：

| | 做法 | 代价 |
|---|---|---|
| ① **建到受限工作区之外**（推荐） | `pnpm tools release install --write --out E:\Projects\amayui-test-install` | 无（同卷 ⇒ ALF 硬链接照旧）；只是落点不在仓库里 |
| ② 就地改标签（快） | `pnpm tools release install --write --relabel-medium`（= `icacls "<树>" /setintegritylevel Medium /T /C`） | ★ 改完这棵子树就**落在沙箱可写范围之外**了（受限子进程只能写带 Low 标签的根，实测：改完往 `dist/install` 写文件 = Access denied）⇒ 以后在沙箱会话里重建这里要么提权、要么重建后再改一次标签 |

★ 工作区根上的可继承 Low 标签**不会**因为改子树而消失 ⇒ **在工作区里重建，标签会再回来**（这正是把 `--relabel-medium` 留在命令里的理由）。
★ 本工具会**查一次并如实告警**（`icacls` 跑不起来时也告警，不当成"没问题"）。

## 4. 碰过的坑（别改回去）

1. **硬链接可能被环境拒绝**（`EPERM` / `EXDEV`）：它改了源文件的链接数，**不是纯读**。
   本工具因此在落盘**之前先探一次**（一次 link + unlink，只动元数据），拿到两种成因就当场说清怎么处置：
   不同卷 ⇒ 把树放到游戏所在的盘，或 `--alf copy`；权限不足 ⇒ 用 `--alf copy`，或在能写游戏目录的环境里跑。
   ★ 失败时**不留半棵树**：这一轮新建的整棵撤掉（否则会留下一棵"有一半基础件、又没有清单"的假树）。
2. **mtime 只能保住到毫秒**：`utimesSync` 收的是 `Date`，而 `statSync().mtimeMs` 带亚毫秒小数
   ⇒ 复制件会比源**大不到 1 ms**。所以"要不要重拷"的判据是**大小 + mtime（±2 ms）**，
   拿 `===` 判会让幂等性当场失效（每次重跑都重拷一遍）。
3. **"先铺基础件、再写覆盖件"要跳过覆盖件那几名**：否则每次重跑都白拷一遍那些
   "基础树里也有、又正好是变更件"的文件（散装 BIN 就是这一类）。
4. **过时覆盖件的名字若回到基础树里**：不能一删了事 —— 得让基础树重铺一遍（删了就少一个游戏件）。
5. **zip 必须先写临时件、读回复验过了再改名**：否则一次回读失败会把上一版 zip 毁掉。
   打包的确定性由 `tools/lib/zip.mjs` 保证（固定时间戳 + 固定条目序 + 无 `generatedAt`）。
6. **zip 名由 `release/CHANGELOG.md` 的版本节决定**：`## v1.14（开发中）` ⇒ `v1.14-dev.zip`；
   `## v1.13（2026-09-01）` ⇒ `v1.13-260901.zip`（与旧仓那批 `vX.Y-YYMMDD.zip` 一脉相承）。
   没有版本节 / 标注既不是日期也不是「开发中」⇒ **不出包**（脱节的包比没有包更糟）。
7. **别用 mtime 判"要不要重拷"**（见第 2 条），也别拿"大小相同"当同一份 —— 判据是**大小 + mtime（±2 ms）**。
8. **测试树能不能被 Locale Emulator 起来，是环境问题**（§3.1 的 Low 完整性标签）：它**不是**内容问题，
   所以"双击能开、LE 开不了"这类现象出现时先查标签，别去改数据。

## 5. 与谁相邻（边界）

| | 归谁 |
|---|---|
| `patch.json` 的读写 | `pnpm tools patch`（本工具**只读**它） |
| AGF 的生成与像素判据 | `pnpm tools ui-bake`（本工具**只读**它的产物） |
| 字体 / AGERC 的入库与来源 | `pnpm tools corpus`（本工具**只读**入库件） |
| 包的形状 / 随包文本规格 / 缺口 | `release/README.md`（本文件不复述） |
| 产物落点 | `dist/`（生成物区，`.gitignore` 命中）——**产物一律不入库** |

## 6. 怎么查 / 怎么改

```bash
pnpm tools release describe          # ★ 先看：变更集 / 形状 / 不变量 / 操作
pnpm tools release plan install      # 测试树计划（不落盘）
pnpm tools release plan pack         # 发行包计划（不落盘）
pnpm tools release install --write   # → dist/install/ + dist/install-manifest.json
pnpm tools release install --write --le-cmd "E:\Downloads\Locale.Emulator.2.5.0.1\LEProc.exe" \
        --le-profile <guid>          # 顺带写一份 启动游戏-LE.cmd（cd /d "%~dp0" 再调 LE）
pnpm tools release install --write --relabel-medium   # 受限工作区里建树后把 Low 完整性标签改回 Medium（§3.1）
pnpm tools release pack    --write   # → dist/patch/<版本>.zip + 同名 .manifest.json
pnpm test                            # 基建契约：tools/test/release.test.mjs
```

* ❌ **不要手改** `dist/` 里的清单（生成物，重跑即覆盖）；
* ❌ **不要手工拼包**（手工拼出来的包没有可复现性，出问题分不清是源错了还是手抖了）；
* ❌ **不要在测试树里改东西**（ALF 与本体同 inode；要改就改真源再重跑本命令）；
* ❌ 不要把本工具的产物当来源（`dist/install` 与 zip 都是**下游**，真源永远在上表那三处）；
* ✅ 缺 AGF ⇒ 先 `pnpm tools ui-bake build`（要提权；判据见 `tools/ui-bake.md` §5.1），再 `release pack`。
