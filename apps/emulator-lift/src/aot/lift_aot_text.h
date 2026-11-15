// emulator-lift · Shape A / T1b：**文本入口文件**与"落在 `.text`"的**唯一**判据
//
// 为什么单独一份：`closure.txt`（入库件）由 `emulator-lift-closure` 写、由 `emulator-lift-gen --check` 读。
// 如果两边各写一套 hex 文法与"可执行节"判据，那 closure.txt 就可能"生成器能产、检查器判红"——
// 这是典型的**静默漂**。⇒ 文法与节区间只留在这里（inline，两处 include 同一份）。
//
// ★ `--check` 的格式判据（父级裁决 D5 / T1 §4 候选 A-1）：`0x` + **恰好 8 位**十六进制。
//   ⛔ 不认 `0x436D00`（6 位）、⛔ 不认无前缀、⛔ 不认负号 / `u` 后缀。
// ★ "落在 `.text`" 的判据**不是硬编码节名**，而是 PE 节头里的**可执行位**（`.text` 的可执行位 =
//   `0x20000000`）。理由：节名是**那份 PE 的**事实，换一份 exe 节名可能不同；而 lift 的语义前提是
//   "这是一段可执行字节"。（本仓的 AGE.EXE 只有 `.text` 带可执行位 —— 读数见 T1b 报告。）
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/image.h"

namespace lift {
namespace aot {

struct ExecRange {
    std::uint32_t begin = 0;  // 含
    std::uint32_t end = 0;    // 不含
    std::string name;
};

// PE 里所有**可执行**节的 EA 区间（升序；`begin + size` 溢出直接跳过并报错不划算 ⇒ 用 64 位判）。
inline std::vector<ExecRange> execRanges(const lift::PeImage& img) {
    std::vector<ExecRange> out;
    for (const lift::PeSectionInfo& s : img.sections) {
        if (!s.executable()) continue;
        const std::uint32_t size = s.vsize != 0 ? s.vsize : s.raw_size;
        if (size == 0) continue;
        const std::uint64_t begin = static_cast<std::uint64_t>(img.image_base) + s.va;
        const std::uint64_t end = begin + size;
        if (end > 0xFFFFFFFFull) continue;  // 32 位 guest 装不下 ⇒ 不是可执行目标
        out.push_back(ExecRange{static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end),
                                std::string(s.name)});
    }
    return out;
}

inline const ExecRange* execRangeOf(const std::vector<ExecRange>& ranges, std::uint32_t ea) {
    for (const ExecRange& r : ranges) {
        if (ea >= r.begin && ea < r.end) return &r;
    }
    return nullptr;
}

// 去掉 `#` 起的注释 + 两端空白（closure.txt / census.txt 共用）。
inline std::string stripCommentAndTrim(const std::string& line) {
    std::string s = line;
    const std::size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) --e;
    return s.substr(b, e - b);
}

// 严格文法：`0x` + 恰好 8 位十六进制（大小写不敏感）。成功 ⇒ `*ea` 写好、返回 true。
inline bool strictParseEa(const std::string& token, std::uint32_t* ea) {
    if (token.size() != 10) return false;
    if (token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) return false;
    std::uint32_t value = 0;
    for (std::size_t i = 2; i < token.size(); ++i) {
        const char ch = token[i];
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) return false;
        const int digit = std::isdigit(static_cast<unsigned char>(ch)) != 0
                              ? ch - '0'
                              : (std::tolower(static_cast<unsigned char>(ch)) - 'a' + 10);
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    *ea = value;
    return true;
}

}  // namespace aot
}  // namespace lift
