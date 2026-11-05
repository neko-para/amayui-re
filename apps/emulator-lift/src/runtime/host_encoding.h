// emulator-lift · 宿主进程的**编码契约**（只有一层，跨平台）
//
// 本仓的文本口径是 **UTF-8**（AGENTS.md §2 / §10），但 Windows 进程入口有两个地方
// 会在主程序看到之前就把这条口径破坏掉：
//
//   ① **argv**：`main(int, char**)` 的 `argv` 由 CRT 按 **ANSI 代码页**（中文 Windows = 936）
//      从 UTF-16 命令行转出来 ⇒ 日文路径变成 GBK 字节。本仓随后按"UTF-8"去解析它
//      （`vfs.cpp` 的 `pathFromUtf8`）⇒ `std::filesystem` 拿到非法 UTF-8，直接
//      `std::terminate`（实测退出码 3，只打出 font 一行）。⇒ `argvUtf8()` 从
//      `GetCommandLineW` 重建，主程序只看见 UTF-8。
//   ② **控制台输出**：`printf` 写的是**字节**，控制台按自己的**输出代码页**解释它。
//      源码按 UTF-8 编译（`/utf-8`）⇒ 吐出 UTF-8 字节，而中文 Windows 控制台默认 936
//      ⇒ 用户看到乱码。⇒ `initConsoleUtf8()` 在进程**挂着控制台**时把码页切到 65001；
//      重定向到文件 / 管道时**字节本身不转码**（仍是 UTF-8，供工具与测试消费），但码页是
//      控制台级属性，只要挂着控制台就会切 —— 正常退出 / 已知 fatal 路径会恢复。
//
// ★ 非 Windows 平台：argv 本来就是 UTF-8，控制台是字节流；两个函数都是 no-op / 直通。
// ★ 这两件事是**进程入口**的事，⛔ 不要散到各 shim 里去做。
#pragma once

#include <string>
#include <vector>

namespace lift {

// 主程序的 UTF-8 argv（argv[0] = 可执行文件名；Windows 上按 GetCommandLineW 重建）。
std::vector<std::string> argvUtf8(int argc, char** argv);

// Windows：若进程挂着控制台，把控制台输出码页切到 UTF-8；正常退出时恢复。不是控制台 ⇒ no-op。
void initConsoleUtf8();

// 立即恢复控制台码页（给 `_Exit` / `abort` 这些**不跑析构**的 fatal 路径用；可重复调用）。
void restoreConsoleUtf8();

// 宿主 `wchar_t` → UTF-8（Windows 上给 argvUtf8 / 测试用；其它平台返回 UTF-8 的窄化结果）。
// ★ 与 guest 侧的 `wideToUtf8Text`（kernel32_process.cpp）不是一回事：那是 guest 的 UTF-16 文本。
std::string wideToUtf8(const wchar_t* w);
// UTF-8 → 宿主 `wchar_t`；非法序列返回空串（不抛）。
std::wstring utf8ToWide(const std::string& s);

}  // namespace lift
