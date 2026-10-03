# apps/workbench/ — 项目工作台（需求 + AGE 脚本反汇编）

`data/requirements/`（需求树与进度）与 `data/translations/patch.json`（AGE 脚本的反汇编视图）
的一层 HTTP + 浏览器视图。它是**只读**的：本服务**没有任何写路径** ——
改台账只有 `pnpm tools requirements set --write`，改 patch 只有 `pnpm tools patch edit --write`。

> ★ 这不是 DSH 插件，也和 DSH **没有任何关系**：不引 DSH 的包 / 主题 token，也不要求 DSH 在跑。
> 做成网页只是因为"切 tab 比切应用快"。
>
> ★ 它是 `tools/lib/*` 那套模型的**第三个消费者**（前两个是 `requirements plan` / `patch view`）：
> 树、聚合、引用解析、小节切分、反汇编正文**全部**来自模型，这里一行都不重抄。

## 怎么跑

```sh
# ① 只跑服务端（需要先构建客户端，否则 / 会说"还没构建"）
cd apps/workbench
node scripts/vite-cli.mjs build        # → dist/web/（生成物，已在 .gitignore 的 dist/ 里）
node server.ts                         # → http://127.0.0.1:7788/

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
（基线根里最大的那支 + 有变更的一支 + 没有条目的一支 + **名字过滤器没命中的四支**）；④ 只读（写动作一律 `405`）与边界
（未知引用 / 非法 kind / 不是脚本的 BIN）；⑤ **只要旧 `apps/requirements/` 还在**，就把两个服务的
`/api/tree`、`/api/node` 响应**逐字段对照**（旧目录按需求单退役后这一步自动跳过）。

页面里怎么用：

| 想做什么 | 怎么做 |
|---|---|
| 看整棵树与进度 | `#/`（需求） |
| 找一条 | 顶部**搜索**：短名 / 标题片段 / 完整 id 都行（命中项的祖先会作为上下文留着） |
| 只看看未收口的 | 顶部 `只看未收口`（默认开） |
| 看某一条的细节 | 点行的**标题**或**短名** → `#/req/<ref>`（行里没有单独的「打开详情」按钮：标题就是入口） |
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
- **依赖必须扁平（根 `.npmrc` 的 `shamefully-hoist`）**：pnpm 默认布局用 junction 链到 `node_modules/<包>`，
  而本机的 `fs.realpathSync` **不解析 junction** ⇒ `vite` 里 `import 'rolldown'` 会 `ERR_MODULE_NOT_FOUND`。
  hoist 之后依赖真的躺在 `node_modules/<包>` 下，解析不再依赖 realpath。代价是放弃了
  "依赖不可提升"的严格性（本仓其余部分没有运行期依赖，所以代价目前为零）。
- **Monaco 是瘦引入**：`monaco-editor/editor/editor.api.js`（只有 API）+ `monaco-editor/features/register.all.js`
  （编辑器**功能**：查找 / 折叠 / 多光标 / 跳行…，**不含** `languages/**`）+ `editor/editor.worker?worker`。
- **Monaco 还是懒加载的**：`MonacoViewer.vue` 里用 `await import('../monaco')`，Vite 因此把它切成单独的 chunk
  —— 构建产物是 `index.js`（外壳 + 需求视图，约 90 KB）＋ `monaco.js`（约 3.9 MB，gzip 约 1 MB）。
  ⇒ 打开"查看需求"不下载编辑器；只有真的进"脚本"页才下。
  ★ **不要**把那个 `import` 提到模块顶层（那会退化成"每个页面都先下几 MB"）。
  ★ 布局（`.monaco-wrap` 的 `flex: 1` / `min-height: 0`）在组件的 scoped 样式里，别在 `styles.css` 再写一份。
  子路径按 monaco 0.57 的 `exports`（`"./*": "./esm/vs/*.js"`）解析 ⇒ 是
  `monaco-editor/editor/editor.api.js`，**不是** `monaco-editor/esm/vs/…`（后者会映射成 `esm/vs/esm/vs/…`）。
  全量入口会把 ts/css/html/json 的语言 worker 一起打进来（`ts.worker` 是 MB 级的），这里不要。
- **服务端是 `server.ts` 由 Node 直跑**（v24 原生 type stripping）⇒ 服务端**零构建**；
  因此只用**可擦除**语法（无 `enum` / `namespace` / 装饰器 / 参数属性），相对 import 带扩展名。
  这是 `AGENTS.md` §3 里列明的例外之一。
- **静态资源是白名单**：`dist/web/` 下的文件在启动时列成一张表，请求路径**只当查表的键**，
  从不参与拼路径 ⇒ 没有 `..` 这一说。dev 模式完全不管静态（页面由 Vite dev server 出）。
- **不写状态数字**：本文件只写口径与"怎么查"。要知道现在有多少节点 / 多少支脚本，
  看 `pnpm tools requirements plan`、`pnpm tools patch status`、或页面本身。

## 端点（全部 GET，全部只读；`Cache-Control: no-store`；出错一律 JSON `{ ok:false, error }`）

| 端点 | 用途 | 形状 |
|---|---|---|
| `/api/tree` | 需求树 + 表头 + 预算 + 标记表 + 自描述 | 与旧 `apps/requirements/server.mjs` **逐字段相同** |
| `/api/node/<ref>` | 一条需求：字段 + 正文小节 + 父链 + 直接子 | 同上（`ref` 认完整 id / 唯一前缀 / 唯一后缀） |
| `/api/scripts` | **全部可反汇编的 AGE 脚本**一览（「旧管线标注过」只是标签；**只算元信息**，不含正文） | `{ name, baseFrom, baseBytes, annotated, hasPatch, opCount }[]`（在 `scripts` 下）+ 汇总 `{ count, annotated, hasPatch, unchanged, nonScript, source: 'baseline' }` |
| `/api/script/<name>?kind=data\|src` | 一支脚本的反汇编正文（名字有没有被标注、在不在 patch 里都不影响） | `{ name, kind, text, bytes, rows, stats, baseFrom, baseBytes, hasPatch }` |
| `/api/health` | 轻量自检（节点数 / patch 脚本数 / 两个根 / 缓存命中数） | —— |
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
- **只有回环**：默认 `127.0.0.1`。绑到非回环**必须显式** `--host`（否则等于把仓库内容放到局域网上）。
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
├── server.ts             # node:http；五个只读端点 + 白名单静态；Node 直跑（无构建）
├── smoke.ts              # 离线自检：同进程 listen(0) + fetch，对真源逐条核对
├── src/
│   ├── main.ts App.vue styles.css        # 外壳（导航 / 主题）+ 自己的一份样式（不引 CDN / UI 库）
│   ├── api.ts data.ts router.ts theme.ts # 取数 / 缓存 / hash 路由 / 浅深色
│   ├── monaco.ts                         # ★ Monaco 的瘦引入（见上）
│   ├── components/{CopyId,MonacoViewer}.vue
│   └── views/{RequirementsOverview,RequirementDetail,ScriptsView}.vue
└── dist/                 # 构建产物（不入库）
```

**为什么在 `apps/` 而不是 `tools/`**：`tools/test/layering.test.mjs` 要求每个 `tools/*.mjs` 只能 import
`./lib/*`，所以 `tools/` 下的 CLI **不能** import 本项目；反过来本项目 import `tools/lib/` 是允许的
（测试不扫 `apps/`）。启动器 `tools/requirements.mjs --serve` 因此只做一件事：**spawn 本项目的 `server.ts`**
（用 `process.execPath`，`stdio: 'inherit'` —— 不捕获输出，沙箱里捕获会 `EPERM`）。

## 已知边界（不做 / 未验）

- **不写**：端点是只读的，任何写动作一律 `405`；永远不写 `data/`。改数据只有 CLI。
- **不做完整 Markdown 渲染**：正文只在 `## ` 处切小节（台账自己的写法，切法在模型里），其余按纯文本排版。
  引一个 Markdown 库就要顺带背"要不要 sanitize"的问题，不值得；需要时先加一条判据。
- **类型门禁只在 `src/`**：`pnpm typecheck`（`vue-tsc --noEmit`）覆盖客户端。★ 它**必须**配 `typescript@5.x` ——
  曾经装到 `typescript@7`（原生端口），而 `vue-tsc@3` 要从 `typescript/lib/tsc` 进去，那个子路径在 TS 7 的
  `exports` 里不存在 ⇒ `ERR_PACKAGE_PATH_NOT_EXPORTED`。所以这里把 TS 钉在 `~5.9`（升 TS 主版本前先跑一遍 `typecheck`）。
  **服务端 `server.ts` 不在门禁里**（它由 Node 的类型擦除直跑，`tsconfig.json` 的 `include` 也不含它）；
  它靠 `smoke.ts` 的行为断言兜底。
- **未做视觉验证**：`smoke.ts` 只断言 HTTP 与数据一致性，**没有**浏览器/截图手段，
  所以"页面看起来对不对"（布局、浅深色、中键开新 tab、Monaco 的滚动手感）**没有被自动验证过** ——
  那些是开一次页面就能确认的事，而 agent 开不了浏览器。
- **大文件的边界只在服务端量过**：基线根里最大的那几支服务端能出全文且缓存命中（规模见
  `pnpm tools patch status`），浏览器侧只做了"Monaco 虚拟滚动 + 关掉最贵的装饰"这一层配置，
  **没有实测帧率**；脚本一览那侧另加了"只渲染前 N 行"的上限（N 与"还有多少行没渲染"写在页面上），
  也**没有**在浏览器里量过滚动/筛选手感 —— 那是开一次页面就能确认的事。
- **不缓存需求树**：每次请求重读节点文件（预算是 ≤40 节点 / ≤80 行，整棵树连正文几十 KB），
  比"缓存什么时候失效"简单得多。脚本侧在进程内按 `mtime+size` 缓存：**基线根**、patch 文档、
  一览（名字 + 标注/有变更标签 + 字节数）、以及每支的正文（正文缓存见上）。
  ★ 一览的口径是"基线根里每个 `.BIN` 都读一遍"（体积与耗时由 `/api/health` 与页面自己报），
  所以它**必须**缓存，判据是 patch + 基线根的 `mtime+size`（与正文缓存同一条纪律：看见真源）。
- **没有 `--open`**：不自动弹浏览器，启动时打印地址。

## 怎么查 / 怎么改

```sh
node smoke.ts                            # ★ 离线自检（真源一致 + 只读 + 边界）
node scripts/vite-cli.mjs build          # 构建（必须走这个入口）
node server.ts --help                    # 端口 / 主机 / dir / dev
node server.ts --port 7788               # 起服务后 GET /api/scripts：名单与汇总（口径与数字都现算）
pnpm tools requirements plan             # 进度真源（本页显示的数字都来自它用的那份函数）
pnpm tools patch status                  # patch 规模与分布
pnpm tools requirements describe         # 需求台账的字段 / 不变量（本文件不复述）
pnpm tools patch describe                # patch 的字段 / 不变量 / 操作（本文件不复述）
```

```sh
node tools/requirements.mjs --serve      # 脱离派发器直接起服务（等价于 pnpm tools requirements serve）
```

**怎么改**：改客户端 → `node scripts/vite-cli.mjs build` 之后**刷新页面**（或者用上面的 dev 双进程 + HMR）；
改 `server.ts` 要**重启进程**。新增端点时记住两条纪律：**只读**（写路径一律不给）与
**不重造轮子**（数据一律从 `tools/lib/*` 取，别在这里再写一份树 / 聚合 / 反汇编）。

设计评估与支撑观测：`docs/01-translation/patch-design.md`（`data` / `src` 为什么都是视图、
锚为什么是基线行序、为什么 BIN 不能反推中文）。
