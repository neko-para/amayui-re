// emulator-lift · 纯档 conformance 的 runner（判据 A1）
//
// 夹具本体在 `conformance_cases.cpp`。这里只做三件事：
//   ① 每条用例前后各拍一次全状态快照，**除哨兵 ret 的 esp+4 外完全相同 = 恒等夹具 = 红**（plan §8.2）；
//   ② `verify` 里必须至少有一次断言（否则"什么都没验"也是红）；
//   ③ 汇总失败数并给出可复现的用例名。
// ★ `LIFT_CONFORMANCE_SELFCHECK=1` 时额外跑一条**负控**（空心夹具必须只因①红）——
//   那是本判据"会红"的机械证据（CTest：`conformance-rejects-hollow-case`）。
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "harness.h"

namespace lift_test {

// 恒等判据的失败计数（单独记：负控要断言"红的原因**只**是这条判据"）。
int g_identity_failures = 0;

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

bool Fixture::Snapshot::sameStateIgnoringSentinelRet(const Snapshot& before) const {
    // ★ `eip` **不**参与比较：`run()` 的终点是哨兵 `kSentinelEa`，而快照是在 setup 之后、
    //   run 之前拍的（那时 eip = 第一条指令的 EA）⇒ 前后 eip **必然**不同。
    //   把它算进"恒等"只会让这条判据多一个恒假的输入（`harness.h` 列的"全状态"也不含 eip）。
    // ★ 哨兵 `ret` 的唯一合法副作用 = **esp 恰好 +4**（弹出哨兵）。实测空心夹具（setup 只有一条
    //   `C3`）跑完只有 esp 变：6FFFFFFC → 70000000，而 memHash / x87Hash / xmmHash / eflags /
    //   mxcsr 全不变（栈页本来就被清零，弹出的 4 字节写回去还是 0）⇒ 早先"memHash 必变"的说法是错的。
    //   esp 在这里**单独**检查：净 0 / ±8 都说明夹具自己动了栈，仍算"状态变了"。
    // ⛔ 不许把 memHash / xmmHash 移出比较：那会让"只改内存 / 只改 XMM"的恒等夹具静默通过。
    if (eflags != before.eflags || memHash != before.memHash || x87Hash != before.x87Hash) return false;
    if (mxcsr != before.mxcsr || xmmHash != before.xmmHash) return false;
    for (int i = 0; i < 8; ++i) {
        if (i == lift::kEsp) continue;  // esp **单独**判（哨兵 ret 的 +4），不参与"逐位相等"
        if (r[i] != before.r[i]) return false;
    }
    return r[lift::kEsp] == before.r[lift::kEsp] + 4u;
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
        if (f.snapshot().sameStateIgnoringSentinelRet(before)) {
            // ★ 单独计数：负控要断言"红的原因**只**是这条判据"（见 main 的 LIFT_CONFORMANCE_SELFCHECK）。
            ++g_identity_failures;
            failf(__FILE__, __LINE__,
                  "夹具前后状态完全相同（只差哨兵 ret 的 esp+4）—— 恒等夹具（代码没执行 / expected 抄成了 actual）");
            continue;
        }
        (void)checks_before_all;
        ++ran;
    }
    return g_failures;
}

namespace {

// ---- 负控夹具（`LIFT_CONFORMANCE_SELFCHECK=1`）----
// ★ 必须是"**能到哨兵但什么都没改**"的形状：字面"setup/verify 都空"的夹具会先红在别处
//   （`run did not reach the sentinel` / `verify 里一次断言都没有`）⇒ 那证明不了恒等判据会红。
//   这条：setup 只放一条 `ret`（run 直接到哨兵），verify 一条恒真断言。
void setup_hollow(Fixture& f) { f.codeHex(kCodeEa, "C3"); }
void verify_hollow(Fixture& f) { LIFT_CHECK_EQ_U32(f.cpu().eax(), 0u); }

const Case kHollowCase = {"hollow-fixture", setup_hollow, verify_hollow};

}  // namespace

// 负控：`real_failures` = 真用例表的失败数（必须 0）。再跑空心夹具，要求它**只**因恒等判据红
// —— 总失败恰好 +1、identity 恰好 +1。返回 true = 自检通过。
bool runIdentitySelfCheck(int real_failures) {
    const int before_failures = g_failures;
    const int before_identity = g_identity_failures;
    runCases(&kHollowCase, 1);
    const int total_delta = g_failures - before_failures;
    const int identity_delta = g_identity_failures - before_identity;
    std::printf("conformance-selfcheck: hollow fixture red by identity=%d\n", identity_delta);
    return real_failures == 0 && total_delta == 1 && identity_delta == 1;
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
    // ★ 负控：`LIFT_CONFORMANCE_SELFCHECK=1` ⇒ 先要求真用例表 **0 红**，再要求空心夹具只因恒等判据红
    //   （总失败 +1 / identity +1）。把 esp 规范化改回 `== before` 时这条会红 —— 那就是"判据会红"的证据。
    const char* selfcheck = std::getenv("LIFT_CONFORMANCE_SELFCHECK");
    if (selfcheck && *selfcheck && std::strcmp(selfcheck, "0") != 0) {
        if (!lift_test::runIdentitySelfCheck(failures)) {
            std::fprintf(stderr, "conformance-selfcheck: 负控失败（空心夹具没有被恒等判据判红）\n");
            return 1;
        }
        return 0;
    }
    return failures == 0 ? 0 : 1;
}
