// emulator-lift · `.lst` 索引（**只读**、诊断与对照用）
//
// ★ 口径（README §2.1）：`.lst` 不是执行源，它给 `EA → 指令文本` 与**指令边界**；
//   A3 就是拿它的边界去钉我们的解码器。⛔ 任何"解析友好化"只在本索引的内存视图里发生，
//   ⛔ 不许写回语料文件（AGENTS.md §1.5）。
// ★ 它同时是 fault 诊断的文本来源（"EA + `.lst` 文本 + 寄存器 + guest 返回栈"）。
// ★ 语料**不在仓库里**（`corpus/disasm/files/` 被 gitignore）⇒ 依赖它的判据落 **assets 档**。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lift {

struct LstEntry {
    std::uint32_t ea = 0;
    std::uint32_t next_ea = 0;   // 同段里**下一条指令行**的 EA（0 = 没有下一行）
    std::uint32_t seg = 0;       // 段下标（segments()）
    std::string text;            // 去掉 `段:EA ` 前缀后的整行
    std::string mnemonic;        // 第一个 token（IDA 的助记符，原样）
};

// `.lst` 里的函数入口（`sub_XXXX proc near`）。★ 只用于**报告 / 归属**，不是执行单位
// （README §2.2：执行单位是 basic block；函数是聚合视图）。
struct LstFunction {
    std::uint32_t ea = 0;
    std::uint32_t seg = 0;
    std::string name;       // `sub_401000` / `nullsub_1` / …
    std::string signature;  // 紧挨着的 `; int __cdecl sub_401000(...)`（可能为空）
};

class LstIndex {
public:
    bool load(const std::string& path, std::string* err);
    bool loaded() const { return loaded_; }

    // EA → 行（同一 EA 出现多次时取**第一条指令行**）
    const LstEntry* find(std::uint32_t ea) const;
    // 诊断用文本：找不到 ⇒ 空串
    // ⚠ 返回 `std::string` ⇒ **会分配**。只许用在非 fault 路径（如 `dumpAtEa`）；
    //   fault hook 在异常上下文里跑，必须用下面的 `textPtr`。
    std::string textAt(std::uint32_t ea) const;
    // 诊断用文本的**零分配**形态（2026-11-04 / REQ-01M6JFZFN15MBPG7Y89WNZACFW #4）：
    //   返回指向内部字符串的指针（生命周期 = 本索引，`load` 之后不再变），找不到 ⇒ nullptr。
    //   ★ fault hook 在异常上下文里跑：⛔ 不许 new / 不许增长容器 ⇒ 这里只给内部指针，不做任何拷贝。
    const char* textPtr(std::uint32_t ea) const {
        const LstEntry* e = find(ea);
        return e ? e->text.c_str() : nullptr;
    }

    const std::vector<LstEntry>& entries() const { return entries_; }
    // 函数入口（按 EA 升序；来自 `proc near` 行；签名来自紧邻的注释行，可能为空）
    const std::vector<LstFunction>& functions() const { return functions_; }
    // EA 所属函数（EA 之前最近的函数入口；没有 ⇒ nullptr）。★ 近似：chunk / 交错函数会归错，
    // 只用于 profile 的**聚合视图**；逐块坐标仍以 block EA 为准。
    const LstFunction* functionAt(std::uint32_t ea) const;
    // 按名字找函数入口（`_memcpy` / `sub_401000` / …）；同名多入口时取第一个（IDA 下不会）。
    const LstFunction* findFunction(std::string_view name) const;
    // 既不是指令、也判不成数据/伪指令的 token（A2 的失败清单；空 = A2 绿）
    const std::vector<LstEntry>& unknownLines() const { return unknown_; }
    const std::vector<std::string>& segments() const { return segments_; }
    const std::string& headerSha256() const { return header_sha256_; }
    const std::string& path() const { return path_; }
    std::size_t collapsedCount() const { return collapsed_; }

private:
    bool loaded_ = false;
    std::string path_;
    std::string header_sha256_;
    std::vector<std::string> segments_;
    std::vector<LstEntry> entries_;
    std::vector<LstFunction> functions_;
    std::vector<LstEntry> unknown_;
    std::unordered_map<std::uint32_t, std::size_t> by_ea_;
    std::size_t collapsed_ = 0;
    char error_[192] = {};
};

// 从 `.lst` 头部读 `; Input SHA256 : <hex>`（用于 A3 ⓪ 的字节对齐校验）
bool readLstHeaderSha256(const std::string& lst_path, std::string* out_hex, std::string* err);

}  // namespace lift
