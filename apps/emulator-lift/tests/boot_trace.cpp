// emulator-lift · boot trace（`LIFT_STRESS` 档：跑真游戏，判据 = 到达 L2 + 输入清单可比）
//
// ★ 判据（plan §5 A4 的原判据已随虚拟时钟一起移除，2026-11-27）：跑到 L2（`--until ea 0x4BA890`）
//   成功；两次运行的**输入清单**（exe 路径串 / guest 命令行 / 环境块）相同。
//   ⚠ 时钟 = **墙钟**（唯一档）⇒ `insns` / trace 字节 / 状态 hash **不承诺跨次相同**，⛔ 不作判据。
//   （旧判据"同一条命令跑两次 trace 逐字节相同"依赖虚拟时钟，已删除。）
// ★ 2026-11-13 瘦身：4 次 run → 2 次（两次都带 shim trace）；trace 文件仍写出，只作诊断。
// ★ 如果没到 L2，本测试**红**并打印卡在哪个 EA —— 那是诚实的结果，不是"跳过"。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "corpus/lst_index.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"

namespace {

struct Outcome {
    lift::StopReason reason = lift::StopReason::None;
    std::uint32_t ea = 0;
    std::uint64_t insns = 0;
    std::uint64_t hash = 0;
    std::uint64_t inputs_hash = 0;  // 输入清单的指纹（与"执行结果"分开记）
    std::string detail;
    bool ok = false;
};

std::uint64_t fnv(std::uint64_t h, const void* p, std::size_t n) {    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

Outcome runOnce(const char* exe, const char* lst_path, std::uint32_t target, std::uint64_t max_insns,
                std::FILE* trace_out = nullptr) {    Outcome out;
    lift::Machine m;
    std::string err;
    if (!m.init(&err) || !m.loadImage(exe, &err) || !m.loadLst(lst_path, &err) || !m.prepare(0, &err)) {
        out.detail = "setup: " + err;
        return out;
    }
    lift::RunOptions o;
    o.until_ea = target;
    o.max_insns = max_insns;
    o.trace_shims = trace_out != nullptr;
    o.trace_out = trace_out;
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
    // ★ 输入清单（plan §5 A4）：只覆盖输入，**不含**墙钟读数 ⇒ 两次运行可比。
    out.inputs_hash = m.traceInputsHash();
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

    // ★ 2 次 run（两次**都带** shim trace）。2026-11-13：原来还有两次不带 trace 的 run，判据完全相同
    //   —— 那是把同一件事做两遍（4 run → 2 run）；trace 字节与 hash 在这一对 run 上一起比。
    // ★ 文件名必须带进程号（2026-11-04 实测）：固定名 `lift-boot-trace-a.log` 在两份 ctest
    //   （并行构建目录 / 两个代理同时跑）**互相撞名** ⇒ 一个 `fopen` 失败 ⇒ 假红。
    //   `file_io.cpp` 早就是这么做的（`-<pid>`），这里跟上。
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const std::string suffix = "-" + std::to_string(pid) + ".log";
    const std::filesystem::path ta =
        std::filesystem::temp_directory_path() / ("lift-boot-trace-a" + suffix);
    const std::filesystem::path tb =
        std::filesystem::temp_directory_path() / ("lift-boot-trace-b" + suffix);
    auto utf8 = [](const std::filesystem::path& p) {
        const std::u8string s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    };
    std::FILE* fa = lift::openHostFileUtf8(utf8(ta), "wb");
    std::FILE* fb = lift::openHostFileUtf8(utf8(tb), "wb");
    LIFT_CHECK(fa != nullptr);
    LIFT_CHECK(fb != nullptr);
    if (fa == nullptr || fb == nullptr) {
        if (fa != nullptr) std::fclose(fa);
        if (fb != nullptr) std::fclose(fb);
        return 1;
    }

    g_current_case = "run1";
    const Outcome a = runOnce(exe, lst_path, kWinMain, kMaxInsns, fa);
    std::fclose(fa);
    g_current_case = "run2";
    const Outcome b = runOnce(exe, lst_path, kWinMain, kMaxInsns, fb);
    std::fclose(fb);
    if (!a.ok || !b.ok) {
        std::fprintf(stderr, "setup 失败：run1=%s run2=%s\n", a.detail.c_str(), b.detail.c_str());
        std::remove(utf8(ta).c_str());
        std::remove(utf8(tb).c_str());
        return 1;
    }
    std::printf("run1: reason=%s eip=%08X insns=%llu hash=%016llX inputs=%016llX\n", lift::stopReasonName(a.reason),
                a.ea, (unsigned long long)a.insns, (unsigned long long)a.hash, (unsigned long long)a.inputs_hash);
    if (!a.detail.empty()) std::printf("run1: detail=%s\n", a.detail.c_str());
    std::printf("run2: reason=%s eip=%08X insns=%llu hash=%016llX inputs=%016llX\n", lift::stopReasonName(b.reason),
                b.ea, (unsigned long long)b.insns, (unsigned long long)b.hash, (unsigned long long)b.inputs_hash);

    // ★ 输入清单：两次运行必须看到同一组输入（`--exe` 的路径串 / 命令行 / 环境块）。
    //   ⚠ 时钟 = 墙钟 ⇒ 执行结果（insns / trace 字节 / hash）**不承诺**两次相同，不在这里断言。
    g_current_case = "same-inputs";
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.inputs_hash), static_cast<std::int64_t>(b.inputs_hash));

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

    // ③ trace 确实产出了内容（⛔ 不断言两次相同：墙钟档下 trace 字节不作判据）。
    g_current_case = "trace-produced";
    {
        std::ifstream in_a(ta, std::ios::binary);
        std::ifstream in_b(tb, std::ios::binary);
        const std::string sa((std::istreambuf_iterator<char>(in_a)), std::istreambuf_iterator<char>());
        const std::string sb((std::istreambuf_iterator<char>(in_b)), std::istreambuf_iterator<char>());
        std::printf("trace: run1=%zu bytes run2=%zu bytes\n", sa.size(), sb.size());
        LIFT_CHECK(!sa.empty());  // 空 trace 不能算"跑过"
        LIFT_CHECK(!sb.empty());
    }
    std::remove(utf8(ta).c_str());
    std::remove(utf8(tb).c_str());

    return finish("boot-trace");
}
