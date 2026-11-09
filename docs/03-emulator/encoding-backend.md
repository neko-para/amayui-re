# CP932 编码后端：系统接口（Windows MBCS / POSIX iconv）能不能替代自带表

> **本文是一次评估的读数与结论，不是合同、也不是进度**。合同在 `contracts-win32.md`（`L1-CP-*`），
> 待办在需求树（标题含「CP932 编码后端」的那条）。
> 背景：license 不再是约束（纯开源非商业项目），所以"能不能不自己维护编码表"重新变成可选项。
> 现实现：`apps/emulator-lift/src/runtime/cp932.cpp` + 派生表 `cp932_table.h`（1909 行 / 9604 对 / 256 单字节），
> 生成器 `tools/lift-cp932-gen.mjs`。

## 1. 评估的问题

CP932 ⇄ UTF-16 的两个方向，系统 API 能不能给出**与现在逐字节相同**的结果？

| 平台 | 原生选择 |
|---|---|
| Windows | `MultiByteToWideChar(932, …)` / `WideCharToMultiByte(932, …)` |
| macOS / glibc Linux | `iconv_open("UTF-16LE", "CP932")`（系统 libiconv / glibc；musl **没有** iconv） |

判据不是"能转"，而是**同一份字符串在三个平台上编出同样的字节** —— 这正是当年不用宿主码页的理由
（`cp932.h` 头注释）。

## 2. 怎么量的（可重跑）

把"我们的表"与"系统 iconv"在**全定义域**上逐条对照：256 个单字节 + 所有合法 lead/trail 组合
（lead `0x81–0x9F` / `0xE0–0xFC` × trail `0x40–0xFC`）。两个小程序分别打印 `字节 → 码位`（解码方向）
与 `码位 → 字节`（编码方向），再 `diff`。量于 2026-11-09 · macOS arm64（系统 libiconv）。

```bash
# 解码方向：ours vs iconv（程序见当次评估的 /tmp 脚本，逻辑就是上面的枚举）
diff ours.txt iconv.txt
# 编码方向
diff enc_ours.txt enc_iconv.txt
```

## 3. 读数

### 3.1 解码方向：**等价**（9790 条相同，5 条差异）

| 字节 | 我们的表 | 系统 iconv |
|---|---|---|
| `0x80` | U+0080 | 非法 |
| `0xA0` | U+F8F0 | 非法 |
| `0xFD` | U+F8F1 | 非法 |
| `0xFE` | U+F8F2 | 非法 |
| `0xFF` | U+F8F3 | 非法 |

这 5 条恰好是 **Windows CP932 的扩展单字节**：Windows 的 `MultiByteToWideChar(932)` 给的就是上表右边那组码位，
而 libiconv 按 Shift_JIS 判非法。⇒ 解码侧换系统接口只需 **5 条平台胶水**（Windows 原生就对；POSIX 侧把这 5 个
字节补成常量），**不需要表**。

### 3.2 编码方向：**不等价**（373 个码位）

双字节区里有**重复映射**（同一码位有两个合法编码）：NEC 选定的 IBM 扩展（`0xED40–0xEEFC`）与
IBM 扩展（`0xFA40–0xFC4B`）解到同一批字符。两边挑的字节不同：

| 码位 | 我们的表 | 系统 iconv |
|---|---|---|
| U+2170（Ⅰ） | `0xEEEF`（NEC） | `0xFA40`（IBM） |
| U+4E28（丨） | `0xED4C` | `0xFA68` |
| … | … | … |

去重后 **373 个码位**属于这一类（`diff enc_*` 的 1497 行里，重复行是"每个 pair 打印一次"造成的）。
反方向（解码）完全一致，所以这是**纯编码选择**的分歧，不是映射错。

⇒ 直接换系统编码器，会**静默改变**这 373 个字符写出去的字节：Windows（MBCS）与 macOS/Linux（iconv）
可能各挑各的 ⇒ 同一份存档名 / 配置串在两台机器上不同字节。这与"三平台逐字节一致"的口径冲突。

### 3.3 其余读数

- `0x5C` / `0x7E` 两边都解成 `\` / `~`（不是 `¥` / `‾`）—— 与 Windows CP932 一致，没有 Shift_JIS 的老坑。
- 可用性：macOS / glibc 有 `CP932`；**musl 没有**（Alpine 之类要显式装 libiconv）；Windows 恒有。
- 成本：现在的表是进程内 `lower_bound`（O(log n)）；iconv 每次转换要 `iconv_open`（可缓存描述符），
  路径 / 配置这类调用不热，量级上不是问题。

## 4. 结论（建议）

**解码可以换、编码不能直接换。** 分两条：

- **路线 A（现状，推荐先保持）**：表就是"三平台一致"的保证本身；1909 行是生成物（`tools/lift-cp932-gen.mjs`），
  维护成本主要是重新生成，不是手改。
- **路线 B（要换就换全套）**：解码走系统接口（Windows MBCS；POSIX iconv + 5 条单字节胶水）；
  编码**必须**把 373 个重复映射的选择钉成一张**显式策略表**（以 Windows `WideCharToMultiByte(932)` 为准），
  再加一条跨平台等价守卫（全定义域摘要 / 或与本表对拍，至少 `@env pure` 在 Windows + macOS 各跑一次）。
  ⇒ 净收益是删掉 1909 行表，代价是新增 373 条策略 + 一层平台分支；**在拿到 Windows 侧选择之前，B 不成立**。

### 待补的实测（Windows 侧，需 Windows 机器）

```c
// 对 U+2170 / U+4E28 等 373 个码位调用 WideCharToMultiByte(932, 0, …) 看它挑 NEC 还是 IBM
```

读数回来后才能定 B 的策略表；在那之前不动 `cp932.cpp`（它是 boot 的必经路径，换实现的回归面很大）。

## 5. 怎么查（现状真源）

- 现实现与理由：`apps/emulator-lift/src/runtime/cp932.h:1-16`（为什么当年不用 iconv）
- 表：`apps/emulator-lift/src/runtime/cp932_table.h`（生成物，别手改）
- 生成：`node tools/lift-cp932-gen.mjs --write`（口径 = WHATWG shift_jis + Windows 的 8 个单字节修正）
- 本评估的待办与重开条件：需求树里标题含「CP932 编码后端」的那条
