# core spec —— 机制地图（**组织真源**）

> 本文件只回答三件事：core 切成哪些机制、每个机制凭什么算完、文件和 id 怎么放。
> **不写进度**（进度 = `pnpm tools requirements plan`）、**不写 schema**（= `pnpm tools spec describe`）。
> 硬口径见 `docs/00-origin/project-intent.md` 与技能 `amayui-spec`；本文件是它们的**落点补充**，不重复。

## 1. 范围

**在 core 里**：引擎自身的行为 —— 脚本解码、指令驱动、帧、池与值、指令语义（引擎数据部分）、启动入口、容器格式。
**不在 core 里**：render / audio / input 三个相对独立的模块。

★ 这三个模块**不实现、也不记录 side effect**：我们对它们的语义没有理解，记录只会制造假观测面。
⇒ core 条目对"外部调用"只声明两件事：**它是否改变引擎数据、改变了哪一部分**；外部效果**不作断言**。

写条目时的推论：

* 一个 opcode 若只发外部请求、不改引擎数据 ⇒ 它的 core 合同是"**无引擎数据效果**"，标 `external-out-of-scope`；
* ⛔ 不要凭接口名字 / 旧仓的注释猜外部语义（`AGENTS.md` §0.3）。

### 1.1 显式延后的机制（记录，不是沉默）

| 机制 | 现在的替代 | 解除条件 |
|---|---|---|
| `CORE-FS` | 临时引导直接读特定文件（硬编码路径） | 需要按真实路径 / 搜索规则装载时 |
| `CORE-BOOT` | 临时引导（从特定文件进主循环） | 机制归属重梳完成后 |

★ 延后 **不等于** 未规定：条目里必须写 `deferred` + 理由 + 解除条件；
⛔ 不许静默跳过（那是把"没写"冒充"不适用"）。

## 2. 机制表

| 机制 | 回答什么 | 闭集（可枚举） | 依赖 | 迁移自 |
|---|---|---|---|---|
| `CORE-ISA` | 脚本字节 → 指令：头部 / `argc` 边界 / `type`·`raw` / 指令表 | 指令表条目 | — | `10-format` |
| `CORE-VM` | 指令驱动迭代：一次迭代的步骤 / 何时回泵 / 何时执行一条指令 / 退出与中止 | 8 个层段 + opcode 域 `[0,0x400)` + 状态选择位 | ISA, FRAME | `10-format` 执行模型 |
| `CORE-FRAME` | 帧与上下文：帧记录 / 局部池绑定 / `call-script` / `call-frame` / `load-frame` / `ret` | 帧类 opcode + 帧字段（shape） | VM, POOL | `10-format` 残差 |
| `CORE-POOL` | 数据池与值：族 / 槽 / 五种访问原语 / 标签支持集 / 编解码 / 容量 / 字符串记录 | 操作数类型标签 | — | `40-pools` |
| `CORE-CONTAINERS` | 容器句柄：懒分配 / 登记 / 元素寻址 | 容器句柄类型（`0x8003/5/9/B`）+ 分配与登记点 | POOL | `40-pools` 的域外项 |
| `CORE-OBJECTS` | 对象模型：根对象 / 基类 / 类型身份 / 1000 槽表 | 对象类型表（shape）+ 1000 槽表 | POOL | 无（新写） |
| `CORE-OPS` | 指令语义：逐 opcode 回答"改变什么" | 指令表全部条目（handler + 缺省） | ISA, VM, POOL, FRAME | `50-engine-state` |
| `CORE-CONFIG` | 配置：键 / 存在性 / 默认值 | 配置键的读点 | FS | 无（新写） |
| `CORE-TIMING` | 时间：时钟 / 帧率 / 等待 | 时间读取点 + 等待门 | VM | 无（新写） |
| `CORE-BOOT` | 启动入口：入口 → 初始化 → 调用主循环（**不返回**） | 入口 + 主循环 callee | FMT, POOL, FS, TIMING | 无（**临时**） |
| `CORE-FS` | 文件读写原语：路径解析 / 打开 / 读 / 写 / 存在性 / 搜索路径 | 引擎打开的文件 + API 调用点 | — | 无（**显式延后**） |
| `FMT-ALF` | ALF 归档：索引头 / 段 / LZSS / TOC / 载荷定位 | 文件类型 + 签名 | — | `20-alf` |
| `FMT-AGF` | AGF 图像容器：ACGF / meta / body / ACIF 段表 | 文件类型 + 签名 | — | `30-agf` |
| `FMT-SAVE` | 存档序列化：写入 / 读取原语（不改语义） | 存档样本字段 | POOL | 无 |

★ 「迁移自」是**内容来源**，不是"一章只归一个机制"：`10-format` 的内容分到 ISA / VM / FRAME 三处。

★ **像素排布 / 纹理上传 / 混音 / 输入事件**归各自模块（core 外）。`FMT-AGF` 只到"段表 + 尺寸 + 像素起点"。

### 2.1 与 `audit.planned[]` 的对齐

`audit.planned[]` 是"还没成篇的域"（旧口径），机制表是新口径。映射如下（**改一边必须改另一边**）：

| planned | 机制 |
|---|---|
| `containers` | `CORE-CONTAINERS` |
| `objects` | `CORE-OBJECTS` |
| `files` | `CORE-FS` + `CORE-BOOT` |
| `save` | `FMT-SAVE` |
| `config` | `CORE-CONFIG` |
| `timing` | `CORE-TIMING` |
| `input` | **core 外**（不实现、不记录） |
| `presentation` | **core 外**（不实现、不记录） |

★ 机制表是组织真源，`planned[]` 只是 `audit` 数据的一个视图。等 `data/spec/chapters/core/` 落地后，
把 `planned[]` 的 id 改成机制 id（同批改 `outOfScope[].chapter`），并让不变量检查"每个机制都有需求子单"。

### 2.2 `CORE-VM` 的子机制（先扫后拆的结果）

主循环**不是**"多层嵌套 while"——那是反编译视图。实测骨架 = **1 个外层迭代 + 1 个消息泵 + 6 个有界循环**；
一条指令只在状态选择位为 0 时执行。⇒ 拆成五个子机制（**同属 `CORE-VM`**，不是五个新机制）：

| 子机制 | 回答什么 | 闭集 | 依赖 | 条目量级 |
|---|---|---|---|---|
| `VM-LOOP` | 一次迭代由哪些步骤组成；何时回泵；何时执行一条指令 | 8 个层 / 段 | ISA, FRAME | 5–7 |
| `VM-DISPATCH` | 取指 → 域检查 → 处理器选择 → `ip` 推进 | opcode 域 + 处理器表 + 2 个字段 | ISA, FRAME | 3–4 |
| `VM-GATE` | 什么条件本趟不推进；等消息 / 定时对象 / 状态位 | 11 个 Sleep + WaitMessage + timer 对象 + watchdog | LOOP | 6–9 |
| `VM-STATE` | 选位 / 清位 / 优先级 / 快照 | 位域 + 派发顺序 + 快照纪律 | LOOP | 5–7 + 1 张表 |
| `VM-EXC` | 谁进来、怎么出去、错误怎么冒泡 | 3 throw + 5 catch + 尾声 + 入口边 | LOOP | 5–6 |

**反堆叠纪律**（写条目之前先看）：

1. `VM-STATE` ≤ 8 条；**逐位掩码只进 `bindings` 的附表**，不逐位写条目。
2. 每条逐位规则必须给出消费它的 `CORE-OPS` / `CORE-BOOT` 条目 id（反向引用，不构成实现依赖）。
3. 拆轴是**控制流层**，不是状态：每个状态位块自带等待 / Sleep / 门控；按状态拆会把同一个块写 23 遍。

## 3. 组织规则

1. **机制 = 有闭集 + 有契约的单位**；一个机制一个目录：`data/spec/chapters/core/<机制小写>/`。
2. **规则 id 带机制前缀**：`CORE-ISA-*` / `CORE-VM-*` / `CORE-FRAME-*` / `CORE-POOL-*` / `CORE-CONTAINERS-*` /
   `CORE-OBJECTS-*` / `CORE-OPS-*` / `CORE-CONFIG-*` / `CORE-TIMING-*` / `CORE-BOOT-*` / `CORE-FS-*` /
   `FMT-ALF-*` / `FMT-AGF-*` / `FMT-SAVE-*`。
3. **函数 ↔ 机制多对多**：⛔ 不做"一个函数属于一个机制"的分区；大函数内部可以驱动多个机制。
4. **覆盖 = marks + 去重**：mark 记"这条陈述处理了哪些指令的哪些效果"（局部 / 绝对两种）；
   多条陈述可共同解释同一义务；同一义务**只能计一次**。
5. **依赖是偏序**：机制间 `dependsOn` 表达"实现 B 前必须先有 A 的哪条行为"；阅读顺序 = 拓扑序，⛔ 不靠编号。
6. **分析不读旧实现**：`apps/emulator/src/**` 视为废弃代码，只允许读它的架构文档（`apps/emulator/README.md`）。
7. **大机制先扫后拆**：预计一组条目装不下时（`CORE-VM` 就是：嵌套 while 主循环 + 大量状态标记），
   先让子代理做**浅层扫描**（控制流层 / 状态标记清单 / 建议拆分），再定子机制；
   ⛔ 不许把整个机制堆进一组条目。

## 4. "机制闭环"的判据

一个机制算闭环，当且仅当：

1. 闭集里的**每个元素**都有归宿：被某条条目回答 / 明示"未规定"（带理由）/ 残差（带 `next`）；
2. 每条条目都有 **spec ↔ EA 映射**（§5）；
3. 至少一个**真实程序**能执行到该机制的前沿（未实现即抛错，缺口按 opcode / 脚本行计数）。

⛔ 不用"条目数量""覆盖率数字"代替这三条。

## 5. spec ↔ EA 映射（每条条目必须有）

* 每条条目在 `bindings.json` 里有机器映射（**字段名见 `pnpm tools spec describe`，本文不复述 schema**）：
  站点 EA / 函数符号 / `storage` 指针（`layout:` / `shape:`）/ 证据（`KN:` / `guard:`）；
* EA、函数符号、偏移**只进** `bindings` / `audit`，⛔ 不进正文（自包含不变量会红）；
* 映射完整性：闭集元素 ↔ 站点；还没映射上的写进**条目自己的残差**（`audit.rules[].residual` / `pilot.residuals`），带下一步问题。

## 6. 草稿与合并（子代理协作）

* 子代理**只写** `.tmp/spec-draft/<机制>/`（不入库），产出：
  * `README.md` —— 机制概述 / 闭集清单 / 依赖 / 未决；
  * `entries.md` —— 条目草稿（id、合同、条件、状态对应、marks、证据、残差）；
  * `links.json` —— `规则 id → {sites, functions, storage, evidence, covers}`；
    `covers` 是 effect 选择器（合并时直接进 `audit.rules[].covers`）——⛔ 只给 `sites` 会让合并者手工重译；
  * `feedback.md` —— 对基建 / 技能 / 口径的质疑（**必须附可执行反例**）。
* **证据只能用 `KN:` / `guard:`**；草稿里可用 `draft:` 占位，但合并前必须换成前者（`spec validate` 的 #7 会红）。
* **合并由外层做**（审 + 落 `data/spec` + 跑门禁）：子代理不直接改 `data/spec`。
* 子代理若认为口径不合理，**说出来**；⛔ 不许默默绕过或自创一套。

## 7. 阶段（依赖序，不是进度表）

1. `CORE-ISA` + `CORE-POOL`：解码与内存模型（不需要指令语义）。
2. `CORE-VM` + `CORE-FRAME`：跑得起来的最小执行器。
3. `CORE-OPS`：按行为族推进，闭集逐条回答。
4. `FMT-*` + `CORE-BOOT`：容器与临时引导。
5. `apps/emulator-new`：**只读 `data/spec`** 的参考实现；先落已闭环机制，用真实语料验证并回灌缺口。

## 8. 未决与风险（不藏）

* `CORE-BOOT` 是**临时**的：先按已有知识从特定文件引导；归属与顺序后续重新梳理（它现在不是稳定机制）。
* 外部副作用不记录 ⇒ 涉及 render / audio / input 的行为**本轮无法验证**：这些 opcode 只能验"引擎数据部分"；
  任何依赖外部行为的测试都必须显式标 `external-out-of-scope`，⛔ 不许当通过。
* 替代 `age-format` 的新实现包（spec 的可信版本）名字 / 边界未定；`age-format` 预期最终废弃。
* 旧分章的迁移是**内容搬迁**，不是重写：旧条目先搬进机制目录，再逐条补 marks / 映射。

## 9. 怎么查 / 怎么改

```bash
pnpm tools spec validate      # 不变量（正文自包含 / 跨文件一一对应）
pnpm tools spec report        # 覆盖 / 内部省略 / 残差
pnpm tools spec chapters      # 章节骨架与每章规则
pnpm tools spec show <id>     # 一条条目 + 证据有效状态
pnpm tools opcodes handlers   # opcode → handler EA（闭集与映射的入口）
pnpm tools requirements plan  # 还要做什么、到哪一步
```

* 改机制表 ⇒ 改本文件 + 在需求树开 / 关对应单；
* 改条目 ⇒ 改 `data/spec/chapters/core/<机制>/`（正文）+ `bindings.json` / `audit.json`（映射）；
* 改判定口径 ⇒ 先改技能 `amayui-spec`，再回改这里（技能是口径真源，本文件是落点）。
