// emulator-lift · 测试侧拼 Windows 命令行的**唯一实现**（纯函数；不引 SDL / 平台头）
//
// ★ 为什么需要它：`CreateProcessW` 收的是一条**字符串**，而子进程会按自己那套规则把它重新切成
//   argv（本项目产品二进制用的是 `CommandLineToArgvW`，见 `runtime/host_encoding.cpp` 的
//   `argvUtf8`；MSVC CRT 的规则与此逐条一致，见下面的原文）。手拼 `" + arg + "` 只在
//   "参数里没有反斜杠紧挨引号"时碰巧成立 —— 实测反例：`--game-dir C:\...\Temp\`（Windows 的
//   临时目录**以 `\` 结尾**）会拼成 `"...\Temp\"`，那对 `\"` 是**字面引号** ⇒ 引号提前闭合
//   ⇒ 后面所有开关被并进同一个 argv，子进程静默看不到它们（`.tmp/close-path/analysis.md` §2）。
//
// ★ 规则原文（Microsoft Learn，"Parsing C++ command-line arguments"，
//   https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args?view=msvc-170）：
//   · "A string surrounded by double quote marks is interpreted as a single argument, which may
//      contain white-space characters. ... Within a quoted string, a pair of double quote marks is
//      interpreted as a single escaped double quote mark."
//   · "A double quote mark preceded by a backslash (\") is interpreted as a literal double quote
//      mark (")."
//   · "Backslashes are interpreted literally, unless they immediately precede a double quote mark."
//   · "If an even number of backslashes is followed by a double quote mark, then one backslash (\) is
//      placed in the argv array for every pair of backslashes (\\), and the double quote mark (") is
//      interpreted as a string delimiter."
//   · "If an odd number of backslashes is followed by a double quote mark, then one backslash (\) is
//      placed in the argv array for every pair of backslashes (\\). The double quote mark is
//      interpreted as an escape sequence by the remaining backslash, causing a literal double quote
//      mark (") to be placed in argv."
//
// ⇒ 编码只做两件事（其余字符原样；**总是**加引号，所以空串/含空格都安全）：
//   ① 一个 `"` 之前若已有 n 个连续 `\`，写 2n+1 个 `\` 再写 `"`（奇数 ⇒ 那个引号是字面引号）；
//   ② 结尾的 n 个连续 `\` 写 2n 个（偶数 ⇒ 收尾引号是分隔符，不会被转义）。
//   ★ 结尾反斜杠就是本仓两次 `exit=3` 红的那一格，所以它是守卫的第一档。
//
// ★ 为什么放在头里而不是某个 .cpp：`runCaptured` / `runChild` 这类"起子进程"的夹具在**六七个**
//   测试二进制里各有一份（`gpu_batch` / `exit_code` / `cli_time_limit` / `cli_dump_frame` /
//   `feeder_seg_e2e` / `host_encoding` / `movie_logo`）。根因只有一处实现才有意义
//   （AGENTS.md §10：不写第二份）；否则修了 `gpu_batch` 只是把地雷留给下一个夹具。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace lift_test {

/** 把一个 argv token 编码成 Windows 命令行里的一段（两侧带引号；结尾反斜杠加倍）。 */
inline std::wstring quoteArg(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    out.push_back(L'"');
    std::size_t backslashes = 0;  // 紧邻当前位置的连续 `\` 个数
    for (const wchar_t c : s) {
        if (c == L'\\') {
            out.push_back(c);
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // 2n+1 个反斜杠 + 引号 ⇒ 解析回来是 n 个反斜杠 + 一个字面引号。
            out.append(backslashes + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.push_back(c);
        backslashes = 0;
    }
    // 结尾 run：2n 个反斜杠 ⇒ 解析回来是 n 个反斜杠，且收尾引号仍是分隔符。
    out.append(backslashes, L'\\');
    out.push_back(L'"');
    return out;
}

/** argv（含 argv[0]）⇒ 一条可直接交给 `CreateProcessW` 的命令行。 */
inline std::wstring joinCommandLine(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (const std::wstring& a : args) {
        if (!out.empty()) out.push_back(L' ');
        out += quoteArg(a);
    }
    return out;
}

/**
 * `quoteArg` 的逆：按上面引到的 CRT 规则把命令行切回 token（供**往返**守卫用）。
 * ⛔ 只实现文档里的那几条规则；不认识的形态（比如未闭合的引号）按"读到行尾"处理 ——
 *    那是文档明写的："If the command line ends before a closing double quote mark is found,
 *    then all the characters read so far are output as the last argument."
 */
inline std::vector<std::wstring> splitCommandLine(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        while (i < n && (s[i] == L' ' || s[i] == L'\t')) ++i;
        if (i >= n) break;
        std::wstring cur;
        bool in_quotes = false;
        while (i < n) {
            const wchar_t c = s[i];
            if (!in_quotes && (c == L' ' || c == L'\t')) break;
            if (c == L'\\') {
                std::size_t run = 0;
                while (i + run < n && s[i + run] == L'\\') ++run;
                if (i + run < n && s[i + run] == L'"') {
                    cur.append(run / 2, L'\\');
                    if ((run % 2) == 0) {
                        in_quotes = !in_quotes;  // 偶数个反斜杠 ⇒ 引号是分隔符
                    } else {
                        cur.push_back(L'"');  // 奇数 ⇒ 最后一个反斜杠把引号转义成字面引号
                    }
                    i += run + 1;
                } else {
                    cur.append(run, L'\\');
                    i += run;
                }
                continue;
            }
            if (c == L'"') {
                // 引号内的 `""` = 一个字面引号；否则翻转"在引号内"。
                if (in_quotes && i + 1 < n && s[i + 1] == L'"') {
                    cur.push_back(L'"');
                    i += 2;
                } else {
                    in_quotes = !in_quotes;
                    ++i;
                }
                continue;
            }
            cur.push_back(c);
            ++i;
        }
        out.push_back(cur);
    }
    return out;
}

}  // namespace lift_test
