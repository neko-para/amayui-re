# A2 的「未映射助记符 = 0」机制上恒绿（分类器把未知指令判成数据）

- id: REQ-01M6JFZ7X57T6EH0699KHBFH2X
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/mnemonics.cpp#lst-classify
- repro: apps/emulator-lift/src/runtime/mnemonics.cpp#LstKind::DataOrPseudo
- severity: S2
- tags: [emulator-lift, m1, a2]

## 一句话
A2 的"未映射助记符 = 0"在机制上**恒绿**：`classifyLstLine` 把任何 identifier-first 行默认判成数据 / 伪指令，而 `entries` 只收 Instruction ⇒ 表里没有的指令既不进 `unknown` 也不进 `entries`，A2/A3 同时跳过它。

## 落地（2026-11-04）
- `classifyLstLine`：**裸标识符**（`first` 是标识符、`second` 为空）不再默认 `DataOrPseudo`，改报 `Unknown`
  ⇒ 表里没有的指令（如 `cld`）会出现在 A2 的未分类行里。
- 仍判数据的两类补全：`second` 是普通标识符（`ExceptionInfo _EXCEPTION_POINTERS <…>` 的"名字 + 类型 + 值"）⇒ 数据；
  IDA 头部汇编器指令（`.686p` / `.mmx` 等）⇒ 数据。
- `tests/mnemonics.cpp` 的 `lst-classify` 增 5 条纯档断言（裸标识符 / 数据项 / 汇编器指令）。
- A3 补 `len_checked`：真正比过"长度 == 下一行 EA 差"的行数，并断言 `len_checked == checked`
  （旧实现里 `next_ea==0` 的行会静默跳过长度）。
- `plan` §3 F5 加"现查"注：当前 `.lst` 的 `entries=298299` / `collapsed=1805`；`299,249` 是旧导出经验值，
  差 950 未查清，引用静态指令数一律以 `lift-decode-crosscheck` 输出为准。

## 判据 / 变异
- `mnemonics#lst-classify`：`classifyLstLine("cld","") == Unknown`；数据项 / `.686p` / `.mmx` 仍判数据。
- `corpus-mnemonics`（assets）：`未分类行 = 0`、`未映射 = 0`、`entries=298299`、`collapsed=1805`。
- `decode-crosscheck`（assets）：`len_checked=298299`，`len_checked == checked`。
- 变异：裸标识符改回 `DataOrPseudo` ⇒ `mnemonics` 红；`len_checked` 不计数 ⇒ `decode-crosscheck` 红。

## 保留
- **950 差**：仍是"旧导出经验值、成因未查清"（plan §3 F5 的 299,249 vs 当前 298,299）。已写进 plan 的"现查"注，
  不再当断言；若将来要追，需要 2026-11-03 那次导出的口径（当前 `.lst` 没有那份旧文件）。
- 分类器对"未知的**带操作数**助记符"仍可能走 `second` 规则；当前语料 0 例。

## 证据等级
审计报告 `.tmp/m1a-audit/B-cpu.md`（F3/F5）；本轮代码复核 + assets 实跑。
