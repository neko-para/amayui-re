# 全量构建在 clang-cl -Werror 下多处红（native_override/ddraw/宏重定义）：挡住全量 ctest 与 mutate-check 收尾

- id: REQ-01M7T8MKYFNV1FR1SQAMXBANYT
- type: bug
- status: done
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- repro: apps/emulator-lift/tests/native_override.cpp#g_current_case
- severity: S2
- done_reason: 三处根因在 HEAD 都已是修好的形态：tests/native_override.cpp:3280 用存活到循环末的 const std::string case_name；tests/ddraw.cpp:745 两枚举各自 static_cast 到 uint32_t；tests/insn_cost.cpp:70 加 #ifndef _CRT_SECURE_NO_WARNINGS（这三个文件最后一次改动都是 de1c23a，均在 main）。判据的 win32 全量 build/ctest 读数需在 win32 会话复核；若再红则重开。
- tags: [emulator-lift, tests, build]

## 现象

全量构建 / 全量 ctest 在 clang-cl 下红，且会阻断 `tools/mutate-check.mjs` 的收尾**全量重建**
（工具报「exe 未能重建回基线 ⇒ out/ 里可能有停在变异版的 exe」）：

```
apps/emulator-lift/tests/native_override.cpp(3277,45): error: object backing the pointer
'lift_test::g_current_case' will be destroyed at the end of the full-expression
[-Werror,-Wdangling-assignment-gsl]
```

## 根因

`apps/emulator-lift/tests/check.h:40` 的 `g_current_case` 是 `const char*`；`native_override.cpp:3277`
把 `(std::string("mdct/decline-") + c.name).c_str()` 的临时 buffer 存下来 ⇒ **悬垂指针**。
这不只是编译警告：后续 `LIFT_CHECK` 打印的用例名读的是已析构的 buffer（UB）。

## 复现

```bash
cd apps/emulator-lift
cmake --build --preset windows-x64            # 或 --target lift-native-override
# 首行错误即上面的 -Wdangling-assignment-gsl
```

2026-11-19 / win32 / commit `48e6977` / clang-cl。该文件最后一次改动是 `b887b7a`（11-15），
不是 2026-11-19 本轮引入。

## 修法（已在 dev 工作树实施，未提交）

让名字活过这一轮：

```cpp
const std::string case_name = std::string("mdct/decline-") + c.name;
lift_test::g_current_case = case_name.c_str();
```

`case_name` 活到循环体结束，`g_current_case` 的所有使用都在其生命周期内。

## 同一轮暴露的同类基线红（同一 clang-cl `-Werror` 口径）

- `apps/emulator-lift/tests/ddraw.cpp:745`：`-Wdeprecated-enum-enum-conversion`
  （`lift::ddraw::kOffPixelFormat + lift::ddraw::kPfBitCount` 两个不同枚举相加）。
- 另有一处 `_CRT_SECURE_NO_WARNINGS` 重定义（命令行已是 `1`，源码又 `#define`）——
  具体文件见 `.tmp/build-red.log` 的 `error:` 行。

⇒ 都是**测试代码 / 编译口径**问题，不改产品行为；但全量 build / ctest 在修完之前不可用。

## 判据

- `cmake --build --preset windows-x64` 全量绿；
- `ctest --test-dir out` 全量绿（至少 `lift-native-override` 绿）；
- `node tools/mutate-check.mjs --match '总样本数'` 的收尾全量重建成功（不再报"exe 未能重建回基线"）。
