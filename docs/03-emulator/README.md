# 03 · 模拟器域

## 0. 知识落点地图（★ 先读这一节）

模拟器这一域**有真正的文档**（微软的 Win32 / GDI / D3D9 / DDraw / DirectShow）—— 所以"我们相信什么"落在
文档里，**不落在测试里**。测试只留"变了就是 bug、而且变了是**静默**的"那一小撮。
本目录按**知识的种类**分家（§1–§7 是本域的来源与口径沿革，知识本身在下面这几份）：

| 想知道什么 | 看哪份 |
|---|---|
| **我们对外承诺的 Win32 / D3D9 / DDraw / DirectShow 行为**（L1 边界合同：每条带 API、依据、实现落点、守卫） | `contracts-win32.md` |
| **"我们测到过 X"**（观察：命令 + 读数 + 日期 + 口径；过期不删，标 `stale`） | `observations.md` |
| **"曾经有 bug Y"**（现象 / 根因 / 修法 / 代表守卫 / 复现命令） | `defects.md` |
| **数字怎么比**（profiler 口径、墙钟在哪个 build 上可比、音频 underrun 判据） | `measurement.md` |
| **"为什么这么实现"**（内部取舍 = L3，重写实现时可自由改，但要保住它列出的可观察现象） | `design-notes.md` |
| **我们依赖哪些外部文档**，以及**我们偏离文档的地方**（偏在哪 / 为什么 / 谁受益） | `external-docs.md` |
| **仓库自己的机械装置还成立吗**（素材清单 / 语料定位 / 台账 / shape / spec / 写路径 / 插件闭接口） | `repo-guards.md` |
| **一条 opcode / 一个 EA 的取证** | 技能 `amayui-re-engine`（知识层，不在这里） |
| **交互模型**（任务 = 节点图；`when` → `action` → `next`；三种失败全部响亮；两种识别源） | `interaction-model.md` |

### 0.1 判据：什么进 test，什么进文档

**只有一种东西配一条守卫**：这条断言失效 ⇒ **用户可见的行为变了，或数据被写坏了**，
**而且**变了之后**没有人会发现**（构建、类型检查、别的守卫、人眼都抓不住）。

三问（**全答"是"才写守卫**）：

- **(a) 失败是否意味着用户可见的错、或数据损坏？** —— "看起来更慢 / 更省内存 / 地址变了"不算。
- **(b) 会不会有人发现？** —— 会的话（崩、黑屏、启动不了、`git diff` 看得见）就不需要这条守卫。
- **(c) 删掉它，会不会让某个错误静默通过？** —— 不会 ⇒ 删掉，写进文档。

补三问：

- **(d) 换一份实现之后，这条断言还需不需要成立？** 需要 ⇒ 它是 L1/端到端；不需要 ⇒ 它是 L3，进 `design-notes.md`。
- **(e) 能不能只留一处？** 两处都写的东西，要么删一处，要么用守卫钉住两处一致。
- **(f) 它红了有意义吗？** 恒真断言、把 `expected` 抄成 `actual`、断言"源码里还有这句话"都不算守卫。

**进文档的三种**：观察（我们测到过什么）· 缺陷史（我们曾经错在哪）· 内部取舍（我们为什么这么写）。
文档的硬要求：**必须带命令与读数**（"怎么看见的"+"看到的是多少"），否则它不比没有强。

### 0.2 命名规则（id 是跨文件的引用点，代码注释与需求单只引用 id）

| 前缀 | 含义 | 在哪份文件 | 一条写几行 |
|---|---|---|---|
| `L1-<族>-NN` | 边界合同（由微软文档定义，或"文档未定义、我们自定"） | `contracts-win32.md` | 1 行，带 `文件:行` 与守卫名 |
| `OBS-<yyyymmdd>-NN` | 观察（**不是**合同，不设断言） | `observations.md` | 1 行，带命令与读数 |
| `BUG-NN` | 缺陷史（现象 / 根因 / 修法 / 一条代表守卫） | `defects.md` | 1 行 |
| `M-<主题>-NN` | 测量口径（能比什么 / 不能比什么） | `measurement.md` | 1 行 |
| `L3-<域>-NN` | 内部取舍（重写时必须保住的现象） | `design-notes.md` | 1 行 |
| `MS-<api>` | 外部文档条目（页面 + 抓取日期 + 它定了哪条合同） | `external-docs.md` | 1 行 |
| `T-<域>-NN` | 仓库自身工具链 / 台账的合同 | `repo-guards.md` | 1 行 |

`<族>` 用**短族名**（`MEM` / `FILE` / `HANDLE` / `CP` / `GDI` / `MSG` / `SYNC` / `D3D9` / `DDRAW` / `DSHOW` / `AUDIO` / `PE`），
`NN` 两位数。id 只增不改：结论错了就追加一条新的、在旧的那条写 `→ 被 L1-xxx-yy 取代`。

### 0.3 反模式（★ 这一节是这套分家的全部理由）

- ❌ **为了"记住发生过的事"加 test**。"我们测到过 X"是 `OBS-*`，不是用例 —— 观察会过期，用例会变成负债。
- ❌ **把一次性缺陷的复现路径变成永久用例**。修好之后只留**一条代表性守卫**证明"这类会红"，其余进 `defects.md`。
- ❌ **用 test 断言当前取值 / 当前数字**（`total === 544`、`hits === 1`、`calls === 10`、某个扫描计数上界）。
  这些数字一改就红，而红的含义只是"仓库变了"，不是"有 bug"。
- ❌ **给每个常量加一条 mutation 条目**。变异条目的职责是证明"**这类断言**会红"；每类挑一条代表。
- ❌ **用 test 锁内部结构**（堆策略、缓存键、批合并数、分项计时定义）：那是 L3，重写实现时**本来就该变**。
- ❌ **把 L1 合同写成"复述代码逻辑"**。合同是"调用方看得到的结果"（返回值 / LastError / 内存内容 / 边界行为），
  不是"我们的 if 怎么写"。
- ❌ **文档里手写进度与计数**（详见 `AGENTS.md` §10）：要数字就给命令，让人自己去读。

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
* **实现按层落地**（★ 落点地图，不含进度）：
  **平台无关核心** = `apps/emulator/src/`（`model/` 语义模型 · `host/` 宿主抽象 · `vm/` 执行核心）
  —— 整个 `src/` **零 Node 依赖**，由那份 tsconfig 的 `"types": []` 机械强制；
  **前端** = `apps/emulator/frontends/`（headless 先落地；渲染器 / Electron / 浏览器壳在 M4-2）
  —— 前端**允许**用 Node，因此它有**自己的一份 tsconfig**（见 `AGENTS.md` §3）。
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

## 7. 第二条线：`apps/emulator-lift`（执行拆壳件原机器码的 guest-state 模拟器）

它不是本文 §2 那条"按 spec 重写"的延续，而是**执行原机器码**；它**也不是**
`platform-effort.md` §5.4 说的**第三条路线**（Wine 直接跑）—— 平台层仍自写，
只是"形状"与 Wine 同类（口径见 `repo:apps/emulator-lift/README.md` §1.1）：

- 落点 `repo:apps/emulator-lift`（CMake + SDL3；vcpkg **跨目录复用** `repo:apps/emulator-cxx/vcpkg`）；
- **执行源**：**拆壳件 `AGE.EXE__dumped.sectfix.EXE` 的机器码字节**，已入库（LFS）为
  `repo:corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`（`binary/age-sectfix`，清单条目 `role: baseline`）；
  载入时按 `.lst` 头部的 `Input SHA256` 自校验。
  `.lst` 是**符号 / 对照 / 覆盖**真源，`.c` 只作阅读对照（32→64 是移植，不是机械翻译）。
  口径见 `repo:apps/emulator-lift/README.md` §2.1；
- **执行方式**：**纯内存解释 + 块缓存**，默认零生成代码（2026-11-03 决定；口径见同文件 §2.4）；
- **执行单位**：basic block（跨函数 `jmp` / chunk / 跳转表）；`sub_` 只是聚合视图；
- **覆盖**：清单 + 块级调度表（`replace` / `stub` / `hook`）；本线**不生成代码**（运行时解码 + 块缓存）；
- **决策全文**：`repo:apps/emulator-lift/README.md`；显式偏差：`repo:apps/emulator-lift/SCOPE.md`；
  数据与方法：`repo:docs/03-emulator/platform-effort.md` §4.1、§5.5–§5.7；
  实现计划（M1 = guest CPU 核心 + §3.1 基座）：`repo:apps/emulator-lift/docs/plan-m1-cpu-base.md`。
- ★ 两条线的共同点只有**平台 shim 的语义**（Win32/GDI/DDraw）；业务逻辑一个是重写、一个是原码执行。
