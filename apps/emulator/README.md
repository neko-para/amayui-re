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

* `packages/age-format`（容器格式）、`packages/script-dsl`（脚本）、`packages/host-input`（原生输入）；
* **是知识线 K2/K3 的前置**：旧仓"被可执行守卫验证过"的知识要靠真正跑得起来的守卫复核，
  而那些守卫现在还在旧仓模拟器的 `test/` 里 ⇒ **K2/K3 必须等本项落地**（见 `docs/00-origin/knowledge-rebuild.md` §3）。

## 5. 迁移批次

**M4**：`apps/emulator` + `packages/host-input` + `plugins/amayui-emulator` 落地；
把 10 个跨域守卫拆出去。前置：M2 / M3。
