# AGENTS.md — 环境与权限须知

> ★ 本文件只保留两条工作线：**§12 一 · lift 线**（`apps/emulator-lift`：执行 `AGE.EXE` 原机器码的模拟器 ——
> 当前唯一在推进的实现）与 **§13 二 · 翻译线**（汉化；后续会用）。
>
> ★ **其余内容已剥离到 `docs/00-origin/agents-archive.md`**：spec 层（已冻结）· shape 层 · 语料与格式工具 ·
> 知识台账与 DB 口径 · TS / pnpm workspace 的实施细节 · 已废弃的模拟器实现 · 旧仓盘点。
> ⛔ 那份**不是执行指令**，只是"将来重启时不必重写"的存档。
>
> ★ **章节号冻结**：本仓有 200+ 处 `AGENTS.md §N` 的引用（`src/`、`tools/`、`docs/`、`data/requirements/`）。
> 被剥离的节**只留标题 + 指针**，所以 `§N` 仍然解析得到；新增内容一律**接在末尾**（§12、§13…），
> ⛔ **不要重排既有编号**。
>
> ★ **规则只有可执行才算规则**：每条纪律要么落在 `.gitattributes` / `.gitignore` / `pnpm` 脚本里，
> 要么落在 `tools/` 的守卫里。写成散文的 `policy: "..."` 机器执行不了、agent 也不会去读 —— 那是伪装成数据的文档。

---

## 0. 三条最常被踩的

1. **旧仓 `E:\Games\Eushully\天結` 是只读来源**：不改、不删、不移动它任何东西（可读、可 `git status` 证明未变）。
2. **不要 `git add` / `git commit`**：提交时机由用户决定（沿用旧仓的长期要求）。
3. **不要凭记忆写游戏 / 引擎语义结论**（字段含义、函数用途、opcode 语义、脚本角色…）：那些属于**待重建的知识层**，
   必须绑可再校验的观察（锚点锚 **EA**，不锚语料行号）之后才准登记。

## 1. 五条硬纪律（存储）

1. 仓库**不得跟踪**任何 `*.sqlite` / `*.sqlite3` / `*.db`（二进制库无法合并，提交即"你覆盖我"）。
2. 需要入库的大二进制 / 只读语料**一律走 LFS**（规则已落在 `.gitattributes`）。
3. DB 永远可删（一条命令从 git 文本真源确定性重建）。
4. 台账真源是 append-only 文本日志。
5. **原始语料逐字节忠实**：任何"解析友好化"只能是派生的内存视图，**不得落回语料文件**。
   ★ 铁证（为什么）：旧仓 `sanitize_symbols.py` 把同一份语料从 `::` 4754 / `this` 36753 改成只剩 3 / 1
   ⇒ 据此得出的字符串层结论全不可信。

★ 第 1–4 条的正文与"为什么" → `docs/00-origin/agents-archive.md` §1（当前工作线只需第 1、2、5 条在场）。

## 2. 跨平台六条 —— 其中第 4/5/6 条**不是**限制

| # | 项 | 怎么做 |
|---|---|---|
| 1 | 换行 | `.gitattributes` 首行 `* text=auto eol=lf`；二进制 / LFS 加 `-text`。**不要**依赖 `core.autocrlf` |
| 2 | 生成物 | 生成视图与 DB 全部写进 `.gitignore`（两台机器各自重建时不许互相打架） |
| 3 | 大小写 | 保持**零大小写冲突**（win/mac 不敏感、Linux CI 敏感）。加文件前顺带确认路径大小写唯一 |
| 4 | 符号链接 | **可以用**：外部素材就用它引用（如旧仓 `raw/`）。只要**不 track** |
| 5 | LFS | **直接用**：大二进制走 LFS；两台机器都 `git lfs install`。**不要**为"不支持 LFS"设计降级 |
| 6 | 非 ASCII 路径 | **可以用**：中 / 日文件名照旧。**不要**为了"怕出问题"改名，也不要把符号链接改造成配置索引 |

★ **开发平台优先级**（用户口径）：**win32 优先**；macOS 侧只做**兼容性验证**、不阻塞任何一轮。
⇒ 为此付出的唯一代价是"纯文本依赖"（一切结论落在文本里，而不是某个平台专属的工具状态里）。

## 3. 语言与代码口径

**两条线的落地只有两种语言**：`apps/emulator-lift` 是 **C++/CMake（CTest）**；仓库工具与守卫是 **`.mjs`**（Node 直接跑，**无构建步骤**）。
★ 跨包引用走包名 `@amayui/x`（前提：消费者 `dependencies` 里写了它 + `pnpm install`）；⛔ 不用 `../../packages/...`。
★ 包管理**只用 pnpm**（`pnpm-workspace.yaml` 是真源）；⛔ 不混用 `npm install`（会生成 `package-lock.json`）。
★ `pnpm typecheck` 是 `tsc --noEmit`（**六份 tsconfig**，覆盖 `packages/age-format` 与两个已废弃 app —— 它们留在门禁里只为
**发现"冻结件被无意改动"**，不是为了继续演进）。
★ 为什么 TS 只在 `apps/emulator` / `apps/emulator-new` / `apps/workbench` / `packages/age-format`、
为什么 `age-format` 的源码是 `.mts` 一份真源、为什么 `core` 是 `"types": []`、
为什么 store 落在仓库内、junction 与提权的关系 —— 全文 → `docs/00-origin/agents-archive.md` §3。

## 4. 搜索约定

* 用 `rg`（ripgrep）搜内容，用 `glob` 类工具搜路径。
* **不要** `grep -r` / `Get-ChildItem -Recurse | Select-String`：慢、会爬进 `node_modules` 与 `.staging/`。
* 搜素材时记住 `corpus/disasm/files/`（解压产物）与 `.staging/` 是**本地临时区**，不入 git、不要在其中写结论。

## 5. 怎么跑（两条线共用的命令面）

```bash
pnpm tools                      # ★ 先看这个：域地图（域 → 数据 → 读写 → 操作）
pnpm tools requirements plan    # ★ 进度真源：还要做什么、到哪一步
pnpm tools requirements validate # 需求台账守卫（红 = 退出码 1）
pnpm test                       # 合同档（默认只跑 @env pure）
pnpm verify                     # 工具链档（台账 / 写路径 / 知识层）；也必须绿
pnpm test:list                  # 看分级集合与每个文件的声明（先看这个）
pnpm test:mutation              # ★ 守卫自检：改坏一处关键常量 ⇒ 确认对应守卫当场红
pnpm typecheck                  # tsc --noEmit，六份工程
```

**lift 线的构建 / 测试 / 验收命令见 §12；翻译线的工具见 §13；CTest 的档位判据（注册位置）见 §12.4。**
★ 每个工具也都能**独立跑**（脱离 DSH / 脱离 pnpm）：`node tools/requirements.mjs --validate`。

### 5.1 三个环境口径（沙箱 / 管道 / 提权）

① **不要捕获子进程输出**（`stdio: 'pipe'`）：受限沙箱里捕获输出要开命名管道 ⇒ `spawn EPERM`。
`tools/corpus.mjs` 的 `runCapture()` 用**文件描述符重定向**替代管道，拿到同一份 git/recipe 答案。
② **`pnpm test` 用 `--test-isolation=none`**：默认隔离模式由 runner 起子进程走管道，同样会 EPERM。
③ **headless Chrome 在受限沙箱下跑不了**（`tools/ui-bake` 的渲染步骤）：要提权。判据与处置见 `tools/ui-bake.md` §5.1。
★ 装依赖往工作区外写（store）也属这一类；**Windows 上更要提权**（pnpm 的目录符号链接要 `SeCreateSymbolicLinkPrivilege`，
沙箱里被拒后它会**静默降级成 junction**）⇒ **提权只发生在"装依赖"与"headless 渲染"两步**；
`pnpm test` / `pnpm tools` / vite 构建都只读 `node_modules`，不提权照跑。

★ **第四条（Windows + DSH ≥ 0.2 才有）：工作区里的文件带 Low 完整性标签**
⇒ **从带 Low 标签的 exe 起的进程本身就是 Low 完整性**，于是"产物要被别的进程执行"的那类会在 DSH 之外也受影响：
实测症状 = 测试安装树用 Locale Emulator **进程起来、窗口没建出来**（直接双击却正常）、游戏写存档目录被拒。
**处置**：产物建到工作区**之外**（同卷），或落盘后 `icacls <目标> /setintegritylevel Medium /T /C`
（代价：该子树随即落在沙箱可写范围之外）。成因 / 实测 / 命令见 `tools/release.md` §3.1。
★ 只影响"要被执行"的产物；纯文本产物与入库件不受影响。

### 5.2 测试分级（判据写在文件首行的 pragma 里，**不建清单文件**）

每个 `*.test.mjs` 首行必须是：

```js
/** @env pure @kind gate @why 一句话说清"红了意味着什么" */                  // 省略 @suite = contract
/** @env pure @kind gate @suite toolchain @why 一句话说清"红了意味着什么" */   // 工具链档
```

`@env` ∈ `pure`（只读仓库内文本 / 纯函数，**`pnpm test` 只跑这档**）· `assets`（要 LFS 资产或游戏安装）· `external`（要旧仓 / 真机）。
`@kind` ∈ `gate` · `contract` · `safety` · `product`。`@suite` ∈ `contract`（缺省）/ `toolchain`（`pnpm verify` 跑）。
★ **两档之和 = 全量**，⛔ 不许有文件两档都不进；`pnpm verify:all` 是工具链档的 `--all`。
★ **测试卫生**：缺席时用 `t.skip(...)`，**不许** `t.diagnostic(...)` + 裸 `return`
（实测那会被 node:test 记成 **pass**）；`layering.test.mjs` 有一条守卫盯着这件事。

### 5.3 变异自检（`pnpm test:mutation`）

"写了守卫"与"守卫真的会红"是两件事 —— 恒真断言、把 `expected` 抄成 `actual` 的断言都能"一直绿"。
它按 `tools/mutate-check.mjs` 里的清单，对每个关键常量施加**一处已知破坏**，要求指定守卫**退出码非 0**。
⇒ 加新守卫时**顺手往清单里加一条**，否则"它会红"只是个声称。
★ **清单有几条、是哪几条不写在这里**（写了必随下一次加守卫而变错）：`node tools/mutate-check.mjs --list`。
★ 它动的是**工作树里的文件**：施加前原文进内存、`finally` 里写回并**比对全文**；不一致就**立刻中止**。

## 6. 知识准入门（骨架）

1. 每条语义结论**必须绑定至少一条可再校验的观察**（二进制 EA / 语料行区间 + 内容摘要 / 可执行守卫用例 id）。
2. **锚点锚 EA，不锚语料行号**：换一次反汇编只重建 `EA → 行号` 映射，不改任何锚。
3. 观察失效 ⇒ 该条降级为 `stale`，**不得再进 accepted**，但**不删除**；
   冲突要**显式化**为产物，⛔ 不许静默改写已有结论。
★ 全文（含"闸门是每条都绑观察、不是整本必须空"、`kind=knowledge-source` 的素材限制）
→ `docs/00-origin/agents-archive.md` §6。

## 7. agent 基建怎么注册（**环境级动作**）

| | 落点 | 注册方式 |
|---|---|---|
| **技能** | **`.agents/skills/<名字>/SKILL.md`** —— 路径**固定、不可改名/移位** | DSH 按该固定路径发现，**无需注册**（落盘即进技能目录）。内容从零重写；**有哪些技能看目录本身** —— 不在这里列清单 |
| **DSH 插件** | `plugins/` 只是**源码落点**，位置自由 | **`dsh plugin --profile web install "<插件绝对路径>"`**（环境级、要提权：写 `$DSH_HOME`）。包必须声明 `dsh.bundle.patch`（否则只当普通依赖装进来、**不会**被组合）；宿主插件代码**不热重载** ⇒ 改完要重启。现有两个：`plugins/deploy`（游戏目录硬链接 / headless Chrome / 完整性标签）与 `plugins/pnpm-priv`（**提权安装依赖**：让 pnpm 建**真符号链接**而不是 junction） |

★ **不要为了迎合注册方式去扭曲仓库结构**：技能必须遵守固定路径，而插件位置自由。

## 8. 只读素材消费规则

* `corpus/assets.json` 是 **lockfile 性质**的清单（记"来源与去向"），**不是**约束；唯一能把它变成约束的是
  `pnpm tools corpus validate` 必须红 —— **没有守卫的清单等于一份 Markdown**。
* 反汇编语料 `readOnly`，**锚点锚二进制 EA**；⛔ 不要在 `external-only` 的素材上"就地修改"。
* 全文（`storage` 四种去向 / `deferred` → `lfs` 的进度口径）→ `docs/00-origin/agents-archive.md` §8。

## 9. 不要做的事（反模式）

* ❌ **不要为每一轮 / 每个 agent 新建构建树**（见 §12.3 的"构建树只有一个"）；❌ 不要删别人的构建树（清理由人来做）。
* ❌ **不要用 test 记住发生过的事**（那是 `observations.md` / `defects.md`）；
  ❌ **不要用 test 断言当前取值 / 当前数字**；❌ **不要给每个常量配一条 mutation 条目**（每类挑一条代表）。
* ❌ **不要为了让守护变绿而改弱判据**（把"回到同一屏"换成"进程没了"这类）。
* ❌ **不要为提速改 guest 可见语义**：要改语义就另开语义单，走 §6 的准入门。
* ❌ **不要按"这块慢"给某个 EA 开特判**；要补就补**按 opcode 的覆盖**。
* ❌ 把旧仓的**知识文档**"顺手"复制进 `docs/`；❌ 在 `data/ledger/` 里塞旧条目。
* ❌ 手工编辑 `corpus/assets.json` 的 `sha256` / 大小（唯一写入口是 `pnpm tools corpus scan --write`）。
  ⇒ 其余与 DB / 台账 / 语料清单相关的反模式 → `docs/00-origin/agents-archive.md` §9。

## 10. 文档纪律：README **不写状态**

* ✅ 只写**不变的东西**：口径 / 禁令 / 不变量 / 落点地图 / 怎么跑。
* ★ **守卫的门槛**：只有"**变了就是 bug、而且变了是静默的**"才配一条守卫。
  **观察** · **缺陷史** · **实现取舍** 一律进 `docs/03-emulator/`，**不进 test**。三问全答"是"才写守卫：
  ① 失败是否意味着**用户可见的错或数据损坏** · ② **会不会有人发现** · ③ **删掉它，会不会让某个错误静默通过**。
  判据全文见 `docs/03-emulator/README.md` §0.1。
* ★ **文档必须带命令与读数**：一条观察要写清"哪条命令 + 看到多少 + 什么口径（build 类型 / 平台 / 有无安装）+ 哪天"。
  读数本身**不是**断言 —— 它进文档，不进 test。
* ❌ **状态、进度、计数、体积、哈希、快照数字一律不手写**。自检一句话：
  **"这句话会不会因为下次干活而变错？"** 会 ⇒ 不要写进 README，改成**"怎么查"**。
* **怎么查**：进度 = `pnpm tools requirements plan` · 语料保真 = `pnpm tools disasm verify` ·
  条目与去向 = `pnpm tools corpus list` · 变更历史 = `git log` / `git log -L`。
* ❌ 不要开**内部**变更记录 / 进度表 / "已完成"清单：**`git log` 就是变更记录**。
  ★ 例外两处：① **生成物**（由脚本生成 + 文件头写明"别手改"）；② **面向用户的发行文本** `release/CHANGELOG.md`
  与 `release/安装说明.md`（它们是**产品的一部分**，规格真源是 `release/README.md` §4）。
* **结构化数据不进散文**；**JSON 是不透明数据**（字段语义只看控制脚本的 `--describe`）；
  **同名说明书**（`foo.json` ↔ `foo.md`，含 `## 怎么查` / `## 怎么改`，由 `tools/test/json-docs.test.mjs` 守）。
  ⇒ 这些规则的推导与范例 → `docs/00-origin/agents-archive.md` §10。

## 11. 项目本体：交付物是 spec，不是 emulator

★ **2026-11-10 起 spec 层短期冻结（用户口径）**：`data/spec/` **暂不维护**（不新增规则、不修订、不做审计 / 复审 / 残差收敛）。
由 spec 派生的待办一律 **drop**，重启时**整体重审**并从 git 恢复被 drop 的单。
★ **当前唯一在维护的实现是 `apps/emulator-lift`**（见 §12）。`apps/emulator-cxx` 是按 spec 重写的**对照实现**：
仍可执行、可作语义对照，但**新功能不落那里**。`apps/emulator` 与 `apps/emulator-new` **已废弃**（只读遗产）：
⛔ 不新增功能、不补守卫、不为它们改 spec。冻结件仍留在 `pnpm typecheck` / `pnpm test` 里，只为
**发现"冻结件被无意改动"**，不是为了让它继续演进。
★ **措辞纪律**：说"与所有可获得的独立证据不矛盾、且能被独立实现"，⛔ **不说"与真机等价"**；
且 **投影一致 ≠ 与引擎一致**（多路推导只发现内部矛盾与遗漏，发现不了共同误解）。
★ 全文（两层本体 / 平台 API 是 spec 的边界 / `evidence` 挂哪个 CTest）→ `docs/00-origin/agents-archive.md` §11。

---

## 12. 一 · lift 线（`apps/emulator-lift`）

`apps/emulator-lift` 是 C++/CMake 的 x86-32 guest-state 模拟器，执行 `AGE.EXE` 的**原机器码**。
三个可执行产物：解释器 `emulator-lift` · AOT 臂 `emulator-lift-aot`（构建期把 guest 代码抬升成 LLVM IR 再链进去）·
构建期工具 `emulator-lift-gen` / `emulator-lift-closure`。

### 12.1 落点

- `src/runtime/` 内核（`X86` / `Insn` / `exec*` / `decode*` / shim 与内存）
- `src/shims/` Win32 / D3D9 / DDraw / DSound / winmm 的 shim（判据 = **微软文档 + 公开规范**，⛔ 不靠观察）
- `src/aot/` AOT 臂：`lift_gen*`（闭包发现 + IR 发射）、`aot_run` / `aot_host` / `aot_dispatch`
- `src/corpus/` 语料索引（只读视图）
- `aot/closure.txt` **入库件**（闭包根，3,277 行 + 为什么）；生成物（`.ll` / `.obj` / 派发表 / pin）落 `out/`，⛔ 不入库
- `docs/` 下每个 `.md` 只写**口径 / 实测 / 偏差**；引擎语义结论走 §6

### 12.2 构建 / 测试 / 验收

```bash
cd apps/emulator-lift
cmake --preset windows-x64      # 或 linux-x64 / macos-arm64
cmake --build --preset windows-x64
ctest --test-dir out --output-on-failure
```

★ **性能 / 音频验收固定用 RelWithDebInfo**（`windows-x64` 预设的缺省）：Debug 下解释器慢到 boot skip-logo > 30 s，
音频 fill 会必然 underrun。⛔ 不要用 Debug 的墙钟 / 音频结论做判据。

### 12.3 构建树只有一个

★ **`apps/emulator-lift/out`**（一棵树 ≈ 2 GB，磁盘有限）。
⛔ **不要为每一轮 / 每个 agent 新建 `out-<名字>`** —— 同一份改动就在这棵树里**增量重建**。
唯一例外是"真要两套 config 同时存在"，那种情况下也**复用同一个固定名字**。
⚠ 收工时**不要删别人的树**（清理由人来做）；也不要把 `out/` 里的产物当结论 —— 它是 gitignored 的生成物。

### 12.4 CTest 档位：**判据 = 注册位置，⛔ 不按"资产在不在"**

**顶层无条件 `add_test` ⇒ `pure` 档**（默认 `ctest` 就跑）；落在 `if` / `foreach` / `function` / `macro` / `while` / `block` 里 ⇒ `assets` 档。
解析在 `tools/lib/ledger.mjs` 的 `cmakeTestTier()`（判据不建清单），两侧守卫见 `tools/test/ledger.test.mjs` 与 `tools/test/spec.test.mjs`。
⇒ **想让一条 CTest 成为 reviewed 级证据，就把它注册在最外层。**

★ **按「用途」分档**：默认（顶层无条件）= **按 shim 接口设计的合同**（判据 = 微软文档 / 公开规范）·
`LIFT_STRESS` = **压力 / 效率档**（判据 = 读数 + 阈值；**唯一**允许跑真游戏的一档）· `repro` 档**已取消**
（缺陷复现件只作复现命令留在 `defects.md` / `observations.md`）。
判据：新增一条用例先回答"这是**按 shim 接口的合同**还是**压力 / 效率**"；⛔ **业务正确性不作判据**
（"到达某界面 / 存档链走通 / 播完了"是游戏的事），⛔ 也不许把"再回归一遍历史缺陷"的用例放进默认门禁。
输入（`links/install`、LFS 语料、两份宿主字体）**假定在场**：缺了不改变档位；显式开档而缺 ⇒ `FATAL_ERROR`。

★ **删 / 改任何 CTest 名或 `g_current_case` 之前**：先 `rg '<名字>' data/requirements`；命中任一 `verify:` / `repro:`
⇒ **不删**，报告裁决（删了等于让一个已收口的结论失去凭据，而 `requirements validate` 会因为锚点解析不了而红）。
同理：删 / 改 `add_test` 的 `--flag` 之前先确认目标源文件里有对应分派（空壳用例 `ctest` 照样绿）。

### 12.5 AOT 口径

- **执行源被 pin**：`lift-gen` 把所读 PE 的 `sha256` + `entry` 写进生成物；AOT 启动前核对，不一致 `exit 2`。
  `--exe` / 环境变量只是诊断口径，⛔ 不能换成另一份 PE。
- ⛔ **AOT 臂没有 native override 这套东西了**（2026-11-16 起）：`--native-overrides` / `--lst` 现在是**未知参数**
  （`exit 2`）。理由与边界见 §12.8 第 1 条。⇒ 判据永远在"只有 IR 与 shim"的形状上取，没有"两档"可选。
- **profile 只是归因**，⛔ 不进任何判据；它的读数（`dispatch-top:` 的 `ms` 列等）是"派发份额"，**不是**该 EA 的实测成本。
- 生成物与解释器**共用同一个 `X86` 语义真源**；⛔ 不许为某个 EA 开特判路径。
- `out/` 里的 `lift_aot.ll` 是生成物（可重生成）；`aot/closure.txt` 是入库件（坏闭包会在构建期硬闸红）。

### 12.6 性能与语义验收口径

- **优先用静态计数**（`llvm-objdump -d` 的指令行数）当效率判据；⚠ `.ll` 行数**不是**效率指标。
- 墙钟只在 **release、同一台机器、交错 A/B（≥3 对）、取中位数**时可比；这台机器分辨不了 <5% 的差异。
- 语义指纹 = `exit` + `frames` + `stop @ <EA>` + arena 三元组 + `table misses` **逐位相同**。
  ⚠ `insns` 有 ~0.2% 的墙钟抖动，**不作判据**；`lastHash` / 逐帧 hash 也不是稳定指纹。
- 任何"x87 / 浮点精度"类偏离必须在需求单或 `docs/` 里**写成显式裁决**，⛔ 不许当"实现细节"。

### 12.7 变异自检

见 §5.3。新守卫必须往 `tools/mutate-check.mjs` 加一条，并实测红。
★ **变异必须能点名机制**（2026-11-16 的教训，见 `defects.md#BUG-24`）：设计单给的"把 `ja` 反转"、
"去掉某个过滤器"两条**在默认档都不红**（被兜底策略吸收了）。能红的那两条是"项数取错 ⇒ 收敛硬红并点名 EA"
与"不做注入 ⇒ 某个具体性质红在具体几张表上"。⇒ 只"整体退出码非 0"的变异可能**假绿**。

### 12.8 三条落地口径（2026-11 定，都有实测出处）

1. **AOT 臂的执行来源只有两个：IR 与 shim。** ⛔ 不许有第三个语义实现（宿主的"某 EA 用 C++ 重实现"就是第三个）。
   那套 `--native-overrides` 已从 AOT 臂**整条删除**；`tools/test/aot-arm-shape.test.mjs` 把它的 33 个标识符
   列为**禁用词**（要装回去必须显式改那张表）。要换某个 EA 的实现 ⇒ 那是**构建期**决定（改闭包 / 改发射的 IR），
   产物可以重新 pin、可以 diff、可以审。
   ★ 副作用（重要）：解释器侧的 native override 是**解释器**的提速手段，**不是 AOT 的缺口**。
   看到"某 EA 需嵌套 guest 执行器 / 需段级入口"这类旧结论，先查它在不在派发表里。
2. **shim 回调的续延 = 一次普通 shim 调用。** 它的 EA **必须来自桩页分配**（`addRuntime` / `ShimRegistry::bind` 发号），
   ⛔ **不许写字面量**。判据不是"这条臂能跑"，而是"**每一条臂的识别集合里都有它**"——
   解释器多一条 `BlockCache` 路径（能装**任意** EA），所以"只有解释器认得"的字面量会静默地只在 AOT 上炸。
   出处：`defects.md#BUG-25`（1 处，字面量在桩页外）；对照：合成 EA `0x7FFEF004/008/010/014` 也是字面量，
   但**两臂各有一段 pump** 才没出事。
3. **构建形状与输出默认档都是判据面，都要被断言。**
   - IR 拆 8 份（clang 段 120.7 s → 21.6 s）**必须仍是 `-c -O2`**：configure 期 `FATAL_ERROR` 钉住
     （出现 `-O0/-O1/-Os/-Oz/-flto/llc` 即失败），形状进二进制（`LIFT_AOT_SHAPE="split8-O2"`）并在启动打一行，
     分片清单自洽由 CTest `lift-aot-split-manifest` 判。回退 = `-DLIFT_AOT_SPLIT=1`（一行，⛔ 不用回滚源码）。
   - 闭包五条判据在**生成那一趟**里做（`--closure-gate`），⛔ 不再单独跑一次 `--check`（那是同一份分析的重复）。
   - **默认档不许有周期性输出**（每帧 / 每 N 次派发 / 每次 shim）。周期性输出一律 opt-in（如 `--frame-log`）；
     ★ **一次性**输出（停机原因 / 启动 readout / pin 核对 / 缺口清单）⛔ **不许**收——出问题时那是唯一的消息面。
     判据不是"看着吵就删"，而是"这条会在一次 run 里出现几次"。

## 13. 二 · 翻译线（后续会用）

汉化：把 AGE 脚本的日文译文做成变更叠加层。**这一部分现在不推进，但资产与口径要保持可用。**

### 13.1 真源

- **`data/translations/patch.json` 是唯一入库的变更叠加层**（日文原文 ↔ 当前中文）。
  ⛔ 不要另开一份"翻译表"；⛔ 不要把译文写回原始语料。
- 字段语义与操作看 `node tools/patch.mjs --describe`（⛔ 文档里不复述 schema）。
- 判据：`pnpm tools patch verify`（或 `node tools/patch.mjs --verify`）必须绿。

### 13.2 参考资产的迁移口径（唯一一处"可以整棵搬进来"的例外）

**翻译参考资产**（术语表 / 角色语气 / 剧情联动）按 **只读快照 + 显式"未复核"口径**迁进 `docs/01-translation/ref/`，
并由守卫钉住**字节保真**。理由与边界见 `docs/01-translation/README.md` §5。
★ 这与"⛔ 不把旧仓知识文档顺手复制进 `docs/`"**不矛盾**：翻译资产的失效方式是与**当前译文**比对（机械可判活/死），
而引擎知识必须绑 EA / 语料行区间等可再校验观察（§6）。

### 13.3 发行文本

- **`release/CHANGELOG.md` 与 `release/安装说明.md` 是产品的一部分**（随补丁包发给玩家），
  不是内部沿革 —— 它们是 §10 的第二处例外。规格真源是 `release/README.md` §4（技能只引用、不复述）。
- 发行目录叫 `release/`，⛔ 不叫 `patch/`（"patch" 已被变更叠加层占用）。

### 13.4 边界

- ⛔ 翻译线**不产出**引擎语义结论：那些要走 §6（锚 EA + 可再校验观察）。
- ⛔ 不为翻译去改 `AGE.EXE`（汉化项目与修改 `AGE.EXE` 无关）。
- 整篇重译与局部更新走同一个闭环（检索 → 改中文 → 生成编辑清单 → 逐条应用 → 过判据），细节见 `amayui-translate` 技能。
