# 项目工作台（网页）：需求 + AGE 脚本反汇编

- id: REQ-01M3XWRXVB04WQ4J0MA6AXZY9D
- type: req
- status: doing
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 40
- tags: [web, workbench, tooling]

## 范围
把 `apps/requirements/`（单页只读看板）**重建**为一个**通用项目工作台**：可成长的本地网页应用。首期两类内容：

1. **查看需求**：整棵树 + 进度 + 单条详情（旧能力，行为口径不变，重写在新技术栈上）；
2. **查看 AGE 脚本的反汇编正文**：**全部能反汇编的脚本**一览 → 打开某一支的全文（可搜索、可跳行、大文件不卡）。
   ★ 不是"官方集"——那个说法是错的（见 `docs/01-translation/patch-design.md` §2.3）：范围是**全部可反汇编的脚本**。

落点：**新目录 `apps/workbench/`**（旧 `apps/requirements/` 的退役见 §退役）。
它长出来的下一层（Markdown 渲染 / 组件库 / 网页建单）在子节点，不在这里堆。

## 技术选型（已定，2026-10）
| 层 | 选型 | 为什么 |
|---|---|---|
| 客户端 | **Vue 3 + TS + Vite**（`<script setup>`） | 数据面板型页面，单文件组件把"取数 + 渲染"压在一处；Vite 原生支持 `?worker` ⇒ Monaco 不需要额外插件 |
| 组件库 | **`naive-ui`**（子节点引入） | 通用控件不再各写一遍；TS 原生、可摇树、无需注册全局组件、样式自注入（不与 Monaco 的样式表打架）；`darkTheme` 接我们那份 `themeMode` |
| Markdown | **`markdown-it`**（子节点引入，`html: false`） | 正文本来就是 Markdown；`html: false` 同时消掉"要不要 sanitize"这个问题。★ 渲染在**客户端**（服务端照旧只给文本） |
| 编辑器 | **`monaco-editor`**（只读、**瘦引入**） | 正文动辄几万行；`<pre>` 撑不住。★ 只要"编辑器 + `editor.worker`"，**不要**语言 worker（会打进 MB 级的 `ts.worker`） |
| 服务端 | **`server.ts` 由 Node 直跑**（v24 原生 type stripping，**无构建**） | 类型照样有，省掉"服务端也要一条工具链"。★ 只用**可擦除**语法（不用 enum/namespace/decorator） |
| 构建 | 只有客户端一条：`vite build` → `dist/web/` | 服务端零构建 ⇒ `serve` 的启动路径不变（spawn 一个进程） |

★ 这是 `AGENTS.md` §3（"只有 `apps/emulator` 用 TypeScript"）的**第二处例外**，已同步写进 `AGENTS.md` 与 `docs/00-origin/decisions.md`。
★ **环境约束**（依赖必须扁平 / 构建入口不能是 `vite` / TS 钉 `~5.9`）**只在 `apps/workbench/README.md` 写一份**，本节点不复述；
唯一值得记在这里的坑：根 `.npmrc` 的键必须写 `shamefully-hoist=true`，写成 `npm_config_shamefully_hoist` 是**环境变量的形式**，
pnpm 在 `.npmrc` 里不认它 ⇒ install 出来不 hoist，症状要到运行期才炸。

## 数据来源（**不重造轮子**）
| 内容 | 来自 |
|---|---|
| 需求树 / 单条 | `tools/lib/requirements.mjs` 的 `flatten` / `rollup` / `describeNode`（与 `requirements plan` **同一份**聚合） |
| **建单**（子节点） | `tools/lib/requirements.mjs` 的 `planAdd()` —— 与 `pnpm tools requirements add` **同一个函数** |
| 脚本一览 / 反汇编正文 | `tools/lib/patch.mjs` 的 `allScriptNames()` / `buildView('data'\|'src', …)` —— 与 `patch view` **同一份** |
| 自描述 | `requirements describe` / `patch describe`（字段与不变量的真源，页面**不复述** schema） |

⇒ 工作台是这套模型的**第三个消费者**，不是第二份实现；"新建一张需求单"是它第一个**写入方向**的消费者。

## 判据（全部已达成）
1. **真源一致**：`/api/tree`、`/api/node/<ref>` 的每个字段与模型的聚合**逐条相等**（同进程 `listen(0)` + `fetch`，**不 spawn**
   —— 受限沙箱里捕获子进程输出会 EPERM）。
2. **脚本正文逐字节可信**：`/api/script/<name>` 的文本 == `buildView` 的输出（**逐字节**比较）；客户端不二次加工指令行。
3. **大文件能开**：最大的那支（规模现算）靠 Monaco 虚拟滚动打开；服务端侧实测能出全文并命中缓存。
4. **构建可复现且被守卫**：`pnpm --filter @amayui/workbench verify` = `typecheck + build + smoke`；产物在 `dist/`（gitignore）。
5. **GET 不改数据**：读端点没有任何写路径（非 GET/HEAD 一律 405）。★ 后来长出的**唯一**写端点 `POST /api/nodes`
   （新建需求单）在子节点：三重限制（只在监听回环时开 · 只收 `application/json` · 只新建），规则与 CLI 共用 `planAdd()`。

## 已交付
* 应用：`apps/workbench/`（`server.ts` / `smoke.ts` / `src/**` / `README.md` / `scripts/vite-cli.mjs`）；
* 启动器：`pnpm tools requirements serve` spawn 的是 `apps/workbench/server.ts`；
* 自检：`apps/workbench/smoke.ts`（真源一致 / 逐字节 / 边界 / **写路径** / **Markdown 口径** / 静态白名单 /
  与旧服务的**读**端点同形对照；项数看它的输出，不在这里写死）；
* 口径：`AGENTS.md` §3、`docs/00-origin/decisions.md` 索引行、根 `.npmrc`（hoist 的理由写在里面）。

## 非目标
* 不重写工作台里"一眼能看出是不是真 `<a href>`"的那些行（树 / 脚本一览）：组件库只用在通用控件上；
* 不做鉴权 / 多用户（只听回环；绑 `0.0.0.0` 必须显式 `--host`，**且那种情况下写路径会关掉**）；
* 不引 CDN（必须离线可跑）；
* 不做浏览器侧的自动化视觉验证（agent 开不了浏览器）—— 布局 / 浅深色 / 手感只能开一次页面确认；
* ~~不在工作台里编辑需求~~ · ~~不做 Markdown 全量渲染~~：★ **这两条已被用户推翻**（2026-10），
  实现与理由见子节点「工作台：Markdown 渲染 · 组件库 · 网页建单」。留着划掉的原句，因为"当时为什么不做"也是信息。

## 退役
`apps/requirements/`（无构建的旧版）**在新的一能跑之后删掉**，不并留两份。
目前**留着**的唯一理由：`smoke.ts` 拿它当**离线对照物**（逐字段比对**读**端点 `/api/tree`、`/api/node`；
写路径是新增能力，旧目录没有对应的东西）⇒ 旧目录一删，那一组自动 skip。
⇒ 用户确认后同一提交里删目录 + 更新 `smoke.ts` 的说明。
