# 03 · 模拟器域

## 1. 来源（旧仓）

| 旧仓位置 | 规模 | 在新仓的落点 |
|---|---|---|
| `app/amayui-emulator/` | 589 跟踪文件（+268 测试文件）；含 TS 工具链 | `apps/emulator/` |
| `app/amayui-emulator/electron/` | 8 个文件（`main` / `preload` / `windows` / `nativeAddon` / …） | `apps/emulator/` 的**窗口壳**（同一 app 的一部分，不是独立项目） |
| `native/host-input/` | 15 跟踪文件（N-API + CMake + LFS prebuilds：darwin / win32） | `packages/host-input/` |
| `plugins/amayui-emulator/` | DSH 插件（软链接注册） | `plugins/`（源码落点，位置自由） |

★ **`apps/inspector` 不在本域**：`app/amayui-inspector/`（C# / .NET 10 + WPF）是**真机探针** ——
识别 AGE 引擎进程、`ReadProcessMemory` 读 VM 解释器 `this` 与全局表快照；落点 `apps/inspector/`、批次 **M7**。
它与模拟器是**两个不同的工具**，"按新结构重写"这条口径同时适用于两者，但不等于它们是一件事。

## 2. ★ 决定：**按新结构重写**（不搬旧代码）

用户口径（本轮确认）：`apps/emulator` 与 `apps/inspector` **全部按新结构重写** —— 旧实现只作**重写参考**，
因此旧代码在 `corpus/assets.json` 里登记为 `kind: tooling` + `role: rebuild`（`app/emulator-old` / `app/inspector-old` /
`native/host-input-old`），**不迁移**。

由此推出的落点事实：

* **`apps/emulator` 已进 workspaces**（判据 = "有没有自己的依赖"：它的模型 `import @amayui/age-format`）⇒
  必须有 `apps/emulator/package.json`，跨包引用走包名（口径见 `AGENTS.md` §3）；
* **实现按层落地**：**语义层** = `apps/emulator/src/model/`（它**不含执行核心** ⇒ 只靠它模拟器启动不了）；
  **表现层与窗口** = M4-2。
  ★ **哪一层到哪一步不写在这里**：看 `apps/emulator/README.md` §1 与 `pnpm tools requirements plan`（`AGENTS.md` §10）。

## 3. 语言与工具链口径

* **`apps/emulator` 有自己的工具链，但同样没有构建步骤**：`.ts` 由 Node 原生 type stripping 直接跑，
  `tsc` 只做 `noEmit` 检查（`pnpm typecheck`）。
  ★ **语言口径的真源是 `AGENTS.md` §3**，本文档**不复述**（复述必漂）；
* `native/host-input` 走 **CMake / node-gyp**（C++），**不进 npm workspaces**；
  预编译产物的口径（哪些平台预置、为什么预置）随重写重新定，旧口径见旧仓 `native/host-input/README.md`。
* 旧仓的 **10 个跨域守卫寄生在模拟器的 `test/` 下** —— 这是"目录混关注点"的典型症状，
  重写时要把它们**拆回各自域的包**（M4 的一项明确任务）。

## 4. 与本仓其它域的关系

* 依赖 `packages/age-format`（容器格式；★ **AGE 脚本的解析 / 组装 / reflow 也在这个包**，见其 §3.4）
  与 `packages/host-input`（原生输入）；
* 是**知识线 K2/K3 的前置**：旧仓里"被可执行守卫验证过"的那一类知识（`capabilities` 的 `modeled-verified`、
  票据的 `tests[]`）必须靠**真正跑得起来的守卫**来复核，而那些守卫现在还在旧仓模拟器的 `test/` 里。
  ★ **但"必须等 M4"要拆开看**：实测 79 条知识的**全部**守卫文件都在 **T0/T1 档**
  （T2 全仓只有 `e4-gamestart-shot.test.ts` 一个文件，且不进 `all`/`verify`）⇒
  **复核不需要真窗口、不需要原生输入、不需要真游戏安装**，只需**无头核心 + fixture 驱动**。
  ⇒ 真正的前置只是 **M4-1（无头核心）**，而**不是**整个 M4：M4-2（表现层与窗口）与 K2/K3 无关。

### 4.1 M4 的两层

| 层 | 含什么 | 与 K2/K3 |
|---|---|---|
| **M4-1 · 无头核心** | `src/{vm,script,save,text,audio,frame,host,util}` + fixture 驱动的测试设施 + 拆跨域守卫 | ★ **硬前置** |
| **M4-2 · 表现层与窗口** | `src/renderer`（Pixi）· `electron/`（窗口壳）· `src/web`（浏览器 / 消息桥）· `packages/host-input` | 无关（T2 档唯一那个用例不进 `verify`） |

★ **拆守卫的归属按"它真正校验的域"判**（旧仓用 pragma `@subsystem` 标过，正好 10 个 `ledger`）：
4 个（`journal` / `capability-ledger` / `script-ledger` / `ticket-ledger`）**只读台账、零 `../src` import**
⇒ 归**台账域**、随 **M3** 落地；`capability-gap` / `no-dead-writes` 归模拟器；`organization` /
`harness-convergence` 是测试组织自身的棘轮（随 M4-1）；`doc-model` / `audit-report-completeness`
要的是**文档域的 kind/state 约定**，归文档域。

## 5. 明确不做

* ❌ 不搬任何旧代码（含 589 个跟踪文件与 268 个测试文件）—— 旧实现只作**重写参考**；
* ❌ 不引入构建步骤：`.ts` 由 Node 原生剥壳直接跑，`tsc` 只做 `noEmit` 检查。

## 6. 状态与快照（★ 真源在 `apps/emulator/README.md` §7，这里只留指针）

模拟器迟早需要"把某一刻的引擎态存下来、之后反复回到这一刻做对照"的能力 —— 排查的本质是**二分**
（"这一帧的状态是从哪一步开始不对的"）。★ 但**口径与格式的真源不在本文档**：

| 要什么 | 看哪 |
|---|---|
| **口径**（状态四分 · 快照三条不变量 · 边界 · 恢复 = 构造新实例） | `apps/emulator/README.md` §7 |
| **可执行形式**（状态分区表） | `apps/emulator/src/model/pools.ts` 的 `STATE_PARTITION` |
| **守卫** | `tools/test/emulator-state-partition.test.mjs`（反射核"每个可变字段必须归类"）· `tools/test/emulator-snapshot.test.mjs`（同样状态 ⇒ 逐字节相同） |
| **待定项与重开条件**（别名语义 / RNG 承载面 / float 落槽宽度 / `initZero` 边界 / **文件格式与版本号**） | 需求树：`REQ-01M48ZGEJ2S8TDVC1DAY4MM0KQ`（挂在 M4-1 下） |

★ **文件格式与版本号故意还没定**（理由见上面那两处）：此刻没有"状态的第二个来源"，定格式只能靠猜。
