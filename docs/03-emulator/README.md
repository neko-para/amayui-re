# 03 · 模拟器域

## 1. 来源（旧仓）

| 旧仓位置 | 规模 | 在新仓的落点 |
|---|---|---|
| `app/amayui-emulator/` | 589 跟踪文件（+268 测试文件）；含 TS 工具链 | `apps/emulator/` |
| `native/host-input/` | 15 跟踪文件（N-API + CMake + LFS prebuilds：darwin / win32） | `packages/host-input/` |
| `plugins/amayui-emulator/` | DSH 插件（软链接注册） | `plugins/`（源码落点，位置自由） |

## 2. ★ 决定：**按新结构重写**（不搬旧代码）

用户口径（本轮确认）：`apps/emulator` 与 `apps/inspector` **全部按新结构重写** —— 旧实现只作**重写参考**，
因此旧代码在 `corpus/assets.json` 里登记为 `kind: tooling` + `role: rebuild`（`app/emulator-old` / `app/inspector-old` /
`native/host-input-old`），**不迁移**。

由此推出的两条落点事实：

* `apps/*` **本轮不进 workspaces**（`pnpm-workspace.yaml` 只列 `packages/*` 与 `tools`）—— 代码还没落地；
* 重写完成前，`apps/emulator/README.md` 只承担"重写计划"。

## 3. 语言与工具链口径

* **`apps/emulator` 是本仓唯一使用 TypeScript 的子项目**（自带其工具链），M4 落地后进 workspaces；
  其余 `packages/*` / `tools/*` 与所有测试**一律 `.mjs`**（见 `AGENTS.md` §3）。
* `native/host-input` 走 **CMake / node-gyp**（C++），**不进 npm workspaces**；
  预编译产物的口径（哪些平台预置、为什么预置）随重写重新定，旧口径见旧仓 `native/host-input/README.md`。
* 旧仓的 **10 个跨域守卫寄生在模拟器的 `test/` 下** —— 这是"目录混关注点"的典型症状，
  重写时要把它们**拆回各自域的包**（M4 的一项明确任务）。

## 4. 与本仓其它域的关系

* 依赖 `packages/age-format`（容器格式）与 `packages/host-input`（原生输入）；
* 是**知识线 K2/K3 的前置**：旧仓里"被可执行守卫验证过"的那一类知识（`capabilities` 的 `modeled-verified`、
  票据的 `tests[]`）必须靠**真正跑得起来的守卫**来复核，而那些守卫现在还在旧仓模拟器的 `test/` 里
  ⇒ **K2/K3 必须等 M4**（见 `../00-origin/knowledge-rebuild.md` §3）。

## 5. 本轮明确不做

* ❌ 不搬任何旧代码（含 589 个跟踪文件与 268 个测试文件）；❌ 不建 TS 工具链；❌ 不写 `apps/emulator` 的实现。
