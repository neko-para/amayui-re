# core spec —— 机制地图（**组织真源**）

> 本文件只回答三件事：core 切成哪些机制、每个机制凭什么算完、文件和 id 怎么放。
> **不写进度**（进度 = `pnpm tools requirements plan`）、**不写 schema**（= `pnpm tools spec describe`）。
> 硬口径见 `docs/00-origin/project-intent.md` 与技能 `amayui-spec`；本文件是它们的**落点补充**，不重复。

## 1. 范围

**在 core 里**：引擎自身的行为 —— 脚本解码、指令驱动、帧、池与值、逐 opcode 的引擎数据效果、启动入口、容器格式。
**不在 core 里**：render / audio / input / ADV 文字 / Live2D 等相对独立的模块的**内部语义**。

★ core **不实现**这三个模块；但它们**向平台发出的请求边界**（请求 + 参数 + 时序）可以写在 spec 的专门章节：
`06-window`（启动期窗口 / 图形设备 / 消息泵）、`60-ops-render`（渲染目标 / 场景混合 / 离屏文字）
与 `60-ops-input`（输入字 / 回调登记与派发 / 键鼠 / 触点查询 / 主循环模式字）。
这些章节只记录"引擎发出了什么、改了哪些引擎数据"，⛔ 不断言平台是否受理、也不断言像素 / 混音结果。
⇒ core 条目对"外部调用"只声明两件事：**它是否改变引擎数据、改变了哪一部分**；外部效果**不作断言**。

写条目时的推论：

* 一个 opcode 若只发外部请求、不改引擎数据 ⇒ 它的 core 合同是"**无引擎数据效果**"（`disposition=handled`），
  外部效果本身写进该规则的残差（带 `next`）；⛔ 不另造 "core 外" 的标记（枚举只有 `pnpm tools spec describe` 那五个）；
* ⛔ 不要凭接口名字 / 旧仓的注释猜外部语义（`AGENTS.md` §0.3）。

### 1.1 显式延后的机制（记录，不是沉默）

| 机制 | 现在的替代 | 解除条件 |
|---|---|---|
| `CORE-FS` | 临时引导：松散文件优先 + ALF 归档切片（S4 族）；S5 / `S3IN` / `APPEND*.AAI` 叠加索引未覆盖；**松散文件的搜索根与搜索顺序未覆盖**（`25-fs` 只规定"按条目登记名直接打开"） | 需要按真实路径 / 搜索规则装载时 |
| `CORE-BOOT` | 临时引导：`SYS4INI.BIN` 索引 → 根脚本 id 0；窗口 / 图形设备 / 消息泵 / 定时器已写在 `06-window`；自更新 / 单实例 / COM / 命令行仍不在本版 | 机制归属重梳完成后 |

★ 延后 **不等于** 未规定：它必须落成 `audit.planned[]` 的一条（`status` + `residual` 带 `next`），
  或落在已写章节的残差里；解除条件写进那条残差的 `next`；
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
| `CORE-OPS` | 逐 opcode 的引擎数据效果：回答"改变什么" | 指令表全部条目（handler + 缺省）；未成篇的表项按族登记在 `audit.gaps[]`（唯一落点） | ISA, VM, POOL, FRAME | `50-engine-state` |
| `CORE-CONFIG` | 配置：默认表 / 玩家文件 / 保存 / 脚本访问 | 配置键词表 + 五条规则 | FS | 无（新写） |
| `CORE-TIMING` | 时间：时钟 / 帧率 / 等待 | 时间读取点 + 等待门 | VM | 无（新写） |
| `CORE-INPUT` | 输入边界：输入字 / 键总数 / 回调登记与派发 / 键鼠 / 触点查询 / 主循环模式字 | 输入族 opcode（`0xfe/0xff/0xfb/0xcc/0xcd/0x100/0x108/0x109/0x2fc/0x24e`）+ 32 个输入位 + 每帧按键回调表 | VM, TIMING | 无（新写） |
| `CORE-BOOT` | 启动入口：入口 → 初始化 → 调用主循环；**已证出口 = 退出消息 → 异常**（见 `06-window` 的事件入口） | 入口 + 主循环 callee + 启动期请求 | FMT, POOL, FS, TIMING | 无（**临时**） |
| `CORE-FS` | 文件读写原语：路径解析 / 打开 / 读 / 写 / 存在性 / **搜索路径（未覆盖）** | 引擎打开的文件 + API 调用点 | — | 无（**显式延后**） |
| `FMT-ALF` | ALF 归档：索引头 / 段 / LZSS / TOC / 载荷定位 | 文件类型 + 签名 | — | `20-alf` |
| `FMT-AGF` | AGF 图像容器：ACGF / meta / body / ACIF 段表 | 文件类型 + 签名 | — | `30-agf` |
| `FMT-SAVE` | 存档序列化：写入 / 读取原语（不改语义） | 存档样本字段 | POOL | 无 |

★ 「迁移自」是**内容来源**，不是"一章只归一个机制"：`10-format` 的内容分到 ISA / VM / FRAME 三处。

★ **像素排布**写在 `30-agf`（属于 FMT-AGF 的合同）；**纹理上传 / 混音 / 输入模块内部**归各自模块（core 外）。`FMT-AGF` 到"段表 + 尺寸 + 像素排布"为止，不到平台上传。

### 2.1 与 `audit.planned[]` 的对齐

`audit.planned[]` 是"还没成篇的域"（旧口径），机制表是新口径。映射如下（**改一边必须改另一边**）：

| planned | 机制 |
|---|---|
| `containers` | `CORE-POOL` 的容器句柄两条（**部分**已成篇） |
| `objects` | `CORE-OBJECTS` |
| `save` | `FMT-SAVE` |
| `presentation` | **请求边界已成篇**（规则挂在 `CORE-BOOT` / `CORE-OPS` 下，见 `06-window` / `60-ops-render`）；表现层**内部**（像素 / 混音 / 音频输出）仍未成篇，仍属 `core-out` |
| `adv-text` | **core 外**（ADV 文字显示内部；请求侧 / 字符串侧的条目已散在 `60-ops-text` / `60-ops-list`） |
| `live2d` | **core 外**（Live2D 内部；接口 / 不变量 / source binding 三件套未做） |

★ `config` / `timing` 两行已删除：它们**已有章节（机制未闭环）**（`55-config` / `70-vm`），不在 `planned[]` 里。
★ `files` 一行已删除：`CORE-FS` / `CORE-BOOT` **已有章节（机制未闭环）**（`25-fs` / `05-boot`），不再算"还没成篇的域"；两者的未闭合项见 §1.1 与本文件 §8 —— **有章不等于机制闭环**（判据见 §4）。`CORE-FS` 的**搜索路径仍未覆盖**（§1.1 第一行）。
★ `containers` 的机制归属按上表仍是 `CORE-POOL` 的容器句柄两条：它们在 `40-pools` 里已有规则，
但"元素类型 / 生命周期 / 失败路径"还没成篇 ⇒ 保留 planned，不冒成 `CORE-CONTAINERS` 的独立章。
★ 机制表是组织真源，`planned[]` 只是 `audit` 数据的一个视图；**改一边必须改另一边**。
★ **机器形态**：每条 `planned[]` 带 `mechanism` 字段（上表的右列；`core-out` = core 外），
`tools/test/spec-mechanisms.test.mjs` 守三件事 —— 闭集、每项都有归属、每个机制至少有一个需求单在跟踪。
⛔ 不要只改表不加字段（那样守卫会红，而不是"表更新了"）。

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

1. **机制 = 有闭集 + 有契约的单位**；规则**平铺落在章文件**里（`data/spec/chapters/<章>.md`），
   机制只体现在**规则 id 前缀**上；⛔ 不为机制建目录（阅读顺序由 `spec.json` 的 `chapters[]` 决定）。
2. **规则 id 带机制前缀**：`CORE-ISA-*` / `CORE-VM-*` / `CORE-FRAME-*` / `CORE-POOL-*` / `CORE-CONTAINERS-*` /
   `CORE-OBJECTS-*` / `CORE-OPS-*` / `CORE-CONFIG-*` / `CORE-TIMING-*` / `CORE-BOOT-*` / `CORE-FS-*` /
   `FMT-ALF-*` / `FMT-AGF-*` / `FMT-SAVE-*`。
   ★ `SPC-*`（`SPC-POOL-*` / `SPC-OPERAND-*` / `SPC-VALUE-*`）是**试点期的历史前缀**，仍合法、不再新增；
   新规则按机制前缀命名。
3. **函数 ↔ 机制多对多**：⛔ 不做"一个函数属于一个机制"的分区；大函数内部可以驱动多个机制。
4. **覆盖 = marks + 去重**：mark 记"这条陈述处理了哪些指令的哪些效果"（局部 / 绝对两种）；
   多条陈述可共同解释同一义务；同一义务**只能计一次**。
5. **依赖是偏序**：机制间 `dependsOn` 表达"实现 B 前必须先有 A 的哪条行为"；阅读顺序 = 拓扑序，⛔ 不靠编号。
6. **分析不读旧实现**：`apps/emulator/src/**` 视为废弃代码，只允许读它的架构文档（`apps/emulator/README.md`）。
7. **大机制先扫后拆**：预计一组条目装不下时（`CORE-VM` 就是：嵌套 while 主循环 + 大量状态标记），
   先让子代理做**浅层扫描**（控制流层 / 状态标记清单 / 建议拆分），再定子机制；
   ⛔ 不许把整个机制堆进一组条目。

### 3.1 `reviewed` 与语料型机制（2026-10-21 裁决）

`reviewed` 的判据不变：**全部证据 strong + 至少一条 accepted 台账记录或 `@env pure` 守卫 + 有 falsifiers**。
语料型机制（判据必须读语料、守卫只能是 `@env assets`）的升档路径是**补一条 accepted 台账记录**（锚 EA），
而不是把 assets 守卫当强证据：assets 档默认门禁不跑，"绿"只在有人手动跑时成立。
拿不到 accepted 记录时保持 `draft`，并在残差里写"缺哪条 accepted 观察"——⛔ 不许用 assets 守卫冒充。

## 4. "机制闭环"的判据
一个机制算闭环，当且仅当：

1. 闭集里的**每个元素**都有归宿：被某条条目回答 / 明示"未规定"（带理由）/ 残差（带 `next`）；
2. 每条条目都有 **spec ↔ EA 映射**（§5）；
3. 至少一个**真实程序**能执行到该机制的前沿（未实现即抛错，缺口按 opcode / 脚本行计数）。

⛔ 不用"条目数量""覆盖率数字"代替这三条。

## 5. spec ↔ EA 映射（每条条目必须有）

* 每条条目在 `bindings.json` 里有机器映射（**字段名见 `pnpm tools spec describe`，本文不复述 schema**）：
  站点 EA / 函数符号 / `storage` 指针（`shape:`）/ 证据（`KN:` / `guard:`）；
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
* ★ **已合并的历史草稿不回填 `covers`**（2026-10-21 裁决）：`.tmp/spec-draft/*` 是过程材料、不入库，且其规则已合并且 `covers` 由 `audit.json` 单独维护；给旧草稿补 `covers` 不会改变任何产物。`covers` 是**新草稿**的硬要求（合并时省一次重译），不是历史债务。
* **合并由外层做**（审 + 落 `data/spec` + 跑门禁）：子代理不直接改 `data/spec`。
* 子代理若认为口径不合理，**说出来**；⛔ 不许默默绕过或自创一套。

## 7. 阶段（依赖序，不是进度表）

1. `CORE-ISA` + `CORE-POOL`：解码与内存模型（不需要逐 opcode 的语义）。
2. `CORE-VM` + `CORE-FRAME`：跑得起来的最小执行器。
3. `CORE-OPS`：按行为族推进，闭集逐条回答。
4. `FMT-*` + `CORE-BOOT`：容器与临时引导。
5. `apps/emulator-new`：**只读 `data/spec`** 的参考实现；先落已闭环机制，用真实语料验证并回灌缺口。

## 8. 未决与风险（不藏）

* `CORE-BOOT` 是**临时**的：先按已有知识从特定文件引导；归属与顺序后续重新梳理（它现在不是稳定机制）。
* render / audio / input 的**内部**副作用不记录 ⇒ 涉及它们的平台侧行为**本轮无法验证**：请求边界条目只能验"引擎发出了什么、改了什么引擎数据"（输入的引擎侧边界已写进 `60-ops-input`，但平台采样 / 物理键映射仍不可验证）；
  任何依赖平台受理 / 像素 / 混音的测试都必须读规则残差里那句"外部效果本版不规定"，⛔ 不许把它当通过。
* 替代 `age-format` 的新实现包（spec 的可信版本）名字 / 边界未定；`age-format` 预期最终废弃。
* 旧分章的迁移是**内容搬迁**，不是重写：旧条目先搬进机制目录，再逐条补 marks / 映射。

## 9. 怎么查 / 怎么改

### 机器细节去哪查（★ 正文**不写**"见内部绑定"）

规范正文是**可分发**的：机器细节（引擎**内存**里的偏移 / 布局 / 实现容量 / 编号偏移）在正文里一律写"属机器细节，本规范不规定"。
★ 文件协议的字段偏移（容器头 / TOC / meta）**不是**机器细节 —— 它们是规范要定义的协议，写在正文里。
⛔ 不写"见内部绑定" —— 那对读者是**悬空指针**（`bindings.json` 不随规范分发，读者无从解析）。

查法（内部复核材料）：

- 一条规则的站点 / 函数 / storage 指针 / 证据 / 残差：`pnpm tools spec show <规则 id>`
- 反查某个 EA 被哪些规则引用：`pnpm tools spec at --ea <EA>`
- storage 指针指向的机器本体：`data/shape/types.json` 的类型与成员（`pnpm tools shape describe` 看 schema）；
  指针里的 `@0x…` 是该类型成员表里的**成员起点**（`shape:T1@0x60` = 第一条帧记录的 `+0x60` 那一格）


```bash
pnpm tools spec validate      # 不变量（正文自包含 / 跨文件一一对应）
pnpm tools spec report        # 覆盖 / 内部省略 / 残差
pnpm tools spec chapters      # 章节骨架与每章规则
pnpm tools spec show <id>     # 一条条目 + 证据有效状态
pnpm tools opcodes handlers   # opcode → handler EA（闭集与映射的入口）
pnpm tools requirements plan  # 还要做什么、到哪一步
```

* 改机制表 ⇒ 改本文件 + 在需求树开 / 关对应单；
* 改条目 ⇒ 改它所在的 `data/spec/chapters/<章>.md`（正文）+ `bindings.json` / `audit.json`（映射）；
* 改判定口径 ⇒ 先改技能 `amayui-spec`，再回改这里（技能是口径真源，本文件是落点）。
