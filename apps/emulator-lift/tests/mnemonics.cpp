// emulator-lift · 助记符表自检（pure）—— A2 的"表本身是良构的"那一半
//
// ★ A2 = 语料里每个助记符都能映射或判为数据/伪指令（未映射 = 0）。它需要语料 ⇒ assets 档。
//   这里只测**表本身**：规范名唯一、别名不互相打架、分类器该认的认、该拒的拒。
//   ⛔ 这两半分开放不是"降级判据"：没有语料的机器跑这一半，有语料的机器两半都跑。
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/decode.h"

int main() {
    using namespace lift_test;

    // ① 规范名唯一且非空
    {
        g_current_case = "canonical-unique";
        std::set<std::string> seen;
        for (const char* c : lift::canonicalMnemonics()) {
            LIFT_CHECK(c && c[0]);
            LIFT_CHECK(seen.insert(c).second);  // 重复 = 表里同一行写了两次
        }
        std::printf("canonical mnemonics: %zu\n", seen.size());
    }

    // ② 每个规范名都能从它自己映射回来（大小写不敏感）
    {
        g_current_case = "canonical-self-map";
        for (const char* c : lift::canonicalMnemonics()) {
            const char* back = lift::canonicalFromIda(c);
            LIFT_CHECK(back != nullptr && std::string(back) == c);
        }
    }

    // ③ IDA 与 Zydis 的别名必须归一（A3 的两侧对照就靠这个）
    {
        g_current_case = "aliases";
        const std::pair<const char*, const char*> cases[] = {
            {"retn", "ret"},   {"jz", "je"},      {"jnz", "jne"},   {"jnb", "jae"},
            {"setnz", "setne"}, {"setz", "sete"}, {"setnl", "setge"}, {"jle", "jle"},
            {"wait", "wait"},  {"fwait", "wait"}, {"fstsw", "fstsw"}, {"fnstsw", "fnstsw"},
            {"sal", "shl"},    {"jc", "jb"},      {"jnc", "jae"},
        };
        for (const auto& c : cases) {
            const char* got = lift::canonicalFromIda(c.first);
            LIFT_CHECK(got != nullptr);
            if (got) LIFT_CHECK(std::string(got) == c.second);
        }
        // Zydis 侧（大写）走同一张表
        LIFT_CHECK(std::string(lift::canonicalFromZydis("JZ")) == "je");
        LIFT_CHECK(std::string(lift::canonicalFromZydis("RET")) == "ret");
    }

    // ④ 分类器：该是指令的、该判掉的、该报 Unknown 的
    {
        g_current_case = "lst-classify";
        LIFT_CHECK(lift::classifyLstLine("mov", "ebp") == lift::LstKind::Instruction);
        LIFT_CHECK(lift::classifyLstLine("rep", "movsd") == lift::LstKind::Instruction);
        LIFT_CHECK(lift::classifyLstLine("lock", "xadd") == lift::LstKind::Instruction);
        LIFT_CHECK(lift::classifyLstLine("fstp", "[ebp+arg_20]") == lift::LstKind::Instruction);
        LIFT_CHECK(lift::classifyLstLine("db", "0FFh") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("align", "10h") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("arg_0=", "dword") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("loc_401013:", "mov") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("a1bit", "db") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("UnwindMapEntry", "<-1,") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine(";", "comment") == lift::LstKind::DataOrPseudo);
        // 数据项的**续行**（`UnwindMapEntry <-1,` 之后是 `0, 0, 0>`）必须判成数据，不是 Unknown
        LIFT_CHECK(lift::classifyLstLine("0,", "0,") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("<", "") == lift::LstKind::DataOrPseudo);
        // 既不是助记符、也不是标识符 / 数字 / 数据片段 ⇒ 必须报 Unknown（A2 的红点）
        LIFT_CHECK(lift::classifyLstLine("###", "") == lift::LstKind::Unknown);
        LIFT_CHECK(lift::classifyLstLine("%", "") == lift::LstKind::Unknown);
        // ★ 审计 T4：**裸标识符**（不在助记符表里的 `cld`）必须报 Unknown，⛔ 不许默认判数据；
        //   而"名字 + 类型 + 值"的数据项 / IDA 头部汇编器指令必须仍判数据。
        LIFT_CHECK(lift::classifyLstLine("cld", "") == lift::LstKind::Unknown);
        LIFT_CHECK(lift::classifyLstLine("xmmword_530230", "xmmword") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine("ExceptionInfo", "_EXCEPTION_POINTERS") ==
                   lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine(".686p", "") == lift::LstKind::DataOrPseudo);
        LIFT_CHECK(lift::classifyLstLine(".mmx", "") == lift::LstKind::DataOrPseudo);
    }

    // ⑤ 前缀识别（A3 组合助记符时用）
    {
        g_current_case = "prefixes";
        LIFT_CHECK(lift::isLstInstructionPrefix("rep"));
        LIFT_CHECK(lift::isLstInstructionPrefix("repne"));
        LIFT_CHECK(lift::isLstInstructionPrefix("lock"));
        LIFT_CHECK(!lift::isLstInstructionPrefix("mov"));
    }

    return finish("mnemonics");
}
