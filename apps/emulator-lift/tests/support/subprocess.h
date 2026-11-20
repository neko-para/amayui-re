// emulator-lift · 测试侧起子进程的**唯一实现**（跨平台：Win32 `CreateProcessW` / POSIX `fork+execv`）
//
// ★ 判据（红了意味着什么）：`runChild` 的任一子判据（argv 逐 token / stdout-stderr 分文件 /
//   退出码 / 信号 / 超时 / cwd / started）坏了 ⇒ 所有 CLI 级用例读到的都是"别的进程的结论"，
//   它们照绿也只能证明那条用例没在验东西。
// ★ 捕获用**两个普通文件**（⛔ 管道：受限沙箱里 `pipe()`/`CreatePipe` 会 EPERM，AGENTS §5.1）；
//   临时文件放 OS temp、名字带 pid+序号，成功/失败路径都由 RAII 清理。
// ★ `argv` token 一律 **UTF-8**（POSIX 直接 `execv` 字节；Windows 用 `MultiByteToWideChar(CP_UTF8)`
//   转宽后交给 `cmdline.h` 的 `joinCommandLine`）⇒ 调用方只有一套写法。
// ★ ⛔ 不 `abort`：失败只通过 `started` / `timed_out` / `exit_code` 表达。
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lift_test {

struct ChildOptions {
    /** 子进程 cwd（空 ⇒ 继承父进程 cwd）。 */
    std::filesystem::path cwd;
    /** 超时（到点强杀；默认 60 s）。 */
    std::chrono::milliseconds timeout{60000};
};

struct ChildResult {
    /** 正常退出 = 退出码；被信号终止 = `128+sig`；超时 = 991；`started=false` = -1（⛔ 不当 0）。 */
    int exit_code = -1;
    /** 子进程真的起来了（exe 不存在 / 不可执行 / `CreateProcessW` 失败 ⇒ false）。 */
    bool started = false;
    /** 到点强杀（POSIX `SIGKILL` + 收尸；Windows `TerminateProcess`）。 */
    bool timed_out = false;
    std::string out;  // stdout（UTF-8 字节）
    std::string err;  // stderr（UTF-8 字节）

    /** `out + err`；**只**做 Windows 的 `\r\n` → `\n` 归一（⛔ 不假设两流时序）。 */
    std::string text() const;
};

/**
 * 起 `exe` 并等它结束。`argv` **不含** exe 本身（argv[0] 由实现填 `exe`）。
 * 所有失败（含 exe 缺失）都通过返回值表达，不抛、不 abort。
 */
ChildResult runChild(const std::filesystem::path& exe, const std::vector<std::string>& argv,
                     const ChildOptions& opts = {});

/** `fs::path` → UTF-8 字节（Windows 走 `WideCharToMultiByte(CP_UTF8)`；POSIX 直接 native 字节）。 */
std::string pathToUtf8(const std::filesystem::path& p);

/** 当前进程 id（临时目录/临时文件命名用）。 */
std::uint64_t processId();

}  // namespace lift_test
