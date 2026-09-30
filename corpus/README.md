# corpus/ — 只读素材

本目录是**素材的登记与消费层**：语料 / 二进制 / fixtures 从哪来、去哪、走哪种存储、怎么消费。
机器真源是 `assets.json`（lockfile 性质），人类叙述是本文件与各载荷目录的 `README.md`。

## 1. 目录分工

```text
corpus/
  assets.json      # ★ 素材清单（唯一的机器真源；唯一写入口是 tools/corpus.mjs）
  README.md        # 本文件：总则
  disasm/          # 反汇编语料：入库的是 zip，解压产物 gitignore
    README.md      #   4 文件清单 + 来源 + sha256 + 解压/转码/断言口径
    disasm-20260930.zip   ← M1 产出（LFS）
    files/         ← gitignore：解压出的 4 个文件，agent 直接读这里
  binaries/        # 脱壳件 / 节表修补件 —— 后续再处理（本轮为空，M1 也不含）
  fixtures/        # 玩家存档样本（旧仓 cache/）落点（本轮为空）
  game/            # 外部游戏目录 / 安装目录的**指针**（不搬数据）
```

## 2. `assets.json` 是什么、不是什么

| | 是什么 | 该放哪 |
|---|---|---|
| **约束 / 纪律** | "什么能进 git"、"语料不得改写"、"锚点锚 EA"、"禁 `.sqlite`" | `AGENTS.md` 的规则条目 + `tools/` 的守卫 |
| **素材清单** | 每个只读素材：从哪来、去哪、走哪种存储、怎么消费 | 本目录的 `assets.json` |
| **派生数据** | 大小、入库件校验和、LFS oid、解压产物、文件/条目计数 | **不写进来**（git / LFS / 文件系统已是权威） |

⇒ `assets.json` 是 **lockfile**（同 `package-lock.json` 的角色）：记录"来源与去向"，
**不记录规则**，也**不重复校验和**。**唯一能把它变成约束的是守卫**：`pnpm validate` 必须红。
**没有守卫的清单等于一份 Markdown。**

### 2.1 字段口径（详见 `../docs/00-origin/decisions.md` §7）

* `storage ∈ { lfs, git, external-only, deferred }` 是**存储去向**；
  `dest` 与它严格自洽（`external-only`/`deferred` ⇔ `dest === null`）。
* `origin[].sha256` **只写"不入库件"**（那是"转码 / 搬运之前它长这样"的唯一证据）；
  入库件的校验和交给 git / LFS。**目录型 origin 不写 sha256**（不可稳定复现）。
* `derivedFrom` 是**真前身**：这份东西是从哪个件、用什么工具产生的（`kind=disasm-corpus` 且入库时必填，
  且必须指向 `kind=binary` 的条目）。
* **刻意不写**：`bytes`、入库件 `sha256`、LFS oid、`status`、`generatedAt`，以及一切能从磁盘 / git 推导的计数。

### 2.2 唯一写入口

```bash
pnpm validate                    # 守卫（§3.3 的 9 条断言）；红了就是错，不是"看看"
pnpm corpus -- --scan            # 补 origin[].sha256（缺省 dry-run）
pnpm corpus -- --scan --write    # 落盘（写后回读 + 复验；不绿则回滚）
pnpm corpus -- --add '<entry>'   # 加条目
pnpm corpus -- --set <id> '<patch-json>'   # 改条目（如 M1 把 deferred 翻成 lfs）
```

★ **不要手工改 `assets.json` 的 `sha256`** —— 那是 `--scan` 的活；手工改会漂。

## 3. 按需迁移

* **大件一律不搬**：真游戏安装（7.7 GB）、外部素材（8.3 GB）、发行 zip（1.4 GB）留在仓库外，只用**路径登记**
  （必要时用符号链接引用，链接本身 gitignore）。
* **需要入库的只读语料 / 二进制 / fixtures** 走 LFS：先 `git lfs track` 声明（`.gitattributes` 已有按扩展名的规则），
  再提交；**解压产物 / 中间视图永远 gitignore**。
* **出仓带回来的东西先过 `.staging/`**（仓库内的中转区，gitignore）：它是**中转，不是"来源"**，
  所以 `roots` 里不写桌面路径，投递件的 sha256 仍记进清单以便复核。

## 4. 消费规则

1. `readOnly: true` 的件**禁止就地修改**；任何"解析友好化"只能是**派生的内存视图**，不得落回语料文件。
2. 反汇编语料：**锚点锚二进制 EA**，语料只提供 `EA → 行号` 映射（换反汇编只重建映射）。
3. `kind=knowledge-source` 的素材（旧仓 `analysis/` `tickets/` `docs-new/` 等）
   **禁止直接引用**：它们要过清理 / 校验 / 重分类（见 `../docs/00-origin/knowledge-rebuild.md`）。
4. `external-only` 的件不参与构建，也不进 `pnpm install` —— 它们只是**来源登记**。
