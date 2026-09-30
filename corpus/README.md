# corpus/ — 只读素材

本目录是**素材的登记与消费层**：语料 / 二进制 / fixtures 从哪来、去哪、走哪种存储、怎么消费。
机器真源是 `assets.json`（lockfile 性质），人类叙述是本文件与各载荷目录的 `README.md`。

## 1. 目录分工

```text
corpus/
  assets.json      # ★ 素材清单（唯一的机器真源；唯一写入口是 tools/corpus.mjs）
  README.md        # 本文件：总则
  disasm/          # 反汇编语料：入库的是 zip，解压产物 gitignore
    README.md      #   4 文件清单 + 来源 + sha256 + 转写规则 + 断言口径
    disasm-20260930.zip   ← 入库（LFS）
    files/         ← gitignore：解压出的 4 个文件，agent 直接读这里
  binaries/        # 脱壳件 / 节表修补件 —— 后续再处理（尚未入库）
  fixtures/        # 真存档样本（槽 76/77/78/79）+ samples.json（结构化描述）+ README
  game/            # 外部游戏目录 / 安装目录的**指针**（不搬数据）
```

**两个载荷条目的分工**（"入库没有"一律看清单的 `storage`/`dest`，**不要**在文档里记进度）：

| 条目 | 落点 | 存储 | 守卫 |
|---|---|---|---|
| `disasm/bundle` | `corpus/disasm/disasm-20260930.zip`（4 个 UTF-8 文件） | LFS（`*.zip`） | `recipe` 真跑 9 条保真断言（#7）；原件缺席时由 zip 反解 + 清单 sha256 自证 |
| `fixtures/save-samples` | `corpus/fixtures/SAVE{76,77,78,79}.{DAT,STH}` | LFS（`*.DAT`/`*.STH`） | dest 下**每个已跟踪文件**的 `filter=lfs`（#6，不靠 origin 枚举）；文件级事实（槽定位 / mtime）另见 `corpus/fixtures/samples.json` |

★ **只有"有加工链的入库件"才配来源记录**（`disasm/bundle` ↔ `disasm/raw-source-*`，`external-only` + 逐件 sha256）
—— 那是"转码前的忠实参照"这个能力的来源：**来源 ≠ 入库件**时才谈得上复核。
`fixtures/save-samples` **不配**：它是**自足条目**（固化资源，没有加工链、没有可再取的上游，入库的那一份就是原件），
登记 origin 等于把 dest 抄第二遍。口径见 `pnpm tools corpus describe` 的「自足条目」。

## 2. `assets.json` 是什么、不是什么

⇒ **`assets.json` 是 lockfile**（同 `package-lock.json` 的角色）：记录"来源与去向"，
**不记录规则**，也**不重复校验和**。**唯一能把它变成约束的是守卫**：`pnpm tools corpus validate` 必须红
—— **没有守卫的清单等于一份 Markdown**。

★ **它的字段字典、9 条不变量、以及"怎么查 / 怎么改"全部聚合在它旁边那份同名说明书里：
[`assets.md`](./assets.md)** —— 本文件不重复那些内容（约定见 `../docs/00-origin/decisions.md` §6）。

一句话版：`storage ∈ { lfs, git, external-only, deferred }`，`dest` 与它严格自洽；
`origin[].sha256` **只写不入库件**；**唯一写入口是 `pnpm corpus`**，别手改 JSON。

## 3. 按需迁移

* **大件一律不搬**：真游戏安装（7.7 GB）、外部素材（8.3 GB）、发行 zip（1.4 GB）留在仓库外，只用**路径登记**
  （必要时用符号链接引用，链接本身 gitignore）。
* **需要入库的只读语料 / 二进制 / fixtures** 走 LFS：先 `git lfs track` 声明（`.gitattributes` 已有按扩展名的规则），
  再提交；**解压产物 / 中间视图永远 gitignore**。
* **出仓带回来的东西先过 `.staging/`**（仓库内的中转区，gitignore）：它是**中转，不是"来源"**，
  所以 `roots` 里不写桌面路径，投递件的 sha256 仍记进清单以便复核。
* ★ **`.staging/` 可以随时丢弃**：它只是中转。原件缺失时，`disasm/bundle` 的断言会自动切到
  "由 zip 反解回原件 + 用清单里的 sha256 自证"（`pnpm tools disasm restore` 也能把原件写回来），
  所以 fresh clone（`.staging/` 天生为空）上 `pnpm tools corpus validate` 一样是绿的。

## 4. 消费规则

1. `readOnly: true` 的件**禁止就地修改**；任何"解析友好化"只能是**派生的内存视图**，不得落回语料文件。
2. 反汇编语料：**锚点锚二进制 EA**，语料只提供 `EA → 行号` 映射（换反汇编只重建映射）。
3. `kind=knowledge-source` 的素材（旧仓 `analysis/` `tickets/` `docs-new/` 等）
   **禁止直接引用**：它们要过清理 / 校验 / 重分类（见 `../docs/00-origin/knowledge-rebuild.md`）。
4. `external-only` 的件不参与构建，也不进 `pnpm install` —— 它们只是**来源登记**。
