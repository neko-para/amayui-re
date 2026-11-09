// emulator-lift · 解码器（Zydis → 自有 `Insn`）
//
// ★ Zydis 只**解码**：我们只取"这是什么指令 + 操作数 + 长度"，语义全在 `Op` 上（D2）。
// ★ 指令边界由 Zydis 给的长度决定（A3 就是拿它去对 IDA 的边界，两边独立）。
// ★ 跨页读字节：只读**当前页内**最多 15 字节；若指令需要更多字节 ⇒ 失败并说明
//   （跨到未提交页的指令本来就执行不了；⛔ 不要靠"读越界再补 0"来猜）。
#include "runtime/decode.h"

#include <cstdio>
#include <cstring>

#include <Zydis/Zydis.h>

#include "runtime/exec.h"

namespace lift {
namespace {

// 读最多 15 字节（x86 单条指令的上限）。★ 指令**可以跨页**（实测语料里 143 条正好落在
// `…FFF` 附近），所以不能只看当前页；跨到未提交页才停 —— 那条指令本来就执行不了。
std::size_t readCode(const GuestMemory& mem, std::uint32_t ea, std::uint8_t* buf) {
    const std::uint32_t page_left = 0x1000u - (ea & 0xFFFu);
    if (page_left >= 15u && mem.committed(ea, 15u)) {
        for (std::size_t i = 0; i < 15; ++i) buf[i] = mem.rd8(ea + static_cast<std::uint32_t>(i));
        return 15;
    }
    std::size_t n = 0;
    while (n < 15u) {
        const std::uint32_t a = ea + static_cast<std::uint32_t>(n);
        if (!mem.committed(a, 1)) break;
        buf[n] = mem.rd8(a);
        ++n;
    }
    return n;
}

Reg mapReg(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_EAX: return Reg::EAX;
        case ZYDIS_REGISTER_ECX: return Reg::ECX;
        case ZYDIS_REGISTER_EDX: return Reg::EDX;
        case ZYDIS_REGISTER_EBX: return Reg::EBX;
        case ZYDIS_REGISTER_ESP: return Reg::ESP;
        case ZYDIS_REGISTER_EBP: return Reg::EBP;
        case ZYDIS_REGISTER_ESI: return Reg::ESI;
        case ZYDIS_REGISTER_EDI: return Reg::EDI;
        case ZYDIS_REGISTER_AX: return Reg::AX;
        case ZYDIS_REGISTER_CX: return Reg::CX;
        case ZYDIS_REGISTER_DX: return Reg::DX;
        case ZYDIS_REGISTER_BX: return Reg::BX;
        case ZYDIS_REGISTER_SP: return Reg::SP;
        case ZYDIS_REGISTER_BP: return Reg::BP;
        case ZYDIS_REGISTER_SI: return Reg::SI;
        case ZYDIS_REGISTER_DI: return Reg::DI;
        case ZYDIS_REGISTER_AL: return Reg::AL;
        case ZYDIS_REGISTER_CL: return Reg::CL;
        case ZYDIS_REGISTER_DL: return Reg::DL;
        case ZYDIS_REGISTER_BL: return Reg::BL;
        case ZYDIS_REGISTER_AH: return Reg::AH;
        case ZYDIS_REGISTER_CH: return Reg::CH;
        case ZYDIS_REGISTER_DH: return Reg::DH;
        case ZYDIS_REGISTER_BH: return Reg::BH;
        case ZYDIS_REGISTER_ST0: return Reg::ST0;
        case ZYDIS_REGISTER_ST1: return Reg::ST1;
        case ZYDIS_REGISTER_ST2: return Reg::ST2;
        case ZYDIS_REGISTER_ST3: return Reg::ST3;
        case ZYDIS_REGISTER_ST4: return Reg::ST4;
        case ZYDIS_REGISTER_ST5: return Reg::ST5;
        case ZYDIS_REGISTER_ST6: return Reg::ST6;
        case ZYDIS_REGISTER_ST7: return Reg::ST7;
        // ★ MMX 寄存器**复用 x87 的物理槽**（Intel：两者共用同一组寄存器）⇒ 映射到 ST0..ST7，
        //   语义层用操作数宽度（8 = MMX / 10 = x87）区分。
        case ZYDIS_REGISTER_MM0: return Reg::ST0;
        case ZYDIS_REGISTER_MM1: return Reg::ST1;
        case ZYDIS_REGISTER_MM2: return Reg::ST2;
        case ZYDIS_REGISTER_MM3: return Reg::ST3;
        case ZYDIS_REGISTER_MM4: return Reg::ST4;
        case ZYDIS_REGISTER_MM5: return Reg::ST5;
        case ZYDIS_REGISTER_MM6: return Reg::ST6;
        case ZYDIS_REGISTER_MM7: return Reg::ST7;
        case ZYDIS_REGISTER_ES: return Reg::ES;
        case ZYDIS_REGISTER_CS: return Reg::CS;
        case ZYDIS_REGISTER_SS: return Reg::SS;
        case ZYDIS_REGISTER_DS: return Reg::DS;
        case ZYDIS_REGISTER_FS: return Reg::FS;
        case ZYDIS_REGISTER_GS: return Reg::GS;
        default: return Reg::None;
    }
}

// XMM 不在 `Reg` 枚举里（`regs.h` 是既有接口，S4 不动）⇒ 用 `OpKind::Xmm` + 下标承载。
int xmmIndex(ZydisRegister r) {
    switch (r) {
        case ZYDIS_REGISTER_XMM0: return 0;
        case ZYDIS_REGISTER_XMM1: return 1;
        case ZYDIS_REGISTER_XMM2: return 2;
        case ZYDIS_REGISTER_XMM3: return 3;
        case ZYDIS_REGISTER_XMM4: return 4;
        case ZYDIS_REGISTER_XMM5: return 5;
        case ZYDIS_REGISTER_XMM6: return 6;
        case ZYDIS_REGISTER_XMM7: return 7;
        default: return -1;
    }
}

struct CondRow {
    const char* name;
    Cond cc;
};
const CondRow kConds[] = {
    {"o", Cond::O},   {"no", Cond::NO}, {"b", Cond::B},   {"c", Cond::B},   {"nae", Cond::B},
    {"ae", Cond::AE}, {"nb", Cond::AE}, {"nc", Cond::AE}, {"e", Cond::E},   {"z", Cond::E},
    {"ne", Cond::NE}, {"nz", Cond::NE}, {"be", Cond::BE}, {"na", Cond::BE}, {"a", Cond::A},
    {"nbe", Cond::A}, {"s", Cond::S},   {"ns", Cond::NS}, {"p", Cond::P},   {"pe", Cond::P},
    {"np", Cond::NP}, {"po", Cond::NP}, {"l", Cond::L},   {"nge", Cond::L}, {"ge", Cond::GE},
    {"nl", Cond::GE}, {"le", Cond::LE}, {"ng", Cond::LE}, {"g", Cond::G},   {"nle", Cond::G},
};

Cond condFromSuffix(const char* s) {
    if (!s || !*s) return Cond::None;
    for (const CondRow& r : kConds)
        if (std::strcmp(s, r.name) == 0) return r.cc;
    return Cond::None;
}

struct OpRow {
    const char* name;
    Op op;
};
const OpRow kOpMap[] = {
    {"mov", Op::Mov},     {"movzx", Op::Movzx},   {"movsx", Op::Movsx},   {"lea", Op::Lea},
    {"xchg", Op::Xchg},   {"push", Op::Push},     {"pop", Op::Pop},       {"pushad", Op::Pushad},
    {"popad", Op::Popad}, {"pushfd", Op::Pushfd}, {"popfd", Op::Popfd},   {"xadd", Op::Xadd},
    {"cmpxchg", Op::Cmpxchg},
    {"add", Op::Add},     {"or", Op::Or},         {"and", Op::And},       {"sub", Op::Sub},
    {"xor", Op::Xor},     {"cmp", Op::Cmp},       {"test", Op::Test},     {"inc", Op::Inc},
    {"dec", Op::Dec},     {"neg", Op::Neg},       {"not", Op::Not},       {"adc", Op::Adc},
    {"sbb", Op::Sbb},
    {"shl", Op::Shl},     {"shr", Op::Shr},       {"sar", Op::Sar},       {"rol", Op::Rol},
    {"ror", Op::Ror},     {"rcl", Op::Rcl},       {"rcr", Op::Rcr},       {"shld", Op::Shld},
    {"shrd", Op::Shrd},
    {"mul", Op::Mul},     {"imul", Op::Imul},     {"div", Op::Div},       {"idiv", Op::Idiv},
    {"cdq", Op::Cdq},     {"cwd", Op::Cwd},       {"cwde", Op::Cwde},     {"cbw", Op::Cbw},
    {"bswap", Op::Bswap},
    {"bt", Op::Bt},       {"bts", Op::Bts},       {"btr", Op::Btr},       {"btc", Op::Btc},
    {"bsf", Op::Bsf},     {"bsr", Op::Bsr},
    {"jmp", Op::Jmp},     {"call", Op::Call},     {"ret", Op::Ret},       {"leave", Op::Leave},
    {"nop", Op::Nop},     {"hlt", Op::Hlt},       {"int", Op::Int},       {"int3", Op::Int3},
    {"pause", Op::Pause}, {"cpuid", Op::Cpuid},   {"sahf", Op::Sahf},     {"lahf", Op::Lahf},
    {"movsb", Op::Movs},  {"movsw", Op::Movs},    {"movsd", Op::Movs},    {"stosb", Op::Stos},
    {"stosw", Op::Stos},  {"stosd", Op::Stos},    {"lodsb", Op::Lods},    {"lodsw", Op::Lods},
    {"lodsd", Op::Lods},  {"scasb", Op::Scas},    {"scasw", Op::Scas},    {"scasd", Op::Scas},
    {"cmpsb", Op::Cmps},  {"cmpsw", Op::Cmps},    {"cmpsd", Op::Cmps},
    // x87
    {"fld", Op::Fld},     {"fst", Op::Fst},       {"fstp", Op::Fstp},     {"fild", Op::Fild},
    {"fist", Op::Fist},   {"fistp", Op::Fistp},   {"fadd", Op::Fadd},     {"fsub", Op::Fsub},
    {"fsubr", Op::Fsubr}, {"fmul", Op::Fmul},     {"fdiv", Op::Fdiv},     {"fdivr", Op::Fdivr},
    {"faddp", Op::Faddp}, {"fsubp", Op::Fsubp},   {"fsubrp", Op::Fsubrp}, {"fmulp", Op::Fmulp},
    {"fdivp", Op::Fdivp}, {"fdivrp", Op::Fdivrp}, {"fiadd", Op::Fiadd},   {"fisub", Op::Fisub},
    {"fisubr", Op::Fisubr},{"fimul", Op::Fimul},  {"fidiv", Op::Fidiv},   {"fidivr", Op::Fidivr},
    {"fcom", Op::Fcom},   {"fcomp", Op::Fcomp},   {"fcompp", Op::Fcompp}, {"fucom", Op::Fucom},
    {"fucomp", Op::Fucomp},{"fucompp", Op::Fucompp},{"fnstsw", Op::Fnstsw},{"fstsw", Op::Fnstsw},
    {"fldcw", Op::Fldcw}, {"fnstcw", Op::Fnstcw}, {"fstcw", Op::Fnstcw},  {"fxch", Op::Fxch},
    {"ffree", Op::Ffree}, {"fld1", Op::Fld1},     {"fldz", Op::Fldz},     {"fchs", Op::Fchs},
    {"fabs", Op::Fabs},   {"fninit", Op::Fninit}, {"finit", Op::Fninit},  {"wait", Op::Fwait},
    {"fwait", Op::Fwait}, {"ftst", Op::Ftst},     {"fsqrt", Op::Fsqrt},   {"frndint", Op::Frndint},
    {"fprem", Op::Fprem}, {"fscale", Op::Fscale}, {"fsin", Op::Fsin},     {"fcos", Op::Fcos},
    {"fptan", Op::Fptan}, {"fpatan", Op::Fpatan}, {"fyl2x", Op::Fyl2x},   {"fldln2", Op::Fldln2},
    {"fldl2e", Op::Fldl2e},{"fldlg2", Op::Fldlg2},{"fldl2t", Op::Fldl2t}, {"fldpi", Op::Fldpi},
    {"f2xm1", Op::F2xm1},
    // ---- x87（S4 补齐：语料里 47 个形态之外，指令集里常见的那几个也登记成本）----
    {"fisttp", Op::Fisttp},
    {"fcomi", Op::Fcomi},   {"fcomip", Op::Fcomip},   {"fucomi", Op::Fucomi}, {"fucomip", Op::Fucomip},
    {"fcmovb", Op::Fcmovb}, {"fcmove", Op::Fcmove},   {"fcmovbe", Op::Fcmovbe}, {"fcmovu", Op::Fcmovu},
    {"fcmovnb", Op::Fcmovnb}, {"fcmovne", Op::Fcmovne}, {"fcmovnbe", Op::Fcmovnbe}, {"fcmovnu", Op::Fcmovnu},
    {"fldenv", Op::Fldenv}, {"fnstenv", Op::Fnstenv}, {"fnsave", Op::Fnsave}, {"frstor", Op::Frstor},
    {"fdecstp", Op::Fdecstp}, {"fincstp", Op::Fincstp}, {"fnop", Op::Fnop},
    {"fclex", Op::Fclex},   {"fnclex", Op::Fclex},
    {"fprem1", Op::Fprem1}, {"fyl2xp1", Op::Fyl2xp1}, {"fsincos", Op::Fsincos},
    {"fxam", Op::Fxam},     {"fxtract", Op::Fxtract},
};

// ---- MMX / SSE 的整数打包（`Op::Packed` + `PkOp` 子码）----
// ★ 只登记**语料里真的出现过**的 MMX 形态 + 同族的整数 SSE2 形态；浮点 SSE 是**另一族**
//   （`Op::Sse` + `kSseMap`，见下），⛔ 不要把两者塞进同一个子码表。
struct PkRow {
    const char* name;
    PkOp pk;
};
const PkRow kPkMap[] = {
    {"movd", PkOp::Movd}, {"movq", PkOp::Movq},
    {"pand", PkOp::Pand}, {"pandn", PkOp::Pandn}, {"por", PkOp::Por}, {"pxor", PkOp::Pxor},
    {"paddb", PkOp::PaddB}, {"paddw", PkOp::PaddW}, {"paddd", PkOp::PaddD}, {"paddq", PkOp::PaddQ},
    {"paddsb", PkOp::PaddSb}, {"paddsw", PkOp::PaddSw}, {"paddusb", PkOp::PaddUsb}, {"paddusw", PkOp::PaddUsw},
    {"psubb", PkOp::PsubB}, {"psubw", PkOp::PsubW}, {"psubd", PkOp::PsubD}, {"psubq", PkOp::PsubQ},
    {"psubsb", PkOp::PsubSb}, {"psubsw", PkOp::PsubSw}, {"psubusb", PkOp::PsubUsb}, {"psubusw", PkOp::PsubUsw},
    {"pcmpeqb", PkOp::PcmpeqB}, {"pcmpeqw", PkOp::PcmpeqW}, {"pcmpeqd", PkOp::PcmpeqD},
    {"pcmpgtb", PkOp::PcmpgtB}, {"pcmpgtw", PkOp::PcmpgtW}, {"pcmpgtd", PkOp::PcmpgtD},
    {"punpcklbw", PkOp::Punpcklbw}, {"punpcklwd", PkOp::Punpcklwd}, {"punpckldq", PkOp::Punpckldq},
    {"punpcklqdq", PkOp::Punpcklqdq}, {"punpckhbw", PkOp::Punpckhbw}, {"punpckhwd", PkOp::Punpckhwd},
    {"punpckhdq", PkOp::Punpckhdq}, {"punpckhqdq", PkOp::Punpckhqdq},
    {"packsswb", PkOp::Packsswb}, {"packssdw", PkOp::Packssdw}, {"packuswb", PkOp::Packuswb},
    {"pmullw", PkOp::Pmullw}, {"pmulhw", PkOp::Pmulhw}, {"pmulhuw", PkOp::Pmulhuw},
    {"pmuludq", PkOp::Pmuludq}, {"pmaddwd", PkOp::Pmaddwd},
    {"psllw", PkOp::Psllw}, {"pslld", PkOp::Pslld}, {"psllq", PkOp::Psllq},
    {"psrlw", PkOp::Psrlw}, {"psrld", PkOp::Psrld}, {"psrlq", PkOp::Psrlq},
    {"psraw", PkOp::Psraw}, {"psrad", PkOp::Psrad},
    {"emms", PkOp::Ems},
};

PkOp pkFromMnemonic(const char* name) {
    if (!name) return PkOp::None;
    for (const PkRow& r : kPkMap)
        if (std::strcmp(name, r.name) == 0) return r.pk;
    return PkOp::None;
}

// ---- SSE 标量单精度 + 128 位搬移 / 位运算（`Op::Sse` + `SseOp` 子码）----
// ★ 这 14 条在 `.lst` 里都是 0 条（判据：全文件 `xmm<数字>` 操作数 = 0），但 A2 的别名表登记了
//   它们；真撞到时按 Intel SDM 执行，而不是落到 `Op::Other` 响亮失败。
// ★ `movsd`（F2 0F 10，标量 double）**没有**登记：它和串操作 `movsd`（A5）共用同一个 Zydis 助记符，
//   归一化后会落到 `Op::Movs` —— 那是既有口径，本单不碰（见 S4 审计 §1：语料里 `movsd` 全是串操作）。
struct SseRow {
    const char* name;
    SseOp sse;
};
const SseRow kSseMap[] = {
    {"movaps", SseOp::Movaps},   {"movups", SseOp::Movups},   {"movss", SseOp::Movss},
    {"movdqa", SseOp::Movdqa},   {"movdqu", SseOp::Movdqu},
    {"xorps", SseOp::Xorps},
    {"cvtsi2ss", SseOp::Cvtsi2ss}, {"cvttss2si", SseOp::Cvttss2si},
    {"addss", SseOp::Addss},     {"subss", SseOp::Subss},     {"mulss", SseOp::Mulss},
    {"divss", SseOp::Divss},
    {"comiss", SseOp::Comiss},   {"ucomiss", SseOp::Ucomiss},
};

SseOp sseFromMnemonic(const char* name) {
    if (!name) return SseOp::None;
    for (const SseRow& r : kSseMap)
        if (std::strcmp(name, r.name) == 0) return r.sse;
    return SseOp::None;
}

// `rep` / `lock` 前缀后的规范名（A3 要跟 `.lst` 的 `rep movsd` / `lock xadd` 对上）
const char* prefixedName(const char* base, std::uint8_t rep, bool lock) {
    if (lock) {
        if (std::strcmp(base, "xadd") == 0) return "lock xadd";
        if (std::strcmp(base, "cmpxchg") == 0) return "lock cmpxchg";
        if (std::strcmp(base, "inc") == 0) return "lock inc";
        if (std::strcmp(base, "dec") == 0) return "lock dec";
        if (std::strcmp(base, "add") == 0) return "lock add";
        return "lock";
    }
    if (!rep) return base;
    if (std::strcmp(base, "movsd") == 0) return "rep movsd";
    if (std::strcmp(base, "movsw") == 0) return "rep movsw";
    if (std::strcmp(base, "movsb") == 0) return "rep movsb";
    if (std::strcmp(base, "stosd") == 0) return "rep stosd";
    if (std::strcmp(base, "stosw") == 0) return "rep stosw";
    if (std::strcmp(base, "stosb") == 0) return "rep stosb";
    if (std::strcmp(base, "lodsd") == 0) return "rep lodsd";
    if (std::strcmp(base, "lodsw") == 0) return "rep lodsw";
    if (std::strcmp(base, "lodsb") == 0) return "rep lodsb";
    if (std::strcmp(base, "scasd") == 0) return "rep scasd";
    if (std::strcmp(base, "scasw") == 0) return "rep scasw";
    if (std::strcmp(base, "scasb") == 0) return "rep scasb";
    if (std::strcmp(base, "cmpsd") == 0) return "rep cmpsd";
    if (std::strcmp(base, "cmpsw") == 0) return "rep cmpsw";
    if (std::strcmp(base, "cmpsb") == 0) return "rep cmpsb";
    // `F3 C3` 是 CRT 里的 "rep ret" 惯用法（IDA 也原样写 `rep retn`）
    if (std::strcmp(base, "ret") == 0) return "rep ret";
    return base;
}

Op opFromCanonical(const char* canon) {
    if (!canon) return Op::Other;
    // jcc / setcc / cmovcc 是家族：先按前缀认，再按后缀取条件码
    if (canon[0] == 'j' && canon[1] != '\0') {
        const Cond cc = condFromSuffix(canon + 1);
        if (cc != Cond::None) return Op::Jcc;
    }
    if (std::strncmp(canon, "set", 3) == 0 && condFromSuffix(canon + 3) != Cond::None) return Op::Setcc;
    if (std::strncmp(canon, "cmov", 4) == 0 && condFromSuffix(canon + 4) != Cond::None) return Op::Cmovcc;
    for (const OpRow& r : kOpMap)
        if (std::strcmp(canon, r.name) == 0) return r.op;
    return Op::Other;
}

}  // namespace

struct DecoderImpl {
    ZydisDecoder decoder{};
    ZydisFormatter formatter{};
    bool ready = false;
};

Decoder::Decoder() : impl_(std::make_unique<DecoderImpl>()) {
    const ZyanStatus st =
        ZydisDecoderInit(&impl_->decoder, ZYDIS_MACHINE_MODE_LONG_COMPAT_32, ZYDIS_STACK_WIDTH_32);
    if (!ZYAN_SUCCESS(st)) {
        std::snprintf(last_error_, sizeof(last_error_), "ZydisDecoderInit 失败: 0x%08X", static_cast<unsigned>(st));
        return;
    }
    const ZyanStatus fst = ZydisFormatterInit(&impl_->formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    if (!ZYAN_SUCCESS(fst)) {
        std::snprintf(last_error_, sizeof(last_error_), "ZydisFormatterInit 失败: 0x%08X", static_cast<unsigned>(fst));
        return;
    }
    impl_->ready = true;
    ok_ = true;
}

Decoder::~Decoder() = default;

bool Decoder::decode(const GuestMemory& mem, std::uint32_t ea, Insn* out, std::string* why) const {
    if (!ok_) {
        if (why) *why = std::string("解码器未初始化：") + last_error_;
        return false;
    }
    std::uint8_t buf[15] = {};
    const std::size_t n = readCode(mem, ea, buf);

    ZydisDecodedInstruction zi{};
    ZydisDecodedOperand zops[ZYDIS_MAX_OPERAND_COUNT] = {};
    const ZyanStatus st = ZydisDecoderDecodeFull(&impl_->decoder, buf, n, &zi, zops);
    if (!ZYAN_SUCCESS(st)) {
        if (why) {
            char tmp[128];
            std::snprintf(tmp, sizeof(tmp), "解码失败 status=0x%08X @ %08X 字节=",
                          static_cast<unsigned>(st), ea);
            *why = tmp;
            for (std::size_t i = 0; i < n; ++i) {
                char b[8];
                std::snprintf(b, sizeof(b), "%02X ", buf[i]);
                *why += b;
            }
        }
        return false;
    }

    Insn& ins = *out;
    ins = Insn{};
    ins.ea = ea;
    ins.length = zi.length;
    ins.zydis_mnemonic = static_cast<std::uint16_t>(zi.mnemonic);
    ins.opsz = static_cast<std::uint8_t>(zi.operand_width / 8);
    if (ins.opsz != 1 && ins.opsz != 2 && ins.opsz != 4) ins.opsz = 4;
    ins.operand16 = (zi.operand_width == 16);
    ins.rep = (zi.attributes & ZYDIS_ATTRIB_HAS_REP) ? 0xF3 : ((zi.attributes & ZYDIS_ATTRIB_HAS_REPNE) ? 0xF2 : 0);
    ins.lock = (zi.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0;

    const char* raw = ZydisMnemonicGetString(zi.mnemonic);
    if (!raw) raw = "?";
    const char* canon = canonicalFromZydis(raw);
    // ★ 别名表里有就用规范名；没有就退回 Zydis 的原名（`fcmovb` 一族 / 部分 MMX 名没进别名表，
    //   但 Zydis 给的名字本身就是 Intel 规范名）。A2 的口径不变：`.lst` 里的助记符必须全在别名表。
    const char* key = canon ? canon : raw;
    ins.mnemonic = prefixedName(key, ins.rep, ins.lock);
    const PkOp pk = pkFromMnemonic(key);
    if (pk != PkOp::None) {
        ins.op = Op::Packed;
        ins.pk = pk;
    } else {
        const SseOp sse = sseFromMnemonic(key);
        if (sse != SseOp::None) {
            ins.op = Op::Sse;
            ins.sse = sse;
        } else {
            ins.op = opFromCanonical(key);
        }
    }
    // `F3 C3`（rep ret）：Zydis 把 F3 当"对 RET 无意义的前缀"吃掉、不置 REP 属性，
    // 所以按**字节**补一次（IDA 写 `rep retn`）。
    if (buf[0] == 0xF3 && ins.op == Op::Ret) ins.mnemonic = "rep ret";
    if (ins.op == Op::Jcc) ins.cc = condFromSuffix(key + 1);
    if (ins.op == Op::Setcc) ins.cc = condFromSuffix(key + 3);
    if (ins.op == Op::Cmovcc) ins.cc = condFromSuffix(key + 4);

    const std::uint8_t visible = zi.operand_count_visible;
    for (std::uint8_t i = 0; i < visible && i < 3; ++i) {
        const ZydisDecodedOperand& zo = zops[i];
        Operand& op = ins.ops[i];
        op.size = static_cast<std::uint8_t>(zo.size / 8);
        switch (zo.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER: {
                const int xi = xmmIndex(zo.reg.value);
                if (xi >= 0) {
                    op.kind = OpKind::Xmm;
                    op.vec = static_cast<std::uint8_t>(xi);
                } else {
                    op.kind = OpKind::Reg;
                    op.reg = mapReg(zo.reg.value);
                }
                break;
            }
            case ZYDIS_OPERAND_TYPE_MEMORY: {
                op.kind = OpKind::Mem;
                op.mem.size = static_cast<std::uint8_t>(zo.size / 8);
                op.mem.base = mapReg(zo.mem.base);
                op.mem.index = mapReg(zo.mem.index);
                op.mem.scale = zo.mem.scale ? zo.mem.scale : 1;
                op.mem.seg_fs = (zo.mem.segment == ZYDIS_REGISTER_FS);
                op.mem.seg_gs = (zo.mem.segment == ZYDIS_REGISTER_GS);
                if (zo.mem.disp.has_displacement) op.mem.disp = static_cast<std::uint32_t>(zo.mem.disp.value);
                op.mem.absolute = (op.mem.base == Reg::None && op.mem.index == Reg::None);
                if (op.mem.absolute) op.mem.disp = static_cast<std::uint32_t>(zo.mem.disp.value);
                break;
            }
            case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
                op.kind = OpKind::Imm;
                // ★ 立即数的 `zo.size` 是**编码宽度**（`83 F8 FF` 的 imm8 = 8 位），而语义层要的是
                //   **有效操作数宽度**（32 位，且 -1 要符号扩展）。照编码宽度截断会把
                //   `cmp eax,-1` 变成 `cmp eax,0xFF` —— 实测正是这个 bug 让 CRT 的
                //   `__crtGetSystemTimeAsFileTime` 惰性解析器走错分支、`call eax` 到 0。
                op.size = ins.opsz;
                std::uint64_t v = zo.imm.is_signed ? static_cast<std::uint64_t>(static_cast<std::int64_t>(zo.imm.value.s))
                                                   : zo.imm.value.u;
                if (ins.opsz == 1) v &= 0xFFull;
                else if (ins.opsz == 2) v &= 0xFFFFull;
                else v &= 0xFFFFFFFFull;
                op.imm = v;
                break;
            }
            default:
                op.kind = OpKind::None;
                break;
        }
    }

    // ★ `9B`（WAIT）作为**前缀**：`FSTSW` / `FSTCW` / `FINIT` 在机器码里是 `9B` + 对应的 `FN*` 指令，
    //   而 IDA 的 `.lst` 把它们写成**一行**（`fstsw ax`，3 字节）。不合并就会在 A3 里
    //   既错长度又错助记符 —— 那是语料的口径，不是我们的解码器错。
    //   合并后的语义与 `FN*` 完全一样（WAIT 只是等挂起的 x87 异常）。
    if (buf[0] == 0x9B && n >= 2) {
        Insn second;
        std::string why2;
        if (decode(mem, ea + 1u, &second, &why2)) {
            const char* merged = nullptr;
            switch (second.op) {
                case Op::Fnstsw: merged = "fstsw"; break;
                case Op::Fnstcw: merged = "fstcw"; break;
                case Op::Fninit: merged = "finit"; break;
                case Op::Fnstenv: merged = "fstenv"; break;
                case Op::Fnsave: merged = "fsave"; break;
                case Op::Fclex: merged = "fclex"; break;
                default: break;
            }
            if (merged) {
                const std::uint8_t total = static_cast<std::uint8_t>(1u + second.length);
                ins = second;
                ins.ea = ea;
                ins.length = total;
                ins.mnemonic = merged;
            }
        }
    }
    return true;
}

bool Decoder::decodeBlock(const GuestMemory& mem, std::uint32_t ea, Block* out, std::string* why) const {
    out->ea = ea;
    out->insns.clear();
    out->terminated = false;
    std::uint32_t cursor = ea;
    for (int guard = 0; guard < 256; ++guard) {
        Insn in;
        if (!decode(mem, cursor, &in, why)) return false;
        if (in.length == 0) {
            if (why) *why = "解码得到长度 0";
            return false;
        }
        const bool control = opIsControl(in.op);
        out->insns.push_back(in);
        cursor += in.length;
        if (control) {
            out->terminated = true;
            return true;
        }
    }
    // ★ 上限不是"错误"，只是**一次解码多少**的分片边界：语料里存在 554 条无分支的单块
    //   （`0x415DF6..0x417359` 的 vtable 初始化）。旧实现到 256 条就 `return false`
    //   ⇒ 运行循环拿到 nullptr ⇒ 误报 DecodeError（detail 还误诊成"落在数据里"）。
    //   返回**部分块**（terminated=false）：run 执行完这 256 条后会从 eip 继续取下一片
    //   （machine.cpp 里 `left_block == false` 的续跑路径）。
    return true;
}

std::string Decoder::text(const GuestMemory& mem, std::uint32_t ea, std::uint8_t* length) const {
    if (!ok_) return {};
    std::uint8_t buf[15] = {};
    const std::size_t n = readCode(mem, ea, buf);
    ZydisDecodedInstruction zi{};
    ZydisDecodedOperand zops[ZYDIS_MAX_OPERAND_COUNT] = {};
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&impl_->decoder, buf, n, &zi, zops))) return "?";
    char text[128] = {};
    ZydisFormatterFormatInstruction(&impl_->formatter, &zi, zops, zi.operand_count_visible, text, sizeof(text),
                                  static_cast<ZyanU64>(ea), nullptr);
    if (length) *length = zi.length;
    return text;
}

}  // namespace lift
