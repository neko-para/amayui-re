# apps/workbench/ — 项目工作台（需求 + AGE 脚本反汇编）

`data/requirements/`（需求树与进度）与 `data/translations/patch.json`（AGE 脚本的反汇编视图）
的一层 HTTP + 浏览器视图。

**读**：GET 一律不改数据（payload 里的 `readOnly: true` 就是"这个端点不改数据"）。
**写**：只有一条 `POST /api/nodes`（在工作台里**新建一张需求单**），而且有三重限制 ——
① 只在**监听回环**时开（绑到局域网等于把写权限交出去）② 只收 `Content-Type: application/json`
（跨站页面发不出这种"简单请求"⇒ 必须先过预检，而本服务**从不回 CORS 头**）③ **只新建**
（改已有节点仍然只有 `pnpm tools requirements set --write`）。
★ 建单的规则**不在服务端**：它和 `pnpm tools requirements add` 共用模型里的 `planAdd()`
（`tools/lib/requirements.mjs`）—— 所以"网页建的"与"命令行建的"不可能长得不一样。

> ★ 这不是 DSH 插件，也和 DSH **没有任何关系**：不引 DSH 的包 / 主题 token，也不要求 DSH 在跑。
> 做成网页只是因为"切 tab 比切应用快"。
>
> ★ 它是 `tools/lib/*` 那套模型的**第三个消费者**（前两个是 `requirements plan` / `patch view`）：
> 树、聚合、引用解析、小节切分、反汇编正文、**建单**全部来自模型，这里一行都不重抄。

## 怎么跑

```sh
# ① 只跑服务端（需要先构建客户端，否则 / 会说"还没构建"）
cd apps/workbench
node scripts/vite-cli.mjs build        # → dist/web/（生成物，已在 .gitignore 的 dist/ 里）
node server.ts                         # → http://127.0.0.1:7788/（启动时打印"建单：开/关"）

# ② 经仓库派发器（薄启动器，只是 spawn 上面的 server.ts）
pnpm tools requirements serve           # 同一个地址
pnpm tools requirements serve --port 7800

# ③ 改前端（HMR）：两个进程一起开
node server.ts --dev                    # 只管 /api/*
node scripts/vite-cli.mjs dev           # → http://localhost:5199/（它的 proxy 把 /api 转给 7788）
```

自检（同进程 `listen(0)` + `fetch`，**不 spawn**）：

```sh
node smoke.ts                           # 断言的"真源一致"见文件头
pnpm typecheck                          # 客户端类型门禁（vue-tsc --noEmit）
pnpm verify                             # typecheck + build + smoke 一条龙
```

它断言的是**真源一致性**，不是"代码看起来对"：① `/api/tree`、`/api/node` 与 `tools/lib/requirements.mjs` 的
`flatten` / `rollup` / `describeNode` **逐条相等**；② `/api/scripts` 的名字集合与模型的 `allScriptNames()`
（= 基线根里**所有能反汇编的 AGE 脚本**；候选 `.BIN` 与签名门槛都在模型里）**逐名相等**、
汇总（`annotated` / `hasPatch` / `unchanged` / `nonScript`）**与真源现算一致**（自检里不写死数字）；
③ `/api/script` 的 `text` 与直接调 `buildView` **逐字节相等**
（基线根里最大的那支 + 有变更的一支 + 没有条目的一支 + **名字过滤器没命中的四支**）；④ 边界
（未知引用 / 非法 kind / 不是脚本的 BIN / 方法用错）；⑤ **写路径**：写在**临时台账副本**上
（自检里同时断言真的 `data/requirements/` 一个文件都没多），断言"服务端没有自己的建单规则"
（表单枚举与模型逐项相等）、被拒的单子**不留残 file**、以及三道门（只在回环 / 只收 JSON / 不吐 CORS 头，
含"绑 `0.0.0.0` ⇒ 建单 403"这一条真的把门关上）；⑥ **Markdown 渲染口径**：直接 import `src/markdown.ts`
断言那四条（原始 HTML 按文本 / 裸文本不变链接 / 仓库内路径不做成可点链接 / 小节小标题降两级）；
⑦ **与旧版服务的读端点逐字段对照**：旧 `apps/requirements/`（无构建的单页看板）**已退役删除**，
所以这一步现在是**自动 skip**；要复现它，从 git 历史取回那一版旧目录再跑（`smoke.ts` 会自己认出来）。
（写路径是新增能力，旧目录没有对应的东西。）

页面里怎么用：

| 想做什么 | 怎么做 |
|---|---|
| 看整棵树与进度 | `#/`（需求） |
| 找一条 | 顶部**搜索**：短名 / 标题片段 / 完整 id 都行（命中项的祖先会作为上下文留着） |
| 只看看未收口的 | 顶部 `只看未收口`（默认开） |
| **记一笔（新建需求 / 缺陷）** | 顶部 `＋ 新建需求单`（或某条详情页的 `＋ 子需求单`，父节点会**预选**成那一条）。**只问三件事**：一句话 · 类型 · 挂在哪条下面（缺陷多一项**复现**）。★ 这是刻意的：**先记一句话，细节之后用短名补**（`pnpm tools requirements set <短名> …`）。正文不填 ⇒ 服务端按类型给一份骨架。守卫不绿时对话框把**逐条不变量问题**列出来，磁盘上**没有留下半成品** |
| 看某一条的细节 | 点行的**标题**或**短名** → `#/req/<ref>`（行里没有单独的「打开详情」按钮：标题就是入口） |
| 正文怎么显示 | 详情页把每个 `## 小节` 的正文按 **Markdown 渲染**（粗体 / 表格 / 列表 / 行内代码 / 引用 / 代码块）；原始 HTML **按文本显示**（口径见 `src/markdown.ts`） |
| **复制完整 id** | `⧉` 按钮 —— 复制的是 **26 位完整 id**（粘进终端 `show <id>` 用），**不是**显示用的短名 |
| 看脚本一览 | `#/scripts`：左列是**全部可反汇编的 AGE 脚本**（**「旧管线标注过」只是标签**），可按名字搜索，并按 `全部 / 只看标注过的 / 只看有变更` 筛选；每行标着「旧管线标注过 / 有变更 N op」、基线来源与字节数 |
| 列表很长时 | 左列**只渲染前 N 行**（N 写在页面上，超过就有一条明写"还有 M 行没渲染"的提示）—— 剩下的用搜索 / 筛选收窄。这个上限是**刻意**的：上千行 DOM 会让滚动发涩 |
| 看某一支的反汇编 | 点左边的名字 → `#/script/<名字>`；`data` / `src` 是**两个真链接**（中键开新 tab）。名义上有没有被标注过、有没有 patch 条目都**不影响能不能看** |
| 在正文里找 / 跳 | Monaco 自带 `Ctrl+F` 查找、`Ctrl+G` 跳行；工具栏还有一个"跳到行"输入框 |
| **并排看两条** | 中键 / `Ctrl`(`Cmd`)+点击任意行 —— 浏览器原生开新 tab |
| 往回走 | 浏览器**后退键**（hash 路由，不用 History API） |

## 非显然口径（为什么是这样）

- **hash 路由 + 每行是真 `<a href>`**：hash **不发给服务器** ⇒ 不需要 History API、不需要服务器
  rewrite，而"中键开新 tab"是白送的。引用解析复用台账自己的规则（完整 id / 唯一前缀 / **唯一后缀**）
  —— 与 `pnpm tools requirements show` 完全同一套（`makeResolver`）。ULID 以时间开头、同批前 10 位相同，
  所以短名才是人能用的那个句柄。
- **`data` 与 `src` 都是视图，真源只有基线与 patch**：`data` = 基线 BIN 的反汇编；
  `src` = 基线 + patch，但**字符串取 patch 里的中文**。★ 不能拿"重建 BIN 的反汇编"当 `src`：
  BIN 里存的是**占位写法**（简体字多半编不进 cp932，要靠同码位日文写法占位），
  BIN → 中文**不可逆**。所以 `src` 视图里出现中文、而 BIN 里是别的写法，**不是 bug**（见 `patch-design.md` §2.5）。
- **正文不在客户端加工**：`/api/script` 的 `text` 就是 `buildView()` 的输出，客户端一个字都不改
  （也不自己数行号 —— `rows` 用的是模型的 `rowsOf()`，与 patch 锚定同一个行序空间）。
- **服务端有一层视图缓存**（最多 8 支，LRU）：`disassemble + replay + assemble + disassemble` 对最大的几支
  要明显的 CPU 时间。失效判据**看见真源**：`patch.json` 的 `mtime+size` **加上**基线 BIN 的 `mtime+size`
  —— 只按"进程启动时间"缓存会让人改完 patch 之后页面一直显示旧的。
- **构建入口是 `scripts/vite-cli.mjs`，不是 `vite`**：受限沙箱里**不许 spawn 子进程并捕获输出**
  （要开命名管道 ⇒ `EPERM`）。Vite 在 Windows 启动时有一次 `exec('net use')` 探测映射网络盘，
  在沙箱里直接抛 `spawn EPERM`，构建在加载配置之前就炸。那个入口把 `exec/execFile` 换成"空结果"桩
  （结论与真实机器一致：本仓不依赖映射盘），**不改变产物**。⇒ 一律 `node scripts/vite-cli.mjs build|dev|preview`。
- **依赖布局**：本仓**不写 `nodeLinker`**（用 pnpm 默认的 `isolated`，2026-10 实测定案）。
  ⚠ 官方口径是 `.npmrc` 只读 auth/registry，结构类设置（`nodeLinker` / `shamefullyHoist`）**只能**写在
  `pnpm-workspace.yaml`，pnpm 11 起会忽略 `.npmrc` 的老写法。**当前该文件里只有 `storeDir`。**
  ★ 曾经写在这里的病因 —— ~~"`fs.realpathSync` 不解析 junction ⇒ `vite` 里 `import 'rolldown'` 会
  `ERR_MODULE_NOT_FOUND`"~~ —— **已被直接实验推翻**（在 `.tmp` 复刻 isolated 布局后从 junction 路径
  `import` 是**成功**的）。所以从未需要 `nodeLinker: hoisted`；实测加了它反而把依赖全摊到**根**上
  （91 个实体目录，遮蔽风险更大）。对照表与判据见 `AGENTS.md` §3。
  ⚠ `node_modules` 的实际形状由**上一次安装时的配置**决定（pnpm 不会替你清理遗留的树）；
  判据是 `node_modules/.modules.yaml` 里的 `nodeLinker` 值。
  ⚠ 数 junction 要用 node:fs 的 `lstatSync`：PowerShell 的 `Get-ChildItem -Directory` **不跟随也不显示** junction。
- **store 落点**：`pnpm-workspace.yaml` 的 `storeDir: '.pnpm-store'`（**相对路径**，相对 workspace 根）
  ⇒ 可移植，且在工作区内 ⇒ 受限沙箱下 `pnpm install` **不需要提权**。判据：`pnpm store path` 在工作区内。
  ⚠ workspace-write 下 `node_modules` 里的文件是**硬链接**（`nlink>1`）⇒ **删不掉**（实测数万条 `Access denied`）；
  换布局要 `Rename-Item` 让开，别指望删除，也别删 store。详见 `AGENTS.md` §3。
- **Monaco 是瘦引入**：`monaco-editor/editor/editor.api.js`（只有 API）+ `monaco-editor/features/register.all.js`
  （编辑器**功能**：查找 / 折叠 / 多光标 / 跳行…，**不含** `languages/**`）+ `editor/editor.worker?worker`。
- **Monaco 还是懒加载的**：`MonacoViewer.vue` 里用 `await import('../monaco')`，Vite 因此把它切成单独的 chunk。
  ⇒ 打开"查看需求"不下载编辑器；只有真的进"脚本"页才下。
  ★ **不要**把那个 `import` 提到模块顶层（那会退化成"每个页面都先下几 MB"）。
  ★ 布局（`.monaco-wrap` 的 `flex: 1` / `min-height: 0`）在组件的 scoped 样式里，别在 `styles.css` 再写一份。
  子路径按 monaco 0.57 的 `exports`（`"./*": "./esm/vs/*.js"`）解析 ⇒ 是
  `monaco-editor/editor/editor.api.js`，**不是** `monaco-editor/esm/vs/…`（后者会映射成 `esm/vs/esm/vs/…`）。
  全量入口会把 ts/css/html/json 的语言 worker 一起打进来（`ts.worker` 是 MB 级的），这里不要。
- **组件库是 `naive-ui`，而且只用在"通用控件"上**：按钮 / 输入 / 下拉 / 表单 / 对话框 / 消息条。
  需求树与脚本一览**仍然是自己的 markup** —— 那两处每个可点的东西都是**真 `<a href="#/…">`**，
  中键开新 tab 是白送的，`n-tree` 给不了这一点。
  ★ **样式来源有两处，别互相盖**：naive-ui 的样式由 css-render **在运行期注入**（不必 import CSS，
  也就不与 Monaco 的样式表排序打架），我们自己的配色仍然只有 `styles.css` 那一份 CSS 变量；
  所以**不要**引 `NGlobalStyle`（它会接管 body 的字体与底色，等于多出第二份全局样式）。
  ★ **组件库的明暗与我们那份 `data-theme` 同源**：`App.vue` 用同一个 `themeMode` 算出 `darkTheme`，
  两者不会一个深一个浅。
  ★ `<n-config-provider abstract>`：`abstract` 让它**不渲染包裹 div** —— 少了它 `#app` 那条 flex 链会多一层，
  `height: 100%` 就断在中间。`n-dialog-provider` / `n-message-provider` 本身就是 Fragment，不产生盒子。
- **建单对话框是懒加载的**（`defineAsyncComponent` + **首次打开才 `v-if` 挂上**）：它带着表单 / 下拉 / 弹窗那几组
  组件（naive-ui 里最重的一块），而"打开总览"用不到它们 —— 与 Monaco 同一条纪律。
  ★ 只写 `defineAsyncComponent` 是**不够**的：模板里一直有这个组件的话，那次 `import()` 会在**打开页面时**就发生，
  分块就等于白分。挂上之后**不再卸**（关掉对话框时保持挂载 ⇒ 关闭动画与滚动锁由 naive-ui 正常收尾）。
  **产物分成几个 chunk、各多大，以 `build` 的输出为准**，本文件不写死这些数字。
- **Markdown 渲染只有一份**：`src/markdown.ts`（`markdown-it`，`html: false`）。详情页用它渲染每个小节。
  ★ 口径四条（原始 HTML 按文本 / 裸文本不自动变链接 / **仓库内相对路径不做成可点链接** / 小节小标题降两级）
  写在那个文件的头注释里，并由 `smoke.ts` 直接 import 它断言 —— 改口径 = 改契约。
  ★ `html: false` 就是"不需要 sanitizer"的**理由**（不是"选了一个 sanitizer"）：markdown-it 在文本节点上转义
  `<`/`>`/`&`，标签只能由 Markdown 语法产生 ⇒ 输出里不可能出现正文写进去的标签。
  ★★ `markdown-it` 的 `entities` 被根 `package.json` 的 `pnpm.overrides` **钉在 `^7`**：本机是全扁平布局
  （见下），一个包名在树里只能有一个版本，而 `@vue/compiler-core` 要 `entities@7`、`markdown-it@15` 要 `8`
  ⇒ 根上 hoist 的那份会遮蔽 compiler 自己那份，`vue-tsc` 一遇到模板里的 `&lt;` 就崩。
  `markdown-it` 只用 `decodeHTMLStrict`，7 与 8 行为相同 ⇒ 钉 7，并**配一条断言**（smoke 里的实体解码哨兵）。
  换 Markdown 库或升它主版本前，先看 `.npmrc` 里那段"扁平布局的代价之二"。
- **服务端是 `server.ts` 由 Node 直跑**（v24 原生 type stripping）⇒ 服务端**零构建**；
  因此只用**可擦除**语法（无 `enum` / `namespace` / 装饰器 / 参数属性），相对 import 带扩展名。
  这是 `AGENTS.md` §3 里列明的例外之一。
- **写路径为什么只有一条、为什么规则不在服务端**：服务端只做三件事 —— 判"能不能写"（监听地址是不是回环）、
  判"是不是 JSON"、把请求体当**值**喂给模型 `planAdd()`。规则（id / parent / 缺省正文 / 写后守卫 + 回滚）
  只有 `tools/lib/requirements.mjs` 那一份 ⇒ 网页与 CLI 不可能建出两种形态的节点。
  ★ **自检绝不写真的台账**：它把 `data/requirements/` 复制到 `.tmp/` 下的临时目录再起第二个服务，
  跑完断言真目录一个文件都没多。
- **静态资源是白名单**：`dist/web/` 下的文件在启动时列成一张表，请求路径**只当查表的键**，
  从不参与拼路径 ⇒ 没有 `..` 这一说。dev 模式完全不管静态（页面由 Vite dev server 出）。
- **不写状态数字**：本文件只写口径与"怎么查"。要知道现在有多少节点 / 多少支脚本 / 自检多少项，
  看 `pnpm tools requirements plan`、`pnpm tools patch status`、`node smoke.ts` 的输出，或页面本身。

## 端点（GET 一律不改数据；`Cache-Control: no-store`；出错一律 JSON `{ ok:false, error }`）

| 端点 | 用途 | 形状 |
|---|---|---|
| `GET /api/tree` | 需求树 + 表头 + 预算 + 标记表 + 自描述 | 形状沿用旧 `apps/requirements/server.mjs`（该目录已退役删除，形状口径不变） |
| `GET /api/node/<ref>` | 一条需求：字段 + 正文小节 + 父链 + 直接子 | 同上（`ref` 认完整 id / 唯一前缀 / 唯一后缀） |
| `GET /api/scripts` | **全部可反汇编的 AGE 脚本**一览（「旧管线标注过」只是标签；**只算元信息**，不含正文） | `{ name, baseFrom, baseBytes, annotated, hasPatch, opCount }[]`（在 `scripts` 下）+ 汇总 `{ count, annotated, hasPatch, unchanged, nonScript, source: 'baseline' }` |
| `GET /api/script/<name>?kind=data\|src` | 一支脚本的反汇编正文（名字有没有被标注、在不在 patch 里都不影响） | `{ name, kind, text, bytes, rows, stats, baseFrom, baseBytes, hasPatch }` |
| `GET /api/health` | 轻量自检（节点数 / patch 脚本数 / 两个根 / 缓存命中数）**+ `writes`**：能不能建单、为什么、表单枚举 | —— |
| **`POST /api/nodes`** | ★ **唯一的写端点**：新建一张需求单（JSON 体 = `planAdd` 的 `spec`） | `201 { created: { id, name, short, title, file }, dir, plan, report }`；守卫不绿 ⇒ `422 { rolledBack: true, report }`（已回滚）；单子不合格 ⇒ `400`；非 JSON ⇒ `415`；非回环 ⇒ `403` |
| `GET /api/nodes` | 405（集合只收 POST；读一个节点用 `/api/node/<ref>`） | —— |
| `/` 及 `/assets/*` | 客户端（`dist/web/` 的白名单视图） | —— |

字段与不变量的**真源不在本文件**：`pnpm tools requirements describe` 与 `pnpm tools patch describe`。

- **`scripts` 的名单口径 = `allScriptNames()`**：基线根里**所有能反汇编的 AGE 脚本**
  （候选 `.BIN` 与签名门槛都在模型里）—— 名单里**没有**任何名字过滤这一层收窄。
- **「旧管线标注过」是标签，不是 patch 的范围**：`annotated` = `SPEAKER_FILTER`（`patch.mjs` 导出的
  那一条）对**名字**的判定，含义只是"旧仓 `scripts/annotate-speaker.js` 给这批做过**页 / 说话人标注**"。
  ★ 它曾被误当成"有译文的脚本集合"（还叫它「官方集」）并被拿来当 patch 的范围 ⇒ 范围被名字**静默收窄**，
  **漏掉了一批真译文**（非 `SC` / `SP` 的脚本里同样有"产物 ≠ 基线"的）。**已修**：范围由
  `allScriptNames()` 给全部脚本，"有没有变更"只由 **patch 有没有条目**（= 产物 ≠ 基线）判定。
  想知道范围有多大 / 现在多少条：`pnpm tools patch status`（条目数）与 `pnpm tools patch verify` 的
  `覆盖` 那行（范围内多少支 · patch 多少支 · 其余多少支视为无变更；有产物根时还会逐支复核
  "无条目 ⇒ 产物 == 基线"，**漏改了就会红**）—— 数字一律现算，本文件不写。
  标签与产物根无关 ⇒ 本服务**不依赖**旧仓 `install/`（`/api/health` 的旧 `target` 字段已废，
  改为报 `annotatedFilter`）。列表里的 `annotated` / `hasPatch` 与汇总（含**没有条目**的 `unchanged`）
  全部由 `/api/scripts` **现算**。
- **`/api/script/<name>` 不要求名字被标注过、也不要求在 patch 里**：基线根里能解析、签名是脚本就给正文。
  没有 patch 条目 ⇒ 空叠加层（`NO_OPS_ENTRY`）⇒ `src` 视图与 `data` 视图**逐字节相同**
  （"没有变更"不是错误，页头会写明）。名字不存在 / 不是 AGE 脚本 ⇒ `404` + 可读错误（两条**分开报**）。
- **`POST /api/nodes` 的请求体 = 模型 `planAdd` 的 `spec`**（`title` / `type` / `status` / `parent` / `order` /
  `tags` / `blocked_by` / `verify` / `repro` / `severity` / `done_reason` / `dropped_reason` / `supersedes` / `body`）：
  字段清单与枚举看 `/api/health` 的 `writes`（**枚举只有模型那一份**，服务端只是转发）。
  ★ 端点的**字段面**是完整的（CLI `--add` 与它同一套），但页面上的对话框**只发其中三个**（`title` / `type` / `parent`，
  缺陷再加 `repro`）—— 那是刻意的"先记一句话"，其余字段留给之后用短名补。**不要**因为对话框没用就把端点收窄：
  收窄的是界面，不是契约。
  ★ **不认得的键一律 400**（静默忽略错别字会让"以为挂上了父节点"变成一棵孤立子树）；
  ★ `parent` 不写 ⇒ 新节点没有父 ⇒ 守卫红 ⇒ 回滚（树"恰好一个根"）；
  ★ **只有新建**：改已有节点没有第二个调用方，规则就先留在 CLI（`--set`）里。
- **只有回环**：默认 `127.0.0.1`。绑到非回环**必须显式** `--host`（否则等于把仓库内容放到局域网上），
  而且那种情况下**写路径会关掉**（`/api/health` 的 `writes.why` 明说为什么，POST 直接 403）。
- **`--dev`**：只服务 `/api/*`，静态交给 Vite dev server。

## 项目结构

```
apps/workbench/
├── README.md             # 本文件
├── package.json          # 依赖已装；scripts 一律经 scripts/vite-cli.mjs（沙箱原因见上）
├── vite.config.ts        # outDir=dist/web；dev 的 /api 代理 → 127.0.0.1:7788
├── tsconfig.json         # 只覆盖客户端 src/ 与 vite 配置（`pnpm typecheck`；服务端不进，见"已知边界"）
├── index.html            # 外壳（真 <a href="#/…">；内容只由 Vue 渲染）
├── scripts/vite-cli.mjs  # ★ 构建 / 开发入口（把那次 net use 探测换成桩，见上）
├── server.ts             # node:http；GET 只读端点 + POST /api/nodes（唯一写端点）+ 白名单静态；Node 直跑（无构建）
├── smoke.ts              # 离线自检：同进程 listen(0) + fetch，对真源逐条核对（含"写在临时台账副本上"）
├── src/
│   ├── main.ts App.vue styles.css        # 外壳（导航 / 主题 / naive-ui 的 provider）+ 自己的一份颜色变量
│   ├── api.ts data.ts router.ts theme.ts # 取数（读 + 建单）/ 缓存 / hash 路由 / 浅深色
│   ├── markdown.ts                       # ★ 正文的 Markdown → HTML（口径写在文件头，自检直接 import 它）
│   ├── monaco.ts                         # ★ Monaco 的瘦引入（见上）
│   ├── components/{CopyId,MonacoViewer,NewRequirementDialog}.vue
│   └── views/{RequirementsOverview,RequirementDetail,ScriptsView}.vue
└── dist/                 # 构建产物（不入库）
```

**为什么在 `apps/` 而不是 `tools/`**：`tools/test/layering.test.mjs` 要求每个 `tools/*.mjs` 只能 import
`./lib/*`，所以 `tools/` 下的 CLI **不能** import 本项目；反过来本项目 import `tools/lib/` 是允许的
（测试不扫 `apps/`）。启动器 `tools/requirements.mjs --serve` 因此只做一件事：**spawn 本项目的 `server.ts`**
（用 `process.execPath`，`stdio: 'inherit'` —— 不捕获输出，沙箱里捕获会 `EPERM`）。

## 已知边界（不做 / 未验）

- **只新建，不编辑**：唯一的写端点是 `POST /api/nodes`（新建一张需求单）。改已有节点（`--set`）、
  改 patch（`patch edit`）**仍然只有 CLI** —— 规则留在模型里，没有第二个调用方就不往网页上搬。
  页面的"新建"按钮在写路径关掉时（绑了非回环）是**禁用 + 悬停说明**，而服务端那边是**真的 403**。
- **类型门禁只在 `src/`**：`pnpm typecheck`（`vue-tsc --noEmit`）覆盖客户端。★ 它**必须**配 `typescript@5.x` ——
  曾经装到 `typescript@7`（原生端口），而 `vue-tsc@3` 要从 `typescript/lib/tsc` 进去，那个子路径在 TS 7 的
  `exports` 里不存在 ⇒ `ERR_PACKAGE_PATH_NOT_EXPORTED`。所以这里把 TS 钉在 `~5.9`（升 TS 主版本前先跑一遍 `typecheck`）。
  **服务端 `server.ts` 不在门禁里**（它由 Node 的类型擦除直跑，`tsconfig.json` 的 `include` 也不含它）；
  它靠 `smoke.ts` 的行为断言兜底。
- **未做视觉验证**：`smoke.ts` 只断言 HTTP 与数据一致性（加"详情页确实调了渲染器"这类**文本级**检查），
  **没有**浏览器/截图手段，所以"页面看起来对不对"（布局、浅深色、对话框手感、naive-ui 的观感、
  中键开新 tab、Monaco 的滚动手感）**没有被自动验证过** —— 那些是开一次页面就能确认的事，而 agent 开不了浏览器。
  ★ 因此**不要去动布局链**：`#app` 的 flex 链靠 `<n-config-provider abstract>` 保持原样（见上），
  这是没有视觉验证时最容易踩坏的地方。
- **大文件的边界只在服务端量过**：基线根里最大的那几支服务端能出全文且缓存命中（规模见
  `pnpm tools patch status`），浏览器侧只做了"Monaco 虚拟滚动 + 关掉最贵的装饰"这一层配置，
  **没有实测帧率**；脚本一览那侧另加了"只渲染前 N 行"的上限（N 与"还有多少行没渲染"写在页面上），
  也**没有**在浏览器里量过滚动/筛选手感 —— 那是开一次页面就能确认的事。
- **不缓存需求树**：每次请求重读节点文件（预算是 ≤40 **活**节点 / ≤80 行，整棵树连正文几十 KB），
  比"缓存什么时候失效"简单得多。脚本侧在进程内按 `mtime+size` 缓存：**基线根**、patch 文档、
  一览（名字 + 标注/有变更标签 + 字节数）、以及每支的正文（正文缓存见上）。
  ★ 一览的口径是"基线根里每个 `.BIN` 都读一遍"（体积与耗时由 `/api/health` 与页面自己报），
  所以它**必须**缓存，判据是 patch + 基线根的 `mtime+size`（与正文缓存同一条纪律：看见真源）。
- **没有 `--open`**：不自动弹浏览器，启动时打印地址。

## 怎么查 / 怎么改

```sh
node smoke.ts                            # ★ 离线自检（真源一致 + 边界 + 写路径 + Markdown 口径）
pnpm verify                              # typecheck + build + smoke 一条龙
node scripts/vite-cli.mjs build          # 构建（必须走这个入口）
node server.ts --help                    # 端口 / 主机 / dir / dev
node server.ts --port 7788               # 起服务后 GET /api/scripts：名单与汇总（口径与数字都现算）
pnpm tools requirements plan             # 进度真源（本页显示的数字都来自它用的那份函数）
pnpm tools requirements describe         # 需求台账的字段 / 不变量 / **写路径**（本文件不复述）
pnpm tools patch status                  # patch 规模与分布
pnpm tools patch describe                # patch 的字段 / 不变量 / 操作（本文件不复述）
```

```sh
node tools/requirements.mjs --serve      # 脱离派发器直接起服务（等价于 pnpm tools requirements serve）
```

**怎么改**：改客户端 → `node scripts/vite-cli.mjs build` 之后**刷新页面**（或者用上面的 dev 双进程 + HMR）；
改 `server.ts` 要**重启进程**。新增端点时记住三条纪律：**GET 不改数据**（要写就先想清楚"规则在哪一份"）、
**不重造轮子**（数据与规则一律从 `tools/lib/*` 取，别在这里再写一份树 / 聚合 / 反汇编 / 建单）、
**新增写端点要有理由**（现在只有一个：网页建单，且规则本来就已经在模型里）。
Markdown 的渲染口径在 `src/markdown.ts` 文件头，改它等于改契约（`smoke.ts` 会红）。

设计评估与支撑观测：`docs/01-translation/patch-design.md`（`data` / `src` 为什么都是视图、
锚为什么是基线行序、为什么 BIN 不能反推中文）。
