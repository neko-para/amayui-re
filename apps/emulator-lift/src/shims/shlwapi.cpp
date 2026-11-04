// emulator-lift · shlwapi 路径族（P4 小族）
//
// 本轮只做导入表里**真出现**的那一条：`PathRemoveFileSpecA`（`.lst` 3 个调用点，启动路径上的是
// `sub_4900F0` 的 0x0049037D）。⛔ 不顺手实现 `PathAppendA` / `PathCombineA` 之类既没导入、
// 也没有调用点的 API（本仓口径：没有观测到的 API 不凭空造）。
//
// 语义（Win32 shlwapi：`Removes the trailing file name and backslash from a path, if they are present.`）
//   · 返回值 = 这次**真的移除了东西**（文档："Returns nonzero if something was removed"）。
//   · 分隔符认 `\` 与 `/`（引擎给的是 `\`；`/` 一并支持，不影响任何既有调用）。
//   · 根目录不动：`C:\` / `\` / `\\server\share\` ⇒ 原样 + FALSE（⛔ 不把根的反斜杠也啃掉）。
//   · `C:\a` ⇒ `C:\`（`\` 是**根**的尾部反斜杠 ⇒ 保留）· `C:\a\` ⇒ `C:\a`（去掉尾部反斜杠）·
//     `C:\a\b` ⇒ `C:\a`。
//   · **不带任何分隔符** ⇒ 变空串 + FALSE（"整串就是一个文件名"：文件说明被去掉了，但没有路径
//     可言 ⇒ 返回 FALSE。这条口径来自本轮任务书给的四个边界，测试逐条钉住）。
//   · ⛔ 只改 guest 内存；这是纯字符串函数，不碰宿主盘、不问 VFS。
//
// ★ 为什么返回"是否移除"很重要：真机调用方**可以不看**它（`0x00490382` 处就是 `nop`），
//   但把它做成恒 TRUE 会让"根目录没动"这件事在 trace 里看不见 —— 那是判据，不是细节。
#include <cstdint>
#include <string>

#include "runtime/cp932.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

bool isSep(char c) { return c == '\\' || c == '/'; }

// 路径**根**的长度（含根自己的尾部分隔符）：
//   "C:"    ⇒ 2      "C:\"  ⇒ 3      "\" ⇒ 1
//   "\\server\share" ⇒ 整串（没有 share 之后的段）  "\\server\share\" ⇒ 整串
std::size_t rootLength(const std::string& s) {
    const std::size_t n = s.size();
    if (n >= 2 && isSep(s[0]) && isSep(s[1])) {  // UNC：\\server\share
        std::size_t i = 2;
        while (i < n && !isSep(s[i])) ++i;  // server
        if (i == n) return n;
        ++i;                                // server 后的分隔符
        while (i < n && !isSep(s[i])) ++i;  // share
        if (i < n && isSep(s[i])) ++i;      // share 后的分隔符
        return i;
    }
    if (n >= 2 && s[1] == ':' && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) {
        std::size_t k = 2;
        if (k < n && isSep(s[k])) ++k;
        return k;
    }
    if (n >= 1 && isSep(s[0])) return 1;  // "\a" 的根是 "\"
    return 0;                             // 相对路径：没有根
}

// 日志用：guest 的 ANSI（CP932）字节 → UTF-8（不合法字节按 Windows 的宽松口径跳过）
std::string logText(const std::string& ansi) {
    std::size_t bad = 0;
    const std::u16string w = cp932ToWide(ansi, &bad);
    return utf16ToUtf8(w, &bad);
}

void shimPathRemoveFileSpecA(X86& cpu, ShimCall& call) {
    const char* api = "PathRemoveFileSpecA";
    const std::uint32_t path_ea = call.u32(0);
    if (path_ea == 0) {
        // 真机在这里会解引用 NULL（崩）；本模拟器不崩，但⛔ 也不假装成功。
        shimLog("shlwapi", api, "pszPath 为 NULL ⇒ FALSE（真机会崩；这里只拒绝）");
        call.result = 0;
        return;
    }
    const std::string orig = call.str(0);
    std::string s = orig;
    const std::size_t n = s.size();

    std::size_t last = std::string::npos;  // 最后一个分隔符的位置
    for (std::size_t i = 0; i < n; ++i) {
        if (isSep(s[i])) last = i;
    }

    bool removed = false;
    const std::size_t root = rootLength(s);
    if (last == std::string::npos) {
        // 没有分隔符：整串就是一个文件名 ⇒ 变空串，返回 FALSE（任务书口径）
        s.clear();
    } else if (root >= n) {
        // 整串就是根（"C:\" / "\" / "\\server\share\" …）⇒ 原样不动
        removed = false;
    } else if (last + 1 == n) {
        // 结尾是分隔符 ⇒ 去掉它（"C:\a\" → "C:\a"）；根的情况已在上面拦掉
        s.resize(last);
        removed = true;
    } else if (last + 1 <= root) {
        // 分隔符是**根的**尾部分隔符 ⇒ 保留它，只去掉后面的文件名（"C:\a" → "C:\"）
        s.resize(last + 1);
        removed = true;
    } else {
        // 分隔符在路径中间 ⇒ 连它一起去掉（"C:\a\b" → "C:\a"）
        s.resize(last);
        removed = true;
    }

    // ★ 只写回"新串 + 结尾 0"：真机也**不**清掉旧串尾巴上的字节（调用方常常紧跟 strcat_s）。
    if (s != orig) cpu.mem->writeBytes(path_ea, s.data(), s.size() + 1);
    shimLog("shlwapi", api, "\"%s\" ⇒ \"%s\"（%s）", logText(orig).c_str(), logText(s).c_str(),
            removed ? "TRUE" : "FALSE");
    call.result = removed ? 1u : 0u;
}

}  // namespace

void registerShlwapiShims(ShimRegistry& s) {
    s.add("shlwapi", "PathRemoveFileSpecA", Conv::Stdcall, 1, &shimPathRemoveFileSpecA);
}

}  // namespace lift
