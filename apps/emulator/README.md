# apps/emulator — 模拟器（**重写计划**）

## 1. 本轮状态：占位

本目录**只有这份重写计划**，没有代码。**本轮不进 workspaces**（`pnpm-workspace.yaml` 只列 `packages/*` 与 `tools`）。

## 2. 决定：按新结构重写（不搬旧代码）

用户口径（本轮确认）：旧仓 `app/amayui-emulator`（589 跟踪文件 + 268 测试文件）**不迁移**，
只作**重写参考**（登记为 `corpus/assets.json` 的 `app/emulator-old`，`role: rebuild`）。

## 3. 重写时必须解决的三件事（来自旧仓的实测症状）

1. **跨域守卫寄生**：旧仓有 **10 个跨域守卫住在模拟器的 `test/` 下** —— 重写时把它们**拆回各自域的包**
   （格式 / 台账 / 翻译 / 语料各管各的）。
2. **工具链单一化**：本仓**只有这里用 TypeScript**（自带工具链）；其余一切 JS 是 `.mjs`，无构建步骤。
3. **输入层边界**：与 `packages/host-input`（N-API/CMake，不进 workspaces）的接口要显式化 ——
   包括"原生产物缺失时降级"的行为（旧仓是加载器降级 + 一行诊断）。

## 4. 依赖关系

* `packages/age-format`（容器格式；★ **AGE 脚本本身也在这里** —— 解析 / 组装 / reflow，见该包 §3.4）、
  `packages/host-input`（原生输入）；
* **是知识线 K2/K3 的前置**：旧仓"被可执行守卫验证过"的知识要靠真正跑得起来的守卫复核，
  而那些守卫现在还在旧仓模拟器的 `test/` 里 ⇒ **K2/K3 必须等"守卫能真跑"**（见 `docs/00-origin/knowledge-rebuild.md` §3）。
  ★ 但**只有"无头核心"那一半是硬前置**：实测 79 条知识的守卫文件**全在 T0/T1 档**
  （T2 全仓只有 1 个文件 `e4-gamestart-shot.test.ts`，且不进 `all`/`verify`）⇒ 复核**不需要窗口、不需要原生输入**。
  因此本项分两层推进，见 §5.1。

## 5. 迁移批次

**M4**：`apps/emulator` + `packages/host-input` + `plugins/amayui-emulator` 落地；
把 10 个跨域守卫拆出去。前置：M2 / M3。

### 5.1 两层（★ 判据不同，别混）

| 层 | 含什么 | 前置 | 判据 |
|---|---|---|---|
| **M4-1 · 无头核心** | `src/vm` · `script` · `save` · `text` · `audio` · `frame` · `host`（实例注册表 / 宿主服务）· `util` + 测试驱动（fixture 驱动）+ **拆 10 个跨域守卫** | `packages/age-format`（已落地） | 无头实例可跑；跨域守卫各自落在本域；**K2 的复核因此解锁** |
| **M4-2 · 表现层与窗口** | `src/renderer`（Pixi 后端）· `electron/`（**窗口壳 = 本 app 的一部分**）· `src/web`（浏览器 / 消息桥）· `packages/host-input` | M4-1 | 真窗口 + 原生输入下的行为对齐 |

* ★ **`apps/inspector` 不属于本项**（那是"真机探针"，C# / .NET 10 + WPF，读**真游戏进程**内存），
  归 **M7**；两者是不同工具，别把"重写"这条口径当成一件事。
* ★ **原生输入不进 M4-1 的关键路径**：实测 B 类知识的守卫跑的是 `StubNative`，
  旧仓引用 `native/host-input` 的地方只有 3 处、全在输入路径 ⇒ 可以整体押后到 M4-2。
* ★ **跨域守卫的归属要按它真正校验的域判**（旧仓用 `@subsystem` 标过，正好 10 个 `ledger`）：
  其中 4 个（`journal` / `capability-ledger` / `script-ledger` / `ticket-ledger`）**只读台账、零 `../src` import**
  ⇒ 归**台账域**，随 M3 落地；其余各归其域（渲染 / 文档约定 / 测试组织）。
