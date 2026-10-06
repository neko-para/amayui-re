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

## 6. 已落地的第一块：`src/model/`（批 R1 迭代点 ④ —— **通用数据区域**）

```text
apps/emulator/src/model/pools.ts        ← 池模型（**语义**：有哪几族池 / 谁要编解码 / 越界怎么办）
apps/emulator/src/model/numeric-ops.ts  ← 纯数值指令族（**语义**：opcode / 助记符 / argc / 语义）
apps/emulator/src/model/iterate.ts      ← 迭代系统：字节流 → 指令（只切边界，不解释）
tools/test/emulator-*.test.mjs          ← 它们的守卫（@env assets：回语料复核常量）
```

### ★ 模拟器**不碰镜像布局**（这条是硬口径，由守卫钉住）

模拟器是"按建模后的语义**重新实现**"的东西，所以 `src/model/` 里**不许**出现：

* ❌ 绝对地址（`Engine+0x5D880` 之类）· ❌ 帧内偏移（`+0x38` 之类）· ❌ EA / 反汇编片段 · ❌ IDA 符号（`sub_420xxx`）

理由：**那些随这一份反汇编导出而变**。写进模拟器 = 让模拟器变成"这份导出的附庸"，
换一份导出/换一个镜像，实现就得跟着改 —— 而"实现"和"观察"是两件事。

它们各自的落点在**知识层**（`packages/age-format/src/engine/`），带 EA 出处、由守卫回语料复核：

```text
packages/age-format/src/engine/layout.mts    ← 槽位/偏移/步长/池的计数槽与基址槽（观察）
packages/age-format/src/engine/handlers.mts  ← opcode → handler（IDA 符号；"哪段代码实现了它"）
```

⇒ 接口是**语义名**：`localPoolByTypeTag(tag)` ↔ `LOCAL_POOL_SLOTS[].name`。
守卫 `tools/test/emulator-model.test.mjs` 一边回语料核布局，一边断言
**模拟器里没有 `base` / `count` 这类偏移字段、也没有按地址算帧基址的 API**。
**偏移可以被替换，语义不必跟着动**；而偏移错了，守卫会红。

### ★ 本目录是 TypeScript，且**没有构建步骤**

（`AGENTS.md` §3 把 `apps/emulator` 定为 TS 落点。）
`.ts` 由 Node v24 的**原生 type stripping** 直接跑，所以守卫（`.mjs`）可以
`import '../../apps/emulator/src/model/pools.ts'` —— 一条命令都别加。
⇒ 由此带来的**写法约束**：只许用**可擦除**语法（类型标注 / `interface` / `type` / `import type`）；
❌ 不用 `enum` / `namespace` / 构造器参数属性 / 装饰器（它们要**代码生成**，会要求构建步骤）。
★ `apps/emulator` **已进 pnpm workspaces**（判据是"有没有自己的依赖"：它的模型要 import `@amayui/age-format`）
⇒ 有 `apps/emulator/package.json`，跨包引用走包名（`@amayui/age-format/src/...`），
**不用** `../../../../packages/...` 那种绑死目录层级的相对路径。
★ 类型检查：`pnpm typecheck`（`tsc -p apps/emulator/tsconfig.json --noEmit`）。
它**只做检查、不产出**；那份 tsconfig 里的 `erasableSyntaxOnly` 把"Node 剥壳跑不了的语法"提前变成类型错误。

**它是什么**：`GLOBAL`（global 池族的基址/`*_alt`/计数槽）· `FRAME`（基址 / 步长 / 帧内偏移）·
`LOCAL_POOLS`（6 个 local 池的计数与基址）· `GlobalPools` / `LocalPools` 两个视图 ·
`frameBaseOf` / `operandAt`（操作数寻址）。

**它不是什么**（★ 与用户口径一致，别指望它跑脚本）：
* ❌ **不含整体执行流程**（帧循环 / 主循环）—— 本批不做，模拟器也还启动不了；
* ❌ 不含引擎体上基于 offset 的字段（那是 `fields.json` 那一层，属别的批次）；
* ❌ 不含循环副作用（渲染 / 音频 / 输入）—— 遇到就登记，不实现。

**两条不许动摇的口径**（都由守卫钉住）：
1. **int 族槽的值是编码位模式** ⇒ 读必须过 DEC、写必须过 ENC（`packages/age-format/src/asm/value-codec.mts`）；
   **float 族不过**；**下标不过**（`base + idx*4` 是纯算术）。`enc_zero ≠ 0` ⇒ "未初始化 = 0"是错的。
2. **引擎不做越界检查** ⇒ 模型也**不** clamp、**不**补 0：未初始化/越界读返回 `null`，并记进 `LocalPools.noteOOB`
   （"引擎没做的事"必须**显式留痕**，不是悄悄替它做）。

★ **语言口径的一处例外声明**：本目录按 §3 是 TypeScript 的落点，但**工具链尚未接**（不进 workspaces、无构建步骤），
而 `pnpm test` 必须能直接 `node --test` 跑起来 ⇒ 本轮用 **`.mjs` + JSDoc 类型注释**。
接 TS 时（含 `pnpm-workspace.yaml` 与构建）应整体迁过去，**不许**只迁一半。

★ **模型里的常量都可回语料复核**（守则会跑一遍），因此**不许**填"看起来整齐"的数：
实测与旧仓说法不一致的地方（例如 `local_float` 的基址格 `帧+0x38` 语料里零次出现）
一律标 **`baseUnverified`** 并开单跟踪，见需求树。
