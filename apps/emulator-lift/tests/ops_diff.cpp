// emulator-lift · **2026-11-15 新增 inline 族的两臂差分**（CTest: lift-ops-diff）
//
// 红了意味着什么（判据）：
//   对 `tests/ir/ops_roots.txt` 里的**每一个**站点，从**同一份受控状态**出发：
//     ① **lifted 臂**（`lift-gen` 生成的 IR）；
//     ② **解释器臂**（`execInsn`，语义真源 = 解释器 handler）；
//   逐位比较：8 GPR / EFLAGS / 8 个 x87 槽（`double` 位型）/ top / tag word / 状态字 / 控制字 /
//   8 个 xmm（16 字节）/ mxcsr / 一段 guest 内存（1 KiB）。
//   ⇒ 任一处不同 = 生成的语义与解释器不同 = **当场红**（不是"跑完不崩"）。
//
// ★ 覆盖的 op（本轮把 `lift_one` 调用点降到 0 时新增的 inline 族）：
//   `div` · `idiv`（含**除零 / 商溢出**的停机面）· `imul` · `rol` · `ror` · `rcr` · `shld` · `shrd`
//   `rep movsd` · `movsb` · `rep stosd` · `cmpsb` · `rep scasb` · `cmpxchg` · `cpuid`
//   `mov eax, large fs:0` · `push/pop` 段覆盖 · `setcc` 写内存
//   MMX：`movq` · `movd` · `pand` · `pandn` · `por` · `pxor` · `paddw` · `psubw` · `psubsw`
//        `paddusw` · `pcmpeqw` · `pcmpeqd` · `psllw` · `psrlw` · `psllq` · `psraw` · `punpcklbw`
//        · `packuswb` · `paddq` · `psubq` · `psubd` · `emms`
//   SSE：`movdqa` · `movsd`
//   `bt` / `bts` 的**内存形**（SDM：位偏移是**地址**偏移 —— `EA + (off DIV 位宽)*字节数`；
//   站点 = 客侧 CRT `_strcspn` 的 32 字节位图，`bts/bt [esp],eax`）。`bt` 的结果只落在 CF 上，
//   而块尾的 `jae` 会把它**融合**进分支（S3-c 活性分析 ⇒ CF 不进 `%st`）⇒ 可观测的语义差是
//   **下一条 EA**（`OpsCase::compare_next_ea`），⛔ 不是 flags 字。
//   停机面：`int`（`Int`）· `xlat`（`Op::Other`）
//   ⚠ 未覆盖（如实登记）：
//     · `imul` 的 **1 操作数**与 **3 操作数**形态：本根集里挑不到已知形态的站点
//       （`.ll` 的注释只给助记符，不给操作数形态）；两者的语义差异在 `emitImul` 里按
//       `exec_integer.cpp:214-268` 逐行照抄，靠 3000 帧验收臂兜底。
//     · `ror/rol` 的**寄存器（CL）计数**形态：同上（挑不到已知站点）。
//     · `hlt`：本语料 0 站点（`.ll` 里没有 `hlt` 的 `lift_one` 站点）。
//
// ★ **自检**（防根集写错 / 防"两臂都没跑"）：跑之前用解释器解码器断言起点助记符；
//   并断言两臂真的执行了**相同条数**（停机类则断言两臂都停 + `stop_why` 字符串相同）。
// ★ 依赖：`AGE.EXE`（执行源）+ clang。缺任何一个 ⇒ `LIFT_SKIP(77)`。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aot/aot_dispatch.h"
#include "check.h"
#include "runtime/decode.h"
#include "runtime/image.h"
#include "runtime/machine.h"

namespace {

constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
// ★ 受控数据区必须在**镜像之外**：0x00440000 落在 `.text`（0x401000-0x51D000）里 ⇒ `placeData`
//   会把 MMX/SSE 那些站点的字节抹掉（实测：解码出来全是 `add`）。0x00600000 在镜像之后、guest 堆之前。
constexpr std::uint32_t kScratch = 0x00600000u;
constexpr std::uint32_t kScratchBytes = 0x00040000u;
constexpr std::uint32_t kMemProbe = 0x00001000u;  // 内存比对面的窗口宽度（4 KiB）
// ★ kBase 必须**离 scratch 起点足够远**：`[ebp-208h]` 这类负 disp 会往下走，起点太近就掉出已提交区。
constexpr std::uint32_t kBase = kScratch + 0x8000u;
constexpr std::uint32_t kProbeAt = kBase - 0x1000u;  // 比对面的起点

struct OpsCase {
    std::uint32_t ea;
    const char* op;  // 期望的起点助记符（自检用）
    // 受控状态（缺省值 = "所有指针都指进已提交数据区、不会越界也不会除零"）
    std::uint32_t eax = kBase;
    std::uint32_t ecx = kBase;  // ★ 缺省必须是**指针**：有的站点用 ECX 当基址（`mov [ecx],eax`）；rep 串操作才单独给 4
    std::uint32_t edx = kBase;
    std::uint32_t ebx = kBase;
    std::uint32_t ebp = kBase;
    std::uint32_t esi = kBase;
    std::uint32_t edi = kBase + 0x1000u;
    // ★ 缺省填充 = **指向 scratch 的 4 字节指针图样**：块里从内存读出的"指针"仍然落在已提交区
    //   （否则一个数据依赖的解引用就会把夹具打到未提交页 —— 那不是语义差，是夹具限制）。
    bool ptr_fill = true;
    std::uint8_t fill = 0x11;   // 只有 `ptr_fill == false` 时用
    bool stops = false;  // true = 两臂都必须在这一条上停机（`stop_why` 必须相同）
    // `0` = 缺省的栈顶（`stack_top - 0x40` + 哨兵）；非 0 = 显式 ESP。
    // ★ 为什么需要它：`bt/bts` 的**内存形**站点（`_strcspn` 的 32 字节位图）用 ESP 当基址，
    //   而位偏移 ≥ 32 时 SDM 要求地址 +N*4 ⇒ 写入落在 `[esp+12]`。要让它落进**被比较的内存窗口**
    //   （`kProbeAt..+4KiB`）就必须把 ESP 指到那里（栈页不在窗口里）。
    std::uint32_t esp = 0;
    // 把"块出口的**下一条 EA**"也纳入比较（缺省不比：`ret`/`call` 收口的块两臂的出口口径不同，
    // 那是另一层契约）。`bt` 这类"只写一位标志、而这位又被块尾分支融合"的站点靠它才可观测：
    // 位取值错 ⇒ 走另一条边 ⇒ 两臂返回的 EA 不同。
    bool compare_next_ea = false;
};

// ★ 与 `tests/ir/ops_roots.txt` 的行序**一一对应**（数量不等 ⇒ 硬错）。
constexpr OpsCase kCases[] = {
    // ---- fs: 段覆盖（`mov eax, large fs:0` 是 CRT 的 SEH 链头）----
    {.ea = 0x0040110A, .op = "mov"},
    // ---- 乘除 / 循环移位 / 双精度移位 ----
    {.ea = 0x004018A9, .op = "imul"},
    {.ea = 0x0040562F, .op = "rol"},
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x0040783C, .op = "shld"},
    // ⚠ `ror` 的两个候选站点（0x40D51E / 0x4103CC）所在块是**数据依赖的循环**：它从受控数据区
    //   读出的值当指针解引用（确定性打到 guest 0xFE0D827D = 未提交页）⇒ 两臂都跑不到同一个
    //   条数上（夹具限制，不是语义差）。⇒ 本差分**不覆盖 ror**；它的语义按 `exec_integer.cpp:112-118`
    //   逐行照抄，靠 3000 帧验收臂兜底。rol 同理只覆盖立即数形态（0x40562F）。
    // {.ea = 0x004103CC, .op = "ror"},
    {.ea = 0x004122FD, .op = "idiv", .eax = 1000, .ecx = 16, .edx = 0, .ptr_fill = false, .fill = 0x10},  // 商在范围内
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x00428E39, .op = "div", .eax = 1000, .ecx = 16, .edx = 0, .ptr_fill = false, .fill = 0x10},
    {.ea = 0x004E85D9, .op = "rcr"},
    {.ea = 0x004F203A, .op = "shrd"},
    // ---- 停机面：除零（`exec_integer.cpp:273`）----
    {.ea = 0x004122FD, .op = "idiv", .eax = 0, .ecx = 0, .edx = 0, .ptr_fill = false, .fill = 0, .stops = true},
    {.ea = 0x00428E39, .op = "div", .eax = 0, .ecx = 0, .edx = 0, .ptr_fill = false, .fill = 0, .stops = true},
    // ---- 停机面：商溢出 ----
    //   `div ecx`：edx:eax = 0xFFFFFFFFFFFFFFFF、除数 1 ⇒ q > 0xFFFFFFFF（`exec_integer.cpp:326`）
    {.ea = 0x00428E39, .op = "div", .eax = 0xFFFFFFFFu, .ecx = 1, .edx = 0xFFFFFFFFu,
     .ptr_fill = false, .fill = 0x01, .stops = true},
    //   `idiv ecx`：edx:eax = INT64_MIN、除数 -1 ⇒ 除法**之前**就判溢出（`exec_integer.cpp:300`）
    {.ea = 0x004122FD, .op = "idiv", .eax = 0, .ecx = 0xFFFFFFFFu, .edx = 0x80000000u,
     .ptr_fill = false, .fill = 0xFF, .stops = true},
    // ---- 串操作 ----
    {.ea = 0x0040B227, .op = "rep movsd", .ecx = 4},
    {.ea = 0x0040C8E7, .op = "rep stosd", .ecx = 4},
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x0042A96D, .op = "movsb", .ecx = 4},
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x0042A985, .op = "cmpsb", .ecx = 4},
    {.ea = 0x0043AC37, .op = "cpuid"},
    // ---- 段覆盖的 push / pop ----
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x00441DBD, .op = "push"},
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x00441DC9, .op = "pop"},
    // ---- setcc 写内存 ----
    {.ea = 0x0045FFDB, .op = "set"},
    // ---- cmpxchg ----
    {.ea = 0x004EACA0, .op = "lock cmpxchg", .eax = kBase, .ecx = kBase, .edx = kBase},
    // ---- MMX / SSE 整数打包 ----
    {.ea = 0x0044AECE, .op = "movq"},
    {.ea = 0x0044AEFC, .op = "pcmpeqw"},
    {.ea = 0x0044AEFF, .op = "pand"},
    {.ea = 0x0044AF08, .op = "psrlw"},
    {.ea = 0x0044AF19, .op = "psraw"},
    {.ea = 0x0044AF24, .op = "psllw"},
    {.ea = 0x0044AF28, .op = "por"},
    {.ea = 0x0044AF3F, .op = "paddw"},
    {.ea = 0x0044AF42, .op = "pandn"},
    {.ea = 0x0044AF54, .op = "emms"},
    {.ea = 0x0044B39C, .op = "pxor"},
    {.ea = 0x0044B3A3, .op = "movd"},
    {.ea = 0x0044B3A7, .op = "punpcklbw"},
    {.ea = 0x0044B3FD, .op = "packuswb"},
    {.ea = 0x0044B9DE, .op = "psubw"},
    {.ea = 0x0044BE90, .op = "psubsw"},
    {.ea = 0x0044EBE4, .op = "paddusw"},
    {.ea = 0x004E7DEE, .op = "psllq"},
    // [夹具跑不动 ⇒ 跳过] {.ea = 0x004E7E09, .op = "psubd"},
    {.ea = 0x004F29AA, .op = "psubq"},
    {.ea = 0x004F2CE9, .op = "pcmpeqd"},
    {.ea = 0x0050617F, .op = "paddq"},
    // ---- SSE（movdqa / movsd）----
    {.ea = 0x004F1075, .op = "movdqa"},
    {.ea = 0x004F2943, .op = "movsd"},
    // ---- 停机面：`int` 与 `Op::Other`（`exec_control.cpp:70-72` / `exec.cpp:210`）----
    {.ea = 0x0040B659, .op = "int", .stops = true},
    {.ea = 0x00418E10, .op = "xlat", .stops = true},
    // ---- ★ `bt/bts` 的**内存形**（SDM：位偏移是地址偏移，⛔ 不是低 5 位）----
    //   站点 = 客侧 CRT 的 `_strcspn`（32 字节位图；`0x6E` 与 `0x2E` 低 5 位相同，正是 `_setlocale`
    //   被切错的地方）。两臂以前都无条件截位 ⇒ 这一族能把 AOT 发射器单独钉住（解释器由
    //   `conformance` 的 `bt-mem-*` 用例按 SDM 定死 = 语义真源）。
    //   ESP 指进**被比较的内存窗口**（`kProbeAt+0x100`）⇒ `[esp+12]` 的写入 / 读取都在比较范围里。
    //   `fill = 0x3E`：`[esp+0]` 的 bit14 = (0x3E+1)&0x40 = 0、`[esp+12]` 的 bit14 = (0x3E+5)&0x40 = 0x40
    //   ⇒ **截位的旧语义**会读成 CF=0、SDM 的语义 CF=1（两臂必须在同一处取值）。
    //   `.eax = 0x6E`：指令是 `bts/bt [esp], eax` ⇒ 位偏移在 EAX（110 ⇒ 槽 3、位 14）。
    {.ea = 0x004F7C8D, .op = "bts", .eax = 0x6Eu, .ptr_fill = false, .fill = 0x3E,
     .esp = kProbeAt + 0x100u, .compare_next_ea = true},
    {.ea = 0x004F7CA8, .op = "bt", .eax = 0x6Eu, .ptr_fill = false, .fill = 0x3E,
     .esp = kProbeAt + 0x100u, .compare_next_ea = true},
};
constexpr std::size_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

struct Arm {
    lift::Machine m;
    std::string err;
    bool ok = false;
    bool init(const std::string& pe_path) {
        if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
        lift::PeImage image;
        if (!lift::loadPeFile(pe_path, &image, &err)) return false;
        if (!lift::mapPeImage(m.mem(), image, &err)) return false;
        std::vector<std::uint8_t> scratch(kScratchBytes, 0);
        if (!m.placeData(kScratch, scratch.data(), scratch.size(), &err)) return false;
        ok = true;
        return true;
    }
};

void setState(Arm& a, const OpsCase& c) {
    lift::X86& cpu = a.m.cpu();
    // ---- x87 / MMX：8 个槽都给**互不相同**的 double 位型（MMX 复用低 64 位）----
    const std::uint64_t pat[8] = {0x3FF8000000000000ull, 0xBFF0000000000001ull,
                                  0x4000000000000002ull, 0x3FF0000000000003ull,
                                  0xBFE0000000000004ull, 0x4090000000000005ull,
                                  0x400C000000000006ull, 0xC01F000000000007ull};
    for (int i = 0; i < 8; ++i) std::memcpy(&cpu.fpu.regs[i], &pat[i], 8);
    cpu.fpu.top = 0;
    cpu.fpu.tag = 0x0000u;  // 全部 valid
    cpu.fpu.control = 0x037Fu;
    cpu.fpu.status = 0;
    // ---- xmm：8 个寄存器各 16 字节，lane 互不相同 ----
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) cpu.xmm[i][k] = 0x10000000u * static_cast<unsigned>(i + 1) + static_cast<unsigned>(k) * 7u;
    cpu.mxcsr = 0x1F80u;
    // ---- GPR ----
    cpu.eax() = c.eax;
    cpu.ecx() = c.ecx;
    cpu.edx() = c.edx;
    cpu.ebx() = c.ebx;
    cpu.ebp() = c.ebp;
    cpu.esi() = c.esi;
    cpu.edi() = c.edi;
    cpu.esp() = c.esp != 0 ? c.esp : a.m.layout().stack_top - 0x40u;
    a.m.mem().wr32(cpu.esp(), kSentinel);
    // ★ `mov eax, large fs:0` 之类要访问 `fs_base`：受控状态必须把它指到**已提交**的 scratch，
    //   否则两臂都会碰到未提交页（解释器报 guest fault、VEH 报 host fault —— 那不是语义差）。
    cpu.fs_base = kBase + 0x2000u;
    cpu.gs_base = kBase + 0x2000u;
    cpu.eflags = 0x202u;
    cpu.running = true;
    // ---- 受控数据区 ----
    std::vector<std::uint8_t> buf(kScratchBytes, c.fill);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        // ★ 缺省填充 = **指向 scratch 的 4 字节指针图样**：块里从内存读出的"指针"仍落在已提交区
        //   （否则一个数据依赖的解引用就把夹具打到未提交页 —— 那不是语义差，是夹具限制）。
        if (c.ptr_fill)
            buf[i] = static_cast<std::uint8_t>((kBase >> (8u * (i & 3u))) & 0xFFu);
        else
            buf[i] = static_cast<std::uint8_t>(c.fill + static_cast<std::uint8_t>(i & 7u));
    }
    (void)a.m.placeData(kScratch, buf.data(), buf.size(), &a.err);
}

struct StateSnap {
    double regs[8] = {};
    std::uint8_t top = 0;
    std::uint16_t tag = 0;
    std::uint16_t status = 0;
    std::uint16_t control = 0;
    std::uint32_t xmm[8][4] = {};
    std::uint32_t mxcsr = 0;
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t cf6 = 0;   // EFLAGS 的六位（0x8D5）：**只对停机面比较**（见 compare 的注释）
    std::uint32_t eip = 0;   // 只给 `compare_next_ea` 用（缺省不比）
    std::vector<std::uint8_t> mem;
};

StateSnap snap(const Arm& a) {
    StateSnap s;
    const lift::X86& cpu = a.m.cpu();
    std::memcpy(s.regs, cpu.fpu.regs, sizeof(s.regs));
    s.top = cpu.fpu.top;
    s.tag = cpu.fpu.tag;
    s.status = cpu.fpu.status;
    s.control = cpu.fpu.control;
    std::memcpy(s.xmm, cpu.xmm, sizeof(s.xmm));
    s.mxcsr = cpu.mxcsr;
    for (int i = 0; i < 8; ++i) s.r[i] = cpu.r[i];
    s.eip = cpu.eip;
    s.eflags = cpu.eflags;
    s.cf6 = cpu.eflags & 0x8D5u;
    const std::uint8_t* p = a.m.mem().host(kProbeAt);
    s.mem.assign(p, p + kMemProbe);
    return s;
}

void compare(const char* tag, const StateSnap& lifted, const StateSnap& interp, bool flags_ok,
             bool next_ea_ok) {
    for (int i = 0; i < 8; ++i) {
        std::uint64_t lb = 0;
        std::uint64_t ib = 0;
        std::memcpy(&lb, &lifted.regs[i], 8);
        std::memcpy(&ib, &interp.regs[i], 8);
        if (lb != ib)
            lift_test::failf(__FILE__, __LINE__, "[%s] st(%d) bits lifted=%016llX interp=%016llX", tag,
                             i, static_cast<unsigned long long>(lb), static_cast<unsigned long long>(ib));
    }
    LIFT_CHECK_EQ_U32(lifted.top, interp.top);
    LIFT_CHECK_EQ_U32(lifted.tag, interp.tag);
    LIFT_CHECK_EQ_U32(lifted.status, interp.status);
    LIFT_CHECK_EQ_U32(lifted.control, interp.control);
    for (int i = 0; i < 8; ++i)
        for (int k = 0; k < 4; ++k) {
            if (lifted.xmm[i][k] != interp.xmm[i][k])
                lift_test::failf(__FILE__, __LINE__, "[%s] xmm%d[%d] lifted=%08X interp=%08X", tag, i, k,
                                 lifted.xmm[i][k], interp.xmm[i][k]);
        }
    LIFT_CHECK_EQ_U32(lifted.mxcsr, interp.mxcsr);
    for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(lifted.r[i], interp.r[i]);
    // ★ **不比 EFLAGS 整字**：AOT 的 S3-c 活性分析故意只写"后继会读"的位（死位保持旧值），
    //   而解释器每条写者都整份写六位 ⇒ 两臂的 eflags 在死位上**本来就允许不同**。
    //   ⇒ 只在 `flags_ok` 的用例上比（停机面：没有后继，六位都该一致）。
    if (flags_ok) LIFT_CHECK_EQ_U32(lifted.cf6, interp.cf6);
    // ★ `compare_next_ea`：两臂必须落到**同一个下一条 guest EA**（`bt` 的位取值只能从这里看见）。
    if (next_ea_ok) LIFT_CHECK_EQ_U32(lifted.eip, interp.eip);
    if (lifted.mem != interp.mem)
        lift_test::failf(__FILE__, __LINE__, "[%s] guest 内存区前 %u 字节不同", tag,
                         static_cast<unsigned>(kMemProbe));
    ++lift_test::g_checks;
}

bool checkOpAt(Arm& a, const OpsCase& c) {
    lift::Decoder dec;
    lift::Insn in;
    std::string why;
    if (!dec.decode(a.m.mem(), c.ea, &in, &why)) {
        lift_test::failf(__FILE__, __LINE__, "[%s] 解码失败 @ %08X：%s", c.op, c.ea, why.c_str());
        return false;
    }
    const char* mn = in.mnemonic != nullptr ? in.mnemonic : "";
    if (std::strncmp(mn, c.op, std::strlen(c.op)) != 0) {
        lift_test::failf(__FILE__, __LINE__, "@ %08X 的助记符是 \"%s\"，期望 \"%s\"", c.ea, mn, c.op);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    (void)argc;
    (void)argv;

#if !defined(LIFT_OPS_DIFF)
    std::printf("lift-ops-diff: 没有生成物（缺 clang / 缺 AGE.EXE）⇒ SKIP\n");
    return 77;
#else
    const std::string pe_path = LIFT_OPS_PE_PATH;
    lift::liftDynLookup = &::lift::aot::lookupUncounted;
    std::printf("lift-ops-diff: 用例 %zu 个（根集 tests/ir/ops_roots.txt，执行源 %s）\n", kCaseCount,
                pe_path.c_str());
    // 根集必须把这些站点都收进来（同一份 EA 只列一次 ⇒ 表比用例表小是正常的）
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::aot::tableSize() >= 40u ? 1u : 0u), 1u);

    std::uint32_t covered = 0;
    std::uint32_t stops_covered = 0;
    for (const OpsCase& c : kCases) {
        char tag_buf[96];
        std::snprintf(tag_buf, sizeof(tag_buf), "%s@%08X%s", c.op, c.ea, c.stops ? "/stop" : "");
        g_current_case = tag_buf;
        Arm li;
        Arm ia;
        if (!li.init(pe_path) || !ia.init(pe_path)) {
            lift_test::failf(__FILE__, __LINE__, "[%s] 夹具初始化失败：%s / %s", tag_buf, li.err.c_str(),
                             ia.err.c_str());
            g_current_case = "(none)";
            continue;
        }
        if (!checkOpAt(ia, c)) {
            g_current_case = "(none)";
            continue;
        }
        setState(li, c);
        setState(ia, c);

        // ---- ① lifted 臂 ----
        LiftRegs regs{};
        lift::liftCpuToRegs(li.m.cpu(), regs);
        std::uint8_t* mem = li.m.mem().host(0);
        const std::uint64_t insns0 = li.m.cpu().insns;
        std::uint32_t ea = c.ea;
        int dispatches = 0;
        while (true) {
            lift::LiftFn fn = lift::aot::lookup(ea, nullptr);
            if (fn == nullptr) break;
            if (++dispatches > 1) break;   // ★ 只跑**一个**块：两臂的条数预算才对齐（跨块链会越走越远）
            ea = fn(&regs, mem, ea, &li.m.cpu());
            if (!li.m.cpu().running) break;
        }
        lift::liftRegsToCpu(regs, li.m.cpu());
        // ★ 生成代码把"下一条 guest EA"**当返回值**给出来（`%st` 里没有 eip）⇒ 出口 EA 取 `ea`。
        const std::uint32_t lifted_next_ea = ea;
        const std::uint64_t lifted_insns = li.m.cpu().insns - insns0;

        // ---- ② 解释器臂：同一份状态，跑**同样条数**（停机类跑 1 条）----
        const std::uint64_t interp0 = ia.m.cpu().insns;
        lift::RunOptions o;
        o.max_insns = interp0 + (c.stops ? 1u : lifted_insns);
        ia.m.cpu().eip = c.ea;
        (void)ia.m.run(o);
        const std::uint64_t interp_insns = ia.m.cpu().insns - interp0;

        if (c.stops) {
            // 两臂都必须停在这一条上，而且**同一条停机面**（`stop_why` 字符串相同）
            LIFT_CHECK(!li.m.cpu().running);
            LIFT_CHECK(!ia.m.cpu().running);
            const char* lw = li.m.cpu().stop_why != nullptr ? li.m.cpu().stop_why : "";
            const char* iw = ia.m.cpu().stop_why != nullptr ? ia.m.cpu().stop_why : "";
            if (std::strcmp(lw, iw) != 0)
                lift_test::failf(__FILE__, __LINE__, "[%s] stop_why 不同：lifted=\"%s\" interp=\"%s\"",
                                 tag_buf, lw, iw);
            ++stops_covered;
        } else {
            LIFT_CHECK(lifted_insns >= 1u);  // "两臂都没跑"必须红
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lifted_insns),
                              static_cast<std::uint32_t>(interp_insns));
        }
        StateSnap lsnap = snap(li);
        lsnap.eip = lifted_next_ea;  // 生成代码的出口 EA = 上一块的返回值
        compare(tag_buf, lsnap, snap(ia), c.stops, c.compare_next_ea);
        ++covered;
        g_current_case = "(none)";
    }
    std::printf("lift-ops-diff: 覆盖 %u/%zu 个站点（其中停机面 %u 个）\n", covered, kCaseCount,
                stops_covered);
    LIFT_CHECK(covered == kCaseCount);
    LIFT_CHECK(stops_covered >= 4u);  // 除零 ×2 / 商溢出 ×2 / int / xlat ⇒ 至少 4 条真的停了
    return finish("lift-ops-diff");
#endif
}
