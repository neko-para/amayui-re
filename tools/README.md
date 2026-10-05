# tools/ — 跨域 CLI（**从零写，不迁旧脚本**）

> 语言口径：本目录**一律 `.mjs`**（无构建步骤、无 `tsconfig`）—— 只有 `apps/emulator` 用 TypeScript。
> 旧仓的工具（台账生成器 / 校验器 / `.tmp/` 里那些一次性脚本）**一律按"重建"处理**，只登记不迁移。

## 0. 分层：**纯工具 → 领域模型 → CLI**（依赖方向单向）

```
tools/
  cli.mjs            # 派发器：域地图 + 薄转发（认识各 CLI，但不实现任何规则）
  corpus.mjs fixtures.mjs disasm-recode.mjs old-repo-inventory.mjs   # CLI：参数 → 模型 → 输出
  lib/
    paths.mjs fsx.mjs exec.mjs zip.mjs time.mjs cp932.mjs   # ★ 纯工具：不认识任何领域数据
    manifest.mjs samples.mjs requirements.mjs                # ★ 领域模型：schema / 不变量 / 读 / 写 / 自描述
    bin-source.mjs cn-jp.mjs patch.mjs                       # ★ 领域模型：BIN 来源解析 / 中文↔BIN 映射 / patch
    release.mjs ui-bake/                                     # ★ 领域模型：发行打包（变更集 → 测试树 / zip）/ UI 烘焙链
  test/*.test.mjs    # 基建契约测试
```

**为什么这么分**（用户口径）：*"为了方便在交叉 import/export；纯粹的工具（例如驱动 git）应该作为独立工具 mjs 提供，而非从业务 mjs 中导出。"*
三层各自的边界：

| 层 | 认什么 | 不认什么 | 例 |
|---|---|---|---|
| **纯工具** `lib/{paths,fsx,exec,zip,time,cp932}` | 路径 / Buffer / 子进程 / 时间 / 编码 | **任何"素材 / 槽 / 语料"概念** | `exec.mjs` 的 `runCapture()`——驱动 git 的纯工具，谁都能用 |
| **领域模型** `lib/{manifest,samples}` | 自己那份数据的 schema、不变量、读写、自描述 | **argv、打印** | `saveManifest()`（清单唯一写入口）；`saveSamples()` |
| **CLI** `tools/*.mjs` | 参数解析、打印、dry-run 计划 | 规则本身（都在模型里） | `corpus.mjs --validate` |

两条硬规则（由 `test/layering.test.mjs` 守）：

1. **`lib/**` 不得 import `tools/*.mjs`** —— 模型不许依赖 CLI（`fixtures` 要用清单模型，import 的是 `lib/manifest.mjs`，**不是** `corpus.mjs`）。
2. **CLI 之间不得互相 import** —— 只有派发器 `cli.mjs` 认识各 CLI；纯工具不得反向依赖领域模型。

★ 判据一句话：**"这个函数认识'素材/槽/语料'吗？"** 不认识 ⇒ 进 `lib/` 的纯工具；认识但只是"数据怎么读怎么写" ⇒ 进领域模型；只有"怎么从命令行调、怎么打印" ⇒ 留 CLI。

## 0.1 入口只有两个（`package.json` 不堆命令）

```bash
pnpm tools          # ① 域地图：域 → 数据 → 读写 → 操作（由各工具的自我声明**派生**）
pnpm test           # ② 全仓测试（不属于任何域）
```

* **域命令一律经派发器**：`pnpm tools <域> <动作> [args…]` —— 位置参数与 flag 直接跟在后面（**不必 `--`**）。
* `pnpm tools <域>` 看该域详情（数据 / 不变量 / 操作）；`pnpm tools --json` 给机器读。
* ★ 为什么要这样：命令多了以后，**扁平的 `scripts` 回答不了"这个脚本动哪片数据"**。
  现在每个工具自己声明 `DOMAIN`（动哪片数据 / 读写方式）与 `OPERATIONS`（有哪些动作、会不会写），
  地图由声明派生 ⇒ **新增工具只改它自己**，`package.json` 不用动，也**漏不进地图**（`test/cli.test.mjs` 守）。
* ★ 契约（`decisions.md` §7）：`cli.mjs` **只转发，不实现任何规则**；写操作永远落在各工具自己的写入口里。

## 1. 现有工具

| 工具 | 域 | 作用 |
|---|---|---|
| `cli.mjs` | —— | 入口：地图 + 薄转发（**不含任何业务规则**） |
| `corpus.mjs` | `corpus` | ★ `corpus/assets.json` 的守卫 + 唯一写入口（schema/不变量见 `pnpm tools corpus describe`） |
| `fixtures.mjs` | `fixtures` | ★ `corpus/fixtures/samples.json` 的查询 + 唯一编辑入口（见 `pnpm tools fixtures describe`） |
| `requirements.mjs` | `requirements` | ★ `data/requirements/` 的进度视图 + 唯一编辑入口（需求/缺陷 + 父子树；见 `pnpm tools requirements describe`） |
| `disasm-recode.mjs` | `disasm` | 反汇编语料的**无损转写**与保真断言（见 `pnpm tools disasm describe`） |
| `patch.mjs` | `patch` | ★ 翻译 patch 的查询 / **提取（唯一写入口）** / 复验 / **视图（`data`·`src`）与全库检索**（见 `pnpm tools patch describe` 与 `tools/patch.md`） |
| `ui-bake.mjs` | `ui-bake` | ★ **UI 图片烘焙链**：原始 ALF → 改图配方（`tools/ui-bake/recipes/*.json`）→ 生效版 PNG / 注回 AGF；判据是**逐像素相同**（见 `pnpm tools ui-bake describe` 与 `tools/ui-bake.md`） |
| `release.mjs` | `release` | ★ **发行打包**：把「当前所有变更过的资源」（patch 的键 + ui-bake 的配方集 + AGERC/字体）算成**测试安装树**（ALF 硬链接）或**发给玩家的 zip**（见 `pnpm tools release describe` 与 `tools/release.md`） |
| `old-repo-inventory.mjs` | `old-repo` | **只读旧仓**：重新实测 → `docs/00-origin/old-repo-inventory.md`；重建翻译参考快照 → `docs/01-translation/ref/`（资产散件 + `archive.zip`） |
| `test/*.test.mjs` | —— | 基建契约测试（`node --test`） |

★ **每个自有的结构化数据文件都有一份同名说明书**（`assets.json` → `assets.md`、`samples.json` → `samples.md`）：
"它是什么 / 怎么查 / 怎么改"全在那一份里，本文件不重复（约定见 `../docs/00-origin/decisions.md` §6，由 `test/json-docs.test.mjs` 守）。
需求台账是**多文件**结构（一个节点一个 `.md`），它的散文说明在同目录 `data/requirements/README.md`（同样只写口径，schema 指向 `--describe`）。

## 2. 常用命令（都经派发器；每个工具也都能独立 `node tools/xxx.mjs …` 跑）

```bash
pnpm tools corpus validate                  # 跑 9 条不变量（全仓门禁）
pnpm tools corpus list                      # 素材条目一览（+ --json）
pnpm tools corpus scan --write              # 补 origin[].sha256（唯一写入口；缺省 dry-run）
pnpm tools corpus set <id> '<patch-json>' --write   # 改条目（写前内存预验；写后回读复验，不绿回滚）
pnpm tools fixtures list                    # 存档样本：槽 / 定位 / 每个文件是否与记录的 instant 一致
pnpm tools requirements plan                # ★ 进度视图：按父子树打印 + 聚合状态（唯一的进度真源）
pnpm tools requirements serve               # 本地网页（需求 + AGE 脚本）：http://127.0.0.1:7788/
pnpm tools requirements validate             # 需求台账的 5 条不变量（红 = 退出码 1）
pnpm tools fixtures restore-mtime --write   # 刚 clone：把 mtime 按记录的 instant 拨回去（跨时区也对）
pnpm tools disasm verify                     # 语料保真断言（原件在就按原件；不在就由 zip 反解 + 清单 sha256 自证）
pnpm tools patch describe                    # ★ 翻译 patch：字段 / 不变量 / 操作（schema 的唯一真源）
pnpm tools patch verify                      # ★ 判据：基线 + patch ⇒ **逐字节**相同（缺产物根时只对 resultSha 自证）
pnpm tools patch extract                     # 从旧仓产物提取（dry-run；--write 才落盘，写前逐条自证）
pnpm tools patch index --write                # ★ 建**基线索引**（名单 + 逐支指纹 + codec 指纹 ⇒ 永不陈旧）
pnpm tools patch view                        # 建/刷新 **base 文本**（缺省只写 data；941 支 ≈ 3.3 s，顺手写索引）
pnpm tools patch find 'ヘタレ'                # ★ 检索：日文查 base、中文查 op 载荷，按锚配对（不物化投影）
pnpm tools patch find '赫塔' --edits e.txt --to '废柴'   # ★ 生成编辑清单（锚寻址；只写清单文件）
pnpm tools patch set --edits e.txt --write    # ★ 按锚直改 op ⇒ 一次写盘进 patch（缺省 dry-run）
pnpm tools patch view --kind src --name <脚本> # 只有要用编辑器整篇改时才物化这一支的 src 草稿
pnpm tools patch edit --name <脚本> --write    # …改完反解（来源对不上会拒绝；折行重排走这条）
pnpm tools disasm build                     # 转写落盘 + 打确定性 zip（需要 .staging/ 里的原件）
pnpm tools disasm restore                   # 由 zip 反解回投递原件（默认写回 .staging/）
pnpm tools old-repo inventory               # 重测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm tools old-repo translate-ref           # 翻译参考快照对账（dry-run；--write 重建 assets/ + archive.zip）
pnpm tools release plan install             # ★ 发行打包：测试安装树的计划（不落盘）
pnpm tools release install --write          # 同步出测试安装树 → dist/install/（ALF 硬链接，其余复制）
pnpm tools release install --write --le-cmd "<LEProc.exe>" [--le-profile <guid>] [--relabel-medium]
                                            #   顺带写 启动游戏-LE.cmd / 把 Low 完整性标签改回 Medium（见 tools/release.md §3.1）
pnpm tools release pack    --write          # 打包给玩家 → dist/patch/<版本>.zip（+ 同名自检清单）
```

> ★ **两个环境口径**（都在代码里，不靠人记）：
> ① **不捕获子进程输出**：`stdio: 'pipe'` 在受限沙箱里要开命名管道 ⇒ `spawn EPERM`。
>    `corpus.mjs` 的 `runCapture()` 把 stdout 重定向到**文件描述符**（临时文件在**系统临时区**，
>    因此调用方那侧一个字节都不会被写），拿到同一份 git/recipe 答案而不开管道；
>    命令真的跑不起来时按 **warning** 报出（不静默放过）；**而"盘点"类工具宁可失败也不写错文件**。
> ② **测试跑在单进程里**：`--test-isolation=none` —— 默认隔离模式由 runner 起子进程并走管道，同样会 EPERM；
>    in-process 还更快。测试内部也**刻意不捕获子进程输出**（能直接调的就直接调）。

## 3. 守卫的 9 条断言在哪

**在控制脚本的自描述里**：`pnpm tools corpus describe`（标题真源是 `corpus.mjs` 的 `CHECK_TITLES`，
`--validate` 与 `--describe` 取同一份 ⇒ 不会两处漂移）。本文件不重复。

## 4. 为什么要有这些工具（而不是手写脚本）

* 旧仓 `.tmp/` 里有 **724 个 `.mjs` + 431 个 `.py`** 一次性脚本 —— 那是"没有工具层"的代价：
  每次都临时写、写完就废、结论无处沉淀。
* 因此新仓的工具是**长期资产**：有 `--help` / `--describe`、有退出码、有测试、有确定的输出格式；
  **不写进 `tools/` 的脚本，就不该被反复用第二次**。
* 这条纪律**立刻兑现过一次**：`corpus/fixtures/samples.json` 起初是用一次性命令生成的，
  结果里面的 `mtimeMs` 整整差了 8 小时（而同一个命令算出的墙上时间是对的，所以没有任何断言发现它）。
  改成 `fixtures.mjs` 之后第一次重取就把这个错纠了出来 —— 因为工具用的是文件系统给的 instant。
