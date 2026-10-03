# AGENTS.md — 环境与权限须知

> 本文件是新仓的"环境层"约定（对应**旧仓同名文件**的角色）。
> ★ **规则只有可执行才算规则**：每条纪律要么落在 `.gitattributes` / `.gitignore` / `pnpm` 脚本里，
> 要么落在 `tools/` 的守卫里。写成散文的 `policy: "..."` 机器执行不了、agent 也不会去读 —— 那是伪装成数据的文档。
> 每条纪律的"为什么"见 `docs/00-origin/decisions.md`；本文件只写"怎么做 / 不许怎么做"。

---

## 0. 三条最常被踩的

1. **旧仓 `E:\Games\Eushully\天結` 是只读来源**：不改、不删、不移动它任何东西（可读、可 `git status` 证明未变）。
2. **不要 `git add` / `git commit`**：提交时机由用户决定（沿用旧仓的长期要求）。
3. **不要凭记忆写游戏 / 引擎语义结论**（字段含义、函数用途、opcode 语义、脚本角色…）：那些属于**待重建的知识层**，
   必须按 §6 的准入规则、绑定可再校验的观察之后才准登记。

## 1. 五条硬纪律（存储）

1. **仓库不得跟踪任何 `*.sqlite` / `*.sqlite3` / `*.db`**。二进制库在两台机器之间**无法合并**，提交它等于制造"你覆盖我"。
   （落在 `.gitignore`；守卫见 `tools/corpus.mjs` 与 `docs/00-origin/decisions.md` 的反模式清单。）
2. **需要入库的大二进制 / 只读语料一律走 LFS**：用显式规则声明（`.gitattributes` 里已有 `*.zip`/`*.BIN`/`*.png`/… ），
   不要靠本地忽略凑合。★ LFS 只解决"大"，**不解决合并** —— 所以它救不了 DB（见第 1 条）。
3. **DB 永远可删**：删掉本地 DB 后，一条命令必须能从 git 文本真源**确定性重建**（同输入同字节）。
4. **台账真源是 append-only 文本日志**：一条记录一行，自带 ULID 或 `时间戳+计数器` 以保证顺序不依赖文件位置；
   按 key 前缀或按月分片以把并发追加的冲突窗口降到接近 0；周期性 `compact` 出 canonical 排序的 snapshot（**仍是文本**）。
   理由：两台设备各自追加的是**不同行** ⇒ git 三路合并天然干净；每条结论都有 `git log -L` 可追的问责链。
5. **原始语料逐字节忠实**：任何"解析友好化"只能是派生的内存视图，**不得落回语料文件**。
   铁证：旧仓 `sanitize_symbols.py` 把同一份语料从 `::` 4754 / `this` 36753 改成只剩 **3** / **1** ⇒ 据此得出的字符串层结论全不可信。

## 2. 跨平台六条 —— 其中第 4/5/6 条**不是**限制

| # | 项 | 怎么做 |
|---|---|---|
| 1 | 换行 | `.gitattributes` 首行 `* text=auto eol=lf`；二进制 / LFS 加 `-text`。**不要**依赖 `core.autocrlf` |
| 2 | 生成物 | 生成视图与 DB 全部写进 `.gitignore`（两台机器各自重建时不许互相打架） |
| 3 | 大小写 | 保持**零大小写冲突**（win/mac 不敏感、Linux CI 敏感）。加文件前顺带确认路径大小写唯一 |
| 4 | 符号链接 | **可以用**：外部素材就用它引用（如旧仓 `raw/`）。只要**不 track**（`.gitignore` 里已备好 `/raw`） |
| 5 | LFS | **直接用**：大二进制走 LFS；两台机器都 `git lfs install`。**不要**为"不支持 LFS"设计降级 |
| 6 | 非 ASCII 路径 | **可以用**：中 / 日文件名照旧。**不要**为了"怕出问题"改名，也不要把符号链接改造成配置索引 |

★ **开发平台优先级**（用户口径）：**win32 优先**；macOS 侧只做**兼容性验证**、不阻塞任何一轮。
⇒ 为此付出的唯一代价是"纯文本依赖"（一切结论落在文本里，而不是某个平台专属的工具状态里）。

## 3. 语言与代码口径

* **TypeScript 只用在"自带工具链的 app"上**：`apps/emulator`（M4 才进 workspaces）与
  `apps/workbench`（项目工作台：客户端 Vue 3 + Vite + TS；服务端 `server.ts` 由 **Node v24 原生 type stripping**
  直接跑 ⇒ 服务端**无构建**，只用可擦除语法）。这是本条规律的**全部例外**，新增一个 app 就写在这里。
* 仓库内**其余一切 JS** —— `packages/*`、`tools/*`、守卫与测试 —— **一律直接写 `.mjs`**。
  根目录**不引入** `typescript` / `tsc` / `tsconfig`，因此没有构建步骤，`node` 直接跑。
* 包管理**用 pnpm**（`pnpm-workspace.yaml` 是 workspace 真源）。禁止混用 `npm install` 生成 `package-lock.json`。
  ★ `apps/workbench` 的 `.npmrc` 设了 `shamefully-hoist=true`：本机的 `fs.realpathSync` **不解析 pnpm 的 junction**，
  严格布局会让 `vite` 里的 `import 'rolldown'` 直接 `ERR_MODULE_NOT_FOUND` ⇒ 那个项目的依赖必须扁平。
* `.NET`(C#) 与 `native`(C++/CMake) 各自独立工具链，**不进 npm workspaces**。

## 4. 搜索约定

* 用 `rg`（ripgrep）搜内容，用 `glob` 类工具搜路径。
* **不要** `grep -r` / `Get-ChildItem -Recurse | Select-String`：慢、会爬进 `node_modules` 与 `.staging/`。
* 搜素材时记住 `corpus/disasm/files/`（解压产物）与 `.staging/` 是**本地临时区**，不入 git、不要在其中写结论。

## 5. 怎么跑

```bash
pnpm install                    # 只有 workspace 链接（包管理用 pnpm，别用 npm）
pnpm tools                      # ★ 先看这个：域地图（域 → 数据 → 读写 → 操作）
pnpm tools corpus validate      # ★ 素材清单守卫（红了就必须修，不是"看看"）
pnpm tools corpus scan --write  # 补 origin[].sha256（唯一写入口）
pnpm tools fixtures list        # 存档样本：槽 / 定位 / mtime 漂移
pnpm tools opcodes report       # 指令表对账：旧表条目数 / 将丢弃哪些知识层字段（handler / status）
pnpm tools opcodes derive --write # 指令表派生（旧表 → 格式层四列；唯一写入口，缺省 dry-run）
pnpm tools requirements plan    # ★ 进度：需求树（还要做什么、到哪一步）+ 聚合状态
pnpm tools requirements validate # 需求台账守卫（红 = 退出码 1）
pnpm tools disasm verify        # 反汇编语料保真断言（逐行反解回字节必须与源逐字节相同）
pnpm tools old-repo inventory   # 重新实测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm test                       # 守卫测试（单进程跑，见下）
```

* **命令一律经派发器**：`pnpm tools <域> <动作> [args…]`（域地图由各工具的自我声明派生 —— `tools/cli.mjs`）。
  位置参数与 flag 直接跟在后面，**不必 `--`**。
* 每个工具也都能**独立跑**（脱离 DSH / 脱离 pnpm，macOS 上一样）：`node tools/corpus.mjs --validate`。

★ **两个环境口径**（代码里已处理，别绕开）：
① **不要捕获子进程输出**（`stdio: 'pipe'`）：受限沙箱里捕获输出要开命名管道 ⇒ `spawn EPERM`。
`tools/corpus.mjs` 的 `runCapture()` 用**文件描述符重定向**替代管道，拿到同一份 git/recipe 答案。
② **`pnpm test` 用 `--test-isolation=none`**：默认隔离模式由 runner 起子进程走管道，同样会 EPERM。

## 6. 知识准入门（本轮**只立规矩，不落数据**）

1. 每条语义结论必须**绑定至少一条可再校验的观察**（二进制 EA / 语料行区间 + 内容摘要 / 可执行守卫用例 id）。
2. **锚点锚 EA，不锚语料行号**：换一次反汇编只重建 `EA → 行号` 映射，不改任何锚。
3. 读取时**重校验**绑定：观察失效（行内容变了、守卫用例没了）⇒ 该条自动降级为 `stale`，
   **不得再进 accepted**，但**不删除**。
4. 冲突**显式化**为产物，不允许静默改写已有结论。
5. **不需要人工审核全部历史结论** —— 靠上面的机械失效暴露问题，而不是靠人逐条看。
6. 知识条目在 **K3 通过前不得进新仓台账**；`kind=knowledge-source` 的素材在清单里只能 `external-only` / `deferred`
   （守卫 #8 会红）。

## 7. agent 基建怎么注册（**环境级动作**）

| | 落点 | 注册方式 |
|---|---|---|
| **技能** | **`.agents/skills/<名字>/SKILL.md`** —— 路径**固定、不可改名/移位** | DSH 按该固定路径发现，**无需注册**。本仓该目录必须存在，但内容从零重写（M6） |
| **DSH 插件** | `plugins/` 只是**源码落点**，位置自由 | 插件通过 DSH 的插件安装机制以**软链接**注册（环境级、要重装）。**确切命令留待 M6 重建第一个插件时补进本节**（标 `TBD-M6`，不凭记忆编） |

★ **不要为了迎合注册方式去扭曲仓库结构**：技能必须遵守固定路径，而插件位置自由。

## 8. 只读素材消费规则

* `corpus/assets.json` 是 **lockfile 性质**的清单（记录"来源与去向"），**不是**约束；
  唯一能把它变成约束的是 `pnpm tools corpus validate` 必须红 —— **没有守卫的清单等于一份 Markdown**。
* 素材按 `storage` 分四种去向：`lfs`（入库走 LFS）/ `git`（入库纯文本）/ `external-only`（留在仓库外，只登记）/
  `deferred`（后续批次才处理）。**不要在 `external-only` 的素材上"就地修改"**。
* 反汇编语料：`readOnly`，**锚点锚二进制 EA**，语料只提供 `EA → 当前这份导出里的行号` 映射。

## 9. 不要做的事（反模式）

* ❌ 把 `.sqlite` 提交进 git / ❌ 用 LFS 存 DB / ❌ 把 DB 当唯一存储 /
  ❌ 用 `sqlite3 .dump` 当文本真源 / ❌ 让 DB 参与写事务再"导出"成文本。
* ❌ 把旧仓的知识文档"顺手"复制进 `docs/`；❌ 在 `data/ledger/` 里塞旧条目。
* ❌ 手工编辑 `corpus/assets.json` 的 `sha256` / 大小：**用 `pnpm tools corpus scan --write`**（那是唯一写入口）。
* ❌ 在 `corpus/assets.json` 里写体积、入库件校验和、LFS oid、`status`、`generatedAt`：那些 git / LFS / 文件系统已经是权威。

## 10. 文档纪律：README **不写状态**

* ✅ 只写**不变的东西**：口径 / 禁令 / 不变量 / 落点地图 / 怎么跑。
* ❌ **状态、进度、计数、体积、哈希、快照数字一律不手写**。自检一句话：
  **"这句话会不会因为下次干活而变错？"** 会 ⇒ 不要写进 README，改成**"怎么查"**。
* **怎么查**（真源）：**进度与"还要做什么" = `pnpm tools requirements plan`（需求树 `data/requirements/`）**·
  入库进度看 `corpus/assets.json` 的 `storage`/`dest`（`deferred` → `lfs` 就是进度）·
  条目与去向 `pnpm tools corpus list` · 语料保真 `pnpm tools disasm verify` · 旧仓数字 `pnpm tools old-repo inventory` ·
  变更历史 `git log` / `git log -L`。
* ❌ 不要开 CHANGELOG / 进度表 / "已完成"清单：**`git log` 就是变更记录**。
* 例外**只有一处**：**生成物**（整篇都是状态，但由脚本生成 + 文件头写明"别手改"），范例 `docs/00-origin/old-repo-inventory.md`。
  ★ 进度**不在散文里**：它由需求树回答（`pnpm tools requirements plan`）——
  所以本仓**没有**手写进度表，也没有"唯一允许手写状态的文件"这条例外。
* **结构化数据不进散文**：文件清单 / 用途 / 定位 / mtime / 哈希一律**只留一份结构化真源**，README **不列表、不抄数**。
* **JSON 是不透明数据**：任何 JSON 的字段语义 / 枚举 / 不变量 / 操作**只看它的控制脚本的自描述**
  （`pnpm tools corpus describe`、`pnpm tools fixtures describe`），
  ❌ **不要在文档里复述 schema**（那是第二份 schema，必然漂）；文档只写"它是什么 + 非显然口径 + 指向"。
* **同名说明书**：每个**自有**的结构化数据文件旁边必须有同名 `.md`（`foo.json` ↔ `foo.md`），
  且必须含 `## 怎么查` / `## 怎么改` 两节并**指向 `--describe`** —— 看到一个 JSON 就知道去哪看怎么处理它。
  生态文件（`package.json` 等）不在范围内。由 `tools/test/json-docs.test.mjs` 守。
  范例：`corpus/assets.md`、`corpus/fixtures/samples.md`。
* **测试只测基建契约**：这里搭的是基建不是业务，所以测试的对象是
  **守卫能不能红 · 写路径会不会写坏 · 发布物是否满足不变量 · 约定是否齐全**；
  ❌ 不测业务结论，❌ 不复述代码逻辑，❌ 不断言数据的当前取值（那是 `pnpm tools corpus validate` 当哨兵的事）。
* 确实需要两处都写的东西 ⇒ **先问"能不能只留一处"**；真需要就**钉住**（测试断言两处一致）。
  ❌ 不加"不许出现某字符串"这类脆弱守卫 —— 守卫要**红得有意义**。详见 `docs/00-origin/decisions.md` §6。
