// emulator-lift · 解码器（Zydis → 自有 `Insn`）+ IDA 助记符别名表
//
// ★ 只用 Zydis **解码**（MIT；plan §7.1），语义仍是我们的。
// ★ 这张别名表是判据 A2 的**唯一**真源：语料里每个助记符要么能映射到规范名，
//   要么被判为"数据 / 伪指令"（`db` / `proc` / `align` / 局部变量定义 `var_4= …`）。
//   ⛔ 未映射 = 0 才算 A2 绿；漏一个就是红。
// ★ A3 用**同一张表**把两边归一：`.lst` 给 IDA 名，我们给 Zydis 名 ⇒ 都变成规范名再比。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/insn.h"
#include "runtime/memory.h"

namespace lift {

struct DecoderImpl;  // Zydis 的句柄（只在 decode.cpp 里认识它；头文件不引 Zydis）

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool ok() const { return ok_; }
    const char* lastError() const { return last_error_; }

    // 在 `ea` 解码一条。失败（非法 / 越界 / 截断）⇒ false + why 说明字节窗口。
    bool decode(const GuestMemory& mem, std::uint32_t ea, Insn* out, std::string* why) const;
    // 解码一块：从 `ea` 起到**含**第一条控制流指令为止（README §2.2）。
    bool decodeBlock(const GuestMemory& mem, std::uint32_t ea, Block* out, std::string* why) const;
    // 反汇编成文本（诊断用；⛔ 不进热路径）
    std::string text(const GuestMemory& mem, std::uint32_t ea, std::uint8_t* length = nullptr) const;

private:
    std::unique_ptr<DecoderImpl> impl_;
    bool ok_ = false;
    char last_error_[192] = {};
};

// ---- 助记符归一（A2 / A3 共用的唯一真源）----
const char* canonicalFromIda(std::string_view ida_mnemonic);      // nullptr ⇒ 未映射
const char* canonicalFromZydis(std::string_view zydis_mnemonic);  // 大写名，如 "JZ"
const std::vector<const char*>& canonicalMnemonics();

// `.lst` 的一行第一个 token 的**分类**（A2 的判据是 Unknown == 0）
enum class LstKind : std::uint8_t {
    Instruction = 0,  // 是指令（映射得到规范名）
    DataOrPseudo,     // 数据 / 伪指令 / 标签 / 局部变量定义 —— 显式判掉，不算 A2 缺口
    Unknown,          // 既不是已知助记符、也判不掉 ⇒ A2 红
};
// `second` 是第二个 token（可为空）：`a1bit db '...'` 这种"带名字的数据项"靠它判
LstKind classifyLstLine(std::string_view first, std::string_view second);
bool isLstInstructionPrefix(std::string_view token);  // rep / repe / repne / lock

}  // namespace lift
