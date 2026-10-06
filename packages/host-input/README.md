# packages/host-input — 原生输入模块（N-API / CMake）

## 1. 职责

把宿主（Windows / macOS）的**真实输入事件**（光标位置、按键、点击注入）暴露给模拟器，
使模拟器的输入路径与真机一致。

## 2. ★ 不进 npm workspaces

本包是 **C++ / CMake / node-gyp** 工具链，与 JS 包的构建模型不同 ⇒
**不列进 `pnpm-workspace.yaml`**（`.NET` 的 `apps/inspector` 同理）。

## 3. 本轮状态

只有本 README，**没有代码**。旧仓 `native/host-input/`（15 跟踪文件：N-API + CMake + LFS prebuilds
darwin/win32）登记为 `corpus/assets.json` 的 `native/host-input-old`（`role: rebuild`）。

## 4. 决定：按新结构重写

用户口径（本轮确认）：`apps/emulator` 与 `apps/inspector` **全部按新结构重写**，旧代码只作重写参考。
本包作为模拟器的输入层随之重写，**预编译产物的平台口径也重新定**
（旧仓的取舍与守卫见旧仓 `native/host-input/README.md`，仅作参考）。

★ **本包落在 M4-2（表现层与窗口），不进 M4-1（无头核心）的关键路径**：旧仓引用它只有 3 处
（`src/vm/native.ts` / `src/vm/handlers/input.ts` / `src/renderer/ipcProtocol.ts`），
且被知识线复核的那些守卫跑的是 `StubNative` ⇒ 无头核心与知识线都不需要它。
★ **`apps/inspector` 不是本包、也不是模拟器**：那是**真机探针**（C# / .NET 10 + WPF，读真游戏进程内存），归 M7。

## 5. 落点与纪律

* 构建产物（`.node` / obj / CMake 中间件 / `prebuilds/`）**永远 gitignore**；
  若某平台确实要**预置**产物，走 LFS 并在 `.gitattributes` 里显式声明（不要靠本地忽略凑合）。
* 重写时必须回答"缺产物时怎么办"：旧仓的做法是**加载器降级 + 一行诊断**（不让缺编译器变成崩溃）。
