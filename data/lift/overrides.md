# 覆盖清单（`data/lift/overrides.json`）—— 这是什么、怎么处理

> **同名的结构化数据**：`data/lift/overrides.json`。字段语义 / 枚举 / 不变量的**唯一真源**是本仓工具
> `tools/lift.mjs` 的自描述（`pnpm tools lift describe`），本文件只写**口径**与**怎么操作**，
> ⛔ 不复述 schema（复述会漂）。
>
> 这份清单回答一件事：**guest 执行到某个平台导入 / 某段 guest 代码时，我们给它什么**。
> 每一条都是**显式**的：要么有实现（`replace`），要么是带理由的 gap（`noop` = 有 fn、按 `SCOPE.md`
> 的偏差返回合理值；`trap` = 无 fn、执行到就响亮失败），⛔ 没有"静默 no-op"这一档。

## 它覆盖什么（三层粒度）

| 粒度 | `kind` | 键 | 本线现状 |
|---|---|---|---|
| 平台 shim（API 级） | `replace` = 有实现；`noop` / `trap` = 不实现但登记理由 | `target: "module!name"` | M1 的全部条目都是这一层 |
| 函数级（EA → 原生） | `replace` | `ea: "0x00401234"` | 尚无（M2/M3 才出现） |
| 子系统级（EA 范围） | `replace` | `range: [start, end]` | 尚无 |

★ **`kind` 不是手选的**：它由 C++ 注册来源决定 —— `add` ⇒ `replace`、`addStub` ⇒ `noop`、
`addUnimplemented` 与派生缺口 ⇒ `trap`（旧值 `stub` 已弃用，`sync --write` 会重写）。

★ **`trap` 不是"假装成功"**：`addUnimplemented` 的条目分到唯一 guest 桩地址，执行到就打印
`module!name` + reason + 返回地址 + 寄存器 + guest 返回栈并**停下**（`SCOPE.md` §3）。
`noop` 的条目按 `SCOPE.md` §1 返回**合理值**并打一行日志，同时在这里登记 kind + reason
（逐条清单看 `pnpm tools lift report`，⛔ 本文件不手抄计数 —— 会腐）。

## 为什么清单不是手写的（两条真源如何保持一致）

- **C++ 注册表是真源**：`apps/emulator-lift/src/shims/*.cpp` 里的
  `add`（有实现）/ `addStub`（有实现但按偏差整体 stub）/ `addUnimplemented`（未实现 + reason）。
- **JSON 是清单视图**：每条 gap 必须与 C++ 里的一条登记**逐字对上**（`target` 与 `reason`），
  且 `kind` 由注册来源决定；`check` 还有一条**逐字节**断言：`overrides.json` 必须等于
  `lift sync` 会生成的文本（手改一个字符就红）⇒ **唯一写入口是 `sync --write`**。
- **派生缺口文件**：`apps/emulator-lift/src/shims/register_import_gaps.cpp` 由本工具生成，
  登记（`executed=true` 的）PE 导入表里有、但既没实现也没登记的那些条目（理由按模块分组，见
  `tools/lift.mjs` 的 `MODULE_REASONS`）。它入库、但**不许手改**。
- `pnpm tools lift check` 双向比对三者（JSON ↔ C++ ↔ PE 导入表）+ 校验 `.lst` 头部 sha256；
  **任何一边漂了都会红**（退出码 1），不是靠人记得。

## 静态缺口的三类账（为什么"缺口数"与"登记条数"不是同一个数）

- **已登记 gap**：导入表里有、C++ 里 `addUnimplemented`/`addStub` 登记过、没有实现。
- **导入表里有但未登记**：裸缺口 —— `check` 会红；`sync --write` 会把它们落进派生缺口文件。
- **`bareUnexecuted`**：`executed=false` 的 guest 模块（今天 = `AGERC.DLL`，它的代码从不执行）
  的裸导入。**只进报告，不判红、不登记**（用户 2026-11-13 裁决）—— 把它们登记成 trap 会在
  C++ 注册链里凭空多出一批永远调不到的桩。
- **`not-referenced`**：C++ 里登记了 gap、但不在**任一** guest 模块的导入表里，且不是被拦截的
  AGERC 导出（`agerc!_*` ⇒ 经 `GetProcAddress` 可达）。**只进报告，不判红**：它们可以是显式裁决
  （例 `SCOPE.md` 点名的线程族）。

恒等式（`check` 每次打印，只对 `executed=true` 的模块）：`静态缺口 = 已登记 gap(导入的) + 裸缺口`。

## 怎么查

```bash
pnpm tools lift describe        # ★ schema / 字段 / 枚举 / 不变量（含"谁在守"）/ 操作
pnpm tools lift check           # 全量校验；绿 = JSON、C++ 注册表、PE 导入表、.lst 锚全部一致
pnpm tools lift report          # 导入级覆盖：implemented / noop / trap / bare / bareUnexecuted / not-referenced
pnpm tools lift report --json   # 机器可读
node tools/lift.mjs --check     # 脱离 pnpm / DSH 也能跑（macOS 上一样）
```

判读要点：`裸缺口` 必须为 0（只算 `executed=true` 的模块）；`bareUnexecuted` 与 `not-referenced`
不是错误（前者是"代码今天不执行的模块"的裸导入，后者是显式裁决），但变多要能解释。

## 怎么改

1. **实现一条导入**：在该族的 `src/shims/kernel32_*.cpp` 里写实现并 `s.add(...)`，
   然后从 `register.cpp` 末尾的未实现清单里**删掉**那一行（一处名字只能注册一次）。
2. **登记一条放弃的导入**：在对应族文件 / `register.cpp` 末尾 `addUnimplemented("module","name","理由")`
   （清单里落成 `kind: trap`）。⛔ 不许登记成"返回成功的空实现"；理由要能指向 `SCOPE.md` 的偏差
   或需求单。
3. **按偏差整体 stub（返回合理值）**：用 `addStub(...)`（清单里落成 `kind: noop`），
   reason 写清"为什么可以不给真实现"。
4. **落盘清单**：`pnpm tools lift sync --write`（**唯一写入口**；缺省 dry-run，只打印将变什么）。
   它重写 `data/lift/overrides.json` 与派生缺口文件 `src/shims/register_import_gaps.cpp`；
   手写的 `replace` / EA 条目会被保留。⛔ **不要手改 `overrides.json`**：`check` 逐字节比对，
   改一个字符就红。
5. **再跑** `pnpm tools lift check`（红 ⇒ 修到绿再提交）。

★ 判据不许降级：`check` 红了就修数据或实现，不要把某条判据改成警告。
