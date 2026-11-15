// emulator-lift · **x87 两臂差分**（CTest: lift-x87-diff）—— 检查点 2 / D49 的主守卫
//
// 红了意味着什么（判据）：
//   对 `tests/ir/x87_roots.txt` 里的**每一个** x87 op（23 个 = 300 帧直方图里 99.6% 的
//   x87 动态调用），从**同一份受控状态**出发：
//     ① **lifted 臂**（`lift-gen` 生成的 IR，**普通 LLVM 浮点指令 + 状态机**）；
//     ② **解释器臂**（`execFpu` / `execInsn`，语义真源）；
//   逐位比较：8 个 x87 槽（`double` 的**位型**，含 NaN payload / ±0）、`top`、tag word、
//   状态字、控制字、8 个 GPR、EFLAGS、以及一段 guest 内存（前 0x400 字节）。
//   ⇒ 任何一处不同 = 生成的 x87 语义与解释器不同 = **当场红**（不是"跑完不崩"）。
//
// ★ 为什么这是唯一能抓住 x87 抬进 IR 的守卫：300 帧的六项语义指纹（frames / stop / arena /
//   misses）对**数值**不敏感 —— 一个 fsub 操作数写反了，游戏照样跑到同一帧、同一停机点
//   （实测：本轮的 tri 计数差 0.004% 就是这么来的）。只有逐位差分能在数值层面抓住它。
// ★ **自检**（防根集写错 / 防"两臂都没跑"）：跑之前用解释器的解码器断言起点助记符包含
//   期望的 op 名；并断言两条臂都**真的执行了 ≥1 条**指令、且条数相等。
// ★ 状态由 `tests/ir/x87_roots.txt` 的顺序与 `kCases` **一一对应**（数量不等 ⇒ 硬错）。
// ★ 依赖：`AGE.EXE`（执行源）+ clang（编译生成物）。缺任何一个 ⇒ `LIFT_SKIP(77)`
//   （与 `lift-ir-gen-*` 同一套处置）。
//
// ⚠ 口径（如实登记）：本夹具比的是**两臂之间**，不是"与真机"。D49 明写"不追 80 位逐位一致"
//   ⇒ 这里绿只证明"生成代码 == 解释器模型"，模型本身与真机的偏差见 `fpu_bridge.h` 文件头。
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

// ★ S7（D53）：`lift_dyn` 的派发表查找（生成物里若含间接 `jmp`/`call` 站点就要它）——
//   与 AOT 臂（`aot_run.cpp`）同一个来源，⛔ 不另建第二张表。
lift::LiftFn lookupForTest(std::uint32_t ea) { return lift::aot::lookupUncounted(ea); }
// 受控数据区：GPR 都指进来 ⇒ 操作数里的 `[reg+disp]` 落在**已提交**页上（否则两臂都会碰到
// 未提交页：解释器读 0、生成代码 AV）。256 KiB 足够覆盖本根集里最大的 `disp`（0x4E0）。
constexpr std::uint32_t kScratch = 0x00440000u;
constexpr std::uint32_t kScratchBytes = 0x00040000u;
constexpr std::uint32_t kMemProbe = 0x00000400u;  // 内存比对面：scratch 的前 1 KiB

struct X87Case {
    std::uint32_t ea;
    const char* op;  // 期望的起点助记符（自检用；⛔ 不是判据的全部——只是"EA 没写错"）
};

// ★ 与 `tests/ir/x87_roots.txt` 的行序**一一对应**（顺序也一致，便于对照）。
// ⚠ 未覆盖的 4 个 op（`fsub` / `fsubp` / `fsubrp` / `fcom`）与其余稀有 op 的理由写在
//   `tests/ir/x87_roots.txt` 的头部（语料 `.lst` 来自另一份 dump，那些站点在本执行源里没有 raw 字节）。
constexpr X87Case kCases[] = {
    {0x41829C, "fld"},    {0x486412, "fstp"},   {0x40584D, "fst"},    {0x4184E5, "fmul"},
    {0x40469B, "fmulp"},  {0x4184DF, "fadd"},   {0x408120, "faddp"},  {0x430AE6, "fsubr"},
    {0x404660, "fdiv"},   {0x4184FB, "fdivp"},  {0x430BA6, "fdivr"},  {0x4035E7, "fxch"},
    {0x4035A2, "fnstsw"}, {0x41C350, "fild"},   {0x4046B5, "fistp"},  {0x40811E, "fchs"},
    {0x42CEEF, "fabs"},   {0x412685, "fcomp"},  {0x40584B, "fldz"},   {0x418299, "fld1"},
    {0x4046B2, "fldcw"},  {0x4081F4, "fnstcw"},
};
constexpr std::size_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

// 一臂 = 一台独立的 Machine（⛔ 不复用：两臂必须从同一份**干净**状态出发）
struct Arm {
    lift::Machine m;
    std::string err;
    bool ok = false;
    bool init(const std::string& pe_path) {
        if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
        lift::PeImage image;
        if (!lift::loadPeFile(pe_path, &image, &err)) return false;
        if (!lift::mapPeImage(m.mem(), image, &err)) return false;
        std::vector<std::uint8_t> zero(kScratchBytes, 0);
        if (!m.placeData(kScratch, zero.data(), zero.size(), &err)) return false;
        ok = true;
        return true;
    }
};

void setState(Arm& a) {
    lift::X86& cpu = a.m.cpu();
    // ---- x87：8 个槽都 valid，值互不相同、覆盖正/负/零/小数（`tag = 0` ⇒ 每槽 00 = valid）----
    const double vals[8] = {1.5, -2.25, 4.0, 0.0, -0.5, 1024.0, 3.5, -7.75};
    for (int i = 0; i < 8; ++i) cpu.fpu.regs[i] = vals[i];
    cpu.fpu.top = 0;
    cpu.fpu.tag = 0x0000u;      // 全部 valid（2 位/槽，00 = valid）
    cpu.fpu.control = 0x037Fu;  // 缺省控制字（RC = 就近偶数、PC = 80 位）
    cpu.fpu.status = 0;
    // ---- GPR：全部指进已提交数据区（+ 栈指针用夹具自己的栈）----
    const std::uint32_t base = kScratch + 0x200u;
    cpu.eax() = base;
    cpu.ecx() = base;
    cpu.edx() = base;
    cpu.ebx() = base;
    cpu.ebp() = base;
    cpu.esi() = base;
    cpu.edi() = base;
    cpu.esp() = a.m.layout().stack_top - 0x40u;
    a.m.mem().wr32(cpu.esp(), kSentinel);  // 若 snippet 里 `ret`，两臂都会弹到这个哨兵
    cpu.eflags = 0x202u;
    cpu.running = true;
}

// ★ 任务 0（D55 的裁决：补回 OE / UE / ES）的第二批受控状态。
//   同一批 op、同一套 GPR / 栈 / 内存，只把 8 个 x87 槽换成"必然溢出 / 必然下溢"的量：
//     mode 0（big）：1e308 ⇒ `fadd`/`faddp`/`fmul`/`fmulp` 上溢到 +inf（**OE**）；
//                    `fistp` 越界（IE + ES）；`fld` 压满栈（栈上溢 = IE + **ES**）。
//     mode 1（small）：st(0)=1e-308、st(1)=1e8 ⇒ `fmul` 1e-292（正常）、`fdiv` = 1e-316（**UE**，次正规）、
//                    `fdivr` = 1e8/1e-308 = +inf（**OE**）。⛔ 不要放 0 ⇒ 避免把 ZE 的既有两臂差异
//                    （`fdivp` 的 ZE 只有解释器置）拉进这条差分。
//   ⇒ 判据仍是"两臂状态字逐位相同"，另外**要求 OE / UE / ES 至少各被置过一次**（见 main）：
//     否则两臂都恒 0 也会绿，那条差分就退化成恒真。
void setStateExtreme(Arm& a, int mode) {
    lift::X86& cpu = a.m.cpu();
    const double big[8] = {1e308, 1e308, 1e308, 1e308, -1e308, 1e308, 1e308, 1e308};
    const double small[8] = {1e-308, 1e8, 1e-308, 1e8, -1e-308, 1e8, 1e-308, 1e8};
    const double* vals = (mode == 0) ? big : small;
    for (int i = 0; i < 8; ++i) cpu.fpu.regs[i] = vals[i];
    cpu.fpu.top = 0;
    cpu.fpu.tag = 0x0000u;  // 全部 valid ⇒ `fld` 走栈上溢（IE + ES）
    cpu.fpu.control = 0x037Fu;
    cpu.fpu.status = 0;
    const std::uint32_t base = kScratch + 0x200u;
    cpu.eax() = base;
    cpu.ecx() = base;
    cpu.edx() = base;
    cpu.ebx() = base;
    cpu.ebp() = base;
    cpu.esi() = base;
    cpu.edi() = base;
    cpu.esp() = a.m.layout().stack_top - 0x40u;
    a.m.mem().wr32(cpu.esp(), kSentinel);
    cpu.eflags = 0x202u;
    cpu.running = true;
}

// 只在"会溢出 / 会下溢"的 op 上跑极端状态（其余 op 与 OE/UE/ES 无关，跑只是多花时间）。
bool extremeWorth(const char* op) {
    static const char* const kOps[] = {"fld", "fmul", "fmulp", "fadd", "faddp",
                                       "fsubr", "fdiv", "fdivp", "fdivr", "fistp"};
    for (const char* k : kOps)
        if (std::strcmp(op, k) == 0) return true;
    return false;
}

struct StateSnap {
    double regs[8] = {};
    std::uint8_t top = 0;
    std::uint16_t tag = 0;
    std::uint16_t status = 0;
    std::uint16_t control = 0;
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t eip = 0;
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
    for (int i = 0; i < 8; ++i) s.r[i] = cpu.r[i];
    s.eflags = cpu.eflags;
    s.eip = cpu.eip;
    const std::uint8_t* p = a.m.mem().host(kScratch);
    s.mem.assign(p, p + kMemProbe);
    return s;
}

// 逐位比较（`double` 用位型比：±0 / NaN payload 都要一致）
void compare(const char* tag, const StateSnap& lifted, const StateSnap& interp) {
    for (int i = 0; i < 8; ++i) {
        std::uint64_t lb = 0;
        std::uint64_t ib = 0;
        std::memcpy(&lb, &lifted.regs[i], 8);
        std::memcpy(&ib, &interp.regs[i], 8);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>((lb >> 32) ^ (ib >> 32)), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lb ^ ib), 0u);
        if (lb != ib)
            lift_test::failf(__FILE__, __LINE__, "[%s] st(%d) bits lifted=%016llX interp=%016llX", tag, i,
                             static_cast<unsigned long long>(lb), static_cast<unsigned long long>(ib));
    }
    LIFT_CHECK_EQ_U32(lifted.top, interp.top);
    LIFT_CHECK_EQ_U32(lifted.tag, interp.tag);
    LIFT_CHECK_EQ_U32(lifted.status, interp.status);
    LIFT_CHECK_EQ_U32(lifted.control, interp.control);
    for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(lifted.r[i], interp.r[i]);
    LIFT_CHECK_EQ_U32(lifted.eflags, interp.eflags);
    // ⚠ `eip` 不比：生成代码不改 `cpu.eip`（只有 `lift_shim` 会写它）。两条臂的"下一个 guest EA"
    //   用下面那条**显式**比较（`lifted_next_ea` vs 解释器的 `eip`）。
    if (lifted.mem != interp.mem)
        lift_test::failf(__FILE__, __LINE__, "[%s] guest 内存区前 %u 字节不同", tag,
                         static_cast<unsigned>(kMemProbe));
    ++lift_test::g_checks;
}

// 起点助记符自检：EA 处解码出来的 mnemonic 必须包含期望的 op 名（防根集写错 ⇒ 假绿）
bool checkOpAt(Arm& a, const X87Case& c) {
    lift::Decoder dec;
    lift::Insn in;
    std::string why;
    if (!dec.decode(a.m.mem(), c.ea, &in, &why)) {
        lift_test::failf(__FILE__, __LINE__, "[%s] 解码失败 @ %08X：%s", c.op, c.ea, why.c_str());
        return false;
    }
    const char* mn = in.mnemonic != nullptr ? in.mnemonic : "";
    if (std::strncmp(mn, c.op, std::strlen(c.op)) != 0) {
        const std::uint8_t* p = a.m.mem().host(c.ea);
        char hex[32] = {};
        for (int i = 0; i < 8; ++i) std::snprintf(hex + 2 * i, 3, "%02X", p[i]);
        lift_test::failf(__FILE__, __LINE__, "@ %08X 的助记符是 \"%s\"（len=%u，字节 %s），期望 \"%s\"",
                         c.ea, mn, static_cast<unsigned>(in.length), hex, c.op);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    (void)argc;
    (void)argv;

#if !defined(LIFT_X87_DIFF)
    std::printf("lift-x87-diff: 没有生成物（缺 clang / 缺 AGE.EXE）⇒ SKIP\n");
    return 77;
#else

    // 执行源路径由 CMake 传进来（`LIFT_X87_PE_PATH`）：⛔ 不在测试里拼相对路径猜仓库布局。
    const std::string pe_path = LIFT_X87_PE_PATH;
    // S7：装上 `lift_dyn` 的派发表查找（这份生成物里若有间接跳转站点，它会用到）。
    lift::liftDynLookup = &lookupForTest;
    std::printf("lift-x87-diff: 用例 %zu 个（根集 tests/ir/x87_roots.txt，执行源 %s）\n", kCaseCount,
                pe_path.c_str());
    // 根集与用例表必须一一对应（数量不等 = 有人只改了其中一份）
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::aot::tableSize() >= kCaseCount ? 1u : 0u), 1u);

    std::uint32_t covered = 0;
    // ---- 任务 0 的读数：OE / UE / ES 各被**两臂同时**置过几次（≥1 才允许这条差分绿）----
    std::uint32_t oe_seen = 0;
    std::uint32_t ue_seen = 0;
    std::uint32_t es_seen = 0;
    // `mode < 0` = 基线受控状态；`mode >= 0` = 任务 0 的极端状态（见 `setStateExtreme`）。
    auto runCase = [&](const X87Case& c, int mode) {
        char tag_buf[80];
        if (mode < 0)
            std::snprintf(tag_buf, sizeof(tag_buf), "%s@%08X", c.op, c.ea);
        else
            std::snprintf(tag_buf, sizeof(tag_buf), "%s@%08X/x%d", c.op, c.ea, mode);
        const char* tag = tag_buf;
        g_current_case = tag;

        // ---- 两臂：各自一台干净 Machine + 同一份受控状态 ----
        Arm li;
        Arm ia;
        if (!li.init(pe_path) || !ia.init(pe_path)) {
            lift_test::failf(__FILE__, __LINE__, "[%s] 夹具初始化失败：%s / %s", tag, li.err.c_str(),
                             ia.err.c_str());
            g_current_case = "(none)";
            return;
        }
        if (!checkOpAt(ia, c)) {
            g_current_case = "(none)";
            return;
        }
        if (mode < 0) {
            setState(li);
            setState(ia);
        } else {
            setStateExtreme(li, mode);
            setStateExtreme(ia, mode);
        }

        // ---- ① lifted 臂：把状态搬进 `%cpu`（`liftRegsToCpu` 只搬 8 GPR + eflags），然后跑 ----
        LiftRegs regs{};
        lift::liftCpuToRegs(li.m.cpu(), regs);
        std::uint8_t* mem = li.m.mem().host(0);
        const std::uint64_t insns0 = li.m.cpu().insns;
        std::uint32_t ea = c.ea;
        int dispatches = 0;
        while (true) {
            lift::LiftFn fn = lift::aot::lookup(ea, nullptr);
            if (fn == nullptr) break;  // 回到表外 EA ⇒ 与解释器的 max_insns 口径对齐
            if (++dispatches > 64) break;
            ea = fn(&regs, mem, ea, &li.m.cpu());
            if (!li.m.cpu().running) break;
        }
        lift::liftRegsToCpu(regs, li.m.cpu());  // 把 8 GPR + eflags 搬回（x87 状态本来就在 %cpu 里）
        const std::uint32_t lifted_next_ea = ea;
        const bool stopped_on_unknown_ea = (lift::aot::lookup(ea, nullptr) == nullptr);
        const std::uint64_t lifted_insns = li.m.cpu().insns - insns0;
        LIFT_CHECK(lifted_insns >= 1u);  // "两臂都没跑"必须红

        // ---- ② 解释器臂：同一份状态，跑**同样条数**（口径 = `X86::insns`，两臂同一份语义）----
        const std::uint64_t interp0 = ia.m.cpu().insns;
        lift::RunOptions o;
        o.max_insns = interp0 + lifted_insns;
        ia.m.cpu().eip = c.ea;
        (void)ia.m.run(o);
        const std::uint64_t interp_insns = ia.m.cpu().insns - interp0;
        // = 条数必须一致（否则比的是"不同时刻的状态"，那种绿没有意义）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lifted_insns), static_cast<std::uint32_t>(interp_insns));

        // ---- ③ 逐位比较（状态字含 OE/UE/ES；两臂不同 ⇒ 当场红）----
        const StateSnap sl = snap(li);
        const StateSnap si = snap(ia);
        compare(tag, sl, si);
        // 两条臂的"下一条 guest EA"必须一致（只在"回到表外 EA"这一种收尾下有意义；预算收尾跳过）
        if (stopped_on_unknown_ea) LIFT_CHECK_EQ_U32(lifted_next_ea, ia.m.cpu().eip);
        // ---- 任务 0：数一遍"这条用例真的把位置起来了吗"（只看被比较过的两臂状态字）----
        if ((sl.status & 0x0008u) != 0 && (si.status & 0x0008u) != 0) ++oe_seen;
        if ((sl.status & 0x0010u) != 0 && (si.status & 0x0010u) != 0) ++ue_seen;
        if ((sl.status & 0x0080u) != 0 && (si.status & 0x0080u) != 0) ++es_seen;
        ++covered;
        g_current_case = "(none)";
    };

    for (const X87Case& c : kCases) runCase(c, /*mode=*/-1);
    // ★ 任务 0（D55 裁决）：同一批 op 在"必然溢出 / 必然下溢"的状态下再比一遍。
    //   ⛔ 只跑与 OE/UE/ES 有关的 op（见 `extremeWorth`）：别的 op 对这些位没有语义。
    for (const X87Case& c : kCases) {
        if (!extremeWorth(c.op)) continue;
        runCase(c, /*mode=*/0);  // big ⇒ OE（fistp 越界 = IE+ES；fld 压满栈 = IE+ES）
        runCase(c, /*mode=*/1);  // small ⇒ UE / OE（见 `setStateExtreme`）
    }
    std::printf("lift-x87-diff: 覆盖 %u/%zu 个 op（含极端状态批次）\n", covered, kCaseCount);
    // ★ 这条判据防"两臂都恒 0 也算绿"：状态字的 OE / UE / ES 必须各自真的被置起来过。
    std::printf("lift-x87-diff: 任务 0 读数 —— 状态字置位：OE %u 次 · UE %u 次 · ES %u 次（两臂同时）\n",
                oe_seen, ue_seen, es_seen);
    LIFT_CHECK(oe_seen >= 1u);  // OE 从没被置过 ⇒ 这条差分没有覆盖到它
    LIFT_CHECK(ue_seen >= 1u);  // UE 同上
    LIFT_CHECK(es_seen >= 1u);  // ES 同上
    return finish("lift-x87-diff");
#endif
}
