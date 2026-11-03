// emulator-lift · 助记符别名表（判据 A2 的**唯一真源**）
//
// ★ 这张表回答两件事，**不要**把它们混起来：
//   ① **映射**（A2）：`.lst` 里出现的每个助记符都能变成规范名 —— 这决定"我们读懂了吗"；
//   ② **实现**（S3/S4）：规范名对应哪个 `Op` —— 这决定"能不能执行"。
//   MMX / SSE 的助记符在①里**必须**有（否则 A2 红），但在②里是 `Op::Other`（M1b 才做）⇒
//   执行到就响亮失败。⛔ 不要为了让 A2 变绿而把它们从表里删掉，也不要为了执行而假装实现。
// ★ IDA 与 Zydis 的拼写不同（`retn`/`ret`、`jz`/`je`、`setnz`/`setne`、`wait`/`fwait`），
//   所以一行可以挂多个别名；查找是**大小写不敏感**的。
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/decode.h"

namespace lift {
namespace {

struct Row {
    const char* canonical;
    const char* aliases;  // 空格分隔（小写）
};

// ⚠ 规范名选型：用 Intel 的短名（`je` 而不是 `jz`），因为它同时是"最常见写法"与 Zydis 的一种拼法。
//   一个 mnemonic 只允许出现在**一行**里（`canonicalMnemonics()` 自检重复）。
const Row kRows[] = {
    // ---- 数据搬运 ----
    {"mov", "mov"},
    {"movzx", "movzx"},
    {"movsx", "movsx"},
    {"lea", "lea"},
    {"xchg", "xchg"},
    {"push", "push"},
    {"pop", "pop"},
    {"pushad", "pushad pusha"},
    {"popad", "popad popa"},
    {"pushfd", "pushfd pushf"},
    {"popfd", "popfd popf"},
    {"xadd", "xadd"},
    {"cmpxchg", "cmpxchg"},
    // ---- 算术 / 逻辑 ----
    {"add", "add"},
    {"or", "or"},
    {"and", "and"},
    {"sub", "sub"},
    {"xor", "xor"},
    {"cmp", "cmp"},
    {"test", "test"},
    {"inc", "inc"},
    {"dec", "dec"},
    {"neg", "neg"},
    {"not", "not"},
    {"adc", "adc"},
    {"sbb", "sbb"},
    // ---- 移位 ----
    {"shl", "shl sal"},
    {"shr", "shr"},
    {"sar", "sar"},
    {"rol", "rol"},
    {"ror", "ror"},
    {"rcl", "rcl"},
    {"rcr", "rcr"},
    {"shld", "shld"},
    {"shrd", "shrd"},
    // ---- 乘除 / 扩展 ----
    {"mul", "mul"},
    {"imul", "imul"},
    {"div", "div"},
    {"idiv", "idiv"},
    {"cdq", "cdq"},
    {"cwd", "cwd"},
    {"cwde", "cwde"},
    {"cbw", "cbw"},
    {"bswap", "bswap"},
    // ---- 位 ----
    {"bt", "bt"},
    {"bts", "bts"},
    {"btr", "btr"},
    {"btc", "btc"},
    {"bsf", "bsf"},
    {"bsr", "bsr"},
    // ---- 控制流 ----
    {"jmp", "jmp"},
    {"je", "je jz"},
    {"jne", "jne jnz"},
    {"jb", "jb jc jnae"},
    {"jae", "jae jnb jnc"},
    {"jbe", "jbe jna"},
    {"ja", "ja jnbe"},
    {"js", "js"},
    {"jns", "jns"},
    {"jp", "jp jpe"},
    {"jnp", "jnp jpo"},
    {"jl", "jl jnge"},
    {"jge", "jge jnl"},
    {"jle", "jle jng"},
    {"jg", "jg jnle"},
    {"jo", "jo"},
    {"jno", "jno"},
    {"seto", "seto"},
    {"setno", "setno"},
    {"setb", "setb setc setnae"},
    {"setae", "setae setnb setnc"},
    {"sete", "sete setz"},
    {"setne", "setne setnz"},
    {"setbe", "setbe setna"},
    {"seta", "seta setnbe"},
    {"sets", "sets"},
    {"setns", "setns"},
    {"setp", "setp setpe"},
    {"setnp", "setnp setpo"},
    {"setl", "setl setnge"},
    {"setge", "setge setnl"},
    {"setle", "setle setng"},
    {"setg", "setg setnle"},
    // cmovcc：**每个条件码一族都要登记**（A2 是"语料里出现的每个助记符都要映射"，
    // 只登记 e/ne 会让 cmovl/cmovg/… 落到 Op::Other ⇒ 执行时响亮失败）。
    {"cmovo", "cmovo"},
    {"cmovno", "cmovno"},
    {"cmovb", "cmovb cmovc cmovnae"},
    {"cmovae", "cmovae cmovnb cmovnc"},
    {"cmove", "cmove cmovz"},
    {"cmovne", "cmovne cmovnz"},
    {"cmovbe", "cmovbe cmovna"},
    {"cmova", "cmova cmovnbe"},
    {"cmovs", "cmovs"},
    {"cmovns", "cmovns"},
    {"cmovp", "cmovp cmovpe"},
    {"cmovnp", "cmovnp cmovpo"},
    {"cmovl", "cmovl cmovnge"},
    {"cmovge", "cmovge cmovnl"},
    {"cmovle", "cmovle cmovng"},
    {"cmovg", "cmovg cmovnle"},
    {"call", "call"},
    {"ret", "ret retn"},
    {"retf", "retf"},
    {"leave", "leave"},
    {"nop", "nop"},
    {"hlt", "hlt"},
    // ★ `int 3` 与 `int3`（CC）是**同一条指令**的两种编码，IDA 一律写 `int 3` ⇒ 归一个规范名。
    {"int", "int int3"},
    {"into", "into"},
    {"pause", "pause"},
    {"cpuid", "cpuid"},
    {"sahf", "sahf"},
    {"lahf", "lahf"},
    {"rdtsc", "rdtsc"},
    // ---- 串操作（`rep` 前缀在 prefix 里单独认）----
    {"movsb", "movsb"},
    {"movsd", "movsd"},
    {"movsw", "movsw"},
    {"stosb", "stosb"},
    {"stosd", "stosd"},
    {"stosw", "stosw"},
    {"lodsb", "lodsb"},
    {"lodsd", "lodsd"},
    {"lodsw", "lodsw"},
    {"scasb", "scasb"},
    {"scasd", "scasd"},
    {"scasw", "scasw"},
    {"cmpsb", "cmpsb"},
    {"cmpsd", "cmpsd"},
    {"cmpsw", "cmpsw"},
    // ---- x87 ----
    {"fld", "fld"},
    {"fst", "fst"},
    {"fstp", "fstp"},
    {"fild", "fild"},
    {"fist", "fist"},
    {"fistp", "fistp"},
    {"fisttp", "fisttp"},
    {"fadd", "fadd"},
    {"fsub", "fsub"},
    {"fsubr", "fsubr"},
    {"fmul", "fmul"},
    {"fdiv", "fdiv"},
    {"fdivr", "fdivr"},
    {"faddp", "faddp"},
    {"fsubp", "fsubp"},
    {"fsubrp", "fsubrp"},
    {"fmulp", "fmulp"},
    {"fdivp", "fdivp"},
    {"fdivrp", "fdivrp"},
    {"fiadd", "fiadd"},
    {"fisub", "fisub"},
    {"fisubr", "fisubr"},
    {"fimul", "fimul"},
    {"fidiv", "fidiv"},
    {"fidivr", "fidivr"},
    {"fcom", "fcom"},
    {"fcomp", "fcomp"},
    {"fcompp", "fcompp"},
    {"fucom", "fucom"},
    {"fucomp", "fucomp"},
    {"fucompp", "fucompp"},
    {"fcomi", "fcomi"},
    {"fcomip", "fcomip"},
    {"fucomi", "fucomi"},
    {"fucomip", "fucomip"},
    {"fnstsw", "fnstsw"},
    {"fstsw", "fstsw"},
    {"fldcw", "fldcw"},
    {"fnstcw", "fnstcw"},
    {"fstcw", "fstcw"},
    {"fxch", "fxch"},
    {"ffree", "ffree"},
    {"fld1", "fld1"},
    {"fldz", "fldz"},
    {"fldpi", "fldpi"},
    {"fldl2e", "fldl2e"},
    {"fldl2t", "fldl2t"},
    {"fldlg2", "fldlg2"},
    {"fldln2", "fldln2"},
    {"fchs", "fchs"},
    {"fabs", "fabs"},
    {"fninit", "fninit"},
    {"finit", "finit"},
    {"fclex", "fclex"},
    {"fnclex", "fnclex"},
    {"wait", "wait fwait"},
    {"ftst", "ftst"},
    {"fsqrt", "fsqrt"},
    {"frndint", "frndint"},
    {"fprem", "fprem"},
    {"fprem1", "fprem1"},
    {"fscale", "fscale"},
    {"fsin", "fsin"},
    {"fcos", "fcos"},
    {"fsincos", "fsincos"},
    {"fptan", "fptan"},
    {"fpatan", "fpatan"},
    {"fyl2x", "fyl2x"},
    {"fyl2xp1", "fyl2xp1"},
    {"f2xm1", "f2xm1"},
    {"fxam", "fxam"},
    {"fnop", "fnop"},
    // ---- MMX / SSE（映射有、执行无：M1b）----
    {"movd", "movd"},
    {"movq", "movq"},
    {"pand", "pand"},
    {"pandn", "pandn"},
    {"por", "por"},
    {"pxor", "pxor"},
    {"paddw", "paddw"},
    {"paddusw", "paddusw"},
    {"psubw", "psubw"},
    {"psubsw", "psubsw"},
    {"pmullw", "pmullw"},
    {"pcmpeqw", "pcmpeqw"},
    {"psllw", "psllw"},
    {"psrlw", "psrlw"},
    {"psraw", "psraw"},
    {"psrlq", "psrlq"},
    {"punpcklbw", "punpcklbw"},
    {"packuswb", "packuswb"},
    {"emms", "emms"},
    {"movaps", "movaps"},
    {"movups", "movups"},
    {"movss", "movss"},
    {"movdqu", "movdqu"},
    {"movdqa", "movdqa"},
    {"xorps", "xorps"},
    {"pxor128", "pxor128"},
    {"cvtsi2ss", "cvtsi2ss"},
    {"cvttss2si", "cvttss2si"},
    {"addss", "addss"},
    {"subss", "subss"},
    {"mulss", "mulss"},
    {"divss", "divss"},
    {"comiss", "comiss"},
    {"ucomiss", "ucomiss"},
};

// 不是指令的 token（数据 / 伪指令 / 段 / 结构定义）
bool isPseudoToken(std::string_view t) {
    static const char* kPseudo[] = {
        "db", "dw", "dd", "dq", "dt", "df", "do", "dy", "dz", "text", "unicode", "align", "even",
        "assume", "public", "extrn", "extern", "proc", "endp", "segment", "ends", "org", "include",
        "end", "dup", "struc", "struct", "ends", "equ", "=", "low", "high", "offset", "short",
        "near", "far", "ptr", "byte", "word", "dword", "qword", "tbyte", "type", "record", "label",
        "title", "subtitle", "page", "name", "comment", "group", "rec", "macro", "endm", "local",
        "purge", "if", "ifdef", "ifndef", "else", "endif", "while", "endw", "repeat", "until",
    };
    for (const char* p : kPseudo) {
        if (t == p) return true;
    }
    return false;
}

bool looksLikeIdentifier(std::string_view t) {
    if (t.empty()) return false;
    const char c = t[0];
    const bool start = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '?' || c == '.' ||
                       c == '$' || c == '@';
    if (!start) return false;
    for (char ch : t) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '_' || ch == '?' || ch == '.' || ch == '$' || ch == '@' || ch == ':' || ch == '=';
        if (!ok) return false;
    }
    return true;
}

struct Tables {
    std::unordered_map<std::string, const char*> byAlias;
    std::vector<const char*> canon;
};

std::string toLowerStr(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

const Tables& tables() {
    static const Tables t = [] {
        Tables out;
        for (const Row& r : kRows) {
            out.canon.push_back(r.canonical);
            // 规范名本身也是别名（`decode` 侧拿到的 Zydis 名可能就是规范名）
            out.byAlias.emplace(toLowerStr(r.canonical), r.canonical);
            std::string_view rest = r.aliases;
            while (!rest.empty()) {
                const std::size_t sp = rest.find(' ');
                const std::string_view tok = (sp == std::string_view::npos) ? rest : rest.substr(0, sp);
                if (!tok.empty()) out.byAlias.emplace(std::string(tok), r.canonical);
                if (sp == std::string_view::npos) break;
                rest = rest.substr(sp + 1);
            }
        }
        return out;
    }();
    return t;
}

}  // namespace

const char* canonicalFromIda(std::string_view mnemonic) {
    const std::string key = toLowerStr(mnemonic);
    const auto& t = tables();
    const auto it = t.byAlias.find(key);
    return it == t.byAlias.end() ? nullptr : it->second;
}

const char* canonicalFromZydis(std::string_view mnemonic) { return canonicalFromIda(mnemonic); }

const std::vector<const char*>& canonicalMnemonics() { return tables().canon; }

bool isLstInstructionPrefix(std::string_view token) {
    return token == "rep" || token == "repe" || token == "repz" || token == "repne" || token == "repnz" ||
           token == "lock";
}

LstKind classifyLstLine(std::string_view first, std::string_view second) {
    if (first.empty()) return LstKind::DataOrPseudo;
    // 注释行（`.text:00401000 ;org 401000h` 这类）
    if (first.front() == ';') return LstKind::DataOrPseudo;
    // 多行数据项的**续行**（例：`UnwindMapEntry <-1,` 之后是 `          0, 0, 0>`）：
    // 第一个 token 是纯数字 / 以 `<` `>` 开头结尾的片段 ⇒ 数据。
    if (first.front() >= '0' && first.front() <= '9') return LstKind::DataOrPseudo;
    if (first.front() == '<' || first.back() == '>' || first.front() == '-' || first.front() == '+') {
        return LstKind::DataOrPseudo;
    }
    if (canonicalFromIda(first)) return LstKind::Instruction;
    if (isLstInstructionPrefix(first)) return LstKind::Instruction;  // `rep movsd` / `lock xadd`
    if (isPseudoToken(first)) return LstKind::DataOrPseudo;
    // `arg_0= dword ptr 8` 这类局部变量定义
    if (first.back() == '=' || first.back() == ':') return LstKind::DataOrPseudo;
    if (looksLikeIdentifier(first)) {
        // 带名字的数据项：`a1bit db '…'` / `pji joyinfoex_tag <?>` / `UnwindMapEntry <-1, …>`
        if (!second.empty()) {
            if (!second.empty() && second.front() == '<') return LstKind::DataOrPseudo;
            if (isPseudoToken(second) || canonicalFromIda(second)) return LstKind::DataOrPseudo;
            if (second == "proc" || second == "segment" || second == "struct" || second == "ends") {
                return LstKind::DataOrPseudo;
            }
        }
        // 单独一个标识符（标签 / 结构名）—— IDA 的 .lst 里只可能是定义行
        return LstKind::DataOrPseudo;
    }
    if (!second.empty() && second.front() == '<') return LstKind::DataOrPseudo;
    return LstKind::Unknown;
}

}  // namespace lift
