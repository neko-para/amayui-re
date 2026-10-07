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

* **TypeScript 只出现在两类落点**：① **自带工具链的 app** —— `apps/emulator` 与
  `apps/workbench`（项目工作台：客户端 Vue 3 + Vite + TS；服务端 `server.ts`）；② **`packages/age-format`**
  （源码是 `.mts` 的**一份真源**，见下 —— 它**没有**自己的工具链，只靠根上的 `tsc --noEmit` 检查）。
  仓库内**其余一切 JS 一律 `.mjs`**。★ 新增一个带工具链的 app 就写在这里。
  ★ **`apps/emulator` 现在也没有构建步骤**：它的 `.ts`（`src/model/*.ts`）由 **Node v24 原生 type stripping** 直接跑，
  守卫（`tools/test/*.mjs`）就 `import '../../apps/emulator/src/model/pools.ts'` —— 实测 `node` 直接从 `.mjs` import `.ts` 可用。
  ⇒ 写法约束：**只许可擦除语法**（类型标注 / `interface` / `type` / `import type`），
  ❌ 不用 `enum` / `namespace` / 构造器参数属性 / 装饰器（它们要**代码生成**，会逼出构建步骤）。
  ★ 它**已进 workspaces**（因为它的模型要 import `@amayui/age-format`）⇒ 有 `apps/emulator/package.json`，
  依赖写在 `dependencies` 里、跨包引用走包名。入列表的判据是**"有没有自己的依赖"**，不是"在不在 `apps/`"。
* 仓库内**其余一切 JS** —— `tools/*`、守卫与测试 —— **一律直接写 `.mjs`**。
  根目录**不引入** `typescript` 的构建步骤，`node` 直接跑（`tsc` 只用于 `noEmit` 检查，见下）。
  ★ **`packages/age-format` 的源码是 `.mts`（一份真源）**：类型就写在**实现里**，
  **没有**配对的 `.d.mts`，也**不许**消费方自己声明别人的类型、或用 `as` 把跨包边界糊过去。
  ⇒ 消费方 `import type { Header } from '@amayui/age-format/src/asm/index.mts'`（类型由入口 `export type { … }` 再导出）。
  ★ **为什么从 `.mjs` + 手写 `.d.mts` 改成 `.mts`**：`.d.mts` 与 `.mjs` **TypeScript 不交叉校验**
  —— 声明里写错**参数类型**`tsc` 完全不响。实测代价：本包那份手写声明里，`roundTripBytes` 被写成
  `boolean`（真值是**字节**）、`parseWhBpp` / `extractPaletteRgb` / `packSection` / `decodeRgba` 的返回
  全写错 —— 名字层面对得上，签名全错。改成一份真源后这些**立刻**变成 `tsc` 错误。
  ★ `tools/test/age-format-types.test.mjs` 仍然守着三条：**幽灵声明**（`export declare` 的名字实现里要有）·
  **公开面类型可达**（自己就是 `.mts`、或有配对 `.d.mts`、或已被入口再导出）· **未注解导出为 0**。
* ★ **跨包引用一律走包名**（`@amayui/age-format/src/...`），**不要**用 `../../packages/...` 相对路径。
  ★ **前提是"声明了依赖"**：pnpm 只为**在 `dependencies` 里写了**的 workspace 包建
  `node_modules/@amayui/*` 链接。没声明 ⇒ 包名导入直接 `ERR_MODULE_NOT_FOUND`
  （实测：`tools/package.json` 原先把 `age-format`/`ledger` 漏在 `dependencies` 之外，
  于是"相对路径能跑、包名跑不了"）。加一个跨包引用 = 在消费者清单里加一行 `"@amayui/x": "workspace:*"` + `pnpm install`。
  ★ 各包 `package.json` 的 `exports` 用 `"./*": "./*"`：**无构建 ⇒ 不做打包边界**，只是让"包内路径"有个显式入口集合。
  ★ Windows 上那个链接**必须是真符号链接**（`lstat.isSymbolicLink === true`）：Node **拒绝为 `node_modules`
  下的文件剥类型**（`ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING`）⇒ 跨包 `.mts` 只在真符号链接下走得通。
  无提权时 pnpm 会**静默降级成 junction**（装完不报错、链接形态却是错的）⇒ 提权安装见 `plugins/pnpm-priv`。
* ★ **类型检查用 `noEmit`，覆盖三个工程**：`pnpm typecheck`
  （= `tsc -p packages/age-format/tsconfig.json --noEmit && tsc -p apps/emulator/tsconfig.json --noEmit && tsc -p apps/emulator/tsconfig.frontends.json --noEmit`）。
  ★ **为什么是三个而不是两个**：`apps/emulator` 自己分成"核心"与"前端"两份 tsconfig ——
  核心那份是 `"types": []`（**守卫本身**），而前端（`apps/emulator/frontends/**`）的**全部职责**
  就是"把 `node:fs` 接上核心定义的接口" ⇒ 它必须有 `"types": ["node"]`。
  两者不能共用一份：要么核心的守卫失效，要么前端写不了 `node:fs`。
  （与 `packages/age-format` 的分工同形：那份给工具侧 `@types/node`，模拟器核心给 `types: []`。）
  ★ 前端工程 `include` 只有 `frontends/**`，但通过 import 会把 `src/**` 一起拉进来检查 —— 这是**有意的**：
  "核心 + 前端"合起来必须自洽；核心的守卫不受影响（`tsconfig.json` 仍只带 `src/**` 且 `types: []`）。
  `.ts`/`.mts` 仍然由 **Node 原生 type stripping** 直接跑 ⇒ **没有构建步骤**，`tsc` 的唯一职责是"把类型写错变成红灯"。
  `erasableSyntaxOnly` 把"Node 剥壳不支持的语法"（`enum`/`namespace`/参数属性/装饰器）提前变成类型错误。
  ★ **模拟器的工程会把 `age-format` 的源码一起拉进来**（它 `import` 它）—— 这是**有意的**：
  一条命令就能看见"消费方 + 被消费方的类型是否自洽"，不需要给 `age-format` 做 `composite` 工程引用
  （那要产出 `.d.ts`，与"无构建"冲突）。
  ★ `packages/age-format` 的 tsconfig 需要 `@types/node`（**工具侧**用 `Buffer` / `node:fs` / `process`）。
  ★ `apps/emulator` 那份是 **`"types": []`** —— 这一行就是**"核心不许碰 Node"的守卫本身**：
  模拟器核心将来跑在**浏览器**里，`Buffer` 要 polyfill、**`node:fs` 没有 polyfill 可打**。
  ⇒ 用**类型系统**而不是"扫源码的测试"来守（后者只认字面量，认不出间接 import 与别名）。
  实测：往核心的可达闭包（如 `asm/bytes.mts`）加一行 `Buffer.alloc(1)` ⇒ `Cannot find name 'Buffer'`；
  加 `import fs from 'node:fs'` ⇒ `Cannot find module 'node:fs'` —— 都当场红。
  ⇒ 推论（**务必记住**）：`age-format` 的 `src/asm/**` **整个目录零 Node 依赖**（判据：`rg 'node:' packages/age-format/src/asm/` 为空）。
  ★ 它是怎么做到的：指令表**随模块自带** —— `import defs from './instruction-set.json' with { type: 'json' }`
  （Node / 打包器 / 浏览器三方都支持 ESM JSON 模块）。**别再用 `node:fs` 读本包自己的数据文件**。
  ⇒ 两个入口的分工因此只是**范围**：`asm/runtime.mts` = 运行期要的那一份（核心用它）；
  `asm/index.mts` = 全部（再加反汇编 / 重汇编 / 码页编解码）。
  ⇒ "加载任意一份表"不是那一层的事：要读别的文件的调用方自己 `buildOpcodeTable(JSON.parse(...), p)`。
  ★ **WHATWG 通用类型**（`TextDecoder` / `URL` / `AbortController` / `structuredClone`…）走 TypeScript
  **内置的宿主库** `lib: ["ES2023", "WebWorker"]` —— 实测：给全这批 API，且**不给** `document` / `window`；
  ⚠ 代价：会给 worker 专属的 `self` / `postMessage`（写错了会在 Node 侧守卫里**当场炸**，不是静默）。
  ★ 不选另两条的理由：`lib: DOM` 会把 DOM 全局全放行（而核心还要能在 Node 里跑）；
  `@types/web` 是 `lib.dom.d.ts` 的**同一份生成物**（仍只有 DOM）。上游"把 Node 与 DOM 公共的 API 抽成
  `lib.common.d.ts`"那条（TypeScript #41727）**至今只是提案**（`Awaiting More Feedback`）⇒ 没有官方方案可用。
* 包管理**用 pnpm**（`pnpm-workspace.yaml` 是 workspace 真源）。禁止混用 `npm install` 生成 `package-lock.json`。
  ★ **结构类设置只写 `pnpm-workspace.yaml`，不写 `.npmrc`**：`.npmrc` 只读 auth 与 registry；
  定义 `node_modules` 结构的键写在那里会在 pnpm 11 起**静默失效**。
  ★ **不写 `nodeLinker`**（用默认 `isolated`）、**不写 `shamefullyHoist`**（默认就是 `true`，写了不生效）。
  ★ **`storeDir: '.pnpm-store'`**（相对路径，相对 workspace 根）⇒ 可移植，且**落在工作区内** ⇒
  store 本身不需要提权。★ **但 Windows 上"装依赖"这一步仍要提权**：pnpm 建的是**目录符号链接**
  （要 `SeCreateSymbolicLinkPrivilege`），受限沙箱里会被拒，而 pnpm 会**静默降级成 junction** ——
  Node **不能 realpath junction**（`lstatSync().isSymbolicLink === false`，实测）⇒ **传递依赖解析不到**
  （实测：`plugins/deploy` 里 `schemastery` 的 `cosmokit` 报 `ERR_MODULE_NOT_FOUND`）。
  判据：`lstatSync('<包>').isSymbolicLink === true`；修法：**提权重跑 `pnpm install`**（删掉那层 `node_modules` 更稳）。
  ★ **判据**：① `pnpm store path` 落在工作区内；② `node_modules/.modules.yaml` 的 `nodeLinker`
  是你期望的布局 —— 实物形状由**上一次安装时的配置**决定，pnpm 不会替你清理遗留的树。
  ★ **数 junction 用 node:fs 的 `lstatSync`**：PowerShell 的 `Get-ChildItem -Directory`
  不跟随也不显示 junction，用它数会得出全 0 的假象。
  ★ **换布局/重装用 `Rename-Item` 让开，不要删**（`isolated` 下包文件是硬链接回 store，
  workspace-write 里删不掉）；让开的树挪进 `.tmp/`，否则会被 `json-docs` 守卫当成自有 JSON 而变红。
  **别删 store**（它是 `node_modules` 的支撑）。
  ★ **换 npm 不解决问题**：npm 的 cache 同样在工作区外、同样拒写，只是**硬失败**而非降级。
  ★ **幽灵依赖**风险仍在：新增依赖前先确认它不是靠"根上恰好有一份"解析的。
  ⇒ **为什么**（`isolated` vs `hoisted` 的实测对照、store 落点、提权边界）见
  `docs/00-origin/decisions.md` §7.1。
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
pnpm tools requirements serve   # 项目工作台（需求 + AGE 脚本），项目在 apps/workbench/
pnpm --filter @amayui/workbench verify # 工作台门禁：typecheck + build + smoke
pnpm tools disasm verify        # 反汇编语料保真断言（逐行反解回字节必须与源逐字节相同）
pnpm tools old-repo inventory   # 重新实测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm test                       # ★ 测试（默认只跑 @env pure；分级见下）
pnpm test:list                  # 看分级集合与每个文件的声明（先看这个）
pnpm test:assets                # 只跑要 LFS/游戏安装的那档
pnpm test:all                   # 全部（含 @env external：旧仓/真机）
pnpm test:mutation              # ★ **守卫自检**：改坏一处关键常量 ⇒ 确认对应守卫**当场红**（红得有意义）
pnpm typecheck                  # ★ `tsc --noEmit`，**三个工程**（`packages/age-format` + `apps/emulator` 核心 + `apps/emulator` 前端）；不产出 ⇒ 仍无构建步骤
```

★ **`pnpm test:mutation` 为什么存在**："写了守卫"与"守卫真的会红"是两件事 —— 恒真断言、把 `expected` 抄成 `actual`
的断言都能"一直绿"。它按 `tools/mutate-check.mjs` 里的清单，对每个关键常量施加**一处已知破坏**，
要求指定守卫**退出码非 0**。⇒ 加新守卫时**顺手往清单里加一条**，否则"它会红"只是个声称。
★ **清单有几条、是哪几条不写在这里**（写了必随下一次加守卫而变错）：`node tools/mutate-check.mjs --list`。
★ 它动的是**工作树里的文件**：施加前原文进内存、`finally` 里写回并**比对全文**；不一致就**立刻中止**（避免半坏的工作树）。

### 测试分级（★ 判据写在文件首行的 pragma 里，**不建清单文件**）

每个 `*.test.mjs` 首行必须是：

```js
/** @env pure @kind gate @why 一句话说清"红了意味着什么" */
```

| `@env` | 这台机器上它有没有意义 | 默认跑？ |
|---|---|---|
| `pure` | 只读仓库内文本 / 纯函数 | ✅ **`pnpm test` 只跑这档** |
| `assets` | 要 LFS 资产或**游戏安装**（控制类内容很稳定 ⇒ 不值得每次跑） | `pnpm test:assets` |
| `external` | 要**旧仓**或真机 | `pnpm test:all` |

`@kind` ∈ `gate`（不变量/清单/约定）· `contract`（领域能力行为）· `safety`（写路径不写坏数据）· `product`（产出物不变量）。

★ 为什么要有分级（实测）：旧形态一条命令跑全部 = 23 文件 / 220 用例 / **~28.7 s**，其中
`inventory` 13.8 s + `corpus-manifest` 9.8 s 占了 **82%**；而且 **13/23 个文件依赖仓库外的东西**，
"这台机器上没跑"与"跑了且绿"在输出里长得一样。选择器是 `tools/test-run.mjs`。
★ **测试卫生**：缺席时用 `t.skip(...)`，**不许** `t.diagnostic(...)` + 裸 `return`
（实测那会被 node:test 记成 **pass**）；`layering.test.mjs` 有一条守卫盯着这件事。

* **命令一律经派发器**：`pnpm tools <域> <动作> [args…]`（域地图由各工具的自我声明派生 —— `tools/cli.mjs`）。
  位置参数与 flag 直接跟在后面，**不必 `--`**。
* 每个工具也都能**独立跑**（脱离 DSH / 脱离 pnpm，macOS 上一样）：`node tools/corpus.mjs --validate`。

★ **三个环境口径**（前两条代码里已处理，别绕开；第三条只能靠提权）：
① **不要捕获子进程输出**（`stdio: 'pipe'`）：受限沙箱里捕获输出要开命名管道 ⇒ `spawn EPERM`。
`tools/corpus.mjs` 的 `runCapture()` 用**文件描述符重定向**替代管道，拿到同一份 git/recipe 答案。
② **`pnpm test` 用 `--test-isolation=none`**：默认隔离模式由 runner 起子进程走管道，同样会 EPERM。
③ **headless Chrome 在受限沙箱下跑不了**（`tools/ui-bake` 的渲染步骤）：要提权。
判据（用 `data:text/html,<h1>x</h1>` 也失败 ⇒ 不是配方问题）与处置见 `tools/ui-bake.md` §5.1。
★ 装依赖往工作区外写（store）也属这一类；**Windows 上更要提权**：pnpm 的目录符号链接要
`SeCreateSymbolicLinkPrivilege`，沙箱里被拒后它会**静默降级成 junction**，而 Node 解析不了 junction
（判据与修法见 §3 的 `storeDir` 那条）⇒ **提权只发生在"装依赖"与"headless 渲染"两步**；
`pnpm test` / `pnpm tools` / vite 构建都只读 `node_modules`，不提权照跑。

★ **第四条（Windows + DSH ≥ 0.2 才有）：工作区里的文件带 Low 完整性标签**
（`@deepseek-ai/dsh-sandbox-windows-acl` 按设计给授权根下的**常驻**可继承标签，会话结束也不撤）⇒
**从带 Low 标签的 exe 起的进程本身就是 Low 完整性**，于是"产物要被别的进程执行"的那类会**在 DSH 之外也受影响**：
实测症状 = 测试安装树用 Locale Emulator **进程起来、窗口没建出来**（直接双击却正常）、游戏写存档目录被拒。
**处置**：产物建到工作区**之外**（同卷），或落盘后 `icacls <目标> /setintegritylevel Medium /T /C`
（代价：该子树随即落在沙箱可写范围之外）。成因 / 实测 / 命令见 `tools/release.md` §3.1。
★ 只影响"要被执行"的产物；纯文本产物与入库件不受影响。

## 6. 知识准入门（本轮**只立规矩，不落数据**）

1. 每条语义结论必须**绑定至少一条可再校验的观察**（二进制 EA / 语料行区间 + 内容摘要 / 可执行守卫用例 id）。
2. **锚点锚 EA，不锚语料行号**：换一次反汇编只重建 `EA → 行号` 映射，不改任何锚。
3. 读取时**重校验**绑定：观察失效（行内容变了、守卫用例没了）⇒ 该条自动降级为 `stale`，
   **不得再进 accepted**，但**不删除**。
4. 冲突**显式化**为产物，不允许静默改写已有结论。
5. **不需要人工审核全部历史结论** —— 靠上面的机械失效暴露问题，而不是靠人逐条看。
6. **知识条目进新仓台账的闸门是"每条都绑可再校验观察"，不是"整本必须空"**。
   ★ **2026-10 变更**（原口径：K3 通过前一律不许写）：批 R1 的"重写核验"工作被显式当作 **K3 的写入者** ——
   它做的是"按准入门核验后登记"，不是"把旧仓条目倒进来"。判据落在守卫里
   （`tools/test/ledger.test.mjs` 的"写入闸门"）：**每条**的锚都必须能解析、域要么留空（待定域）要么追得到词表。
   `kind=knowledge-source` 的素材在清单里仍然只能 `external-only` / `deferred`（守卫 #8 会红）。

## 7. agent 基建怎么注册（**环境级动作**）

| | 落点 | 注册方式 |
|---|---|---|
| **技能** | **`.agents/skills/<名字>/SKILL.md`** —— 路径**固定、不可改名/移位** | DSH 按该固定路径发现，**无需注册**（落盘即进技能目录，当前会话就能用）。内容从零重写（不抄旧仓）；**有哪些技能看目录本身** —— 不在这里列清单（列了必随下一次重建变错，见 §10） |
| **DSH 插件** | `plugins/` 只是**源码落点**，位置自由 | **`dsh plugin --profile web install "<插件绝对路径>"`**（环境级、要提权：写 `$DSH_HOME`）。包必须声明 `dsh.bundle.patch`（否则只当普通依赖装进来、**不会**被组合）；宿主插件代码**不热重载** ⇒ 改完要重启。现有两个：`plugins/deploy`（部署那几步：游戏目录硬链接 / headless Chrome / 完整性标签）与 `plugins/pnpm-priv`（**提权安装依赖**：让 pnpm 建**真符号链接**而不是 junction —— 跨包 `.mts` 的前提） |

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
* ❌ 把旧仓的**知识文档**"顺手"复制进 `docs/`；❌ 在 `data/ledger/` 里塞旧条目。
  ★ **例外只有一处、且口径相反**：**翻译参考资产**（术语表 / 角色语气 / 剧情联动）按
  **只读快照 + 显式"未复核"口径**迁进 `docs/01-translation/ref/`，并由守卫钉住字节保真 ——
  理由（为什么它和引擎知识不同）见 `docs/01-translation/README.md` §5。引擎语义结论仍然只能走 §6 准入门。
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
* ❌ 不要开**内部**变更记录 / 进度表 / "已完成"清单：**`git log` 就是变更记录**。
* 例外**有两处**：
  ① **生成物**（整篇都是状态，但由脚本生成 + 文件头写明"别手改"），范例 `docs/00-origin/old-repo-inventory.md`；
  ② **面向用户的发行文本** —— `release/CHANGELOG.md` 与 `release/安装说明.md`：它们是**产品的一部分**
     （随补丁包发给玩家），不是内部沿革。规格的真源是 `release/README.md` §4（技能只引用、不复述）。
  ★ 进度**不在散文里**：它由需求树回答（`pnpm tools requirements plan`）——
  所以本仓**没有**手写进度表；两处例外都不是进度表。
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
