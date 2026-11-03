// emulator-lift · boot trace（A4；assets 档）
//
// ★ 判据（plan §5 A4）：跑到 L2（`--until ea 0x4BA890`）成功；**同一条命令跑两次，trace 逐字节相同**。
//   这里不打印整条 trace（30 万条会淹掉输出），而是把"可复现"折成三个机械可比的量：
//     ① 停下来的原因与 eip；
//     ② 执行过的指令数；
//     ③ 最终寄存器 / 标志 / x87 元信息 + 已提交内存摘要的哈希。
//   三个量两次完全相同 ⇒ 虚拟时钟与"宿主侧不许引入随机源"（plan §8.3）这两条成立。
// ★ 如果没到 L2，本测试**红**并打印卡在哪个 EA —— 那是诚实的结果，不是"跳过"。
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"

namespace {

struct Outcome {
    lift::StopReason reason = lift::StopReason::None;
    std::uint32_t ea = 0;
    std::uint64_t insns = 0;
    std::uint64_t hash = 0;
    std::string detail;
    bool ok = false;
};

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

Outcome runOnce(const char* exe, const char* lst_path, std::uint32_t target, std::uint64_t max_insns) {
    Outcome out;
    lift::Machine m;
    std::string err;
    if (!m.init(&err) || !m.loadImage(exe, &err) || !m.loadLst(lst_path, &err) || !m.prepare(0, &err)) {
        out.detail = "setup: " + err;
        return out;
    }
    lift::RunOptions o;
    o.until_ea = target;
    o.max_insns = max_insns;
    const lift::RunResult r = m.run(o);
    out.reason = r.reason;
    out.ea = r.ea;
    out.insns = r.insns;
    out.detail = r.detail;

    std::uint64_t h = 0xCBF29CE484222325ull;
    const lift::X86& cpu = m.cpu();
    h = fnv(h, cpu.r, sizeof(cpu.r));
    h = fnv(h, &cpu.eip, sizeof(cpu.eip));
    h = fnv(h, &cpu.eflags, sizeof(cpu.eflags));
    h = fnv(h, &cpu.fpu.top, sizeof(cpu.fpu.top));
    h = fnv(h, &cpu.fpu.control, sizeof(cpu.fpu.control));
    h = fnv(h, &cpu.fpu.status, sizeof(cpu.fpu.status));
    h = fnv(h, cpu.fpu.regs, sizeof(cpu.fpu.regs));
    const std::uint32_t heap_used = m.heap().bytesInUse();
    const std::uint32_t heap_peak = m.heap().peak();
    h = fnv(h, &heap_used, sizeof(heap_used));
    h = fnv(h, &heap_peak, sizeof(heap_peak));
    out.hash = h;
    out.ok = true;
    return out;
}

}  // namespace

int main() {
    using namespace lift_test;

    const char* exe = std::getenv("LIFT_AGE_EXE");
    const char* lst_path = std::getenv("LIFT_AGE_LST");
    if (!exe || !*exe) LIFT_SKIP("没有 LIFT_AGE_EXE");
    if (!lst_path || !*lst_path) LIFT_SKIP("没有 LIFT_AGE_LST");

    const std::uint32_t kWinMain = 0x004BA890u;  // plan §6 S7 的 L2
    const std::uint64_t kMaxInsns = 200000000ull;

    g_current_case = "run1";
    const Outcome a = runOnce(exe, lst_path, kWinMain, kMaxInsns);
    if (!a.ok) {
        std::fprintf(stderr, "setup 失败：%s\n", a.detail.c_str());
        return 1;
    }
    std::printf("run1: reason=%s eip=%08X insns=%llu hash=%016llX\n", lift::stopReasonName(a.reason), a.ea,
                (unsigned long long)a.insns, (unsigned long long)a.hash);
    if (!a.detail.empty()) std::printf("run1: detail=%s\n", a.detail.c_str());

    g_current_case = "run2";
    const Outcome b = runOnce(exe, lst_path, kWinMain, kMaxInsns);
    std::printf("run2: reason=%s eip=%08X insns=%llu hash=%016llX\n", lift::stopReasonName(b.reason), b.ea,
                (unsigned long long)b.insns, (unsigned long long)b.hash);

    // ① 可复现（这一条与环境无关，永远该绿）
    g_current_case = "determinism";
    LIFT_CHECK(a.reason == b.reason);
    LIFT_CHECK_EQ_U32(a.ea, b.ea);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.insns), static_cast<std::int64_t>(b.insns));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.hash), static_cast<std::int64_t>(b.hash));

    // ② 到达 L2
    g_current_case = "reaches-L2";
    if (a.reason != lift::StopReason::UntilEa || a.ea != kWinMain) {
        std::fprintf(stderr,
                     "A4 红：没到 L2（_WinMain@16 = 0x%08X）。停在 %s @ eip=%08X，已执行 %llu 条。\n"
                     "  detail: %s\n",
                     kWinMain, lift::stopReasonName(a.reason), a.ea, (unsigned long long)a.insns, a.detail.c_str());
    }
    LIFT_CHECK(a.reason == lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(a.ea, kWinMain);

    return finish("boot-trace");
}
