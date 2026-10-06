# apps/emulator — 模拟器（按新结构**重写**，不搬旧代码）

## 1. 本目录的边界（★ 口径，不是进度）

* **有**：`src/model/` —— 池模型 · 纯数值指令族 · 迭代系统。这是"按建模语义**重新实现**"的那一层。
* **没有**：**执行核心**（帧循环 / 主循环）⇒ 模拟器**还启动不了**；表现层与窗口在 M4-2（见 §5.1）。
* ★ **文件清单不写在这里**（看目录本身）；"还要做什么"看 `pnpm tools requirements plan`（`AGENTS.md` §10）。

## 2. 决定：按新结构重写（不搬旧代码）

用户口径（本轮确认）：旧仓 `app/amayui-emulator`（589 跟踪文件 + 268 测试文件）**不迁移**，
只作**重写参考**（登记为 `corpus/assets.json` 的 `app/emulator-old`，`role: rebuild`）。

## 3. 重写时必须解决的三件事（来自旧仓的实测症状）

1. **跨域守卫寄生**：旧仓有 **10 个跨域守卫住在模拟器的 `test/` 下** —— 重写时把它们**拆回各自域的包**
   （格式 / 台账 / 翻译 / 语料各管各的）。
2. **工具链单一化**：本仓**只有"自带工具链的 app"用 TypeScript**，其余一切 JS 是 `.mjs`、无构建步骤
   （`AGENTS.md` §3）；★ 本 app 自己也**没有构建步骤**（`.ts` 由 Node 原生剥壳直接跑，见下）。
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

## 6. `src/model/`：通用数据区域（批 R1 迭代点 ④）

```text
apps/emulator/src/model/pools.ts        ← 池模型（**语义**：有哪几族池 / 谁要编解码 / 越界怎么办）
apps/emulator/src/model/numeric-ops.ts  ← 纯数值指令族（**语义**：opcode / 助记符 / argc / 语义）
apps/emulator/src/model/iterate.ts      ← 迭代系统：字节流 → 指令（只切边界，不解释）
tools/test/emulator-*.test.mjs          ← 它们的守卫（★ 分档写在各自首行 pragma 里：回语料复核常量的是 @env assets，
                                           快照/分区那两支是 @env pure ⇒ `pnpm test` 就跑得动）
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

**它是什么**（★ 只有语义，一个偏移都没有）：`LOCAL_POOLS`（6 个 local 池的族 / 元素宽度 / 是否过编解码 / operand type tag）·
`GLOBAL_POOL_NAMES` / `GLOBAL_ENCODED` · `LocalPools` / `GlobalPools` 两个视图（`read` / `write` / `initZero` / `noteOOB`）·
`snapshot()` / `restore()` / `oobSummary()`（快照接缝，见 §7）· `STATE_PARTITION`（状态分区表）· `iterate()`（字节流 → 指令，只切边界）。

★ 池的**计数槽 / 基址槽**（`GLOBAL_SLOTS` / `FRAME_LAYOUT` / `LOCAL_POOL_SLOTS`）与**按地址取操作数**的算法
（`frameBaseOf` / `operandAt`）**不在本目录**：它们是布局知识（上面那条硬口径）。
守卫不只反对"写进模型"，还断言模拟器**不导出**这两个 API。

**它不是什么**（★ 与用户口径一致，别指望它跑脚本）：
* ❌ **不含整体执行流程**（帧循环 / 主循环）—— 本批不做，模拟器也还启动不了；
* ❌ 不含引擎体上基于 offset 的字段（那是 `fields.json` 那一层，属别的批次）；
* ❌ 不含循环副作用（渲染 / 音频 / 输入）—— 遇到就登记，不实现。

**两条不许动摇的口径**（都由守卫钉住）：
1. **int 族槽的值是编码位模式** ⇒ 读必须过 DEC、写必须过 ENC（`packages/age-format/src/asm/value-codec.mts`）；
   **float 族不过**；**下标不过**（`base + idx*4` 是纯算术）。`enc_zero ≠ 0` ⇒ "未初始化 = 0"是错的。
2. **引擎不做越界检查** ⇒ 模型也**不** clamp、**不**补 0：未初始化/越界读返回 `null`，并记进 `LocalPools.noteOOB`
   （"引擎没做的事"必须**显式留痕**，不是悄悄替它做）。★ 这条留痕是**诊断**、不是引擎态 ⇒ 不进快照（见 §7.1）。

★ **模型里的常量都可回语料复核**（守则会跑一遍），因此**不许**填"看起来整齐"的数：
实测与旧仓说法不一致的地方（例如 `local_float` 的基址格 `帧+0x38` 语料里零次出现）
一律标 **`baseUnverified`** 并开单跟踪，见需求树。

## 7. 状态与快照（★ 接缝已就位，**故意没有**文件格式）

快照 / 恢复迟早要有 —— 排查的本质是**二分**："这一帧的状态是从哪一步开始不对的"，
没有"把某一刻的引擎态存下来、之后反复回到这一刻做对照"的手段，每验一个假设都要重跑几分钟。
★ 但本条只定**口径与接缝**：**不**建快照模块、**不**定文件格式与版本号 ——
今天还没有"状态"的第二个来源（没有引擎对象、没有执行循环），此刻定格式只能靠猜。
等执行核心落地、并且有"恢复后重跑到第 N 帧 == 自然跑到第 N 帧"这条判据时再定。

### 7.1 状态四分（每个可变字段必须表态）

| 类别 | 进快照？ | 口径 |
|---|---|---|
| `engine` | ✅ | 引擎态 ⇒ 两份快照**逐字节可比** |
| `derived` | ❌ | 由引擎态算得 ⇒ 灌回去要么无意义、要么与引擎态打架 |
| `diagnostic` | ❌ | 模型的**留痕**（"引擎不做越界检查"这件事本身）。它不是引擎态，而且"恢复后重跑到第 N 帧"与"自然跑到第 N 帧"在它上面**必然**不同（前者没经历过那些访问）⇒ 混进去只会让等价判据**红得没意义** |
| `host` | ❌ | 宿主注入的回调 / 桥。恢复**不该**把调用者的闭包换掉 |

可执行形式 = `src/model/pools.ts` 的 `STATE_PARTITION`。守卫 `tools/test/emulator-state-partition.test.mjs`
用**反射**核：自有字段必须被归类 · 表里不许有过期条目 · 类别落在闭集合里 ·
**`engine` 类的字段恰好就是快照的顶层键**（把"进不进快照"从散文变成等式）· `diagnostic` 的字段不得出现在快照里。

### 7.2 快照的三条不变量（守卫 `tools/test/emulator-snapshot.test.mjs`）

1. **同样状态 ⇒ 逐字节相同的纯数据**。`Map` 的迭代顺序是**插入顺序**（实测：对已存在的键再 `set` **不**改变位置）
   ⇒ 池名与下标一律**升序**。否则"同一状态、不同写入历史"会给出不同字节，而恢复又按键序重建 ⇒ 与自然运行分叉。
2. **稀疏保真**：只记**在场**的槽。"这里没东西"与"这里的值是 0"是两件事 —— int 族的初值是 `enc_zero`，那**不是** 0。
3. **必带 `key`**：int 族槽里是**编码位模式**，没有 `key` 就不可解释。`value-codec.mts` 连默认值都不给
   （静默用 0 = 把"我不知道 key"伪装成"key 是 0"）⇒ 快照同样**不许简写**。
   ★ **不存解码值**：那是第二份真源，会和位模式打架（要看解码值就在恢复出来的实例上调 `read()`）。

### 7.3 三条硬口径

* **恢复 = 构造新实例**（`LocalPools.restore(snap)`），不是"往现有实例里灌"：`key` 是只读的，
  而且"恢复"不该悄悄改掉调用者手上的对象。
* **快照边界 = 帧边界**（现在还没有 `step()` 循环，所以这条只能先立在口径里）。
* **不可信输入要响亮失败**：未知池名 / 下标重复 / **缺池** / 值类型不符 一律**抛**，不静默跳过 ——
  `read()` 内部是 `this.pools.get(name).get(idx)`，缺一个池就是运行期崩；「缺池」是**构造器的不变式**
  ⇒ 凡是能构造出来的实例，`read()` 都是全的（"手工塞半份池"那条路也一并挡住）。
  ❌ **不要拿 `structuredClone(实例)` 当快照**：它**丢原型**（变成纯对象、方法全没）⇒ 只能走 `snapshot()`。

### 7.4 还没有承载面的量 ⇒ **先进分区表，再谈实现**

★ 口径：凡是要进快照的量，实现时必须**先**出现在 `STATE_PARTITION` 里（守卫会核），
不许"顺手 `this.foo = …`"就完事 —— 那正是"有个量没回去"的来历（这类错**不报错**，只让两条路径静静分叉）。
★ 确实有几类量今天还没有承载面（非确定源 · 数组容器与外挂缓冲的**别名语义** · float 族的落槽宽度 ·
`initZero` 的边界）。它们的**待定登记与重开条件不写在这里**：看 `pnpm tools requirements plan`（`AGENTS.md` §10）。
