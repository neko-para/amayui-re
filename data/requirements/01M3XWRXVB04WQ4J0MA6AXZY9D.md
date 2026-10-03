# 项目工作台（网页）：需求 + AGE 脚本反汇编

- id: REQ-01M3XWRXVB04WQ4J0MA6AXZY9D
- type: req
- status: doing
- parent: REQ-01M3SVH2V332E4CJYN0BHH0YK8
- order: 40
- tags: [web, workbench, tooling]

## 范围
把 `apps/requirements/`（单页只读看板）**重建**为一个**通用项目工作台**：一个正式的、可成长的本地网页应用。
首期两类内容：

1. **查看需求**：整棵树 + 进度 + 单条详情（现有能力，行为口径不变，重写在新技术栈上）；
2. **查看 AGE 脚本的反汇编正文**：**全部能反汇编的脚本**一览 → 打开某一支的**反汇编全文**（可搜索、可跳行、大文件不卡）。
   ★ 不是"官方集"——那个说法是错的（见 `docs/01-translation/patch-design.md` §2.3）：范围是**全部 941 支脚本**。

落点：**新目录 `apps/workbench/`**（旧 `apps/requirements/` 的退役见 §退役）。

## 技术选型（已定，2026-10）
| 层 | 选型 | 为什么 |
|---|---|---|
| 客户端 | **Vue 3 + TypeScript + Vite**（`<script setup>`） | 工作台是"数据面板"型页面，单文件组件把"取数 + 渲染"压在一处；Vite 原生支持 `?worker` ⇒ Monaco 不需要额外插件；本仓没有既有前端框架偏好，不引 React 生态的额外依赖树 |
| 编辑器 | **`monaco-editor`**（只读、**瘦引入**） | 反汇编正文动辄几万行；`<pre>` 撑不住，Monaco 的虚拟滚动 + 行号 + 搜索是刚需。★ 只要"编辑器 + `editor.worker`"，**不要**语言 worker（否则会打进 MB 级的 `ts.worker`） |
| 服务端 | **`server.ts` 由 Node 直接跑**（v24 原生 type stripping，**无构建**） | 类型照样有；省掉"服务端也要一条工具链"的代价。★ 只用**可擦除**语法（不用 enum/namespace/decorator） |
| 构建 | 只有客户端一条：`vite build` → `apps/workbench/dist/web/` | 服务端零构建 ⇒ `pnpm tools requirements serve` 的启动路径不变（spawn 一个进程） |

★ 这是 `AGENTS.md` §3（"只有 `apps/emulator` 用 TypeScript"）的**第二处例外**，已同步写进 `AGENTS.md` 与 `docs/00-origin/decisions.md`。

### 环境约束（实测踩出来的，细则见 `apps/workbench/README.md`）
1. **依赖必须扁平**：本机 `fs.realpathSync` **不解析 pnpm 的 junction** ⇒ 严格布局下 `vite` 里的
   `import 'rolldown'` 直接 `ERR_MODULE_NOT_FOUND`。⇒ 根 `.npmrc` 设 `shamefully-hoist`（代价：放弃依赖不可提升；
   本仓 `packages/*` / `tools` 零运行期依赖，代价目前为零）。
2. **构建入口不能是 `vite`**：Vite 在 Windows 启动时有一次 `exec('net use')`，而受限沙箱**不许 spawn 并捕获输出**
   （`spawn EPERM`）⇒ 构建走 `apps/workbench/scripts/vite-cli.mjs`（只把那次探测换成空结果桩，不影响产物）。
3. **`typescript` 钉 `~5.9`**：`vue-tsc@3` 要从 `typescript/lib/tsc` 进去，而 TS 7（原生端口）的 `exports`
   里没有那个子路径 ⇒ 类型门禁跑不起来。升 TS 主版本前先跑一遍 `pnpm typecheck`。
4. `pnpm install` 的缓存/store 必须落在仓内（沙箱外写不了）；那是**本机环境**的事，不进仓库配置。

## 数据来源（**不重造轮子**）
| 内容 | 来自 |
|---|---|
| 需求树 / 单条 | `tools/lib/requirements.mjs` 的 `flatten` / `rollup` / `describeNode`（与 `requirements plan` **同一份**聚合） |
| 脚本一览 | `tools/lib/patch.mjs` 的 `officialNames` + 基线解析层 `tools/lib/bin-source.mjs` |
| 反汇编正文 | `tools/lib/patch.mjs` 的 `buildView('data'\|'src', …)` —— 与 `pnpm tools patch view` **同一份**实现 |
| 自描述 | `requirements describe` / `patch describe`（字段与不变量的真源，页面**不复述** schema） |

⇒ 工作台是这套模型的**第三个消费者**，不是第二份实现。

## 判据（全部已达成）
1. **真源一致**：`/api/tree`、`/api/node/<ref>` 的每个字段与 `tools/lib/requirements.mjs` 的聚合**逐条相等**
   （断言方式是旧 `smoke.mjs` 那一套：同进程 `listen(0)` + `fetch`，**不 spawn** —— 受限沙箱里捕获子进程输出会 EPERM）。
2. **脚本正文逐字节可信**：`/api/script/<name>` 的文本 == `buildView` 的输出（**逐字节**比较）；
   页面展示的就是那份文本，客户端不二次加工指令行。
3. **大文件能开**：最大的那支脚本（规模现算：`/api/script` 返回 `rows`/`bytes`）在浏览器里靠 Monaco 虚拟滚动打开；
   服务端侧实测能出全文并命中缓存。
4. **构建可复现且被守卫**：`pnpm --filter @amayui/workbench verify` = `typecheck + build + smoke`；
   产物在 `dist/`（**gitignore**）；一条命令能跑起来（`pnpm tools requirements serve`）并给出确切地址。
5. **只读**：端点上**没有**任何写路径（任何非 GET/HEAD 一律 405；改数据仍只有 CLI）。

## 已交付
* 应用：`apps/workbench/`（`server.ts` / `smoke.ts` / `src/**` / `README.md` / `scripts/vite-cli.mjs`）；
* 启动器：`pnpm tools requirements serve` 现在 spawn 的是 `apps/workbench/server.ts`；
* 自检：`apps/workbench/smoke.ts`（42 项：真源一致 / 逐字节 / 只读与边界 / 静态白名单 / 与旧服务同形对照）；
* 口径：`AGENTS.md` §3（TS 例外与 `shamefully-hoist`）、`docs/00-origin/decisions.md` 索引行、
  根 `.npmrc`（hoist 的理由写在里面）。

## 非目标
* 不在工作台里编辑需求 / patch（写入口仍在 CLI）；
* 不做 Markdown 全量渲染（沿用旧口径：正文只在 `## ` 处切小节；要引库先加一条判据）；
* 不做鉴权 / 多用户（只听回环；绑 `0.0.0.0` 必须显式 `--host`）；
* 不引 CDN（必须离线可跑）；
* 不做浏览器侧的自动化视觉验证（agent 开不了浏览器）—— 布局/浅深色/手感只能开一次页面确认。

## 退役
`apps/requirements/`（无构建的旧版）**在新的一能跑之后删掉**，不并留两份。
目前**留着**的唯一理由：`smoke.ts` 第 7 组拿它当**离线对照物**（逐字段比对新旧响应），
它的存在使"新旧同形"这件事可机械复核；旧目录一删，那一组自动 skip。
⇒ 用户确认后同一提交里删目录 + 更新 `smoke.ts` 的说明。
