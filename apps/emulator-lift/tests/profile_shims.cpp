// emulator-lift · `--profile` 的宿主 shim 墙钟表（pure：裸 Machine + 一个已知 shim）
//
// ★ 判据：`RunOptions{profile = true}` 跑一次之后，`dumpProfile` 的输出里必须出现
//   `=== profile: shims` 这张表，并且**那个 shim 的行**要带 `calls=1`。
// ★ 为什么这条会红（它守的就是这一行）：`Machine::run` 里是
//     `shims_.setTiming(frame_trace_ || opts.profile);`
//   —— 删掉 `|| opts.profile` 之后 shim 计时关着，`ShimDef::ns` 全是 0，
//   而 shim 表只列 `ns > 0` 的项 ⇒ 表还在、行没了 ⇒ 下面的断言红。
//   （`call_count` 与计时无关，所以**不能**只断言"表里有 calls=1" —— 那样删了开关也照样绿。）
// ★ 反过来也守住"表格式"：标签是 `module!name`、行里有 `calls=`；改坏任一处 ⇒ 红。
//
// ⛔ 不钉任何墙钟数字（ms / us 随机器变），只钉"这张表存在 + 这一行在 + 调用次数对"。
// ⛔ 不依赖游戏 / LFS：自造机器码 + 一个宿主 shim，纯 pure。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>  // _dup / _dup2 / _fileno / _close
#else
#include <unistd.h>  // dup / dup2 / fileno / close
#endif

#include "check.h"
#include "runtime/machine.h"
#include "runtime/shims.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kStubEa = 0x7FFE0040u;

// 只编码本文件要用的几条（位移用 currentEa 算，⛔ 不手填）。
struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t currentEa() const { return kCodeEa + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const std::uint32_t at = currentEa();
        const std::int32_t rel = static_cast<std::int32_t>(target - (at + 5));
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void ret() { u8(0xC3); }
};

// 已知 shim：故意烧一点宿主时间（≥ 几十 µs），保证 steady_clock 的差值 > 0 ⇒ 表里必有这一行。
// ⛔ 不 sleep（避免把测试挂在时钟上）、不碰 guest 内存（这条只测"计时有没有打开"）。
volatile std::uint32_t g_sink = 0;

void shimSlow(lift::X86&, lift::ShimCall& call) {
    std::uint32_t acc = 0;
    for (std::uint32_t i = 0; i < 200000u; ++i) acc = acc * 1664525u + 1013904223u + i;
    g_sink = acc;
    call.result = 0x00000001u;
}

bool attach(lift::Machine& m, const char* name, lift::Conv conv, int argc, lift::ShimFn fn,
            std::uint32_t stub_ea) {
    m.shims().add("test", name, conv, argc, fn);
    lift::ShimDef* def = m.shims().findMutable("test", name);
    if (def == nullptr) return false;
    def->stub_ea = stub_ea;
    m.cache().installShim(stub_ea, def->id);
    return true;
}

bool boot(lift::Machine& m, const Asm& code) {
    std::string err;
    if (!m.initBare(&err)) return false;
    if (!m.prepareStack(&err)) return false;
    if (!m.placeCode(kCodeEa, code.b.data(), code.b.size(), &err)) return false;
    m.cpu().eip = kCodeEa;
    return true;
}

// ---- stdout 捕获（`dumpProfile` 打的是 stdout；Windows 走 _dup/_dup2，POSIX 走 dup/dup2）----
#ifdef _WIN32
using FdT = int;
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
#else
using FdT = int;
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
#endif

struct StdoutCapture {
    std::FILE* tmp = nullptr;
    FdT saved = -1;
    bool active = false;

    bool begin() {
        tmp = std::tmpfile();
        if (tmp == nullptr) return false;
        std::fflush(stdout);
        saved = static_cast<FdT>(dupFd(1));
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 1) < 0) return false;
        active = true;
        return true;
    }

    std::string finish() {
        if (!active || tmp == nullptr) return {};
        std::fflush(stdout);
        dup2Fd(saved, 1);
        closeFd(saved);
        saved = -1;
        active = false;
        std::string out;
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size > 0) {
            out.resize(static_cast<std::size_t>(size));
            std::fseek(tmp, 0, SEEK_SET);
            const std::size_t got = std::fread(out.data(), 1, out.size(), tmp);
            out.resize(got);
        }
        std::fclose(tmp);
        tmp = nullptr;
        return out;
    }
};

// 找含 needle 的第一行（找不到 ⇒ 空串）
std::string lineWith(const std::string& hay, const std::string& needle) {
    const std::size_t at = hay.find(needle);
    if (at == std::string::npos) return {};
    const std::size_t nl = hay.rfind('\n', at);
    const std::size_t line_begin = (nl == std::string::npos) ? 0 : nl + 1;
    const std::size_t end = hay.find('\n', at);
    return hay.substr(line_begin, (end == std::string::npos ? hay.size() : end) - line_begin);
}

}  // namespace

int main() {
    using namespace lift_test;
    // ★ 先把 `LIFT_FRAME_TRACE` 清掉：它也会打开 shim 计时（`run()` 里 `frame_trace_ || profile`），
    //   不清的话"② 不打开 profile ⇒ 表里没有 shim 行"这条会在 dev shell 里假红。
#ifdef _WIN32
    _putenv_s("LIFT_FRAME_TRACE", "");
#else
    unsetenv("LIFT_FRAME_TRACE");
#endif

    // ① 造一次真实的 guest → shim 调用（stdcall，1 个参数），并打开 profile
    {
        g_current_case = "profile-shims-table";
        Asm a;
        a.pushImm(0x11223344u);
        a.callRel(kStubEa);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        LIFT_CHECK(attach(m, "slow", lift::Conv::Stdcall, 1, &shimSlow, kStubEa));

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 10000;
        o.profile = true;  // ★ 这一行就是守卫的对象（run() 里 setTiming(frame_trace_ || profile)）
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.shims().find("test", "slow")->call_count, 1u);

        StdoutCapture cap;
        LIFT_CHECK(cap.begin());
        m.dumpProfile(stdout, 10);
        const std::string log = cap.finish();

        LIFT_CHECK(log.find("=== profile: shims") != std::string::npos);
        LIFT_CHECK(log.find("total shim wall =") != std::string::npos);
        LIFT_CHECK(log.find("两表不可相加") != std::string::npos);
        // 行判据：同一行里既有 `test!slow` 又有 `calls=1 `（宽度 10 的 %-10llu ⇒ 后面必是空格）
        const std::string row = lineWith(log, "test!slow");
        LIFT_CHECK(!row.empty());
        LIFT_CHECK(row.find("calls=1 ") != std::string::npos);
        // 反证：计时没打开时 ns 恒 0 ⇒ 这行不会出现（见文件头的"为什么这条会红"）。
        LIFT_CHECK(row.find("avg=") != std::string::npos);
    }

    // ② 不打开 profile ⇒ 表照常打印（结构在），但**没有**任何 shim 行（计时关着 ⇒ ns 全 0）。
    //    ★ 这条把"表存在"与"计时打开"两件事分开钉住：只删 `|| opts.profile` 时 ① 红、② 绿；
    //      把整张表删掉时 ① 红。
    {
        g_current_case = "profile-off-no-rows";
        Asm a;
        a.pushImm(0x55667788u);
        a.callRel(kStubEa);
        a.ret();
        lift::Machine m;
        LIFT_CHECK(boot(m, a));
        LIFT_CHECK(attach(m, "slow", lift::Conv::Stdcall, 1, &shimSlow, kStubEa));

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 10000;
        o.profile = false;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.shims().find("test", "slow")->call_count, 1u);

        StdoutCapture cap;
        LIFT_CHECK(cap.begin());
        m.dumpProfile(stdout, 10);
        const std::string log = cap.finish();
        LIFT_CHECK(log.find("=== profile: shims") != std::string::npos);
        LIFT_CHECK(log.find("test!slow") == std::string::npos);
    }

    return finish("profile-shims");
}
