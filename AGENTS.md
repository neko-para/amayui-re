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

## 3. 语言与代码口径

* **只有 `apps/emulator` 用 TypeScript**（自带工具链；M4 才进 workspaces）。
* 仓库内**其余一切 JS** —— `packages/*`、`tools/*`、守卫与测试 —— **一律直接写 `.mjs`**。
  根目录**不引入** `typescript` / `tsc` / `tsconfig`，因此没有构建步骤，`node` 直接跑。
* 包管理**用 pnpm**（`pnpm-workspace.yaml` 是 workspace 真源）。禁止混用 `npm install` 生成 `package-lock.json`。
* `.NET`(C#) 与 `native`(C++/CMake) 各自独立工具链，**不进 npm workspaces**。

## 4. 搜索约定

* 用 `rg`（ripgrep）搜内容，用 `glob` 类工具搜路径。
* **不要** `grep -r` / `Get-ChildItem -Recurse | Select-String`：慢、会爬进 `node_modules` 与 `.staging/`。
* 搜素材时记住 `corpus/disasm/files/`（解压产物）与 `.staging/` 是**本地临时区**，不入 git、不要在其中写结论。

## 5. 怎么跑

```bash
pnpm install            # 只有 workspace 链接
pnpm validate           # ★ 素材清单守卫（缺省 dry-run；红了就必须修，不是"看看"）
pnpm test               # 守卫的单元测试 + 端到端测试（node --test "tools/test/**/*.test.mjs"）
pnpm corpus -- --scan --write    # 补 origin[].sha256（唯一写入口）
pnpm inventory          # 重新实测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm recode -- --verify # 反汇编语料保真断言（逐行反解回字节必须与源逐字节相同）
```

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
  唯一能把它变成约束的是 `pnpm validate` 必须红 —— **没有守卫的清单等于一份 Markdown**。
* 素材按 `storage` 分四种去向：`lfs`（入库走 LFS）/ `git`（入库纯文本）/ `external-only`（留在仓库外，只登记）/
  `deferred`（后续批次才处理）。**不要在 `external-only` 的素材上"就地修改"**。
* 反汇编语料：`readOnly`，**锚点锚二进制 EA**，语料只提供 `EA → 当前这份导出里的行号` 映射。

## 9. 不要做的事（反模式）

* ❌ 把 `.sqlite` 提交进 git / ❌ 用 LFS 存 DB / ❌ 把 DB 当唯一存储 /
  ❌ 用 `sqlite3 .dump` 当文本真源 / ❌ 让 DB 参与写事务再"导出"成文本。
* ❌ 把旧仓的知识文档"顺手"复制进 `docs/`；❌ 在 `data/ledger/` 里塞旧条目。
* ❌ 手工编辑 `corpus/assets.json` 的 `sha256` / 大小：**用 `pnpm corpus -- --scan --write`**（那是唯一写入口）。
* ❌ 在 `corpus/assets.json` 里写体积、入库件校验和、LFS oid、`status`、`generatedAt`：那些 git / LFS / 文件系统已经是权威。
