# apps/requirements/ — 需求台账的本地只读网页（**旧版，待删**）

> ⚠️ **已被 `apps/workbench/` 取代**（新技术栈重写：需求 + AGE 脚本反汇编；新版另外还会渲染正文 Markdown、
> 并有一个建单端点 `POST /api/nodes`）。
> `pnpm tools requirements serve` 现在指向 `apps/workbench/server.ts` —— 本文档下面那些"经派发器"的命令
> **已经不再启动本目录**，要跑这个旧版请直接用 `node apps/requirements/server.mjs`。
> 目前**留着**的唯一理由是：`apps/workbench/smoke.ts` 拿它当**离线对照物**
> （把新旧两个服务的**读**端点 `/api/tree`、`/api/node`、错误体**逐字段**比对 —— 写路径是新增能力，
> 旧目录没有对应的东西），"新旧读响应同形"因此是可机械复核的。
> 按需求单 §退役，它**待用户确认后删除** —— 删掉之后那一组会自动 skip。
> 在那之前：**不要往里加新东西，也不要按它改新应用**。

`data/requirements/` 的一层 HTTP + 浏览器视图：**总览**看整棵树与进度，**每条需求有自己的地址**，
用浏览器自己的 tab 并排看。

> ★ 这不是 DSH 插件，也和 DSH **没有任何关系**。它是一个独立的本地网页 ——
> 做成网页只是因为"切 tab 比切应用快"。所以它不引 DSH 的主题 token、不引任何 DSH 包、
> 也不要求 DSH 在跑。
>
> ★ 它是**只读**的：本服务**没有任何写路径**。改台账只有
> `pnpm tools requirements set --write`（唯一写入口）—— 新版工作台例外的那一条见 `apps/workbench/README.md`。

## 怎么跑

```sh
node apps/requirements/server.mjs             # → http://127.0.0.1:7788/（不经派发器，直接跑）
# ★ 派发器那条路 `pnpm tools requirements serve` 已经指向新版：apps/workbench/
```

启动时会把地址与数据目录打印出来（**固定端口 7788**，方便直接记地址；撞端口不会自动漂，
而是明确报错让你换一个 —— 地址会变比报错更烦）。

页面里怎么用：

| 想做什么 | 怎么做 |
|---|---|
| 看整棵树与进度 | 打开 `/`（总览） |
| 找一条 | 顶部**搜索**：短名 / 标题片段 / 完整 id 都行 |
| 只看看未收口的 | 顶部 `只看未收口`（默认开） |
| 看某一条的细节 | 点行的**标题**或**短名** → `#/req/<ref>`（行里没有单独的「打开详情」按钮：标题就是入口） |
| **复制完整 id** | 总览每行、详情页字段区都有 `⧉` 按钮 —— 复制的是 **26 位完整 id**（粘进终端 `show <id>` 用），**不是**显示用的短名 |
| **并排看两条** | 中键 / `Ctrl`(`Cmd`)+点击行 —— 浏览器原生开新 tab |
| 往回走 | 浏览器**后退键**（hash 路由，不用 History API） |
| 看父/子 | 详情页顶部的面包屑（向上）与底部 `子节点`（向下），都能点 |

## 为什么是这样（设计口径）

- **相对跳转为主**：`#/` 总览、`#/req/<ref>` 详情。hash **不发给服务器**，
  所以不需要 History API、不需要服务器 rewrite，而每行是**真 `<a href>`** ⇒
  中键开新 tab 是白送的。不需要收藏，所以不做绝对 URL。
- **引用解析复用台账自己的规则**：完整 id / 唯一前缀 / **唯一后缀**（短名就是唯一的后缀）
  都能开 —— 与 `pnpm tools requirements show` 完全同一套（`makeResolver`）。
  ULID 以时间开头、同批前 10 位相同，所以短名才是人能用的那个句柄。
- **显示短名，复制完整 id**：这一对是刻意的 —— 26 位 ULID 摆在行里对人不提供有效信息，
  但**粘进终端**要的就是它。所以行里显示 8 位短名（可读、可搜、可跳），
  完整 id 由 `⧉` 一键复制（悬停短名也能看到）。
- **树状靠横向偏移**：`--depth`（来自 `flatten`，与 `plan` 的缩进同源）换算成 `margin-left`，
  左侧随缩进移动的竖线当层级标尺 —— 纯纵向折叠只能表达"展开/收起"，表达不了**从属**。
- **聚合不在这里算**：页面显示的所有数字（`[已收口/子孙总数]`、表头、聚合告警）
  都来自 `tools/lib/requirements.mjs` 的 `rollup` / `flatten` / `describeNode` ——
  **`pnpm tools requirements plan` 用的就是同一份函数**。网页只是那套模型的第二个消费者。
- **不引 CDN / 框架**：见下。

## 为什么不引 CDN

问过"能不能用 CDN 的框架来保底样式一致"，结论是**不引**：

1. **它必须能离线跑。** 账号/代理/断网时页面会变成没有样式的裸 HTML —— 一个只读本地视图，
   为它加一个网络依赖是净亏损。
2. **CDN 给不了"和谁一致"。** 一致的参照物是 DSH 的 UI，但没有哪个 CDN 框架长得像它；
   引进来只会得到"像 Bootstrap 的页面"，而不是"像 DSH 的页面"。
3. **这个仓的既有取向是零依赖 + 无构建**：`tools/**` 全是 `.mjs` 直接跑，
   根目录**不引入**任何构建步骤。为一个单页视图引 CDN 与这条口径相反。

所以样式是**这个项目自己的一份 `app.css`**：一处变量定义 + 一套 `prefers-color-scheme`
的浅/深色，再给一个手动 `◐/☀/☾` 切换（存在 localStorage）。它和 DSH 的 token **无关** ——
刻意如此：两边不会一起打开、也没有共享的文档树，唯一的"收益"是把两套颜色和版本偶合起来。

## 项目结构

```
apps/requirements/
├── README.md              # 本文件
├── server.mjs             # node:http；两个只读端点；默认 127.0.0.1:7788
├── smoke.mjs              # 离线自检：同进程 listen(0) + fetch，对真源逐节点核对
└── public/
    ├── index.html         # 骨架（真 <a href="#/req/…">；内容只由 textContent 填）
    ├── app.js             # hash 路由 / 总览 / 详情 / 主题
    └── app.css            # 本页自己的变量 + 浅深色（不引 DSH token、不引 CDN）
```

**为什么在 `apps/` 而不是 `tools/`**：`tools/test/layering.test.mjs` 要求每个
`tools/*.mjs` 只能 import `./lib/*`，所以 `tools/` 下的 CLI **不能** import 本项目；
反过来本项目 import `tools/lib/` 是允许的（测试不扫 `apps/`）。
启动器 `tools/requirements.mjs --serve` 因此只做一件事：**spawn 本项目的 `server.mjs`**。

> ⚠️ AGENTS.md §3 的口径是"只有 `apps/emulator` 用 TypeScript，其余一切直接写 `.mjs`"。
> 本项目是 **`.mjs` + `.html/.js/.css`，无构建、无 `package.json`、不进 workspaces**
> （`pnpm-workspace.yaml` 只有 `packages/*` 与 `tools`，`apps/*` 明确不入）。

## 端点

| 端点 | 用途 |
|---|---|
| `GET /api/tree` | 整棵树 + 表头 + 预算 + 状态标记表 + 字段自描述（`describeText()`） |
| `GET /api/node/<ref>` | 一条：字段 + 正文（服务端切好 `sections`）+ `parentChain` + 直接子 |
| `GET /` `/app.js` `/app.css` | 页面资源（**白名单**，请求路径从不参与拼路径） |

只读、`Cache-Control: no-store`、默认只听回环。**绑到 `0.0.0.0` 必须是显式 `--host`** ——
否则等于把仓库内容放到局域网上。

## 怎么查 / 怎么改

```sh
node apps/requirements/smoke.mjs     # ★ 离线自检（24 项）：与 tools/lib 的聚合逐节点核对
node --check apps/requirements/server.mjs
```

- 自检**同进程** `listen(0)` + `fetch`，**不 spawn 子进程** —— 受限沙箱里捕获子进程输出
  要开命名管道（`spawn EPERM`），同进程没有这个问题。
- 它断言的是**真源一致性**，不是"代码看起来对"：`/api/tree` 的每个节点窗口、标记、深度、
  父、聚合告警，都与 `tools/lib/requirements.mjs` 的 `flatten`/`rollup` 逐条相等。

**怎么改**：改完**刷新页面**即可（没有任何构建步骤）。改 `server.mjs` 要重启进程。

## 已知边界（不做 / 未验）

- **不写**：端点是只读的，永远不写 `data/requirements/`。
- **不做完整 Markdown 渲染**：正文只在 `## ` 处切小节（台账自己的写法），其余按纯文本排版。
  引一个 Markdown 库就要顺带背"要不要 sanitize"的问题，不值得；需要时先加一条判据。
- **不缓存**：每次请求重读 40 个文件。预算就是 ≤40 节点 / ≤80 行（不变量 5），
  整棵树连正文几十 KB —— 比"缓存什么时候失效"简单得多。
- **未做视觉验证**：`smoke.mjs` 只断言 HTTP 与数据，**没有**任何浏览器/截图手段，
  所以"页面看起来对不对"（布局、浅深色、中键开新 tab）**没有被自动验证过** ——
  那些是开一次页面就能确认的事，而我没法开浏览器。
- **没有 `--open`**：不自动弹浏览器；启动时打印地址，自己粘贴/记住。
