// emulator-lift · 指令语义夹具（判据 A1 的用例表）
//
// ★ 每个用例 = `setup`（放**自造**机器码字节 + 置初值）+ `verify`（断言寄存器 / 标志 / 内存）。
//   runner 会机械地要求两件事（见 conformance.cpp）：verify 里至少一次断言、且夹具前后状态必须不同。
//   ⇒ 恒等夹具（expected 抄成 actual / 代码根本没执行）会当场红。
// ★ 覆盖：mov / movzx / movsx / lea / push / pop / add / adc / sub / sbb / cmp / test / and / or / xor /
//   inc / dec / neg / not / shl / shr / sar / rol / ror / imul / mul / div / idiv / cdq / cwde /
//   xchg / bswap / setcc / jcc / leave / rep stosd / rep movsd / call-ret，以及 x87 的
//   fld / fstp / fild / fistp / fadd / fcom+fnstsw（M1a 的最小 x87 子集）。
// ★ 增补的用例在文件中部「增补（判据 A1 加厚）」小节：标志边界 / 半进位 / 进位链 / 移位计数 /
//   三操作数 imul / mul·div 位宽 / setcc·jcc·cmovcc 多条件 / bt·bts·btr·btc / bsf·bsr /
//   xchg 内存 / movzx·movsx 16→32 / lea index*scale / 串操作 / x87 的 fsub·fdiv·fmul·
//   fcompp·fucompp·fldcw·fild·fistp·fchs·fabs·fsqrt·frndint·ffree。
#include "harness.h"

namespace lift_test {
namespace {

// ---------- 数据搬运 ----------
void setup_mov_imm(Fixture& f) { f.codeHex(kCodeEa, "B8 78 56 34 12 C3"); }
void verify_mov_imm(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x12345678u); }

void setup_mov_reg(Fixture& f) {
    f.cpu().ebx() = 0xDEADBEEFu;
    f.codeHex(kCodeEa, "89 D8 C3");  // mov eax, ebx
}
void verify_mov_reg(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xDEADBEEFu); }

void setup_mov_mem_store(Fixture& f) {
    f.dataHex(kDataEa, "00000000");
    f.cpu().eax() = 0x12345678u;
    f.codeHex(kCodeEa, "A2 00 00 48 00 C3");  // mov [0x480000], al
}
void verify_mov_mem_store(Fixture& f) { LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa), 0x00000078u); }

void setup_mov_mem_load(Fixture& f) {
    f.dataHex(kDataEa, "EF BE AD DE");
    f.codeHex(kCodeEa, "8B 05 00 00 48 00 C3");  // mov eax, [0x480000]
}
void verify_mov_mem_load(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xDEADBEEFu); }

void setup_movzx(Fixture& f) {
    f.cpu().ebx() = 0x1234FF80u;
    f.codeHex(kCodeEa, "0F B6 C3 C3");  // movzx eax, bl
}
void verify_movzx(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000080u); }

void setup_movsx(Fixture& f) {
    f.cpu().ebx() = 0x00000080u;
    f.codeHex(kCodeEa, "0F BE C3 C3");  // movsx eax, bl
}
void verify_movsx(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFF80u); }

void setup_lea(Fixture& f) { f.codeHex(kCodeEa, "8D 44 24 08 C3"); }  // lea eax,[esp+8]
void verify_lea(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), f.m().layout().stack_top + 4u); }  // lea 时的 esp = stack_top-4

void setup_xchg(Fixture& f) {
    f.cpu().eax() = 0x11111111u;
    f.cpu().ebx() = 0x22222222u;
    f.codeHex(kCodeEa, "93 C3");  // xchg eax, ebx
}
void verify_xchg(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x22222222u);
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 0x11111111u);
}

void setup_bswap(Fixture& f) {
    f.cpu().eax() = 0x11223344u;
    f.codeHex(kCodeEa, "0F C8 C3");  // bswap eax
}
void verify_bswap(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x44332211u); }

// ---------- 栈 ----------
void setup_push_pop(Fixture& f) { f.codeHex(kCodeEa, "68 78 56 34 12 58 C3"); }  // push imm; pop eax
void verify_push_pop(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x12345678u);
    LIFT_CHECK_EQ_U32(f.cpu().esp(), f.m().layout().stack_top);  // 栈平衡（最后的 ret 弹掉了哨兵）
}

void setup_leave(Fixture& f) {
    // esp = stack_top-4（哨兵）；构造一个帧：ebp = esp；[ebp] 放一个"上一帧"
    f.codeHex(kCodeEa, "55 89 E5 C9 C3");  // push ebp; mov ebp,esp; leave; ret
}
void verify_leave(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eip, kSentinelEa);
    LIFT_CHECK_EQ_U32(f.cpu().esp(), f.m().layout().stack_top);
}

// ---------- 算术 / 逻辑 / 标志 ----------
void setup_add_overflow(Fixture& f) {
    f.cpu().eax() = 0x7FFFFFFFu;
    f.codeHex(kCodeEa, "05 01 00 00 00 C3");  // add eax,1
}
void verify_add_overflow(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x80000000u);
    LIFT_CHECK(f.cpu().of());   // 有符号溢出
    LIFT_CHECK(!f.cpu().cf());  // 无符号不进位
    LIFT_CHECK(f.cpu().sf());
}

void setup_add_carry(Fixture& f) {
    f.cpu().eax() = 0xFFFFFFFFu;
    f.codeHex(kCodeEa, "83 C0 01 C3");  // add eax,1
}
void verify_add_carry(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().zf());
}

void setup_adc(Fixture& f) {
    f.cpu().eax() = 0xFFFFFFFFu;
    f.cpu().ebx() = 0xFFFFFFFFu;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "83 D3 00 C3");  // adc ebx,0  （0xFFFFFFFF + 0 + CF(1) 溢出）
}
void verify_adc(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 0u);
    LIFT_CHECK(f.cpu().cf());
}

void setup_sub_borrow(Fixture& f) {
    f.cpu().eax() = 0u;
    f.codeHex(kCodeEa, "83 E8 01 C3");  // sub eax,1
}
void verify_sub_borrow(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(f.cpu().cf());  // 借位 = 无符号下溢
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
}

void setup_sbb(Fixture& f) {
    f.cpu().eax() = 5u;
    f.cpu().ebx() = 1u;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "83 D8 00 C3");  // sbb eax,0
}
void verify_sbb(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 4u);
    LIFT_CHECK(!f.cpu().cf());
}

void setup_cmp_test(Fixture& f) {
    f.cpu().eax() = 5u;
    f.codeHex(kCodeEa, "83 F8 05 C3");  // cmp eax,5
}
void verify_cmp_test(Fixture& f) {
    LIFT_CHECK(f.cpu().zf());              // 相等 ⇒ ZF=1
    LIFT_CHECK(!f.cpu().cf());
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 5u);  // cmp 不改操作数
}

void setup_logic(Fixture& f) {
    f.cpu().eax() = 0xF0F0F0F0u;
    f.cpu().ebx() = 0x0FF00FF0u;
    f.codeHex(kCodeEa, "21 D8 09 D8 C3");  // and eax,ebx; or eax,ebx
}
void verify_logic(Fixture& f) {
    // (a & b) | b == a | b
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x0FF00FF0u);  // (a & b) | b == b
    LIFT_CHECK(f.cpu().zf() == false);
}

void setup_inc_dec(Fixture& f) {
    // inc/dec 必须**不动 CF**（语料里的 `dec` + `sbb eax,eax` 组合依赖这一点）
    f.cpu().eax() = 0u;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "40 48 C3");  // inc eax; dec eax
}
void verify_inc_dec(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().cf());  // 被保留
    LIFT_CHECK(f.cpu().zf());
}

void setup_neg_not(Fixture& f) {
    f.cpu().eax() = 5u;
    f.codeHex(kCodeEa, "F7 D8 F7 D0 C3");  // neg eax; not eax
}
void verify_neg_not(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 4u);  // ~(-5) = ~0xFFFFFFFB = 4
    LIFT_CHECK(f.cpu().cf());              // neg 的 CF = (a != 0)；not 不改标志
}

// ---------- 移位 ----------
void setup_shl(Fixture& f) {
    f.cpu().eax() = 1u;
    f.codeHex(kCodeEa, "C1 E0 04 C3");  // shl eax,4
}
void verify_shl(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 16u); }

void setup_shr(Fixture& f) {
    f.cpu().eax() = 0x80000000u;
    f.codeHex(kCodeEa, "C1 E8 1F C3");  // shr eax,31
}
void verify_shr(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 1u);
    LIFT_CHECK(!f.cpu().cf());  // CF = 最后被移出的那一位 = 原值的 bit30 = 0
}

void setup_sar(Fixture& f) {
    f.cpu().eax() = 0xFFFFFFF8u;
    f.codeHex(kCodeEa, "C1 F8 04 C3");  // sar eax,4
}
void verify_sar(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(f.cpu().cf());
}

void setup_rol(Fixture& f) {
    f.cpu().eax() = 0x80000001u;
    f.codeHex(kCodeEa, "C1 C0 01 C3");  // rol eax,1
}
void verify_rol(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000003u); }

void setup_ror(Fixture& f) {
    f.cpu().eax() = 0x00000001u;
    f.codeHex(kCodeEa, "C1 C8 01 C3");  // ror eax,1
}
void verify_ror(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x80000000u); }

void setup_shift_by_cl(Fixture& f) {
    f.cpu().eax() = 3u;
    f.cpu().ecx() = 3u;
    f.codeHex(kCodeEa, "D3 E0 C3");  // shl eax,cl
}
void verify_shift_by_cl(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 24u); }

// ---------- 乘除 ----------
void setup_imul_two(Fixture& f) {
    f.cpu().eax() = 7u;
    f.cpu().ecx() = 6u;
    f.codeHex(kCodeEa, "0F AF C1 C3");  // imul eax,ecx
}
void verify_imul_two(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 42u); }

void setup_imul_signed(Fixture& f) {
    f.cpu().eax() = 0xFFFFFFFEu;  // -2
    f.cpu().ecx() = 5u;
    f.codeHex(kCodeEa, "0F AF C1 C3");  // imul eax,ecx
}
void verify_imul_signed(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFF6u); }  // -10

void setup_mul_full(Fixture& f) {
    f.cpu().eax() = 0x00010000u;
    f.cpu().ecx() = 0x00010000u;
    f.codeHex(kCodeEa, "F7 E1 C3");  // mul ecx
}
void verify_mul_full(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK_EQ_U32(f.cpu().edx(), 1u);
    LIFT_CHECK(f.cpu().cf());
}

void setup_div(Fixture& f) {
    f.cpu().eax() = 100u;
    f.cpu().edx() = 0u;
    f.cpu().ecx() = 7u;
    f.codeHex(kCodeEa, "F7 F1 C3");  // div ecx
}
void verify_div(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 14u);
    LIFT_CHECK_EQ_U32(f.cpu().edx(), 2u);
}

void setup_idiv(Fixture& f) {
    f.cpu().eax() = static_cast<std::uint32_t>(-100);
    f.cpu().edx() = 0xFFFFFFFFu;
    f.cpu().ecx() = 7u;
    f.codeHex(kCodeEa, "F7 F9 C3");  // idiv ecx
}
void verify_idiv(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), static_cast<std::uint32_t>(-14));
    LIFT_CHECK_EQ_U32(f.cpu().edx(), static_cast<std::uint32_t>(-2));
}

void setup_cdq(Fixture& f) {
    f.cpu().eax() = 0x80000000u;
    f.codeHex(kCodeEa, "99 C3");  // cdq
}
void verify_cdq(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().edx(), 0xFFFFFFFFu); }

void setup_cwde(Fixture& f) {
    f.cpu().eax() = 0x12348000u;
    f.codeHex(kCodeEa, "98 C3");  // cwde
}
void verify_cwde(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFF8000u); }

// ---------- 条件 ----------
void setup_setcc(Fixture& f) {
    f.cpu().eax() = 0u;
    f.codeHex(kCodeEa, "85 C0 0F 94 C0 C3");  // test eax,eax; sete al
}
void verify_setcc(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax() & 0xFFu, 1u);
    LIFT_CHECK(f.cpu().zf());
}

void setup_jcc_taken(Fixture& f) {
    f.cpu().eax() = 5u;
    // cmp eax,5; jne +5(false 分支); mov ebx,1; jmp +5; mov ebx,2; ret
    f.codeHex(kCodeEa, "83 F8 05 75 07 BB 01 00 00 00 EB 05 BB 02 00 00 00 C3");
}
void verify_jcc_taken(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 1u);
    LIFT_CHECK_EQ_U32(f.cpu().eip, kSentinelEa);
}

void setup_jcc_not_taken(Fixture& f) {
    f.cpu().eax() = 4u;
    f.codeHex(kCodeEa, "83 F8 05 75 07 BB 01 00 00 00 EB 05 BB 02 00 00 00 C3");
}
void verify_jcc_not_taken(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().ebx(), 2u); }

// ---------- 串操作 ----------
void setup_rep_stosd(Fixture& f) {
    f.dataHex(kDataEa, "00000000000000000000000000000000");
    f.cpu().edi() = kDataEa;
    f.cpu().ecx() = 3u;
    f.cpu().eax() = 0xAABBCCDDu;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "F3 AB C3");  // rep stosd
}
void verify_rep_stosd(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa), 0xAABBCCDDu);
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 8), 0xAABBCCDDu);
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 0u);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), kDataEa + 12u);
}

void setup_rep_movsd(Fixture& f) {
    f.dataHex(kDataEa, "1122334455667788");
    f.dataHex(kDataEa + 0x100u, "0000000000000000");
    f.cpu().esi() = kDataEa;
    f.cpu().edi() = kDataEa + 0x100u;
    f.cpu().ecx() = 2u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "F3 A5 C3");  // rep movsd
}
void verify_rep_movsd(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x100u), 0x44332211u);
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x104u), 0x88776655u);
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 0u);
    LIFT_CHECK_EQ_U32(f.cpu().esi(), kDataEa + 8u);
}

// ---------- x87 最小子集 ----------
void setup_fld_fstp(Fixture& f) {
    f.dataHex(kDataEa, "0000C03F");       // float 1.5
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 1D 10 00 48 00 C3");  // fld [0x480000]; fstp [0x480010]
}
void verify_fld_fstp(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x3FC00000u);
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);  // fstp 弹回去了
}

void setup_fild_fistp(Fixture& f) {
    f.dataHex(kDataEa, "07000000");       // int32 7
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "DB 05 00 00 48 00 DB 1D 10 00 48 00 C3");  // fild dword; fistp dword
}
void verify_fild_fistp(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 7u);
}

void setup_fadd(Fixture& f) {
    f.dataHex(kDataEa, "0000C03F");       // 1.5f
    f.dataHex(kDataEa + 0x10u, "00002040");  // 2.5f
    f.dataHex(kDataEa + 0x20u, "00000000");
    // fld [0x480000]; fadd [0x480010]; fstp [0x480020]
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D8 05 10 00 48 00 D9 1D 20 00 48 00 C3");
}
void verify_fadd(Fixture& f) { LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x20u), 0x40800000u); }  // 4.0f

void setup_fcom(Fixture& f) {
    f.dataHex(kDataEa, "0000C03F");          // 1.5f
    f.dataHex(kDataEa + 0x10u, "00000040");  // 2.0f
    // fld [0x480000]; fld [0x480010]; fcom (ST0=2.0 vs ST1=1.5) ⇒ 大于 ⇒ C3=C2=C0=0；fnstsw ax
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 05 10 00 48 00 D8 D1 DF E0 C3");
}
void verify_fcom(Fixture& f) {
    // fnstsw ax 后：C3..C0 = 0 ⇒ 高位全 0（TOP 在第 11..13 位）
    LIFT_CHECK_EQ_U32((f.cpu().eax() & 0x4700u), 0u);
}

// =====================================================================================
// 增补（判据 A1 加厚）。★ 仍然是"一条用例一件事"：`setup` 放手写字节 + 置初值，
// `verify` 只断言寄存器 / 标志 / 内存槽。⛔ 没有一条把 expected 抄成 actual。
//  ① 整数标志：dec 的 OF/SF 边界、AF（半进位）、adc/sbb 的进位链、neg 的最小负数、
//     移位计数 0 / 31 / 大于 31（x86 取低 5 位）。
//  ② 乘除：三操作数 imul（6B / 69）、二操作数的 OF/CF、8 位与 16 位 mul、16 位 div、
//     除数为负的 idiv、cdq（正数）、cbw。
//  ③ 条件：setcc（e/ne/l/ae）、jcc（jl/jg/jbe/ja 各自 taken 与 not-taken）、cmovcc。
//  ④ 栈 / 位 / 数据：pushad·popad、pushfd·popfd、bt·bts·btr·btc、bsf·bsr（含源 0）、
//     xchg 内存形式、movzx·movsx 的 16→32、lea 的 index*scale。
//  ⑤ 串：rep movsb、rep stosw、repne scasb（ZF 提前结束）、lodsb、cmpsb。
//  ⑥ x87：fsub/fdiv/fmul（D8 /4 /6 /1 内存形式）、fcompp·fucompp、fldcw+fnstcw 往返、
//     fild 16/64、fistp 64、fchs、fabs、fsqrt、frndint、ffree。
// =====================================================================================

// ---------- ① 整数 / 标志 ----------
void setup_dec_of_sf(Fixture& f) {
    // dec 0x80000000 ⇒ 0x7FFFFFFF：符号位 1→0 且有符号溢出 ⇒ OF=1；CF 必须被保留
    f.cpu().eax() = 0x80000000u;
    f.cpu().setCf(true);  // 先置 1：若实现把 CF 一起算，它会变 0 ⇒ 红
    f.cpu().setZf(true);  // 先置 1：ZF 必须被算成 0
    f.codeHex(kCodeEa, "48 C3");  // dec eax
}
void verify_dec_of_sf(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x7FFFFFFFu);
    LIFT_CHECK(f.cpu().of());
    LIFT_CHECK(!f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
    LIFT_CHECK(f.cpu().cf());
}

void setup_add_af(Fixture& f) {
    // 半进位：0x0F + 0x01 从 bit3 进到 bit4 ⇒ AF=1（AF 先清 0：不算 AF 就红）
    f.cpu().eax() = 0x0Fu;
    f.cpu().ebx() = 0x01u;
    f.cpu().setAf(false);
    f.codeHex(kCodeEa, "01 D8 C3");  // add eax, ebx
}
void verify_add_af(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x10u);
    LIFT_CHECK(f.cpu().af());
    LIFT_CHECK(!f.cpu().cf());  // 半进位不是进位
}

void setup_sub_af(Fixture& f) {
    // 半借位：0x10 - 0x01 从 bit4 借到 bit3 ⇒ AF=1（AF 先清 0）
    f.cpu().eax() = 0x10u;
    f.cpu().ebx() = 0x01u;
    f.cpu().setAf(false);
    f.codeHex(kCodeEa, "29 D8 C3");  // sub eax, ebx
}
void verify_sub_af(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x0Fu);
    LIFT_CHECK(f.cpu().af());
    LIFT_CHECK(!f.cpu().cf());
}

void setup_adc_carry_in(Fixture& f) {
    // 0xFFFFFFFE + 0 + CF(1) = 0xFFFFFFFF：CF 参与运算，但结果不再进位
    f.cpu().eax() = 0xFFFFFFFEu;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "83 D0 00 C3");  // adc eax, 0
}
void verify_adc_carry_in(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(!f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
}

void setup_adc_carry_out(Fixture& f) {
    // 0xFFFFFFFF + 0 + CF(1) = 0x1_00000000 ⇒ 结果 0、CF=1、ZF=1
    f.cpu().eax() = 0xFFFFFFFFu;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "83 D0 00 C3");  // adc eax, 0
}
void verify_adc_carry_out(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().zf());
}

void setup_sbb_borrow_in(Fixture& f) {
    // 0 - 0 - CF(1) = 0xFFFFFFFF ⇒ CF=1（借位）、SF=1、ZF=0
    f.cpu().eax() = 0u;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "83 D8 00 C3");  // sbb eax, 0
}
void verify_sbb_borrow_in(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
}

void setup_neg_int_min(Fixture& f) {
    // neg 0x80000000 ⇒ 结果仍是 0x80000000（无符号回绕），但 OF=1 / CF=1 / SF=1
    f.cpu().eax() = 0x80000000u;
    f.codeHex(kCodeEa, "F7 D8 C3");  // neg eax
}
void verify_neg_int_min(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x80000000u);
    LIFT_CHECK(f.cpu().of());   // 唯一"取负等于自己"的值 ⇒ 有符号溢出
    LIFT_CHECK(f.cpu().cf());   // CF = (a != 0)
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
}

// 计数 0 的移位**什么都不改**（值 / CF / ZF 都不能动）。为了让"代码真的跑了"可观察，
// 末尾补一条 `mov ebx, 0x0BADF00D`（mov 不动标志）—— 这是唯一一处"辅助可观察"的写法。
template <std::uint8_t kModRm>
void setup_shift_count_0(Fixture& f) {
    f.cpu().eax() = 0x12345678u;
    f.cpu().setCf(true);
    f.cpu().setZf(true);
    const std::uint8_t bytes[] = {0xC1, kModRm, 0x00,          // shl/shr/sar eax, 0
                                  0xBB, 0x0D, 0xF0, 0xAD, 0x0B,  // mov ebx, 0x0BADF00D
                                  0xC3};
    f.code(kCodeEa, std::vector<std::uint8_t>(bytes, bytes + sizeof(bytes)));
}
void verify_shift_count_0(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x12345678u);  // 值不变
    LIFT_CHECK(f.cpu().cf());                       // CF 不被重算成别的
    LIFT_CHECK(f.cpu().zf());                       // ZF 不被重算成别的
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 0x0BADF00Du);  // 代码确实执行到了后面
}

void setup_shl_count_31(Fixture& f) {
    // shl 31：0x00000003 << 31 = 0x80000000；CF = 被移出的 bit1 = 1；OF = MSB(r) XOR CF = 0
    f.cpu().eax() = 0x00000003u;
    f.codeHex(kCodeEa, "C1 E0 1F C3");  // shl eax, 31
}
void verify_shl_count_31(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x80000000u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
    LIFT_CHECK(!f.cpu().of());
}

void setup_shl_count_33(Fixture& f) {
    // 计数取低 5 位：33 ⇒ 1（照 33 直接算会得 0 ⇒ 红）。CF = 移出的原 bit31 = 1
    f.cpu().eax() = 0x80000001u;
    f.cpu().ecx() = 33u;
    f.codeHex(kCodeEa, "D3 E0 C3");  // shl eax, cl
}
void verify_shl_count_33(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000002u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(!f.cpu().sf());
}

void setup_shr_count_33(Fixture& f) {
    // 33 ⇒ 1：0x40000001 >> 1 = 0x20000000，CF = 移出的 bit0 = 1
    f.cpu().eax() = 0x40000001u;
    f.cpu().ecx() = 33u;
    f.codeHex(kCodeEa, "D3 E8 C3");  // shr eax, cl
}
void verify_shr_count_33(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x20000000u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(!f.cpu().zf());
}

void setup_sar_count_31(Fixture& f) {
    // sar 31：0x80000000 ⇒ 0xFFFFFFFF（符号位填满），CF = 移出的 bit30 = 0
    f.cpu().eax() = 0x80000000u;
    f.codeHex(kCodeEa, "C1 F8 1F C3");  // sar eax, 31
}
void verify_sar_count_31(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(!f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
}

void setup_sar_count_33(Fixture& f) {
    // 33 ⇒ 1：0xFFFFFFFE (-2) 算术右移 1 = -1；CF = 移出的 bit0 = 0
    f.cpu().eax() = 0xFFFFFFFEu;
    f.cpu().ecx() = 33u;
    f.codeHex(kCodeEa, "D3 F8 C3");  // sar eax, cl
}
void verify_sar_count_33(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFFFFu);
    LIFT_CHECK(!f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
}

// ---------- ② 乘除 ----------
void setup_imul3_imm8(Fixture& f) {
    // 三操作数：imul eax, ecx, 7（6B /r ib）⇒ eax = ecx * 7 = 42
    f.cpu().ecx() = 6u;
    f.codeHex(kCodeEa, "6B C1 07 C3");
}
void verify_imul3_imm8(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 42u);
    LIFT_CHECK(!f.cpu().of());  // 42 装得下 int32
    LIFT_CHECK(!f.cpu().cf());
}

void setup_imul3_imm32(Fixture& f) {
    // 69 /r id：imul eax, ecx, 5000 ⇒ eax = 3 * 5000 = 15000
    f.cpu().ecx() = 3u;
    f.codeHex(kCodeEa, "69 C1 88 13 00 00 C3");
}
void verify_imul3_imm32(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 15000u);
    LIFT_CHECK(!f.cpu().of());
    LIFT_CHECK(!f.cpu().cf());
}

void setup_imul2_flags(Fixture& f) {
    // 0x10000 * 0x10000 = 0x1_0000_0000 装不进 int32 ⇒ 低 32 位 0、CF=OF=1
    f.cpu().eax() = 0x00010000u;
    f.cpu().ecx() = 0x00010000u;
    f.codeHex(kCodeEa, "0F AF C1 C3");  // imul eax, ecx
}
void verify_imul2_flags(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().of());
    LIFT_CHECK(f.cpu().cf());
}

void setup_mul_8bit(Fixture& f) {
    // F6 /4：AX = AL * CL = 0x10 * 0x20 = 0x0200 ⇒ AH != 0 ⇒ CF=OF=1
    f.cpu().eax() = 0x10u;
    f.cpu().ecx() = 0x20u;
    f.codeHex(kCodeEa, "F6 E1 C3");  // mul cl
}
void verify_mul_8bit(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000200u);
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().of());
}

void setup_mul_16bit(Fixture& f) {
    // 66 F7 /4 → mul cx：DX:AX = 0x1000 * 0x1000 = 0x0100_0000
    f.cpu().eax() = 0x1000u;
    f.cpu().ecx() = 0x1000u;
    f.cpu().edx() = 0x0u;
    f.codeHex(kCodeEa, "66 F7 E1 C3");  // mul cx
}
void verify_mul_16bit(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax() & 0xFFFFu, 0u);
    LIFT_CHECK_EQ_U32(f.cpu().edx() & 0xFFFFu, 0x0100u);
    LIFT_CHECK(f.cpu().cf());
}

void setup_div_16bit(Fixture& f) {
    // 66 F7 /6 → div cx：DX:AX = 0x0001_0000 = 65536，除以 7 ⇒ 商 0x2492、余 2
    f.cpu().edx() = 0x0001u;
    f.cpu().eax() = 0x0000u;
    f.cpu().ecx() = 7u;
    f.codeHex(kCodeEa, "66 F7 F1 C3");  // div cx
}
void verify_div_16bit(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax() & 0xFFFFu, 0x2492u);
    LIFT_CHECK_EQ_U32(f.cpu().edx() & 0xFFFFu, 2u);
}

void setup_idiv_neg_divisor(Fixture& f) {
    // 100 / -7：商朝零截断 = -14，余数跟**被除数**同号 = +2（不是 -2）
    f.cpu().eax() = 100u;
    f.cpu().edx() = 0u;
    f.cpu().ecx() = static_cast<std::uint32_t>(-7);
    f.codeHex(kCodeEa, "F7 F9 C3");  // idiv ecx
}
void verify_idiv_neg_divisor(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), static_cast<std::uint32_t>(-14));
    LIFT_CHECK_EQ_U32(f.cpu().edx(), 2u);
}

void setup_cdq_positive(Fixture& f) {
    // cdq 的正数分支：EDX 必须是 0（先放脏值，好看出它真的被写了）
    f.cpu().eax() = 0x12345678u;
    f.cpu().edx() = 0xDEADBEEFu;
    f.codeHex(kCodeEa, "99 C3");  // cdq
}
void verify_cdq_positive(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().edx(), 0u); }

void setup_cbw(Fixture& f) {
    // 66 98 = cbw：AX = 符号扩展(AL)，EAX 的高 16 位不动
    f.cpu().eax() = 0xFFFF0080u;
    f.codeHex(kCodeEa, "66 98 C3");  // cbw
}
void verify_cbw(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFFFF80u); }

// ---------- ③ 条件 ----------
void setup_setcc_e(Fixture& f) {
    f.cpu().eax() = 5u;
    f.codeHex(kCodeEa, "83 F8 05 0F 94 C0 C3");  // cmp eax,5; sete al
}
void verify_setcc_e(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 1u);
    LIFT_CHECK(f.cpu().zf());
}

void setup_setcc_ne(Fixture& f) {
    // cmp 相等 ⇒ setne 写 0（把 AL 原来的 5 抹掉）
    f.cpu().eax() = 5u;
    f.codeHex(kCodeEa, "83 F8 05 0F 95 C0 C3");  // cmp eax,5; setne al
}
void verify_setcc_ne(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().zf());
}

void setup_setcc_l(Fixture& f) {
    // 3 < 5：SF=1、OF=0 ⇒ SF != OF ⇒ L 成立
    f.cpu().eax() = 0x03u;
    f.codeHex(kCodeEa, "83 F8 05 0F 9C C0 C3");  // cmp eax,5; setl al
}
void verify_setcc_l(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 1u);
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().of());
}

void setup_setcc_ae(Fixture& f) {
    // 3 - 5 无符号借位 ⇒ CF=1 ⇒ AE（CF=0）不成立 ⇒ 写 0
    f.cpu().eax() = 0x03u;
    f.codeHex(kCodeEa, "83 F8 05 0F 93 C0 C3");  // cmp eax,5; setae al
}
void verify_setcc_ae(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().cf());
}

// jcc 的两条路径都**真实执行**：条件成立 ⇒ ebx=1；不成立 ⇒ ebx=2。
// 布局（相对 kCodeEa）：cmp(3) / jcc rel8 +7 → 位移 12（"成立"分支）/ mov ebx,2 / jmp +5 / mov ebx,1 / ret
template <std::uint32_t kEax, std::uint8_t kOpcode>
void setup_jcc_variant(Fixture& f) {
    f.cpu().eax() = kEax;
    const std::uint8_t bytes[] = {0x83, 0xF8, 0x05, kOpcode, 0x07, 0xBB, 0x02, 0x00, 0x00, 0x00,
                                  0xEB, 0x05, 0xBB, 0x01, 0x00, 0x00, 0x00, 0xC3};
    f.code(kCodeEa, std::vector<std::uint8_t>(bytes, bytes + sizeof(bytes)));
}
template <std::uint32_t kExpectEbx>
void verify_jcc_variant(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), kExpectEbx);
    LIFT_CHECK_EQ_U32(f.cpu().eip, kSentinelEa);  // 两条路径最终都走到哨兵
}

void setup_cmovcc_taken(Fixture& f) {
    // cmove eax, ebx（0F 44）：cmp 相等 ⇒ ZF=1 ⇒ eax 取 ebx
    f.cpu().eax() = 5u;
    f.cpu().ebx() = 0xCAFEBABEu;
    f.codeHex(kCodeEa, "83 F8 05 0F 44 C3 C3");  // cmp eax,5; cmove eax,ebx
}
void verify_cmovcc_taken(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xCAFEBABEu);
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 0xCAFEBABEu);  // 源操作数不变
    LIFT_CHECK(f.cpu().zf());
}

void setup_cmovcc_not_taken(Fixture& f) {
    // 4 != 5 ⇒ ZF=0 ⇒ cmove 不写 eax（先置 ZF=1，好让 cmp 之后 ZF=0 也算状态变化）
    f.cpu().eax() = 4u;
    f.cpu().ebx() = 0xCAFEBABEu;
    f.cpu().setZf(true);
    f.codeHex(kCodeEa, "83 F8 05 0F 44 C3 C3");  // cmp eax,5; cmove eax,ebx
}
void verify_cmovcc_not_taken(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 4u);
    LIFT_CHECK(!f.cpu().zf());
}

void setup_cmovl(Fixture& f) {
    // cmovl eax, ebx（0F 4C）：3 < 5 ⇒ L 成立 ⇒ eax 取 ebx。
    // ★ 这条同时钉住助记符映射：`cmovl` 必须在助记符表里有个名字，否则解码即 Op::Other。
    f.cpu().eax() = 3u;
    f.cpu().ebx() = 0xCAFEBABEu;
    f.codeHex(kCodeEa, "83 F8 05 0F 4C C3 C3");  // cmp eax,5; cmovl eax,ebx
}
void verify_cmovl(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xCAFEBABEu);
    LIFT_CHECK(f.cpu().sf());  // cmp 3-5 ⇒ SF=1、OF=0 ⇒ L
    LIFT_CHECK(!f.cpu().of());
}

// ---------- ④ 栈 / 位 / 数据 ----------
void setup_pushad_popad(Fixture& f) {
    // pushad 压 8 个寄存器，中间把它们改掉，popad 全恢复（ESP 压的是原值，弹时丢弃）
    f.cpu().eax() = 0x11111111u;
    f.cpu().ebx() = 0x22222222u;
    f.cpu().ecx() = 0x33333333u;
    f.cpu().edx() = 0x44444444u;
    f.cpu().esi() = 0x55555555u;
    f.cpu().edi() = 0x66666666u;
    f.cpu().ebp() = 0x77777777u;
    // pushad; mov eax,0xEFBEADDE; xor ebx,ebx; popad; ret
    f.codeHex(kCodeEa, "60 B8 DE AD BE EF 31 DB 61 C3");
}
void verify_pushad_popad(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x11111111u);
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 0x22222222u);
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 0x33333333u);
    LIFT_CHECK_EQ_U32(f.cpu().edx(), 0x44444444u);
    LIFT_CHECK_EQ_U32(f.cpu().esi(), 0x55555555u);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), 0x66666666u);
    LIFT_CHECK_EQ_U32(f.cpu().ebp(), 0x77777777u);
    LIFT_CHECK_EQ_U32(f.cpu().esp(), f.m().layout().stack_top);  // 栈平衡（ret 弹掉哨兵）
}

void setup_pushfd_popfd(Fixture& f) {
    // pushfd 把 CF=1 存栈；xor 把 CF 清 0、ZF 置 1；popfd 恢复 ⇒ CF=1 且 ZF 回到 0
    f.cpu().eax() = 0xDEADBEEFu;
    f.cpu().setCf(true);
    f.codeHex(kCodeEa, "9C 31 C0 9D C3");  // pushfd; xor eax,eax; popfd; ret
}
void verify_pushfd_popfd(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u);
    LIFT_CHECK(f.cpu().cf());   // 被 pushfd/popfd 恢复
    LIFT_CHECK(!f.cpu().zf());  // xor 置的 ZF 也一起被恢复成 pushfd 时的 0
    LIFT_CHECK_EQ_U32(f.cpu().esp(), f.m().layout().stack_top);
}

void setup_bt(Fixture& f) {
    // bt eax, ecx：把 eax 的 bit2 送进 CF（eax 本身不变）
    f.cpu().eax() = 0x00000004u;
    f.cpu().ecx() = 2u;
    f.cpu().setCf(false);  // 先清 0：不写 CF 就红
    f.codeHex(kCodeEa, "0F A3 C8 C3");  // bt eax, ecx
}
void verify_bt(Fixture& f) {
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000004u);
}

void setup_bts(Fixture& f) {
    // bts eax, ecx：CF = 原 bit5(0)，然后把 bit5 置 1 ⇒ eax = 0x20
    f.cpu().eax() = 0x00000000u;
    f.cpu().ecx() = 5u;
    f.cpu().setCf(true);  // 先置 1：CF 必须被写成"原 bit 值"
    f.codeHex(kCodeEa, "0F AB C8 C3");  // bts eax, ecx
}
void verify_bts(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00000020u);
    LIFT_CHECK(!f.cpu().cf());
}

void setup_btr(Fixture& f) {
    // btr eax, ecx：CF = 原 bit3(1)，然后把 bit3 清 0 ⇒ eax = 0xF7
    f.cpu().eax() = 0x000000FFu;
    f.cpu().ecx() = 3u;
    f.cpu().setCf(false);
    f.codeHex(kCodeEa, "0F B3 C8 C3");  // btr eax, ecx
}
void verify_btr(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x000000F7u);
    LIFT_CHECK(f.cpu().cf());
}

void setup_btc(Fixture& f) {
    // btc eax, ecx：CF = 原 bit1(1)，然后把 bit1 取反 ⇒ eax = 0x0D
    f.cpu().eax() = 0x0000000Fu;
    f.cpu().ecx() = 1u;
    f.cpu().setCf(false);
    f.codeHex(kCodeEa, "0F BB C8 C3");  // btc eax, ecx
}
void verify_btc(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x0000000Du);
    LIFT_CHECK(f.cpu().cf());
}

void setup_bsf(Fixture& f) {
    // 0x00F00000 的最低置位是 bit20
    f.cpu().eax() = 0xFFFFFFFFu;  // 先放脏值，好看出它被写了
    f.cpu().ecx() = 0x00F00000u;
    f.codeHex(kCodeEa, "0F BC C1 C3");  // bsf eax, ecx
}
void verify_bsf(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 20u);
    LIFT_CHECK(!f.cpu().zf());
}

void setup_bsr(Fixture& f) {
    // 0x00F00000 的最高置位是 bit23
    f.cpu().eax() = 0xFFFFFFFFu;
    f.cpu().ecx() = 0x00F00000u;
    f.codeHex(kCodeEa, "0F BD C1 C3");  // bsr eax, ecx
}
void verify_bsr(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 23u);
    LIFT_CHECK(!f.cpu().zf());
}

void setup_bsf_zero(Fixture& f) {
    // 源为 0：ZF=1，目标**保持不变**
    f.cpu().eax() = 0xDEADBEEFu;
    f.cpu().ecx() = 0u;
    f.cpu().setZf(false);
    f.codeHex(kCodeEa, "0F BC C1 C3");  // bsf eax, ecx
}
void verify_bsf_zero(Fixture& f) {
    LIFT_CHECK(f.cpu().zf());
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xDEADBEEFu);
}

void setup_bsr_zero(Fixture& f) {
    f.cpu().eax() = 0xDEADBEEFu;
    f.cpu().ecx() = 0u;
    f.cpu().setZf(false);
    f.codeHex(kCodeEa, "0F BD C1 C3");  // bsr eax, ecx
}
void verify_bsr_zero(Fixture& f) {
    LIFT_CHECK(f.cpu().zf());
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xDEADBEEFu);
}

void setup_xchg_mem(Fixture& f) {
    // 87 /r 的内存形式：xchg [0x480000], eax（两边互换）
    f.dataHex(kDataEa, "44332211");
    f.cpu().eax() = 0xAABBCCDDu;
    f.codeHex(kCodeEa, "87 05 00 00 48 00 C3");  // xchg [0x480000], eax
}
void verify_xchg_mem(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa), 0xAABBCCDDu);
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x11223344u);
}

void setup_movzx_16(Fixture& f) {
    // 0F B7：16→32 零扩展（高半的 0xFFFF 必须被丢掉）
    f.cpu().ebx() = 0xFFFF8000u;
    f.codeHex(kCodeEa, "0F B7 C3 C3");  // movzx eax, bx
}
void verify_movzx_16(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x00008000u); }

void setup_movsx_16(Fixture& f) {
    // 0F BF：16→32 符号扩展（0x8000 ⇒ 0xFFFF8000）
    f.cpu().ebx() = 0x12348000u;
    f.codeHex(kCodeEa, "0F BF C3 C3");  // movsx eax, bx
}
void verify_movsx_16(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0xFFFF8000u); }

void setup_lea_index_scale(Fixture& f) {
    // 8D 04 8D disp32 = lea eax, [ecx*4 + 0x480000]（SIB：无 base、index=ecx、scale=4）
    f.cpu().ecx() = 0x100u;
    f.codeHex(kCodeEa, "8D 04 8D 00 00 48 00 C3");
}
void verify_lea_index_scale(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), kDataEa + 0x400u); }

// ---------- ⑤ 串操作 ----------
void setup_rep_movsb(Fixture& f) {
    f.dataHex(kDataEa, "0102030405");
    f.dataHex(kDataEa + 0x100u, "0000000000");
    f.cpu().esi() = kDataEa;
    f.cpu().edi() = kDataEa + 0x100u;
    f.cpu().ecx() = 5u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "F3 A4 C3");  // rep movsb
}
void verify_rep_movsb(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd8(kDataEa + 0x100u), 1u);
    LIFT_CHECK_EQ_U32(f.mem().rd8(kDataEa + 0x104u), 5u);
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 0u);
    LIFT_CHECK_EQ_U32(f.cpu().esi(), kDataEa + 5u);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), kDataEa + 0x100u + 5u);
}

void setup_rep_stosw(Fixture& f) {
    // 66 F3 AB：元素宽度 2（stosw），EDI 每次 +2
    f.dataHex(kDataEa, "000000000000");
    f.cpu().eax() = 0xBEEFu;
    f.cpu().edi() = kDataEa;
    f.cpu().ecx() = 3u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "66 F3 AB C3");  // rep stosw
}
void verify_rep_stosw(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd16(kDataEa), 0xBEEFu);
    LIFT_CHECK_EQ_U32(f.mem().rd16(kDataEa + 4u), 0xBEEFu);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), kDataEa + 6u);
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 0u);
}

void setup_repne_scasb(Fixture& f) {
    // "abcX" 里找 'X'：第 4 次命中 ⇒ ZF=1 提前结束，ECX 还剩 6、EDI 停在命中之后
    f.dataHex(kDataEa, "616263580000");
    f.cpu().eax() = 0x00000058u;  // AL = 'X'
    f.cpu().edi() = kDataEa;
    f.cpu().ecx() = 10u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "F2 AE C3");  // repne scasb
}
void verify_repne_scasb(Fixture& f) {
    LIFT_CHECK(f.cpu().zf());
    LIFT_CHECK_EQ_U32(f.cpu().ecx(), 6u);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), kDataEa + 4u);
}

void setup_lodsb(Fixture& f) {
    // lodsb：AL ← [ESI]，ESI += 1（EAX 的高 24 位不动）
    f.dataHex(kDataEa, "AABBCC");
    f.cpu().esi() = kDataEa + 1u;
    f.cpu().eax() = 0x12345600u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "AC C3");  // lodsb
}
void verify_lodsb(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().eax(), 0x123456BBu);
    LIFT_CHECK_EQ_U32(f.cpu().esi(), kDataEa + 2u);
}

void setup_cmpsb(Fixture& f) {
    // [ESI]='C'(0x43) 与 [EDI]='X'(0x58)：0x43 - 0x58 ⇒ CF=1、SF=1、ZF=0
    f.dataHex(kDataEa, "43");
    f.dataHex(kDataEa + 0x100u, "58");
    f.cpu().esi() = kDataEa;
    f.cpu().edi() = kDataEa + 0x100u;
    f.cpu().setDf(false);
    f.codeHex(kCodeEa, "A6 C3");  // cmpsb
}
void verify_cmpsb(Fixture& f) {
    LIFT_CHECK(f.cpu().cf());
    LIFT_CHECK(f.cpu().sf());
    LIFT_CHECK(!f.cpu().zf());
    LIFT_CHECK_EQ_U32(f.cpu().esi(), kDataEa + 1u);
    LIFT_CHECK_EQ_U32(f.cpu().edi(), kDataEa + 0x101u);
}

// ---------- ⑥ x87 最小子集（增补）----------
void setup_x87_fsub(Fixture& f) {
    // D8 /4（内存形式）：ST0 = ST0 - [mem] = 5.5 - 1.25 = 4.25，写回 float 检查位模式
    f.dataHex(kDataEa, "0000B040");          // 5.5f
    f.dataHex(kDataEa + 0x10u, "0000A03F");  // 1.25f
    f.dataHex(kDataEa + 0x20u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D8 25 10 00 48 00 D9 1D 20 00 48 00 C3");
}
void verify_x87_fsub(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x20u), 0x40880000u);  // 4.25f
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);                         // fstp 弹回去了
}

void setup_x87_fdiv(Fixture& f) {
    // D8 /6（内存形式）：ST0 = 5.0 / 4.0 = 1.25
    f.dataHex(kDataEa, "0000A040");          // 5.0f
    f.dataHex(kDataEa + 0x10u, "00008040");  // 4.0f
    f.dataHex(kDataEa + 0x20u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D8 35 10 00 48 00 D9 1D 20 00 48 00 C3");
}
void verify_x87_fdiv(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x20u), 0x3FA00000u);  // 1.25f
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);
}

void setup_x87_fmul(Fixture& f) {
    // D8 /1（内存形式）：ST0 = 1.5 * 2.5 = 3.75
    f.dataHex(kDataEa, "0000C03F");          // 1.5f
    f.dataHex(kDataEa + 0x10u, "00002040");  // 2.5f
    f.dataHex(kDataEa + 0x20u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D8 0D 10 00 48 00 D9 1D 20 00 48 00 C3");
}
void verify_x87_fmul(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x20u), 0x40700000u);  // 3.75f
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);
}

void setup_x87_fcompp(Fixture& f) {
    // DE D9 = fcompp：比 ST0(2.0) 与 ST1(1.5) ⇒ 大于 ⇒ C3=C2=C0=0，然后**弹两次**
    f.dataHex(kDataEa, "0000C03F");          // 1.5f
    f.dataHex(kDataEa + 0x10u, "00000040");  // 2.0f
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 05 10 00 48 00 DE D9 C3");
}
void verify_x87_fcompp(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);                // 两次 fld 之后 top=6，弹两次回到 0
    LIFT_CHECK(f.cpu().fpu.empty(6));                      // 两个槽都被弹空
    LIFT_CHECK(f.cpu().fpu.empty(7));
    LIFT_CHECK_EQ_U32((f.cpu().fpu.status & 0x4700u), 0u); // 2.0 > 1.5 ⇒ C3=C2=C0=0
}

void setup_x87_fucompp(Fixture& f) {
    // DA E9 = fucompp：与 fcompp 同理，比 ST0(2.0) 与 ST1(1.5)，弹两次
    f.dataHex(kDataEa, "0000C03F");          // 1.5f
    f.dataHex(kDataEa + 0x10u, "00000040");  // 2.0f
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 05 10 00 48 00 DA E9 C3");
}
void verify_x87_fucompp(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 0u);
    LIFT_CHECK(f.cpu().fpu.empty(6));
    LIFT_CHECK(f.cpu().fpu.empty(7));
    LIFT_CHECK_EQ_U32((f.cpu().fpu.status & 0x4700u), 0u);
}

void setup_x87_fldcw_roundtrip(Fixture& f) {
    // fnstcw 存缺省控制字(0x037F) → fldcw 改成 PC=53 位(0x027F) → fnstcw 读回 ⇒ 值必须变了
    f.dataHex(kDataEa, "00000000" "7F02" "0000" "0000");
    f.codeHex(kCodeEa,
              "D9 3D 00 00 48 00"  // fnstcw [0x480000]
              "D9 2D 04 00 48 00"  // fldcw  [0x480004]
              "D9 3D 08 00 48 00"  // fnstcw [0x480008]
              "C3");
}
void verify_x87_fldcw_roundtrip(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd16(kDataEa), 0x037Fu);       // 缺省控制字
    LIFT_CHECK_EQ_U32(f.mem().rd16(kDataEa + 8u), 0x027Fu);  // 改过之后读回的是新值
    LIFT_CHECK_EQ_U32(f.cpu().fpu.control, 0x027Fu);         // 内部状态也真的换了
}

void setup_x87_fild16(Fixture& f) {
    // DF /0 = fild m16int：-3 → ST0；再 fstp 成 float 检查是 -3.0
    f.dataHex(kDataEa, "FDFF");  // int16 -3
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "DF 05 00 00 48 00 D9 1D 10 00 48 00 C3");
}
void verify_x87_fild16(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0xC0400000u);  // -3.0f
}

void setup_x87_fild64(Fixture& f) {
    // DF /5 = fild m64int：2^32 = 0x00000001_00000000 → ST0；fstp 成 float ⇒ 4294967296.0f
    f.dataHex(kDataEa, "0000000001000000");
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "DF 2D 00 00 48 00 D9 1D 10 00 48 00 C3");
}
void verify_x87_fild64(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x4F800000u);
}

void setup_x87_fistp64(Fixture& f) {
    // DF /7 = fistp m64int：-3.0 ⇒ 64 位整数 0xFFFFFFFF_FFFFFFFD（低/高 32 位都要对）
    f.dataHex(kDataEa, "000040C0");  // -3.0f
    f.dataHex(kDataEa + 0x10u, "0000000000000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 DF 3D 10 00 48 00 C3");
}
void verify_x87_fistp64(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0xFFFFFFFDu);
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x14u), 0xFFFFFFFFu);
}

void setup_x87_fchs(Fixture& f) {
    // fld -1.5；fchs ⇒ +1.5（只翻符号位）；fstp 写回 float 检查位模式
    f.dataHex(kDataEa, "0000C0BF");  // -1.5f
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 E0 D9 1D 10 00 48 00 C3");
}
void verify_x87_fchs(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x3FC00000u);  // +1.5f
}

void setup_x87_fabs(Fixture& f) {
    // fld -1.5；fabs ⇒ +1.5（清符号位）
    f.dataHex(kDataEa, "0000C0BF");  // -1.5f
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 E1 D9 1D 10 00 48 00 C3");
}
void verify_x87_fabs(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x3FC00000u);  // +1.5f
}

void setup_x87_fsqrt(Fixture& f) {
    // fld 4.0；fsqrt ⇒ 2.0
    f.dataHex(kDataEa, "00008040");  // 4.0f
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 FA D9 1D 10 00 48 00 C3");
}
void verify_x87_fsqrt(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x40000000u);  // 2.0f
}

void setup_x87_frndint(Fixture& f) {
    // fld 2.7；frndint（缺省控制字 RC = 最近偶数）⇒ 3.0
    f.dataHex(kDataEa, "CDCC2C40");  // 2.7f
    f.dataHex(kDataEa + 0x10u, "00000000");
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 FC D9 1D 10 00 48 00 C3");
}
void verify_x87_frndint(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.mem().rd32(kDataEa + 0x10u), 0x40400000u);  // 3.0f
}

void setup_x87_ffree(Fixture& f) {
    // fld 1.5；fld 2.5；ffree st(0)：只把 ST(0) 的 tag 置空，TOP **不变**。
    // 末尾补一条 mov 证明代码真跑过（ffree 不改寄存器栈内容，也没有别的可见变化）。
    f.dataHex(kDataEa, "0000C03F");          // 1.5f
    f.dataHex(kDataEa + 0x10u, "00002040");  // 2.5f
    f.codeHex(kCodeEa, "D9 05 00 00 48 00 D9 05 10 00 48 00 DD C0 BB 01 00 00 00 C3");
}
void verify_x87_ffree(Fixture& f) {
    LIFT_CHECK_EQ_U32(f.cpu().fpu.top, 6u);  // 两次 fld 之后 top=6，ffree 不动它
    LIFT_CHECK(f.cpu().fpu.empty(6));        // ST(0) 的物理槽被标成 empty
    LIFT_CHECK(!f.cpu().fpu.empty(7));       // ST(1) 仍然有效
    LIFT_CHECK_EQ_U32(f.cpu().ebx(), 1u);
}

}  // namespace

const Case* conformanceCases(std::size_t* count) {
    static const Case kCases[] = {
        {"mov-imm", setup_mov_imm, verify_mov_imm},
        {"mov-reg", setup_mov_reg, verify_mov_reg},
        {"mov-mem-store", setup_mov_mem_store, verify_mov_mem_store},
        {"mov-mem-load", setup_mov_mem_load, verify_mov_mem_load},
        {"movzx", setup_movzx, verify_movzx},
        {"movsx", setup_movsx, verify_movsx},
        {"lea", setup_lea, verify_lea},
        {"xchg", setup_xchg, verify_xchg},
        {"bswap", setup_bswap, verify_bswap},
        {"push-pop", setup_push_pop, verify_push_pop},
        {"leave", setup_leave, verify_leave},
        {"add-overflow", setup_add_overflow, verify_add_overflow},
        {"add-carry", setup_add_carry, verify_add_carry},
        {"adc", setup_adc, verify_adc},
        {"sub-borrow", setup_sub_borrow, verify_sub_borrow},
        {"sbb", setup_sbb, verify_sbb},
        {"cmp-test", setup_cmp_test, verify_cmp_test},
        {"logic", setup_logic, verify_logic},
        {"inc-dec-preserve-cf", setup_inc_dec, verify_inc_dec},
        {"neg-not", setup_neg_not, verify_neg_not},
        {"shl", setup_shl, verify_shl},
        {"shr", setup_shr, verify_shr},
        {"sar", setup_sar, verify_sar},
        {"rol", setup_rol, verify_rol},
        {"ror", setup_ror, verify_ror},
        {"shift-by-cl", setup_shift_by_cl, verify_shift_by_cl},
        {"imul-two", setup_imul_two, verify_imul_two},
        {"imul-signed", setup_imul_signed, verify_imul_signed},
        {"mul-full", setup_mul_full, verify_mul_full},
        {"div", setup_div, verify_div},
        {"idiv", setup_idiv, verify_idiv},
        {"cdq", setup_cdq, verify_cdq},
        {"cwde", setup_cwde, verify_cwde},
        {"setcc", setup_setcc, verify_setcc},
        {"jcc-taken", setup_jcc_taken, verify_jcc_taken},
        {"jcc-not-taken", setup_jcc_not_taken, verify_jcc_not_taken},
        {"rep-stosd", setup_rep_stosd, verify_rep_stosd},
        {"rep-movsd", setup_rep_movsd, verify_rep_movsd},
        {"x87-fld-fstp", setup_fld_fstp, verify_fld_fstp},
        {"x87-fild-fistp", setup_fild_fistp, verify_fild_fistp},
        {"x87-fadd", setup_fadd, verify_fadd},
        {"x87-fcom-fnstsw", setup_fcom, verify_fcom},
        // ---- 增补：① 整数 / 标志 ----
        {"dec-of-sf-boundary", setup_dec_of_sf, verify_dec_of_sf},
        {"add-af", setup_add_af, verify_add_af},
        {"sub-af", setup_sub_af, verify_sub_af},
        {"adc-carry-in", setup_adc_carry_in, verify_adc_carry_in},
        {"adc-carry-out", setup_adc_carry_out, verify_adc_carry_out},
        {"sbb-borrow-in", setup_sbb_borrow_in, verify_sbb_borrow_in},
        {"neg-int-min", setup_neg_int_min, verify_neg_int_min},
        {"shl-count-0", setup_shift_count_0<0xE0>, verify_shift_count_0},
        {"shr-count-0", setup_shift_count_0<0xE8>, verify_shift_count_0},
        {"sar-count-0", setup_shift_count_0<0xF8>, verify_shift_count_0},
        {"shl-count-31", setup_shl_count_31, verify_shl_count_31},
        {"shl-count-33-masked", setup_shl_count_33, verify_shl_count_33},
        {"shr-count-33-masked", setup_shr_count_33, verify_shr_count_33},
        {"sar-count-31", setup_sar_count_31, verify_sar_count_31},
        {"sar-count-33-masked", setup_sar_count_33, verify_sar_count_33},
        // ---- 增补：② 乘除 ----
        {"imul3-imm8", setup_imul3_imm8, verify_imul3_imm8},
        {"imul3-imm32", setup_imul3_imm32, verify_imul3_imm32},
        {"imul2-flags-of", setup_imul2_flags, verify_imul2_flags},
        {"mul-8bit", setup_mul_8bit, verify_mul_8bit},
        {"mul-16bit", setup_mul_16bit, verify_mul_16bit},
        {"div-16bit", setup_div_16bit, verify_div_16bit},
        {"idiv-neg-divisor", setup_idiv_neg_divisor, verify_idiv_neg_divisor},
        {"cdq-positive", setup_cdq_positive, verify_cdq_positive},
        {"cbw", setup_cbw, verify_cbw},
        // ---- 增补：③ 条件 ----
        {"setcc-e", setup_setcc_e, verify_setcc_e},
        {"setcc-ne", setup_setcc_ne, verify_setcc_ne},
        {"setcc-l", setup_setcc_l, verify_setcc_l},
        {"setcc-ae", setup_setcc_ae, verify_setcc_ae},
        {"jcc-jl-taken", setup_jcc_variant<3u, 0x7C>, verify_jcc_variant<1u>},
        {"jcc-jl-not-taken", setup_jcc_variant<7u, 0x7C>, verify_jcc_variant<2u>},
        {"jcc-jg-taken", setup_jcc_variant<7u, 0x7F>, verify_jcc_variant<1u>},
        {"jcc-jg-not-taken", setup_jcc_variant<3u, 0x7F>, verify_jcc_variant<2u>},
        {"jcc-jbe-taken", setup_jcc_variant<3u, 0x76>, verify_jcc_variant<1u>},
        {"jcc-jbe-not-taken", setup_jcc_variant<7u, 0x76>, verify_jcc_variant<2u>},
        {"jcc-ja-taken", setup_jcc_variant<7u, 0x77>, verify_jcc_variant<1u>},
        {"jcc-ja-not-taken", setup_jcc_variant<5u, 0x77>, verify_jcc_variant<2u>},
        {"cmovcc-taken", setup_cmovcc_taken, verify_cmovcc_taken},
        {"cmovcc-not-taken", setup_cmovcc_not_taken, verify_cmovcc_not_taken},
        {"cmovl", setup_cmovl, verify_cmovl},
        // ---- 增补：④ 栈 / 位 / 数据 ----
        {"pushad-popad", setup_pushad_popad, verify_pushad_popad},
        {"pushfd-popfd", setup_pushfd_popfd, verify_pushfd_popfd},
        {"bt", setup_bt, verify_bt},
        {"bts", setup_bts, verify_bts},
        {"btr", setup_btr, verify_btr},
        {"btc", setup_btc, verify_btc},
        {"bsf", setup_bsf, verify_bsf},
        {"bsr", setup_bsr, verify_bsr},
        {"bsf-zero", setup_bsf_zero, verify_bsf_zero},
        {"bsr-zero", setup_bsr_zero, verify_bsr_zero},
        {"xchg-mem", setup_xchg_mem, verify_xchg_mem},
        {"movzx-16", setup_movzx_16, verify_movzx_16},
        {"movsx-16", setup_movsx_16, verify_movsx_16},
        {"lea-index-scale", setup_lea_index_scale, verify_lea_index_scale},
        // ---- 增补：⑤ 串操作 ----
        {"rep-movsb", setup_rep_movsb, verify_rep_movsb},
        {"rep-stosw", setup_rep_stosw, verify_rep_stosw},
        {"repne-scasb", setup_repne_scasb, verify_repne_scasb},
        {"lodsb", setup_lodsb, verify_lodsb},
        {"cmpsb", setup_cmpsb, verify_cmpsb},
        // ---- 增补：⑥ x87 ----
        {"x87-fsub", setup_x87_fsub, verify_x87_fsub},
        {"x87-fdiv", setup_x87_fdiv, verify_x87_fdiv},
        {"x87-fmul", setup_x87_fmul, verify_x87_fmul},
        {"x87-fcompp", setup_x87_fcompp, verify_x87_fcompp},
        {"x87-fucompp", setup_x87_fucompp, verify_x87_fucompp},
        {"x87-fldcw-roundtrip", setup_x87_fldcw_roundtrip, verify_x87_fldcw_roundtrip},
        {"x87-fild16", setup_x87_fild16, verify_x87_fild16},
        {"x87-fild64", setup_x87_fild64, verify_x87_fild64},
        {"x87-fistp64", setup_x87_fistp64, verify_x87_fistp64},
        {"x87-fchs", setup_x87_fchs, verify_x87_fchs},
        {"x87-fabs", setup_x87_fabs, verify_x87_fabs},
        {"x87-fsqrt", setup_x87_fsqrt, verify_x87_fsqrt},
        {"x87-frndint", setup_x87_frndint, verify_x87_frndint},
        {"x87-ffree", setup_x87_ffree, verify_x87_ffree},
    };
    *count = sizeof(kCases) / sizeof(kCases[0]);
    return kCases;
}

}  // namespace lift_test
