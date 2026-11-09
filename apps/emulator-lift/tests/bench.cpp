// emulator-lift · 吞吐基准（pure：自造字节块；S8「bench 有数」）
//
// ★ 为什么不用 `emulator-lift --bench N` 当基准：真实镜像会在 36k 条左右停在 M2 的窗口族
//   （`USER32!LoadIconA`），N 再大也跑不完 ⇒ 测出来的是"到窗口之前"的吞吐，不是核心吞吐。
//   这里用一段**紧凑循环**（add / cmp / jb），只测解释器 + 块缓存的分发成本。
// ★ 判据不是"跑到多少 MIPS"（那会随机器变），而是：**真的跑了 N 条**、且数字能打出来。
//   数字进 `pnpm tools lift report --bench N` 的报告（S8）。⛔ 不设性能门槛 = 不给假绿。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;

// 循环体（N 次）：add eax,1; cmp eax,N; jb loop; ret
std::vector<std::uint8_t> makeLoop(std::uint32_t n) {
    std::vector<std::uint8_t> b;
    auto u32 = [&b](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    };
    // xor eax,eax
    b.push_back(0x31);
    b.push_back(0xC0);
    const std::uint32_t loop = kCodeEa + static_cast<std::uint32_t>(b.size());
    // add eax,1
    b.push_back(0x83);
    b.push_back(0xC0);
    b.push_back(0x01);
    // cmp eax,imm32
    b.push_back(0x3D);
    u32(n);
    // jb loop（0F 82 rel32）
    b.push_back(0x0F);
    b.push_back(0x82);
    const std::uint32_t at = kCodeEa + static_cast<std::uint32_t>(b.size());
    u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(at + 4)));
    // ret
    b.push_back(0xC3);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    std::uint32_t n = 5000000;
    // ★ `--profile`（可选）：给 `docs/03-emulator/measurement.md` M-02 的**无安装 A/B** 用。
    //   它只开"块命中 + 块墙钟 + shim 墙钟"那三笔账；⛔ 这不是性能门槛，读数只作两臂对照。
    bool profile = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--profile") == 0) {
            profile = true;
            continue;
        }
        n = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 10));
    }

    lift::Machine m;
    std::string err;
    if (!m.initBare(&err) || !m.prepareStack(&err)) {
        std::fprintf(stderr, "bench setup: %s\n", err.c_str());
        return 1;
    }
    const std::vector<std::uint8_t> code = makeLoop(n);
    if (!m.placeCode(kCodeEa, code.data(), code.size(), &err)) {
        std::fprintf(stderr, "bench placeCode: %s\n", err.c_str());
        return 1;
    }
    m.cpu().eip = kCodeEa;

    lift::RunOptions o;
    o.until_ea = 0xFFFFFFFFu;
    o.max_insns = static_cast<std::uint64_t>(n) * 4ull + 1000ull;
    o.profile = profile;
    const auto t0 = std::chrono::steady_clock::now();
    const lift::RunResult r = m.run(o);
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    g_current_case = "bench";
    LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(m.cpu().eax(), n);
    LIFT_CHECK(r.insns >= static_cast<std::uint64_t>(n) * 3ull);

    const double mips = secs > 0 ? static_cast<double>(r.insns) / secs / 1e6 : 0.0;
    std::printf("bench     : %llu insns in %.3f s = %.2f MIPS（循环 %u 次；纯档自造字节）\n",
                (unsigned long long)r.insns, secs, mips, n);
    // ★ 口径标签（M-02）：`--profile` 两臂必须能被一眼分开，⛔ 不许把两臂的 ms/M 混进同一张表。
    m.dumpRunSummary(r, stdout);
    if (profile) m.dumpProfile(stdout, 12);
    return finish("bench");
}
