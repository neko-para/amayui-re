// emulator-lift · QuickJS 后端的 **PIMPL 边界**（A：QuickJS 内核）
//
// ★ 本头存在的理由只有一个：把"实现头"与"宿主的接口头"分开。
//   `makeQuickJsRuntime()` 的声明在 `ui/script.h`（语言无关）；它的**实现类型**
//   （`ScriptRuntime` / `UiScript` 的派生类、`JSRuntime` / `JSContext` / `JSValue` 的持有者）
//   全部只活在 `script_quickjs.cpp` 的匿名命名空间里 —— 头里**不出现任何 quickjs 类型**，
//   ⛔ 本头也不 include `quickjs.h`（唯一 include 它的 TU 是 `script_quickjs.cpp`）。
// ★ 所以 include 本头 == include `ui/script.h`；加这一层是为了让"边界在哪"可被 grep 到，
//   而不是多一套 API。
#pragma once

#include "ui/script.h"
