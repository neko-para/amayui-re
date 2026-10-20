# `data/spec/` —— 引擎与二进制协议 spec

> 这里回答「**引擎是什么**」以及「**引擎读写的二进制格式是什么**」：实体 / 状态 / 操作 / 规则 / 协议。
> 规范正文是**可分发的 Markdown**；机器绑定与范围审计是内部复核数据，**不进入分发产物**。
> 字段 / 枚举 / 不变量 / 操作**只有一份真源**：`pnpm tools spec describe`（机器可读加 `--json`）。
> 脚本二进制协议（脚本头 / 指令编码 / 操作数编码 / 指令表）由**本规范**定义；
> `packages/age-format` 是现有实现（仿照第三方库写成），**不是规范来源**；协议的待核验状态见
> [`chapters/10-format.md`](chapters/10-format.md)。
> **机制地图**（core 切成哪些机制、每机制凭什么算完、文件与 id 怎么放）是 [`core/README.md`](core/README.md) —— 组织真源。

## 1. 规范正文 vs 复核数据

| 落点 | 回答什么 | 进分发产物？ |
|---|---|---|
| [`spec.json`](spec.json) | **manifest**：标题 + 章节顺序（id / file / title） | 否（只有顺序元数据） |
| [`chapters/*.md`](chapters/) | **规范正文**：自包含的领域合同、定义、状态对应、条件（阅读顺序见 [`chapters/README.md`](chapters/README.md)） | **是** |
| [`bindings.json`](bindings.json) | **机器绑定**：站点 EA / 函数 / `storage` 指针 / 证据 | 否（内部复核） |
| [`audit.json`](audit.json) | **范围审计**：试点块 / effect 归属（规则选择器 + 内部省略 + 残差）/ 成熟度 / 缺口 / 可证伪预言 | 否（内部复核） |
| [`format/opcode-table.json`](format/opcode-table.json) | **本规范的指令表**（当前待核验；不是实现副本） | **是**（伴随数据） |
| [`format/verification.md`](format/verification.md) | **协议核验台账**（EA / 台账 / 守卫；内部） | 否（内部复核） |
| [`format/alf-agf-verification.md`](format/alf-agf-verification.md) | **ALF / AGF 核验台账**（EA / 台账 / 守卫；内部） | 否（内部复核） |

## 2. 规则怎么写

规范正文按章放在 `chapters/*.md`；每条规则是一个 `###` 小节：

```markdown
### 全局值族：类型标签 3 / 4 / 5

<!-- rule: SPC-POOL-GLOBAL-VALUE-FAMILIES -->

正文……（合同 / 状态对应 / 条件；不写汇编引用、台账 id、守卫、本仓路径）
```

- `<!-- rule: ID -->` 是**不可见标记**：校验器用它把正文与 `bindings.json` / `audit.json` 关联；
  它是 HTML 注释，阅读时不可见。
- 规则**不要**写汇编地址、函数符号、台账 id、守卫用例、`layout:`/`shape:` 指针 —— 这些属于复核数据；
  正文只写领域行为。校验器的「自包含」不变量会红。
- 章里没有规则的纯定义/术语是正常的（例如「执行模型」）；`audit.chapters[].status` 区分
  `none` / `partial` / `bootstrap`。

## 2.1 覆盖是 effect 级，不是"整条指令"

- `audit.pilot.sites` 只声明**块入口**（机器块），不是引擎抽象；
- effect 由 `tools/lib/x86-effects.mjs` 从 `.lst` **机械枚举**，带 block / site / op / kind；
- 规则的 `covers` 是**选择器**（`{blocks?, kinds?, ops?, targets?}`），所以同一条指令可以只覆盖一部分 effect；
  调用只记 `call` + 实际符号（`target`），不把 `_atoi` 改名成 `parse-int`；
- `pilot.internal` 逐类给出内部省略的理由（寄存器 / 栈 / 控制流），`pilot.residuals` 是带 `next` 的显式残差；
- 每个 effect 必须落在 covered / internal / residual 之一；未登记 = 0（assets 档现算）。

换一个试点只换 `pilot.sites` 与 effect 枚举口径（枚举器在 `tools/lib/x86-effects.mjs`，不在通用 schema 里）。

## 3. 怎么查

```bash
pnpm tools spec validate      # 不变量（含自包含 / 跨文件一一对应）
pnpm tools spec report        # 审计：effect 覆盖 / 内部省略 / 残差 / 未登记
pnpm tools spec effects --pilot # 从语料机械枚举试点块的 effect（--json 给机器读）
pnpm tools spec chapters      # 章节骨架 + 每章规则与残差
pnpm tools spec show <id|前缀> # 一条规则：正文 + 证据有效状态（内部视图）
pnpm tools spec describe      # 字段 / 枚举 / 不变量 / 操作 / 诚实项（schema 唯一真源）
```

## 4. 怎么改

* **规范内容** → 改 `chapters/*.md`；新增章节时在 `spec.json` 的 `chapters[]` 与 `audit.json` 的
  `chapters[]` 各加一条（`id` 对齐）。
* **规则** → 在章节里加 `### 标题` + `<!-- rule: ID -->` + 正文；并在 `bindings.json` 的 `bindings[]`
  （字段 `rule`）与 `audit.json` 的 `rules[]`（字段 `id`）各加同一条规则（跨文件不变量会检查）。
* **还没有规范内容的义务** → 放 `audit.json` 的 `gaps[]`，**不要**写成规则。
* **机器绑定 / 证据** → 只改 `bindings.json`；证据按 `KN:<ULID>` / `guard:<path>#<用例>` 引用。
* **分发**：直接分发 `chapters/*.md`（阅读顺序见 [`chapters/README.md`](chapters/README.md)）；**不生成单文件**，也没有生成物要维护。

## 5. 本层**不**提供什么（诚实边界）

* 不证明合同与原引擎等价；也不消除"多路推导共同误解"。
* 试点块是**声明**的；effect 由语料机械枚举，但枚举器有边界（见 `x86-effects` 文件头），不证明域外没有同类义务。
* `discarded` / `unknown` 不并进"已解释"；计数不是正确性证据。
* 表现层逐位一致（像素 / 混音）不在 spec：按项目口径进 `discards`。
