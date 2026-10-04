# 工作台：Markdown 渲染 · 组件库（naive-ui）· 网页建单

- id: REQ-01M3YM3840G8DSKC7EKCV6FAEH
- type: req
- status: done
- parent: REQ-01M3XWRXVB04WQ4J0MA6AXZY9D
- verify: apps/workbench/smoke.ts#POST /api/nodes）：写在临时台账上
- tags: [web, workbench, markdown, ui]

## 范围
三件事，都落在 `apps/workbench/`（父节点那个工作台）：

1. **正文按 Markdown 渲染**：需求正文本来就是 Markdown，之前详情页把小节文本原样塞进 `<pre>`
   —— 满屏 `**` 与 `|`。现在引 `markdown-it`（`src/markdown.ts`）；
2. **网页里记一笔**（新建需求 / 缺陷）：`POST /api/nodes` —— 服务端把它交给模型 `planAdd()`
   （与 `pnpm tools requirements add` **同一个函数**）。★ 表单**刻意最小**，见 §"表单为什么这么小"；
3. **引 UI 组件库**：`naive-ui`（对话框 / 表单 / 下拉 / 消息条）—— 免得后面每要一个"更高层的抽象"
   都从零写一遍；深浅色与我们自己那份 `data-theme` **同源**。

## 表单为什么这么小（用户口径，2026-10）
*"我先简单描述，然后我会在 dsh 这边给出单号来细化具体内容。"* ⇒ 对话框**只问三件事**：
**一句话 · 类型 · 挂在哪条下面**（缺陷多一项**复现**）；`status` / `order` / `tags` / `verify` / `severity` /
正文一律**不问** —— 那些是"出单号之后"那一步的活（`pnpm tools requirements set <短名> …`）。
★ 于是"页面能建什么"比"端点接受什么"**窄**：端点（= CLI 的字段面）保持完整，**收窄的是界面，不是契约**。
★ 缺陷那一项不能省：**"缺陷必须有存在证明"是台账不变量 #1**，网页不该把它偷偷降级（没有锚点就先建成需求）。
★ 子节点只在"有第二个调用方"时才进模型/界面（`--set` 至今只有 CLI）—— 有第二个消费者才提取，不预支。

## 选型（已定，2026-10）
| 层 | 选型 | 为什么 |
|---|---|---|
| Markdown | **`markdown-it`**（`html: false`） | 自带类型、双格式（ESM/CJS）；`html: false` 一条同时解决"要不要 sanitize"——**不开 HTML 就不需要 sanitizer** |
| 组件库 | **`naive-ui`** | TS 原生（与 `vue-tsc` 严格模式合得来）、`sideEffects: false` 可摇树、**不需要注册全局组件**也不需要额外引 CSS（样式由 css-render 注入 ⇒ 不必和 Monaco 的样式表排序打架）、`darkTheme` 能直接接我们的 `themeMode` |
| 写路径 | **模型里的 `planAdd()`** | 建单规则（id / parent / 缺省正文 / 写后守卫 + 回滚）原本长在 CLI 里；工作台要建单就只能再抄一遍 ⇒ **把规则移进模型**，CLI 与网页都只是它的调用方（`--add` 与 `POST /api/nodes`） |

★ 建单的三重限制（都刻意）：**只在监听回环时开**（绑到局域网等于把写权限交出去）·
**只收 `application/json`**（跨站页面发不出这种简单请求 ⇒ 必须先过预检，而服务端从不回 CORS 头）·
**只新建**（改已有节点仍然只有 `pnpm tools requirements set --write`）。

## 判据
1. **Markdown 真的渲染了，且口径可机械复核**：`apps/workbench/smoke.ts` 直接 import
   `src/markdown.ts` 断言四条 —— 原始 HTML 按文本（`<script>` 不成为标签）· 裸文本不自动变链接 ·
   **仓库内相对路径不做成可点链接**（工作台不服务仓库文件，做成 `<a>` 只会点出 404；外链才 `target=_blank`）·
   小节里的小标题降两级（卡片标题是 `h3`）；另断言详情页走的是渲染器（旧的 `<pre>` 原文展示已撤掉）。
   同组还有一条**实体解码哨兵**（`&amp;` / `&copy;` / 未知实体 / 裸 `&`）—— 它同时守着下面那条依赖地雷。
2. **网页建的与命令行建的是同一种节点**：服务端**不实现**建单规则，`planAdd()` 是唯一实现；
   `tools/test/requirements.test.mjs` 断言它的契约（缺省正文按 type 分两套、id 撞车在写之前就挡、
   不认得的键报错、空列表不写、落盘即规范形态），并有一条端到端用例走 CLI `--add` 的 dry-run 与 `--write`。
3. **被拒的单子不留残 file**：守卫不绿 ⇒ `422` + `report`（逐条点名），且模型已经**把刚写的文件删掉**；
   自检里断言目录回到原样，并且**自检只写 `.tmp/` 下的副本**（跑完断言真的 `data/requirements/` 一个文件都没多）。
4. **组件库接进来了，而且没把布局链踩坏**：naive-ui 只用在**通用控件**上；`n-config-provider abstract`
   保证 `#app` 的 flex 链与"没有组件库时"逐字节相同（★ 没有视觉验证 ⇒ 布局链不许动）；暗色与我们那份
   `data-theme` 由同一个 `themeMode` 派生；记一笔那个对话框**懒加载且首次打开才挂载**（自检断言外壳不预加载它）。
5. **门禁一条命令可复现**：`pnpm --filter @amayui/workbench verify`（`typecheck + build + smoke`）
   与 `pnpm test` 全绿。

## 依赖地雷（引 `markdown-it` 引出来的，已修 + 已记录）
实测踩到：`markdown-it@15` 要 `entities@^8`、`@vue/compiler-core@3.5` 要 `entities@^7` ⇒ `vue-tsc`
**一遇到模板里出现 `&lt;` 就崩**（`decode.fromCodePoint is not a function`）。修法：`markdown-it` 只用
`decodeHTMLStrict`（7 与 8 行为相同）⇒ 在根 `package.json` 的 `pnpm.overrides` 里把
`markdown-it>entities` 钉 `^7`，并给这条路径配断言（判据 1 的哨兵）。
★ **2026-10 修正前提**：本节原先写"本机是全扁平（hoisted）⇒ 一个包名只能有一个版本"——**不准确**。
实测本仓并非全扁平（`nodeLinker: hoisted` 只是把 junction 从根移到各 workspace 包自己的 `node_modules`），
真正的地雷是**幽灵依赖**：没声明该依赖的包也能解析到根上 hoist 的那一份、两个大版本共存时拿错。
纪律：**新增依赖前先确认它不是靠"根上恰好有一份"解析的**；判据见 `AGENTS.md` §3（机器配置已移至
`pnpm-workspace.yaml`，`.npmrc` 只读 auth/registry）。
## 非目标
* **不重写**需求树 / 脚本页的 markup：树里每一行都是**真 `<a href="#/req/…">`**（中键开新 tab 是白送的，
  `n-tree` 给不了这一点）。组件库只用在"通用控件"上（搜索 / 按钮 / 表单 / 对话框 / 消息条）；
* **不做编辑**：这张表只能**新建**。改字段（`--set`）没有第二个调用方，规则就先留在 CLI 里
  —— 等它也有了第二个调用方再挪进模型（**有第二个消费者才提取**，不预支）；
* 不做浏览器侧视觉验证（agent 开不了浏览器）：布局 / 深浅色 / 对话框手感只能开一次页面确认；
* 不做鉴权 / 多用户（仍只听回环）。

## 与父节点"非目标"的关系（**记录一次口径反转**）
父节点（本工作台）原名两条非目标：**"不在工作台里编辑需求"** 与 **"不做 Markdown 全量渲染"**，
理由分别是"写入口仍在 CLI"与"引 Markdown 库要顺带背 sanitize 的问题"。
用户 2026-10 明确要求这两件都要 ⇒ 本节点推翻它们，父节点那两行已就地改写。反转之所以便宜，是因为
两条理由都被消掉了：① 建单规则**挪进模型**之后，网页建单不再意味着第二份规则（上面「选型」那一行）；
② `html: false` 让"要不要 sanitize"这个问题**不存在**（不是"选了某个 sanitizer"，是根本不产生 HTML 标签）。
