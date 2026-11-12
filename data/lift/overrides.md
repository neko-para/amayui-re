# 覆盖清单（`data/lift/overrides.json`）—— 这是什么、怎么处理

> **同名的结构化数据**：`data/lift/overrides.json`。字段语义 / 枚举 / 不变量的**唯一真源**是本仓工具
> `tools/lift.mjs` 的自描述（`pnpm tools lift describe`），本文件只写**口径**与**怎么操作**，
> ⛔ 不复述 schema（复述会漂）。
>
> 这份清单回答一件事：**guest 执行到某个平台导入 / 某段 guest 代码时，我们给它什么**。
> 每一条都是**显式**的：要么有实现（`replace` / `hook`），要么是带理由的 `stub`（执行到会响亮失败或
> 按 `SCOPE.md` 的偏差返回合理值），⛔ 没有"静默 no-op"这一档。

## 它覆盖什么（三层粒度）

| 粒度 | `kind` | 键 | 本线现状 |
|---|---|---|---|
| 平台 shim（API 级） | `replace` = 有实现；`stub` = 不实现但登记理由 | `target: "module!name"` | M1 的全部条目都是这一层 |
| 函数级（EA → 原生） | `replace` / `hook` | `ea: "0x00401234"` | 尚无（M2/M3 才出现） |
| 子系统级（EA 范围） | `replace` / `hook` | `range: [start, end]` | 尚无 |

★ **M1 的 stub 不是"假装成功"**：`addUnimplemented` 的条目分到唯一 guest 桩地址，执行到就打印
`module!name` + reason + 返回地址 + 寄存器 + guest 返回栈并**停下**（`SCOPE.md` §3）。
`addStub` 的条目按 `SCOPE.md` §1 返回**合理值**并打一行日志，同时在这里登记 `kind: stub` + reason：
当前共 **43 条** = AGERC 2（`_GetInstance@0` / `_OperateMenu@16`；`_ShowDialog@12` 的 id==3 已改成实现
（系统关窗确认），不在 stub 清单）+
version 3（`GetFileVersionInfoSizeA` / `GetFileVersionInfoA` / `VerQueryValueA`）+
user32_menu 3（`LoadMenuA` / `SetMenu` / `TrackPopupMenu`；`MessageBoxA` 2026-11-12 起改为实现
（宿主自绘模态），已不在 stub 清单）+
advapi32 注册表 6（`RegOpenKeyExA` / `RegQueryValueExA` / `RegSetValueExA` / `RegCreateKeyExA` /
`RegEnumValueA` / `RegCloseKey`）+ winmm 音频 11（MCI 3 / mixer 3 / mmio 5）+
winmm 摇杆 3（`joyGetNumDevs` / `joyGetDevCapsA` / `joyGetPosEx`）+
视频 / 媒体 15（MF 7 / MFPlat 6 / EVR 1 / DXVA2 1）。

## 为什么清单不是手写的（两条真源如何保持一致）

- **C++ 注册表是真源**：`apps/emulator-lift/src/shims/*.cpp` 里的
  `add`（有实现）/ `addStub`（有实现但按偏差整体 stub）/ `addUnimplemented`（未实现 + reason）。
- **JSON 是清单视图**：每条 `stub` 必须与 C++ 里的一条登记**逐字对上**（`target` 与 `reason`）。
- **派生缺口文件**：`apps/emulator-lift/src/shims/register_import_gaps.cpp` 由本工具生成，
  登记 PE 导入表里有、但既没实现也没登记的那些条目（理由按模块分组，见 `tools/lift.mjs` 的
  `MODULE_REASONS`）。它入库、但**不许手改**。
- `pnpm tools lift check` 双向比对三者（JSON ↔ C++ ↔ PE 导入表）+ 校验 `.lst` 头部 sha256；
  **任何一边漂了都会红**（退出码 1），不是靠人记得。

## 静态缺口的三类账（为什么"缺口数"与"登记条数"不是同一个数）

- **已登记 stub**：导入表里有、C++ 里 `addUnimplemented`/`addStub` 登记过、没有实现。
- **导入表里有但未登记**：裸缺口 —— `check` 会红；`sync --write` 会把它们落进派生缺口文件。
- **从未被引用**：C++ 里登记了、但 PE 导入表里没有这个名字（历史/防御性登记；仍带 reason，不删）。

恒等式（`check` 每次打印）：`静态缺口 = 已登记 stub(导入的) + 裸缺口`，
`C++ 登记总数 = 已登记 stub(导入的) + 从未被引用`。
⇒ 这两个数一旦对不上，说明有一边的账漏了，`check` 会点名。

## 怎么查

```bash
pnpm tools lift describe        # ★ schema / 字段 / 枚举 / 不变量（含"谁在守"）/ 操作
pnpm tools lift check           # 全量校验；绿 = JSON、C++ 注册表、PE 导入表、.lst 锚全部一致
pnpm tools lift report          # 导入级覆盖：implemented / stub / bare / never-imported + 覆盖率
pnpm tools lift report --json   # 机器可读
node tools/lift.mjs --check     # 脱离 pnpm / DSH 也能跑（macOS 上一样）
```

判读要点：`裸缺口` 必须为 0；`从未被引用` 不是错误（它是显式登记），但变多要能解释。

## 怎么改

1. **实现一条导入**：在该族的 `src/shims/kernel32_*.cpp` 里写实现并 `s.add(...)`，
   然后从 `register.cpp` 末尾的未实现清单里**删掉**那一行（一处名字只能注册一次）。
2. **登记一条放弃的导入**：在对应族文件 / `register.cpp` 末尾 `addUnimplemented("module","name","理由")`。
   ⛔ 不许登记成"返回成功的空实现"；理由要能指向 `SCOPE.md` 的偏差或里程碑。
3. **按偏差整体 stub（返回合理值）**：用 `addStub(...)`，reason 写清"为什么可以不给真实现"。
4. **落盘清单**：`pnpm tools lift sync --write`（**唯一写入口**；缺省 dry-run，只打印将变什么）。
   它重写 `data/lift/overrides.json` 与派生缺口文件 `src/shims/register_import_gaps.cpp`；
   手写的 `replace` / `hook` / EA 条目会被保留。
5. **再跑** `pnpm tools lift check`（红 ⇒ 修到绿再提交）。

★ 判据不许降级：`check` 红了就修数据或实现，不要把某条判据改成警告。
