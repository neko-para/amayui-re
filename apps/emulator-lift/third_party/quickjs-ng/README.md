# quickjs-ng（第三方 JS 引擎）

本目录以 **quickjs-ng v0.18.0** 为基座，**只打了一个补丁**（quickts E1 = TypeScript 纯类型擦除，
见 [`## 本地补丁：quickts E1`](#本地补丁quickts-e1typescript-纯类型擦除)）；除补丁外逐字节与上游相同。
只 vendored 我们编译所需的 4 个 C 源 + 它们 include 的头文件 + 上游 `LICENSE`。
⛔ 没有上游的 `CMakeLists.txt` / `Makefile` / CLI / 测试 / 示例：构建形状由本仓 `CMakeLists.txt`
的 `lift-qjs` target 决定（见下）。

- 上游：<https://github.com/quickjs-ng/quickjs>
- tag：`v0.18.0`
- commit：`d7215b0ad3fc2c562e8317fd9bca058fd4863804`（`Set version to 0.18.0`）
- 许可证：MIT（见 `LICENSE`；文件头也带 MIT 声明）
- 版本宏真源：`quickjs.h` 的 `QJS_VERSION_MAJOR/MINOR/PATCH = 0/18/0`；
  `src/ui/quickjs_value.h` 用 `static_assert` 钉住这三个值（升级漏改那边 ⇒ 编译期红）。

## 文件清单（20 个；`quickjs.c` / `quickjs.h` 带 E1 补丁，其余原样）

| 文件 | 角色 |
|---|---|
| `quickjs.c` | 解释器 / 字节码 / 值 / 内建对象（主体）；**带 E1 补丁**（18 个插入区） |
| `dtoa.c` | double ↔ 字符串（`js_dtoa` / `js_atof`；0.18.0 拆成独立 TU） |
| `libregexp.c` | 正则引擎 |
| `libunicode.c` | Unicode 表 / 大小写 / 标识符 |
| `quickjs.h` | 公共 C API（唯一被宿主 include 的头）；**带 E1 补丁**（+1 行 flag） |
| `quickjs-atom.h` | 预定义 atom 名 |
| `quickjs-opcode.h` | 字节码 opcode 表 |
| `quickjs-c-atomics.h` | C11 / MSVC 原子适配（`quickjs.c` 内部用） |
| `cutils.h` | 容器 / 字符串 / 动态缓冲（0.18.0 起**没有** `cutils.c`：实现以 `static inline` 落在本头里） |
| `dtoa.h` | `dtoa.c` 的接口 |
| `libregexp.h` | 正则接口 |
| `libregexp-opcode.h` | 正则字节码 |
| `libunicode.h` | Unicode 接口 |
| `libunicode-table.h` | Unicode 表（生成物，上游入库） |
| `list.h` | 侵入式链表 |
| `builtin-array-fromasync.h` | 内建 `Array.fromAsync` 源码字面量（C 数组） |
| `builtin-iterator-zip.h` | 内建 `Iterator.zip` 源码字面量（C 数组） |
| `builtin-iterator-zip-keyed.h` | 内建 `Iterator.zipKeyed` 源码字面量（C 数组） |
| `quickts_e1.h` | **本仓新增**（不是上游文件）：quickts E1 的类型擦除逻辑，由 `quickjs.c` 在 `js_parse_seek_token()` 之后 `#include` |
| `LICENSE` | 上游 MIT 文本 |

★ 上游 `qjs_sources`（它的 `CMakeLists.txt` 的 `set(qjs_sources …)`）就是上表前四个 `.c`，
一项不多一项不少；其余 15 个文件是它们 `#include "…"` 的本地头（其中 `quickts_e1.h` 是**本仓新增**，
不从上游取）。
★ 上游 v0.18.0 **删掉了** `xsum.c` / `xsum.h`（tag 树里没有这两个文件；`raw.githubusercontent.com/.../v0.18.0/xsum.c`
也是 HTTP 404）⇒ 我们跟着删。`cutils.c` 则是实现以 `static inline` 进 `cutils.h`（所以 `cutils.h` 还在、
`cutils.c` 没有）。"最小集"总是 = 能编出静态库的那几个 TU 及其 `#include` 的头。

## 为什么 vendor 它

UI 脚本层（`src/ui/script.h`）需要一个**嵌入式、无系统依赖、可设内存/栈/CPU 预算**的 JS 引擎。
quickjs-ng 是 MIT、单层 C、可静态链进 exe（发行树不多一个 DLL），且 v0.18.0 提供
`JS_SetMemoryLimit` / `JS_SetMaxStackSize` / `JS_SetInterruptHandler` 三个预算口
（`quickjs.h` 的 `:515` / `:521` / `:1193`）。⛔ 不用 vcpkg：我们要钉死字节
（语言行为 / 沙箱形状随版本变），且本仓已有的第三方惯例就是"原样副本 + 本文件"。

## 怎么用

- 构建：`apps/emulator-lift/CMakeLists.txt` 末尾的 A 块 `add_subdirectory(third_party/quickjs-ng)`；
  本目录的 `CMakeLists.txt` 建静态库 `lift-qjs`（**4 个 C 源**、C11、`_GNU_SOURCE` +
  `QUICKJS_NG_BUILD`、`if(WIN32)` 的 `WIN32_LEAN_AND_MEAN` / `_WIN32_WINNT=0x0601`、
  非 WIN32 的 `m` 与 `CMAKE_DL_LIBS` / `Threads`、`if(NOT MSVC)` 的 `-funsigned-char`；
  include 目录是 **PUBLIC**，消费者写 `#include "quickjs.h"`）。
  ★ `WIN32_LEAN_AND_MEAN` / `_WIN32_WINNT=0x0601` 与上游 `qjs_defines` **全集对齐**，是**防御性选择**，
  **不是当前的承重项**：这两个 define 是 `PRIVATE`（宿主只 include `quickjs.h`，拿不到）；0.18.0 的
  `cutils.h` 自己先 `winsock2.h`（`:60`）后 `windows.h`（`:61`）、`cutils.c` 已不存在
  ⇒ **当前 4 个 TU 去掉这两个 define 也能编**（2026-11-18 实测：`quickjs.c` / `libunicode.c` 单独编 exit 0）。
  保留它们是为了将来「给 `lift-qjs` 加源」或「上游回退 include 顺序」时仍安全：
  任何"先 include `<windows.h>`"的 TU 再进 `cutils.h` 会撞 `winsock.h` vs `winsock2.h`
  （`fd_set` / `sockaddr` / `IPPROTO_RAW` 重定义），而 `WIN32_LEAN_AND_MEAN` 让 `<windows.h>` 不拉
  `winsock.h`（验证探针 `.tmp/qjs-018/probe5.c`：`<windows.h>` 在前 + `cutils.h`、不加 defines ⇒ error，
  加上 ⇒ 编译过）。`_WIN32_WINNT=0x0601` 只是照上游 define 全集对齐，**未找到本库代码级依赖**。
  ★ `-funsigned-char` 用 `if(NOT MSVC)`：clang-cl 走 MSVC 分支 ⇒ 不加；该 flag 在 clang-cl 上
  本来就是 no-op（`unknown argument ignored`）⇒ 与上游 win32 分支的最终效果等价（signed char）。
- 唯一的**产品** include `quickjs.h` 的 TU = `src/ui/script_quickjs.cpp`（经私有头 `src/ui/quickjs_value.h`；
  `src/ui/script.h` 与 `src/ui/script_quickjs.h` **都不许** include 它）。引擎级 CTest
  `tests/quickts.cpp`（target `lift-quickts`，只链 `lift-qjs`）也直接 include 它 —— 那是**判据**，
  不是第二个产品边界（`tools/test/core-shape.test.mjs` ⑤ 只扫 `src/`）。
- ⛔ 没有 `quickjs-libc.c` / `qjs` CLI / `std` / `os` 模块：脚本侧只拿宿主注入的 `ui` / `game`。

## 怎么改

- **升级**：把上表 20 个文件整体换成新 tag 的逐字节副本（`git -C <上游 checkout> show <tag>:<file>`，
  或 `https://github.com/quickjs-ng/quickjs/archive/refs/tags/<tag>.tar.gz`），删掉新 tag 已不存在的旧文件，
  同步改本文件的 tag / commit / 版本宏说明；上游 `qjs_sources` / `qjs_defines` / 系统库若变了，
  再同步改本目录 `CMakeLists.txt`（每处与上游不同都要写"为什么此时不适用"）。
  ⚠ `quickts_e1.h` **不是**上游文件，不要从新 tag 取/覆盖；升级后必须**重放 E1 补丁**到新的
  `quickjs.c` / `quickjs.h`（`rg -n 'quickts E1' quickjs.c` 应命中 18 个插入区 + 头部 3 处
  `quickts_e1.h` 适配注释；`rg -n 'JS_EVAL_FLAG_TYPESCRIPT' quickjs.h` 应命中 1 处），
  再按上面的判据重新取证（`-R '^quickts$'`）。然后跑
  `cmake --build apps/emulator-lift/out --target lift-qjs` +
  `cmake --build apps/emulator-lift/out --target lift-ui-script-quickjs` +
  `ctest --test-dir apps/emulator-lift/out -R '^(ui-script-quickjs|quickts)$' --output-on-failure`。
  新 tag 若改了 `JS_AddIntrinsicEval` / `JS_SetInterruptHandler` / `JSInterruptHandler` 的签名 /
  错误 `stack` 的形状 / `JS_AddIntrinsicBaseObjects` 装哪些内建，沙箱与预算用例会当场红
  （那正是升级需要人看的地方）。
- **本地补丁**：⛔ 不要直接改这份副本（那就不是"上游基座 + 已知补丁"，也说不清来源）。要改行为就改
  `src/ui/script_quickjs.cpp` 的包装层。若某平台确实编不过、必须打补丁：只打最小补丁，
  并在本文件加一节写明**补丁内容 + 理由 + 何时能删**（当前唯一的补丁 = 下一节的 quickts E1）。
- 不要手工重打本文件（换行 / 编码 / 尾随空格会让"逐字节副本"这个前提失效）。

## 本地补丁：quickts E1（TypeScript 纯类型擦除）

> ★ 这是本目录**唯一**的补丁，也是"为什么这里不是纯上游副本"的唯一原因。补丁的**判据**不在
> 本文件（本文件只记口径）：`apps/emulator-lift/tests/quickts.cpp`（CTest `quickts`，target
> `lift-quickts`）。改了补丁就重跑 `ctest --test-dir out -R '^quickts$' --output-on-failure`。

### 补了什么

| 文件 | 改动 | 规模 |
|---|---|---|
| `quickts_e1.h` | **新增**：擦除用的 static 函数（类型跳过 / 注解 / `import type` / `as`/`satisfies` / 泛型 / class 成员 / 声明） | 1,117 行（含注释；功能面 = 上游 1,163 行删掉下列段落） |
| `quickjs.c` | 18 个插入区 / 36 个代码块：`JSParseState` +5 字段、`ts_type_params` 前置声明、`#include "quickts_e1.h"`、各解析路径的钩子、`__JS_EvalInternal` 读 flag | `+245 / −5`（5 行是 `#06a`/`#10c`/`#10e`/`#11b` 的**改写**，不是删除语义） |
| `quickjs.h` | `JS_EVAL_FLAG_TYPESCRIPT (1 << 8)`（bit 8 原本空闲；只在 `1 << 7` 之后加一行） | `+3 / −0` |

- 语义：**擦除**。类型被当作 token 吃掉、不发射任何东西 ⇒ 同一份源码"带类型"与"删掉类型"的
  字节码相同。运行时行为只由剩下的 JS 决定。
- flags off（所有 `.js`）**完全不变**：每个钩子都在 `if (s->ts)` 里；`s->ts` 只由
  `JS_EVAL_FLAG_TYPESCRIPT` 置位（`js_parse_init()` 的 `memset` 保证缺省 false）。直接判据 =
  CTest `quickts` 的 `js-bytecode-identical`（16/16）+ `js-global-bytecode-identical`（13/13）：
  同一引擎 flags 0 / flags on 各 `JS_Eval(COMPILE_ONLY)` 一次，
  `JS_WriteObject(BYTECODE | STRIP_SOURCE)` 逐字节比较（**故意不** STRIP_DEBUG：pc2line 要在比较里）。
- **宿主按扩展名加 flag**（S1b）：`src/ui/script_quickjs.cpp` 的 `tsFlags(origin)` 是**唯一**判定
  （`.ts` / `.mts`，ASCII 大小写不敏感；`.js` 不加），被四处 `JS_Eval` 共用：插件入口 / module
  loader（被 import 的 `lib/*.ts`）/ classic 探针 / 一般 `load`。`.ts` 入口强制 module
  （`JS_DetectModule` 没有 flags 参数，宿主不拿它探测 `.ts`）。

### 来源 / 删了什么

- 上游：**FrameOS/quickts** <https://github.com/FrameOS/quickts>，commit `66e2847`
  （`VERSION` = `2026-06-04-quickts.2`，2026-11-07），MIT（见 `LICENSE`）。上游文件是
  `quickts.h`（1,163 行）+ 它打在 Bellard QuickJS `3d5e064` 上的 45 hunk。
- 本补丁不是 `.patch` 套用（上游 45 hunk 有 27 个被 ng v0.18.0 拒、贴上的也编不过），是按 ng 的
  `quickjs.c` 结构**逐点重排**的：插入区 ↔ ng 函数在 `quickjs.c` 的注释里都写了。
- 从上游 `quickts.h` **删掉**的段落（机械可复算）：
  1. 构造参数属性段（上游 1-based `[1061,1129]`，69 行：`ts_param_modifiers` /
     `ts_record_param_prop` / `ts_emit_param_props`）—— 它会发射 `this.x = arg`，是 lowering；
  2. 末尾 `/* Constructs that emit code instead of erasing it. */` + `#include "quickts_enum.h"` +
     `#include "quickts_jsx.h"`（上游 `[1161,1163]`，3 行）；
  3. `BOOL` / `TRUE` / `FALSE` → `bool` / `true` / `false`（47 处；ng 没有 `BOOL`）。
- 也**没有**移植上游同时带的 3 个非 TS hunk（`JS_MALLOC_ARENA_SIZE`、newlib 时区、
  `js_def_malloc_usable_size`）与 `strrchr` const cast：它们与 TypeScript 无关，且 ng 侧对应实现
  已经不同（`JSMallocState` / `js_malloc_usable_size_unknown`；`JS_MALLOC_ARENA_SIZE` 在 ng 不存在）。
- 3 处 **ng 适配**（`quickts_e1.h` 里标 `ng adaptation`，不是上游原文）：
  `js_parse_skip_parens_token` 的前置声明用 `bool`（ng 签名）；`ts_next_gt()` 额外移动 `s->mark`
  （ng 的 `token.col_num = mark - eol`，否则 `>>` 拆成两个 `>` 时列号相同）；模板续行的分支额外写
  `last_line_num/last_col_num`（与 ng 自己的 `js_parse_skip_parens_token` 一致）。

### 不支持（E1 边界；在 `JS_EVAL_FLAG_TYPESCRIPT` 下必须报错，⛔ 不许静默接受）

| 语法 | 为什么 | 判据 |
|---|---|---|
| `enum` / `const enum` | 要 lower 成对象 + IIFE（生成代码，不是擦除） | `quickts/ts-rejects`（`enum E { A, B }` 必须 `JS_IsException`） |
| 构造参数属性 `constructor(public x: T)` | 要发射 `this.x = x`（lowering）；上游那段已删 | `quickts/ts-rejects`（必须报错） |
| 含**值**的 `namespace` / `module` | 需要 IIFE + 合并对象；quickts 显式拒绝 | `quickts/ts-rejects`（`namespace N { export const x = 1; }` 必须报错） |
| `import =` / `export =` | TS 的 CJS 语法；quickts 不支持 | 报错（`ts_try_type_decl` 只认 `import`/`export` 的 ES 形式） |
| JSX / `.tsx` | 要 lowering（`quickts_jsx.h` 629 行）+ `js_parse_error_pos`（ng 没有） | 报错 |
| 装饰器 | QuickJS 没有；quickts 也不支持 | 报错 |
| legacy `<T>x` 断言 | 与泛型箭头冲突；上游 README 明确不支持 | 报错（`<` 走比较/泛型箭头两条路，不认断言） |
| `accessor` 字段 | tsc 的 `erasableSyntaxOnly` 也拒 | 报错（`accessor` 当字段名，后面 `x` 报错） |
| `a?.<T>()`（optional-chain 调用上的类型实参） | `?.` 分支自己 `goto parse_property`、不回 for 循环顶 ⇒ 钩子够不到；examples 未用，`tsc --erasableSyntaxOnly` 却会放行 | **已知不支持**：运行期报错。若将来要支持，得改 `js_parse_postfix_expr` 的 `?.` 分支 |

### 回退（一行到三行，⛔ 不用回滚整份副本）

1. 删 `quickts_e1.h`；
2. `quickjs.c`：删 18 个插入区（`rg -n 'quickts E1' quickjs.c` 定位；每个插入区的前一行都有这句注释）；
   删头部 3 处 `ng adaptation` 注释所在的 `quickts_e1.h` 适配（它们在头文件里，随第 1 步一起没了）；
3. `quickjs.h`：删 `JS_EVAL_FLAG_TYPESCRIPT` 那 3 行。

回退后 `.js` 行为**不应**有任何变化（补丁从不为 flags off 改路径）；但唯一能证明这一点的是重新跑
`ctest -R '^quickts$'`（它在回退后应该连编都编不过 —— 那时回退判据变成 `git diff` 为空）。

### 什么时候能删这个补丁

- 上游 quickjs-ng 原生支持 TypeScript 擦除（当前没有这个计划），或
- 本仓不再需要在运行期直接执行 `.ts`（只用 tsc 预编译成 `.js`），或
- 升级 ng 时决定换一条路线（例如换引擎 / 换 E2 实现）。

在那之前：**升级 ng 必须重放本补丁**（见 `## 怎么改` 的"升级"），否则 `.ts` 静默失效。
