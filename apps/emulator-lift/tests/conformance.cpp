// emulator-lift · 纯档 conformance 的 runner（判据 A1）
//
// 夹具本体在 `conformance_cases.cpp`。这里只做三件事：
//   ① 每条用例前后各拍一次全状态快照，**前后相同 = 恒等夹具 = 红**（plan §8.2）；
//   ② `verify` 里必须至少有一次断言（否则"什么都没验"也是红）；
//   ③ 汇总失败数并给出可复现的用例名。
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"

namespace lift_test {

namespace {

std::uint64_t fnv1a(const void* data, std::size_t n, std::uint64_t h = 0xCBF29CE484222325ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseHex(const char* hex, std::vector<std::uint8_t>* out) {
    out->clear();
    int hi = -1;
    for (const char* p = hex; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',' || *p == '_') continue;
        const int v = hexNibble(*p);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return hi < 0;
}

}  // namespace

bool Fixture::Snapshot::operator==(const Snapshot& o) const {
    // ★ `eip` **不**参与比较：`run()` 的终点是哨兵 `kSentinelEa`，而快照是在 setup 之后、
    //   run 之前拍的（那时 eip = 第一条指令的 EA）⇒ 前后 eip **必然**不同。
    //   把它算进"恒等"只会让这条判据多一个恒假的输入（`harness.h` 列的"全状态"也不含 eip）。
    // ⚠ 已知残留：夹具末尾的 `ret` 会把哨兵弹掉 ⇒ **esp 也必然 +4**，所以"恒等夹具"这条判据
    //   实际上仍然**几乎**恒假（实测：把 `xmmHash` 从比较里去掉，236 条用例一条都不红）。
    //   ⇒ 要真守住"夹具什么都没做"，得让 runner 把哨兵 `ret` 的栈效果排除掉 —— 那是另一条单；
    //   本单的 14 条 SSE 形态**不依赖**它：每条都在 `verify` 里显式断言 XMM / MXCSR。
    if (eflags != o.eflags || memHash != o.memHash || x87Hash != o.x87Hash) return false;
    if (mxcsr != o.mxcsr || xmmHash != o.xmmHash) return false;
    for (int i = 0; i < 8; ++i)
        if (r[i] != o.r[i]) return false;
    return true;
}

bool Fixture::init() {
    machine_ = std::make_unique<lift::Machine>();
    std::string err;
    if (!machine_->initBare(&err)) {
        std::fprintf(stderr, "fixture init failed: %s\n", err.c_str());
        machine_.reset();
        return false;
    }
    if (!machine_->prepareStack(&err)) {
        std::fprintf(stderr, "fixture prepareStack failed: %s\n", err.c_str());
        machine_.reset();
        return false;
    }
    return true;
}

bool Fixture::reset() {
    machine_.reset();
    return init();
}

bool Fixture::code(std::uint32_t ea, const std::vector<std::uint8_t>& bytes) {
    std::string err;
    if (!machine_->placeCode(ea, bytes.data(), bytes.size(), &err)) {
        std::fprintf(stderr, "placeCode(0x%08X) failed: %s\n", ea, err.c_str());
        return false;
    }
    return true;
}

bool Fixture::codeHex(std::uint32_t ea, const char* hex) {
    std::vector<std::uint8_t> bytes;
    if (!parseHex(hex, &bytes)) {
        std::fprintf(stderr, "bad hex: %s\n", hex);
        return false;
    }
    return code(ea, bytes);
}

bool Fixture::dataHex(std::uint32_t ea, const char* hex) {
    std::vector<std::uint8_t> bytes;
    if (!parseHex(hex, &bytes)) {
        std::fprintf(stderr, "bad hex: %s\n", hex);
        return false;
    }
    std::string err;
    if (!machine_->placeData(ea, bytes.data(), bytes.size(), &err)) {
        std::fprintf(stderr, "placeData(0x%08X) failed: %s\n", ea, err.c_str());
        return false;
    }
    return true;
}

bool Fixture::run(std::uint64_t max_insns) {
    lift::RunOptions opt;
    opt.until_ea = kSentinelEa;
    opt.max_insns = max_insns;
    const lift::RunResult r = machine_->run(opt);
    if (r.reason != lift::StopReason::UntilEa && r.reason != lift::StopReason::MaxInsns) {
        std::fprintf(stderr, "run stopped: %s @ eip=0x%08X insns=%llu %s\n", lift::stopReasonName(r.reason),
                     r.ea, (unsigned long long)r.insns, r.detail.c_str());
        return false;
    }
    return true;
}

Fixture::Snapshot Fixture::snapshot() const {
    Snapshot s;
    const lift::X86& c = machine_->cpu();
    for (int i = 0; i < 8; ++i) s.r[i] = c.r[i];
    s.eip = c.eip;
    s.eflags = c.eflags;
    s.x87Hash = fnv1a(&c.fpu.control, sizeof(c.fpu.control));
    s.x87Hash = fnv1a(&c.fpu.status, sizeof(c.fpu.status), s.x87Hash);
    s.x87Hash = fnv1a(&c.fpu.top, 1, s.x87Hash);
    s.x87Hash = fnv1a(c.fpu.regs, sizeof(c.fpu.regs), s.x87Hash);
    s.mxcsr = c.mxcsr;
    s.xmmHash = fnv1a(c.xmm, sizeof(c.xmm));

    std::uint64_t h = 0xCBF29CE484222325ull;
    const lift::GuestMemory& mem = machine_->mem();
    const std::uint32_t ranges[][2] = {
        {kCodeEa, 0x1000u}, {kDataEa, 0x1000u},
        {machine_->layout().stack_top - 0x1000u, 0x1000u},
    };
    for (const auto& rg : ranges) {
        if (mem.committed(rg[0], rg[1])) h = fnv1a(mem.host(rg[0]), rg[1], h);
    }
    s.memHash = h;
    return s;
}

int runCases(const Case* cases, std::size_t count) {
    int ran = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Case& c = cases[i];
        g_current_case = c.name;
        Fixture f;
        if (!f.init()) {
            failf(__FILE__, __LINE__, "fixture init failed");
            continue;
        }
        const int checks_before_all = g_checks;
        c.setup(f);
        const Fixture::Snapshot before = f.snapshot();
        if (!f.run()) {
            failf(__FILE__, __LINE__, "run did not reach the sentinel");
            continue;
        }
        const int checks_after_run = g_checks;
        c.verify(f);
        if (g_checks == checks_after_run) {
            failf(__FILE__, __LINE__, "verify 里一次断言都没有（这条用例什么都没验）");
            continue;
        }
        if (f.snapshot() == before) {
            failf(__FILE__, __LINE__,
                  "夹具前后状态完全相同 —— 恒等夹具（代码没执行 / expected 抄成了 actual）");
            continue;
        }
        (void)checks_before_all;
        ++ran;
    }
    return g_failures;
}

}  // namespace lift_test

int main() {
    std::size_t count = 0;
    const lift_test::Case* cases = lift_test::conformanceCases(&count);
    if (!cases || count == 0) {
        std::fprintf(stderr, "conformance: 用例表是空的（说明夹具没登记）\n");
        return 1;
    }
    const int failures = lift_test::runCases(cases, count);
    std::printf("conformance: %zu cases, %d checks, %d failures\n", count, lift_test::g_checks, failures);
    return failures == 0 ? 0 : 1;
}
