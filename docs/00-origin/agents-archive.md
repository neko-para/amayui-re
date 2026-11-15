# AGENTS.md 存档：spec 层 / 语料与格式工具 / 台账与存储 / 语言与包管理 的实施细节

> ⛔ **这不是执行指令**。本仓的执行指令只有 `AGENTS.md`（已瘦身成两条线：lift 线 + 翻译线）。
> 本文件存放的是**从 `AGENTS.md` 剥离出来的正文**，用途只有一个：将来重启这些工作线时不必重写。
> ★ **章节号与原 `AGENTS.md` 的编号一一对应**（本仓有 200+ 处 `AGENTS.md §N` 的引用 ⇒ 编号冻结）；
> 读到 `AGENTS.md §N（正文见存档 §N）` 就翻到这里。
> ★ 剥离日期与裁决：spec 层自 2026-11-10 起冻结（见原 §11），当前唯一实现线是 `apps/emulator-lift`。

---

## 存档 §1 · 五条硬纪律（存储）

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

---

## 存档 §3 · 语言与代码口径（全文）

* **TypeScript 只出现在两类落点**：① **自带工具链的 app** —— `apps/emulator`、`apps/emulator-new`
  （**两者均已废弃**，见原 §11 —— ⛔ 不要在这里写新代码）与 `apps/workbench`（项目工作台：客户端 Vue 3 + Vite + TS；服务端 `server.ts`）；
  ② **`packages/age-format`**
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
* ★ **类型检查用 `noEmit`，覆盖六个工程**：`pnpm typecheck`
  （= `tsc -p packages/age-format/tsconfig.json --noEmit && tsc -p apps/emulator/tsconfig.json --noEmit && tsc -p apps/emulator/tsconfig.frontends.json --noEmit && tsc -p apps/emulator-new/tsconfig.core.json --noEmit && tsc -p apps/emulator-new/tsconfig.renderer.json --noEmit && tsc -p apps/emulator-new/tsconfig.json --noEmit`）。
  ★ **后三个是 `apps/emulator-new`**：核心 / 渲染进程 / 整个 app 分成三份 tsconfig ——
  `tsconfig.core.json`（`include` 只有 `src/core/**`）是 `"types": []` 的**守卫本身**；
  `tsconfig.renderer.json` 是 Electron 渲染进程（`types: []` + `lib: DOM`，不认识 Node）；
  `tsconfig.json` 覆盖核心 + `src/platform/node/**` + `src/platform/electron/**` + `src/main.ts`，所以是 `"types": ["node"]`。
  ★ 它的核心只收**纯数据 + `Host` 接口**（指令表 / 配置键词表由平台层从 `data/spec` 读入；文件系统 / 路径 /
  环境变量全走 `Host`，IO **全部是 `Promise`**）⇒ 核心能整份跑在浏览器 / worker 里，`data/spec` 只有平台层认识。
  ★ **为什么 `apps/emulator` 要两份 tsconfig（核心 / 前端）**：核心那份是 `"types": []`（**守卫本身**），
  而前端（`apps/emulator/frontends/**`）的**全部职责**就是"把 `node:fs` 接上核心定义的接口" ⇒ 它必须有 `"types": ["node"]`。
  两者不能共用一份：要么核心的守卫失效，要么前端写不了 `node:fs`。
  ★ 前端工程 `include` 只有 `frontends/**`，但通过 import 会把 `src/**` 一起拉进来检查 —— 这是**有意的**。
  ★ **核心的落点是 Electron renderer**（2026-10 架构调整）：`src/core/**` 的源码仍然无独立构建，
  但它作为 renderer 的依赖由 `vite build` 一起打包；main / preload 不再运行 core。
  ★ **唯一需要构建的入口是 Electron renderer**：`apps/emulator-new` 的
  `src/platform/electron/renderer/**`（含它 import 的 core）由 `vite build` 产出 `dist/renderer/**`（生成物、已 gitignore）。
  `erasableSyntaxOnly` 把"Node 剥壳不支持的语法"提前变成类型错误。
  ★ **模拟器的工程会把 `age-format` 的源码一起拉进来**（它 `import` 它）—— 这是**有意的**。
  ★ `packages/age-format` 的 tsconfig 需要 `@types/node`（**工具侧**用 `Buffer` / `node:fs` / `process`）。
  ★ `apps/emulator` 那份是 **`"types": []`** —— 这一行就是**"核心不许碰 Node"的守卫本身**：
  模拟器核心将来跑在**浏览器**里，`Buffer` 要 polyfill、**`node:fs` 没有 polyfill 可打**。
  ⇒ 用**类型系统**而不是"扫源码的测试"来守。实测：往核心的可达闭包加一行 `Buffer.alloc(1)` ⇒ `Cannot find name 'Buffer'`；
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
  `@types/web` 是 `lib.dom.d.ts` 的**同一份生成物**（仍只有 DOM）。
* 包管理**用 pnpm**（`pnpm-workspace.yaml` 是 workspace 真源）。禁止混用 `npm install` 生成 `package-lock.json`。
  ★ **结构类设置只写 `pnpm-workspace.yaml`，不写 `.npmrc`**：`.npmrc` 只读 auth 与 registry；
  定义 `node_modules` 结构的键写在那里会在 pnpm 11 起**静默失效**。
  ★ **不写 `nodeLinker`**（用默认 `isolated`）、**不写 `shamefullyHoist`**（默认就是 `true`，写了不生效）。
  ★ **`storeDir: '.pnpm-store'`**（相对路径，相对 workspace 根）⇒ 可移植，且**落在工作区内** ⇒ store 本身不需要提权。
  ★ **但 Windows 上"装依赖"这一步仍要提权**：pnpm 建的是**目录符号链接**
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
  ⇒ **为什么**（`isolated` vs `hoisted` 的实测对照、store 落点、提权边界）见 `docs/00-origin/decisions.md` §7.1。
* `.NET`(C#) 与 `native`(C++/CMake) 各自独立工具链，**不进 npm workspaces**。

---

## 存档 §5 · 原「怎么跑」里的**非 lift / 非翻译**命令（spec / shape / 语料 / 格式 / 旧仓）

```bash
pnpm tools corpus validate      # ★ 素材清单守卫（红了就必须修，不是"看看"）
pnpm tools corpus scan --write  # 补 origin[].sha256（唯一写入口）
pnpm tools corpus list          # 条目与去向
pnpm tools fixtures list        # 存档样本：槽 / 定位 / mtime 漂移
pnpm tools opcodes report       # 指令表对账：旧表条目数 / 将丢弃哪些知识层字段（handler / status）
pnpm tools opcodes derive --write # 指令表派生（旧表 → 格式层四列；唯一写入口，缺省 dry-run）
pnpm tools disasm verify        # 反汇编语料保真断言（逐行反解回字节必须与源逐字节相同）
pnpm tools disasm-at describe   # 逐 EA 定位与切片（不做语义）
pnpm tools shape claims --sample # ★ 对象 shape：claims 校验（`pnpm tools shape describe` 看全部动作）
pnpm tools spec validate        # ★ spec 层：不变量（规范自包含 / 章节规则与 bindings·audit 一一对应 / 试点未登记义务 = 0）
pnpm tools spec report          # ★ 审计账：effect 覆盖 / 内部省略 / 残差（不给百分数；范围闭合 ≠ 解释正确）
pnpm tools spec residuals       # ★ 残差地图：子系统通道 × 类别（A=正文弃权 / B=消费者缺失 / C=知识缺口）
pnpm tools spec effects --pilot # ★ 从语料机械枚举 effect（覆盖判定输入；不是"整条指令"）
pnpm tools ledger report        # 知识台账
pnpm tools ledger domains       # 知识台账域分布
pnpm tools old-repo inventory   # 重新实测旧仓 → docs/00-origin/old-repo-inventory.md
pnpm tools requirements serve   # 项目工作台（需求 + AGE 脚本），项目在 apps/workbench/
pnpm --filter @amayui/workbench verify # 工作台门禁：typecheck + build + smoke
pnpm test:assets                # 只跑要 LFS/游戏安装的那档
pnpm test:all                   # 全部（含 @env external：旧仓/真机）
```

★ 这些命令**仍然存在且仍要绿**（`pnpm test` 里的既有 spec 守卫照跑，防冻结件被无意改坏）；只是它们不再属于当前工作线。

---

## 存档 §6 · 知识准入门（全文）

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

---

## 存档 §8 · 只读素材消费规则（全文）

* `corpus/assets.json` 是 **lockfile 性质**的清单（记录"来源与去向"），**不是**约束；
  唯一能把它变成约束的是 `pnpm tools corpus validate` 必须红 —— **没有守卫的清单等于一份 Markdown**。
* 素材按 `storage` 分四种去向：`lfs`（入库走 LFS）/ `git`（入库纯文本）/ `external-only`（留在仓库外，只登记）/
  `deferred`（后续批次才处理）。**不要在 `external-only` 的素材上"就地修改"**。
* 反汇编语料：`readOnly`，**锚点锚二进制 EA**，语料只提供 `EA → 当前这份导出里的行号` 映射。

---

## 存档 §9 · 原「不要做的事」里与存储 / 台账 / 知识层相关的条目

* ❌ 把 `.sqlite` 提交进 git / ❌ 用 LFS 存 DB / ❌ 把 DB 当唯一存储 /
  ❌ 用 `sqlite3 .dump` 当文本真源 / ❌ 让 DB 参与写事务再"导出"成文本。
* ❌ 把旧仓的**知识文档**"顺手"复制进 `docs/`；❌ 在 `data/ledger/` 里塞旧条目。
  ★ **例外只有一处、且口径相反**：**翻译参考资产**（术语表 / 角色语气 / 剧情联动）按
  **只读快照 + 显式"未复核"口径**迁进 `docs/01-translation/ref/`，并由守卫钉住字节保真 ——
  理由见 `docs/01-translation/README.md` §5。引擎语义结论仍然只能走 §6 准入门。
* ❌ 手工编辑 `corpus/assets.json` 的 `sha256` / 大小：**用 `pnpm tools corpus scan --write`**（那是唯一写入口）。
* ❌ 在 `corpus/assets.json` 里写体积、入库件校验和、LFS oid、`status`、`generatedAt`：那些 git / LFS / 文件系统已经是权威。

---

## 存档 §10 · 原「文档纪律」里被压缩掉的推导（保留规则，理由在这里）

* **守卫的门槛（现行口径）**：只有"**变了就是 bug、而且变了是静默的**"才配一条守卫。
  **观察**（"我们测到过 X"）· **缺陷史**（"曾经有 bug Y"）· **实现取舍**（"我们为什么这么写"）
  一律进 `docs/03-emulator/`，**不进 test**。三问全答"是"才写守卫：
  ① 失败是否意味着**用户可见的错或数据损坏** · ② **会不会有人发现**（崩 / 黑屏 / 构建红 / `git diff` 看得见 ⇒ 不用守）·
  ③ **删掉它，会不会让某个错误静默通过**。判据全文见 `docs/03-emulator/README.md` §0.1。
  ⇒ "没有守卫的清单等于一份 Markdown"那句话**只对机器可校验的清单成立**；它**不**推出
  "每个观察都该配一条用例"，也不推出"内部实现细节要锁住"。
* **"结构化数据不进散文"**：文件清单 / 用途 / 定位 / mtime / 哈希一律**只留一份结构化真源**，README **不列表、不抄数**。
* **"JSON 是不透明数据"**：任何 JSON 的字段语义 / 枚举 / 不变量 / 操作**只看它的控制脚本的自描述**
  （`pnpm tools corpus describe`、`pnpm tools fixtures describe`），
  ❌ **不要在文档里复述 schema**（那是第二份 schema，必然漂）；文档只写"它是什么 + 非显然口径 + 指向"。
* **同名说明书**：每个**自有**的结构化数据文件旁边必须有同名 `.md`（`foo.json` ↔ `foo.md`），
  且必须含 `## 怎么查` / `## 怎么改` 两节并**指向 `--describe`** —— 看到一个 JSON 就知道去哪看怎么处理它。
  生态文件（`package.json` 等）不在范围内。由 `tools/test/json-docs.test.mjs` 守。
  范例：`corpus/assets.md`、`corpus/fixtures/samples.md`。
* **测试只测两类东西**：① **基建契约** —— 守卫能不能红 · 写路径会不会写坏 · 发布物是否满足不变量 · 约定是否齐全；
  ② **对外边界合同**（L1：调用方看得见的结果，真源是**微软文档** + `docs/03-emulator/contracts-win32.md`）。
  ❌ 不测业务结论，❌ 不复述代码逻辑，❌ 不锁**内部实现取舍**（堆策略 / 缓存键 / 批合并数 ⇒ `design-notes.md`），
  ❌ 不断言数据的当前取值（那是 `pnpm tools corpus validate` 当哨兵的事）。
  ⇒ 判断一句话：**这条断言在换一份实现之后还需不需要成立？** 不需要 ⇒ 它不该是 test。
* **两处例外**：① **生成物**（整篇都是状态，但由脚本生成 + 文件头写明"别手改"），范例 `docs/00-origin/old-repo-inventory.md`；
  ② **面向用户的发行文本** —— `release/CHANGELOG.md` 与 `release/安装说明.md`：它们是**产品的一部分**，
  不是内部沿革。规格的真源是 `release/README.md` §4。
* **怎么查（真源清单）**：**进度与"还要做什么" = `pnpm tools requirements plan`（需求树 `data/requirements/`）**·
  入库进度看 `corpus/assets.json` 的 `storage`/`dest`（`deferred` → `lfs` 就是进度）·
  条目与去向 `pnpm tools corpus list` · 语料保真 `pnpm tools disasm verify` · 旧仓数字 `pnpm tools old-repo inventory` ·
  变更历史 `git log` / `git log -L`。

---

## 存档 §11 · 项目本体：交付物是 spec，不是 emulator（全文）

★ **2026-11-10 起 spec 层短期冻结（用户口径）**：`data/spec/` **暂不维护** —— 不新增规则、不修订、不做 spec 审计 / 复审 / 残差收敛 / 语义登记；
由 spec 派生的待办一律 **drop**（`dropped_reason` 指向本冻结）。重启时**整体重审**（不沿用当前审计结论），并从 git 恢复被 drop 的单。
原因：当前实现线是 `apps/emulator-lift`（执行原机器码），spec 对 lift 只作调试对照、不是真源。
★ 这不改变下一条的**长期定位**（交付物仍是 spec），只是本轮不做；`pnpm test` 里的既有 spec 守卫仍照跑（防冻结件被无意改坏）。

* 主交付物是 **spec（引擎规范）**（落点 `data/spec/`，工具 `pnpm tools spec`）；**emulator 是示范与测试项目**
  （cleanroom rewrite，类比 Wine —— 但这里的界面是**未文档化**的私有语义 ⇒ 理解是唯一路径）。
  ⇒ ⛔ **emulator 不许当 spec 的真源**；spec 也不许依赖 emulator 的实现细节（shape 已按此守：`⛔ 不读 apps/emulator`）。
* **当前唯一在维护的模拟器实现是 `apps/emulator-lift`**（执行 AGE.EXE 原机器码的 guest-state 模拟器）。
  `apps/emulator-cxx` 是按 spec 重写的对照实现：保持可执行、可作语义对照，但**新功能不落那里**；
  `apps/emulator` 与 **`apps/emulator-new` 都已废弃**（只读遗产，同级）：
  ⛔ 不新增功能、不补守卫、**不为它们改 spec**、不因为"另一套实现没同步"去改它们。
  判据：新工作（实现 / 守卫 / CTest / 变异）只落 `apps/emulator-lift`；新规则的 `evidence` **优先挂它的顶层 CTest**，
  `tools/test/emulator-new-*.test.mjs` 只能当**辅助**证据、不许当唯一证据（那是冻结件，改它等于解冻）。
  ★ `apps/emulator-cxx` 的既有 CTest / spec 证据照旧保留（仍可执行、`spec validate` 仍绿）；
  要迁移是**单独一条单**，⛔ 不顺手删证据。
  ★ 冻结件仍留在 `pnpm typecheck` / `pnpm test` 里，是为了**发现"冻结件被无意改动"**，不是为了让它继续演进。
  为什么与边界：`docs/00-origin/decisions.md` §8.1。
* **两层本体，之间只走指针**：偏移 / 布局 / 宽度 / 数组 / 池基点 / 步长 = **机器本体（shape，实现细节）**；
  实体 / 状态 / 操作 / 规则 = **领域本体（spec）**。两者之间只允许 `storage` 这类**指针**，⛔ 不许把布局内联进 spec。
* **平台 API 是 spec 的边界**：spec 只写到"引擎观察到的外部行为"（请求 + 参数 + 时序），⛔ 不写 Win32/D3D 细节；
  平台实现可替换，逐位一致进 `discards`。
* **措辞纪律**：说"与所有可获得的独立证据不矛盾、且能被独立实现"，⛔ **不说"与真机等价"**（带壳不可 trace，人工观测只是采样）。
  ★ 且必须记住：**投影一致 ≠ 与引擎一致** —— 多路推导只能发现内部矛盾与遗漏，发现不了共同误解。
* 口径与"为什么"：`docs/00-origin/project-intent.md`（本文件不复述）；
  "如何把汇编整理成 spec"的**方法**真源是需求树里的 spec 层单（`pnpm tools requirements list` 里标题含 "spec 层"）。
