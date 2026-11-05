// emulator-lift · `.lst` 索引（只读；诊断与 A3 对照用）
//
// ★ 只把**指令行**进索引（数据 / 伪指令行只计数）：A3 的判据是"每一条非 collapsed 指令行"，
//   所以数据行不该进对照集合。分类用 `classifyLstLine`（与 A2 共用同一张表）。
// ★ `next_ea` = 同段下一条**指令行**的 EA —— A3 的"长度 == 下一行 EA 差"直接用这个值。
// ★ ⛔ 不写回语料：这里全部是内存视图（AGENTS.md §1.5）。
#include "corpus/lst_index.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "runtime/decode.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

bool isHex8(const char* p) {
    for (int i = 0; i < 8; ++i) {
        const char c = p[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

std::uint32_t hex8(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = p[i];
        const std::uint32_t d = (c >= '0' && c <= '9') ? static_cast<std::uint32_t>(c - '0')
                               : (c >= 'A' && c <= 'F') ? static_cast<std::uint32_t>(c - 'A' + 10)
                                                        : static_cast<std::uint32_t>(c - 'a' + 10);
        v = (v << 4) | d;
    }
    return v;
}

void splitTokens(std::string_view text, std::string_view* t0, std::string_view* t1) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    std::size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    *t0 = text.substr(i, j - i);
    i = j;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    *t1 = (i <= text.size()) ? text.substr(i, j - i) : std::string_view{};
}

}  // namespace

bool readLstHeaderSha256(const std::string& lst_path, std::string* out_hex, std::string* err) {
    std::FILE* f = openHostFileUtf8(lst_path, "rb");
    if (!f) {
        if (err) *err = "打不开 .lst：" + lst_path;
        return false;
    }
    char line[512];
    bool found = false;
    for (int i = 0; i < 200 && std::fgets(line, sizeof(line), f); ++i) {
        const char* p = std::strstr(line, "; Input SHA256 :");
        if (!p) continue;
        p += std::strlen("; Input SHA256 :");
        while (*p == ' ' || *p == '\t') ++p;
        std::string hex;
        for (int k = 0; k < 64; ++k) {
            const char c = p[k];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) break;
            hex.push_back(c);
        }
        if (hex.size() != 64) continue;
        *out_hex = toLower(hex);
        found = true;
        break;
    }
    std::fclose(f);
    if (!found) {
        if (err) *err = ".lst 头部没有 `; Input SHA256 :` 行";
        return false;
    }
    return true;
}

bool LstIndex::load(const std::string& path, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (!f) {
        std::snprintf(error_, sizeof(error_), "打不开 .lst：%s", path.c_str());
        if (err) *err = error_;
        return false;
    }
    path_ = path;
    entries_.clear();
    functions_.clear();
    unknown_.clear();
    by_ea_.clear();
    segments_.clear();
    collapsed_ = 0;

    char buf[1024];
    std::uint32_t last_ea_by_seg[64] = {};  // 段数上限（IDA 的 .lst 里段很少）
    std::size_t pending_next[64] = {};      // 需要回填 next_ea 的条目下标
    bool has_pending[64] = {};
    std::vector<std::size_t> seg_of_entry;
    // 函数入口：IDA 在 `sub_XXXX proc near` 的**紧邻上一行**给签名注释（同一 EA）。
    std::string last_comment;
    std::uint32_t last_comment_ea = 0;

    while (std::fgets(buf, sizeof(buf), f)) {
        std::string_view line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon + 9 > line.size()) continue;
        if (!isHex8(line.data() + colon + 1)) continue;
        const std::string_view seg = line.substr(0, colon);
        const std::uint32_t ea = hex8(line.data() + colon + 1);
        std::string_view rest = line.substr(colon + 9);
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);

        // 头部 SHA256
        if (header_sha256_.empty()) {
            const std::size_t h = rest.find("; Input SHA256 :");
            if (h != std::string_view::npos) {
                std::string_view v = rest.substr(h + std::strlen("; Input SHA256 :"));
                while (!v.empty() && v.front() == ' ') v.remove_prefix(1);
                std::string hex;
                for (char c : v) {
                    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
                    if (!ok || hex.size() >= 64) break;
                    hex.push_back(c);
                }
                if (hex.size() == 64) header_sha256_ = toLower(hex);
            }
        }

        if (rest.empty()) continue;

        // ★ COLLAPSED 行**也是一行**，而且它的 EA 就是下一段的起点 —— A3 的"下一行 EA 差"
        //   必须把它算进去（否则那 1805 处会变成假的边界不一致）。它本身不是指令行，
        //   所以只参与 `next_ea` 的回填，不进 `entries_`。
        const bool collapsed = rest.find("COLLAPSED") != std::string_view::npos;
        if (collapsed) ++collapsed_;
        // ★ CRT 函数在 `.lst` 里常常是 `; [N BYTES: COLLAPSED FUNCTION _memcpy. …]`：
        //   没有 `proc near` 行，但这一行的 EA **就是函数入口**。profile 的归属需要它，
        //   否则 `_memcpy` 的权重会被算到上一个 `sub_` 头上（实测：24% 被记成 sub_4E73EB）。
        std::string collapsed_fn;
        if (collapsed) {
            constexpr std::string_view kKey = "COLLAPSED FUNCTION ";
            const std::size_t k = rest.find(kKey);
            if (k != std::string_view::npos) {
                std::string_view v = rest.substr(k + kKey.size());
                // 名字可能含空格（C++ 模板名）；截到 IDA 的后缀 `. PRESS` / `]` 为止。
                const std::size_t stop = v.find(". PRESS");
                if (stop != std::string_view::npos) v = v.substr(0, stop);
                while (!v.empty() && (v.back() == '.' || v.back() == ' ')) v.remove_suffix(1);
                if (!v.empty()) collapsed_fn = std::string(v);
            }
        }
        std::string_view t0;
        std::string_view t1;
        splitTokens(rest, &t0, &t1);
        if (t0.empty()) continue;
        if (rest.front() == ';') {
            last_comment = std::string(rest);
            last_comment_ea = ea;
        }

        const LstKind kind = classifyLstLine(t0, t1);

        // 段下标
        std::size_t seg_index = 0;
        bool found_seg = false;
        for (std::size_t i = 0; i < segments_.size(); ++i) {
            if (segments_[i] == seg) {
                seg_index = i;
                found_seg = true;
                break;
            }
        }
        if (!found_seg) {
            if (segments_.size() >= 64) continue;
            seg_index = segments_.size();
            segments_.emplace_back(seg);
        }

        // 函数入口（`sub_XXXX proc near` / `COLLAPSED FUNCTION name`）：只做聚合视图
        // （README §2.2），不是执行单位。
        if (!collapsed_fn.empty() && (functions_.empty() || functions_.back().ea != ea)) {
            LstFunction fn;
            fn.ea = ea;
            fn.seg = static_cast<std::uint32_t>(seg_index);
            fn.name = std::move(collapsed_fn);
            functions_.push_back(std::move(fn));
        }
        if (t1 == "proc" && (functions_.empty() || functions_.back().ea != ea)) {
            LstFunction fn;
            fn.ea = ea;
            fn.seg = static_cast<std::uint32_t>(seg_index);
            fn.name = std::string(t0);
            if (last_comment_ea == ea) {
                std::string_view sig = last_comment;
                while (!sig.empty() && (sig.front() == ';' || sig.front() == ' ')) sig.remove_prefix(1);
                fn.signature = std::string(sig);
            }
            functions_.push_back(std::move(fn));
        }

        // ★ `next_ea` = **下一行（任何行）** 的 EA —— 这是 A3 判据的字面口径：
        //   ".lst 里同一条指令之后的那一行"。内联数据（`db 8Dh, 0A4h…`）与 `align` 都会占一行，
        //   所以"下一条**指令**行"的 EA 差会把它们算进去 ⇒ 用前者会造出假的不一致。
        //   同一 EA 的标签 / 注释行不算（要求 ea 严格大于本行的 ea）。
        if (seg_index < 64 && has_pending[seg_index] && ea > entries_[pending_next[seg_index]].ea) {
            entries_[pending_next[seg_index]].next_ea = ea;
            has_pending[seg_index] = false;
        }

        if (kind == LstKind::Unknown) {
            LstEntry e;
            e.ea = ea;
            e.text = std::string(rest);
            e.mnemonic = std::string(t0);
            unknown_.push_back(std::move(e));
            continue;
        }
        if (kind != LstKind::Instruction) continue;

        LstEntry e;
        e.ea = ea;
        e.seg = static_cast<std::uint32_t>(seg_index);
        e.text = std::string(rest);
        e.mnemonic = std::string(t0);
        const std::size_t index = entries_.size();
        entries_.push_back(std::move(e));
        seg_of_entry.push_back(seg_index);

        if (seg_index < 64) {
            pending_next[seg_index] = index;
            has_pending[seg_index] = true;
            last_ea_by_seg[seg_index] = ea;
        }
        if (!by_ea_.count(ea)) by_ea_.emplace(ea, index);
    }
    std::fclose(f);
    (void)last_ea_by_seg;
    std::sort(functions_.begin(), functions_.end(),
              [](const LstFunction& a, const LstFunction& b) { return a.ea < b.ea; });
    loaded_ = true;
    if (err) err->clear();
    return true;
}

const LstEntry* LstIndex::find(std::uint32_t ea) const {
    const auto it = by_ea_.find(ea);
    if (it == by_ea_.end()) return nullptr;
    return &entries_[it->second];
}

std::string LstIndex::textAt(std::uint32_t ea) const {
    const LstEntry* e = find(ea);
    return e ? e->text : std::string();
}

const LstFunction* LstIndex::functionAt(std::uint32_t ea) const {
    if (functions_.empty()) return nullptr;
    // ★ 段约束：不同段之间没有"函数范围"关系（数据段的 EA 不该继承 .text 的最后一个函数）。
    //   找不到该 EA 的指令行时（例如运行时才出现的代码）退回"最近入口"的旧行为。
    const LstEntry* entry = find(ea);
    const std::uint32_t want_seg = entry ? entry->seg : 0xFFFFFFFFu;
    auto it = std::upper_bound(functions_.begin(), functions_.end(), ea,
                               [](std::uint32_t v, const LstFunction& f) { return v < f.ea; });
    while (it != functions_.begin()) {
        --it;
        if (want_seg == 0xFFFFFFFFu || it->seg == want_seg) return &*it;
    }
    return nullptr;
}

const LstFunction* LstIndex::findFunction(std::string_view name) const {
    for (const LstFunction& f : functions_)
        if (f.name == name) return &f;
    return nullptr;
}

}  // namespace lift
