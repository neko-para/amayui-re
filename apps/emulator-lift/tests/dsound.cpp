// emulator-lift · M2 DirectSound silent 层夹具（pure 档：自造机器码，不碰游戏 / LFS）
//
// 判据（SCOPE §1「音频 = 延后实现（silent）」+ 用户 2026-11-05 口径）：
//   ① **动态导出**：`LoadLibraryA("DSOUND.DLL")` 返回非 0 假句柄（与 KERNEL32/Shell32 同一口径）、
//      `GetProcAddress(h, "DirectSoundCreate")` 返回非 0 thunk —— 这正是 boot 缺口
//      （`.lst` 0x004B5C64 / 0x004B5C9F，函数 sub_4B5C50）的两步；
//   ② `DirectSoundCreate(NULL, &pDS, 0)` ⇒ `DS_OK(0)` + guest 可见的 COM 对象；
//      `QueryInterface(IID_IDirectSound8)` 成功（回同一对象），别的 IID ⇒ `E_NOINTERFACE` 且清空出参；
//   ③ 缓冲：`CreateSoundBuffer` 成功 + 对象非 0；`GetFormat` 把 `WAVEFORMATEX` **逐字段**还回来
//      （44100Hz / 2ch / 16bit / blockAlign 4 / avgBytes 176400 —— 与 `.lst` 0x004B5EF6..0x004B5F0B 一致）；
//   ④ `Lock` 给的是**真 guest 内存**（往里写 176400 字节再读回来必须一致）；`Unlock` 把那批字节
//      记进账本（`GetCurrentPosition` 的游标 = 已提交字节数 = 176400）；
//   ⑤ `Play` / `Stop` / `SetVolume` / `SetFrequency` / `SetCurrentPosition` / `GetStatus` 全部
//      `DS_OK` 且**记账**（状态位、读回值）；
//   ⑥ **日志判据**（"silent 但也记账 + 首次调用打一行、重复不打"）：捕获 stderr，断言
//      · `CreateSoundBuffer` 两条（primary / 流式）且流式那条带**原值格式**；
//      · `Unlock` 行里有「累计 PCM 字节 176400」与「非 0 字节 176400」；
//      · `Play` 行**只出现 2 次**（primary + 流式各一次）—— 第 3 次 Play 是重复调用 ⇒ **不许再打**；
//      · `Release` 的账本行里 `pcmBytes=176400`、`plays=1`。
//
// ★ 这些用例**会红**（变异验证见交付）：把 `Unlock` 的记账删掉 ⇒ ⑥ 与 ④ 红；
//   把 `Play` 的"首次才打"去掉 ⇒ ⑥ 的 Play 行数变 3 ⇒ 红。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[obj]; …; call [eax+off]`），
//   测的是"guest 可见的对象"，不是宿主直接调函数。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcLoadLibrary = 0x7FF81000u;
constexpr std::uint32_t kProcGetProcAddress = 0x7FF81004u;
constexpr std::uint32_t kProcDirectSoundCreate = 0x7FF81008u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00400000u;

// IID_IDirectSound8 = {C50A7E93-F395-4834-9EF6-7FA99DE50966}（内存字节序）
const std::uint8_t kIidDirectSound8[16] = {0x93, 0x7E, 0x0A, 0xC5, 0xF3, 0x95, 0x48, 0x34,
                                           0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66};
// IID_IDirectSoundNotify = {B0210783-89CD-11D0-AF08-00A0C925CD16}
const std::uint8_t kIidDirectSoundNotify[16] = {0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11,
                                                0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16};
// 一个跟 DirectSound 无关的 IID（{00000000-0000-0000-C000-000000000046} = IUnknown 之外的垃圾值）
const std::uint8_t kIidBogus[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                    0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01};

// DSBUFFERDESC 的原值（照 `.lst` 0x004B5EEF..0x004B5F0B 抄）
constexpr std::uint32_t kDescBytes = 36u;
constexpr std::uint32_t kPrimaryFlags = 0x000000C1u;    // CTRLVOLUME|CTRLPAN|PRIMARYBUFFER
constexpr std::uint32_t kStreamFlags = 0x000181C8u;     // GLOBALFOCUS|GETCURRENTPOSITION2|POSITIONNOTIFY|VOLUME|PAN|FREQUENCY
constexpr std::uint32_t kStreamBytes = 176400u;         // 0x2B110 = 44100*2ch*16bit*0.5s

struct Asm {
    std::vector<std::uint8_t> b;

    std::uint32_t ea(std::uint32_t base) const {
        return base + static_cast<std::uint32_t>(b.size());
    }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void pushEax() { u8(0x50); }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void movEaxMem(std::uint32_t ea) {  // mov eax, [imm32]
        u8(0xA1);
        u32(ea);
    }
    void movMemEax(std::uint32_t ea) {  // mov [imm32], eax
        u8(0xA3);
        u32(ea);
    }
    void callRel(std::uint32_t base, std::uint32_t target) {
        const std::uint32_t at = ea(base);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void callEax() {
        u8(0xFF);
        u8(0xD0);
    }
    void callMemEaxDisp8(std::uint8_t d) {
        u8(0xFF);
        u8(0x50);
        u8(d);
    }
    void callMemEaxDisp32(std::uint32_t d) {
        u8(0xFF);
        u8(0x90);
        u32(d);
    }
    void ret() { u8(0xC3); }
};

// ---- stderr **tee**（"日志判据"用；Windows 走 _dup/_dup2/_read/_write，POSIX 走 dup/dup2）----
// ★ 为什么是 tee 而不是"最后一次性读出来"：本夹具会故意走到 loud trap / guest fault 上（那是判据的一部分），
//   而 `abort()` **不跑任何收尾**（不 flush stdio、不跑析构、不跑 atexit）⇒ 只捕获不回显的话，
//   一旦中途停住，"为什么停"的全部现场就跟着丢了（实测踩过：变异 D 下整份诊断静默消失）。
//   `pump()` 把"上次搬过之后新增的那一段"同时写进内存 log 与**真 stderr**。
#ifdef _WIN32
using FdT = int;
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
int readFd(int fd, void* buf, unsigned n) { return _read(fd, buf, n); }
int writeFd(int fd, const void* buf, unsigned n) { return _write(fd, buf, n); }
#else
using FdT = int;
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
int readFd(int fd, void* buf, unsigned n) { return static_cast<int>(::read(fd, buf, n)); }
int writeFd(int fd, const void* buf, unsigned n) { return static_cast<int>(::write(fd, buf, n)); }
#endif

struct StderrTee {
    std::FILE* tmp = nullptr;
    FdT saved = -1;
    bool active = false;
    std::size_t moved = 0;  // 已经从 tmp 搬走多少字节（tmp 与 fd 2 共享同一个 open file description）
    std::string log;

    bool begin() {
        tmp = std::tmpfile();
        if (!tmp) return false;
        std::fflush(stderr);
        saved = static_cast<FdT>(dupFd(2));
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;  // POSIX dup2 成功返回目标 fd（2），不是 0
        active = true;
        return true;
    }

    // 把新增的一段搬进 log **并**回显到真 stderr（现场立刻可见）
    void pump() {
        if (!active || !tmp) return;
        std::fflush(stderr);
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size < 0) return;
        const std::size_t total = static_cast<std::size_t>(size);
        if (total <= moved) return;
        std::fseek(tmp, static_cast<long>(moved), SEEK_SET);
        std::vector<char> chunk(total - moved);
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), tmp);
        if (got == 0) return;
        log.append(chunk.data(), got);
        if (saved >= 0) {
            std::size_t off = 0;
            while (off < got) {
                const int n = writeFd(saved, chunk.data() + off, static_cast<unsigned>(got - off));
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
        }
        moved += got;
    }

    std::string finish() {
        if (!active) return log;
        pump();
        dup2Fd(saved, 2);
        closeFd(saved);
        saved = -1;
        active = false;
        if (tmp) {
            std::fclose(tmp);
            tmp = nullptr;
        }
        return log;
    }
};

std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

struct Fixture {
    lift::Machine m;
    std::string err;

    std::uint32_t code_ea = kCodeBase;
    std::uint32_t data_ea = kDataBase;
    std::uint32_t thunk = 0;    // DirectSoundCreate 的 thunk（GetProcAddress 的返回值）
    std::uint32_t thunk_slot_ea = 0;  // 存 thunk 的 guest 槽（callCreate 重新取）
    std::uint32_t device = 0;   // IDirectSound8 对象
    std::uint32_t primary = 0;  // primary buffer
    std::uint32_t stream = 0;   // 流式 buffer（176400 字节 / 44.1kHz）

    bool boot() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerProcessShims(m.shims());
        lift::registerDsoundShims(m.shims());
        if (!hook("kernel32", "LoadLibraryA", kProcLoadLibrary)) return false;
        if (!hook("kernel32", "GetProcAddress", kProcGetProcAddress)) return false;
        if (!hook("dsound", "DirectSoundCreate", kProcDirectSoundCreate)) return false;
        return true;
    }

    bool hook(const char* module, const char* name, std::uint32_t ea) {
        lift::ShimDef* d = m.shims().findMutable(module, name);
        if (!d) return false;
        d->stub_ea = ea;
        m.cache().installShim(ea, d->id);
        return true;
    }

    std::uint32_t placeData(const void* p, std::size_t n) {
        const std::uint32_t ea = data_ea;
        const std::uint32_t aligned = (static_cast<std::uint32_t>(n) + 15u) & ~15u;
        if (!m.placeData(ea, p, n, &err)) return 0;
        data_ea += (aligned < kDataStride) ? aligned : kDataStride;
        return ea;
    }
    std::uint32_t placeU32(std::uint32_t v) { return placeData(&v, 4); }
    std::uint32_t placeString(const char* s) { return placeData(s, std::strlen(s) + 1); }
    std::uint32_t placeBytes(const std::vector<std::uint8_t>& v) { return placeData(v.data(), v.size()); }

    // 放一段 guest 代码并执行到 0xFFFFFFFF 哨兵；返回 EAX
    std::uint32_t run(Asm& a) {
        const std::uint32_t ea = code_ea;
        if (!m.placeCode(ea, a.b.data(), a.b.size(), &err)) return 0;
        code_ea += kCodeStride;
        m.cpu().eip = ea;
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
        m.cpu().running = true;
        lift::RunOptions o;
        o.until_ea = kSentinel;
        o.max_insns = 200000;
        const lift::RunResult r = m.run(o);
        if (r.reason != lift::StopReason::UntilEa) {
            lift_test::failf(__FILE__, __LINE__, "guest 代码没有走到哨兵（reason=%s: %s）",
                             lift::stopReasonName(r.reason), r.detail.c_str());
        }
        return m.cpu().eax();
    }

    // guest 侧的 COM 调用：push 参数 → push this → call [vtbl+off]
    std::uint32_t method(std::uint32_t obj, std::uint32_t offset, const std::vector<std::uint32_t>& args) {
        const std::uint32_t vtbl = m.mem().rd32(obj);
        Asm a;
        a.movEaxImm(vtbl);
        for (std::size_t i = args.size(); i > 0; --i) a.pushImm(args[i - 1]);
        a.pushImm(obj);
        if (offset <= 0x7Fu) a.callMemEaxDisp8(static_cast<std::uint8_t>(offset));
        else a.callMemEaxDisp32(offset);
        a.ret();
        return run(a);
    }

    // ① LoadLibraryA ×2 + GetProcAddress（`.lst` 0x004B5C64 / 0x004B5C9F）
    // ★ 整串必须放在**同一个 Asm 块**里：`callRel` 的相对偏移按块起点算。
    bool loadAndResolve() {
        const std::uint32_t mod = placeString("DSOUND.DLL");
        const std::uint32_t proc = placeString("DirectSoundCreate");
        const std::uint32_t hmod = placeU32(0);
        const std::uint32_t hmod2 = placeU32(0);
        const std::uint32_t thunk_slot = placeU32(0);
        Asm a;
        // 第一次 LoadLibrary
        a.pushImm(mod);
        a.callRel(kCodeBase, kProcLoadLibrary);
        a.movMemEax(hmod);
        // 第二次 LoadLibrary 同一模块 ⇒ 同一句柄（Win32 的引用计数语义）
        a.pushImm(mod);
        a.callRel(kCodeBase, kProcLoadLibrary);
        a.movMemEax(hmod2);
        // GetProcAddress(h, "DirectSoundCreate")
        a.movEaxMem(hmod);
        a.pushImm(proc);
        a.pushEax();
        a.callRel(kCodeBase, kProcGetProcAddress);
        a.movMemEax(thunk_slot);
        a.movEaxImm(0);
        a.ret();
        run(a);
        thunk_slot_ea = thunk_slot;
        const std::uint32_t h = m.mem().rd32(hmod);
        LIFT_CHECK_NE_U32(h, 0u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(hmod2), h);
        thunk = m.mem().rd32(thunk_slot);
        LIFT_CHECK_NE_U32(thunk, 0u);
        return h != 0 && thunk != 0;
    }

    // ② DirectSoundCreate(NULL, &pDS, NULL) —— 动态导出：call eax（`.lst` 0x004B5C7A）
    // ★ 单独一块：thunk == 0 时**不执行**它（否则就是 `call 0` ⇒ guest fault ⇒ 整轮诊断被 abort 吞掉）。
    bool callCreate() {
        const std::uint32_t out = placeU32(0);
        Asm b;
        b.pushImm(0);    // pUnkOuter
        b.pushImm(out);  // ppDS
        b.pushImm(0);    // pcGuidDevice（NULL = 默认设备；引擎就是传 0）
        b.movEaxMem(thunk_slot_ea);
        b.callEax();
        b.ret();
        const std::uint32_t hr = run(b);
        LIFT_CHECK_EQ_U32(hr, 0u);
        device = m.mem().rd32(out);
        LIFT_CHECK_NE_U32(device, 0u);
        return device != 0;
    }

    // DSBUFFERDESC（36 字节）+ 可选 WAVEFORMATEX（18 字节）
    std::uint32_t descOf(std::uint32_t flags, std::uint32_t bytes, const std::vector<std::uint8_t>* wfx) {
        std::vector<std::uint8_t> d(kDescBytes, 0);
        auto put = [&d](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                d[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        put(0, kDescBytes);
        put(4, flags);
        put(8, bytes);
        if (wfx) put(16, placeBytes(*wfx));
        return placeBytes(d);
    }

    std::vector<std::uint8_t> wavFormat() const {
        // WAVEFORMATEX：PCM / 2ch / 44100 / avgBytes 176400 / blockAlign 4 / 16bit / cbSize 0
        std::vector<std::uint8_t> w(18, 0);
        auto put16 = [&w](std::uint32_t off, std::uint32_t v) {
            w[off] = static_cast<std::uint8_t>(v & 0xFF);
            w[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        };
        auto put32 = [&w](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                w[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        put16(0, 1u);          // wFormatTag = WAVE_FORMAT_PCM
        put16(2, 2u);          // nChannels
        put32(4, 44100u);      // nSamplesPerSec
        put32(8, 176400u);     // nAvgBytesPerSec
        put16(12, 4u);         // nBlockAlign
        put16(14, 16u);        // wBitsPerSample
        put16(16, 0u);         // cbSize
        return w;
    }

    std::uint32_t createBuffer(const char* method_case, std::uint32_t flags, std::uint32_t bytes,
                               const std::vector<std::uint8_t>* wfx) {
        (void)method_case;
        const std::uint32_t desc = descOf(flags, bytes, wfx);
        const std::uint32_t out = placeU32(0);
        const std::uint32_t hr = method(device, 12u, {desc, out, 0u});
        LIFT_CHECK_EQ_U32(hr, 0u);
        const std::uint32_t obj = m.mem().rd32(out);
        LIFT_CHECK_NE_U32(obj, 0u);
        return obj;
    }
};

}  // namespace

int main() {
    lift::logSetLevel("info");  // 本夹具断言日志行；默认 warn 会隐藏 Info
    using namespace lift_test;

    Fixture f;
    if (!f.boot()) {
        std::fprintf(stderr, "fixture boot 失败：%s\n", f.err.c_str());
        return 1;
    }

    // ★ 从**第一步**就开始 tee stderr：日志判据要看整条链的留痕（"首次调用打一行"是**一次性**的，
    //   事后再捕获就看不到了）。`pump()` 每段之后把新增日志回显到真 stderr ⇒ 即使后面 abort 了，
    //   前面的现场也已经在屏幕上（⛔ 不把诊断锁进一个只在本进程内可读的临时文件）。
    StderrTee cap;
    LIFT_CHECK(cap.begin());

    // ---- ① 动态导出：LoadLibraryA + GetProcAddress ----
    g_current_case = "dsound-edge-load-library-and-getproc-address";
    LIFT_CHECK(f.loadAndResolve());
    cap.pump();
    if (f.thunk == 0) {
        // 拿不到导出就**不再往下跑**：`call 0` 会把整轮诊断变成 abort（见 StderrTee 的注释）
        cap.finish();
        return lift_test::finish("dsound");
    }

    // ---- ② DirectSoundCreate ⇒ guest 可见的 IDirectSound8 ----
    g_current_case = "dsound-edge-create-device";
    LIFT_CHECK(f.callCreate());
    cap.pump();
    if (f.device == 0) {
        cap.finish();
        return lift_test::finish("dsound");
    }

    // ---- ③ 设备对象 QI + SetCooperativeLevel ----
    g_current_case = "dsound-edge-query-interface-and-cooperative-level";
    {
        const std::uint32_t riid = f.placeBytes(std::vector<std::uint8_t>(kIidDirectSound8, kIidDirectSound8 + 16));
        const std::uint32_t ppv = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(f.device, 0u, {riid, ppv}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), f.device);  // 同一对象（没有 tear-off）
        // QI 成功 = 一次 AddRef ⇒ 必须还回去（否则设备永远到不了 refcount 0，账本行就不会打）
        LIFT_CHECK_EQ_U32(f.method(f.device, 8u, {}), 1u);

        const std::uint32_t bogus = f.placeBytes(std::vector<std::uint8_t>(kIidBogus, kIidBogus + 16));
        const std::uint32_t ppv2 = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(f.device, 0u, {bogus, ppv2}), 0x80004002u);  // E_NOINTERFACE
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv2), 0u);                            // 失败必须清空出参

        // 引擎 0x004B5DDD：SetCooperativeLevel(pDS, hWnd, v10)，v10 ∈ {1,2,3}
        LIFT_CHECK_EQ_U32(f.method(f.device, 24u, {0x00012345u, 2u}), 0u);
    }

    cap.pump();
    g_current_case = "dsound-edge-create-buffers-and-get-format";
    {
        f.primary = f.createBuffer("primary", kPrimaryFlags, 0u, nullptr);
        const std::vector<std::uint8_t> wfx = f.wavFormat();
        f.stream = f.createBuffer("stream", kStreamFlags, kStreamBytes, &wfx);
        LIFT_CHECK_NE_U32(f.primary, 0u);
        LIFT_CHECK_NE_U32(f.stream, 0u);
        LIFT_CHECK_NE_U32(f.primary, f.stream);

        // GetFormat：缓冲 18+ 字节，把 WAVEFORMATEX **逐字段**还回来
        const std::vector<std::uint8_t> zero(32, 0);
        const std::uint32_t buf = f.placeBytes(zero);
        const std::uint32_t written = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 20u, {buf, 32u, written}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(written), 18u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 0), 1u);       // wFormatTag = PCM
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 2), 2u);       // nChannels
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(buf + 4), 44100u);   // nSamplesPerSec
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(buf + 8), 176400u);  // nAvgBytesPerSec
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 12), 4u);      // nBlockAlign
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 14), 16u);     // wBitsPerSample
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 16), 0u);      // cbSize

        // GetCaps：flags / bytes 原值
        const std::uint32_t caps = f.placeBytes(zero);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 12u, {caps}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps + 0), 20u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps + 4), kStreamFlags);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps + 8), kStreamBytes);

        // IDirectSoundNotify：`sub_4B71C0` 在 CreateSoundBuffer 之后 QI 它；缺了会让 WAV 加载失败。
        const std::uint32_t notify_iid = f.placeBytes(
            std::vector<std::uint8_t>(kIidDirectSoundNotify, kIidDirectSoundNotify + 16));
        const std::uint32_t notify_ppv = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 0u, {notify_iid, notify_ppv}), 0u);
        const std::uint32_t notify = f.m.mem().rd32(notify_ppv);
        LIFT_CHECK_NE_U32(notify, 0u);
        const std::uint32_t positions = f.placeBytes(std::vector<std::uint8_t>(32, 0));
        LIFT_CHECK_EQ_U32(f.method(notify, 12u, {4u, positions}), 0u);  // SetNotificationPositions
        LIFT_CHECK_EQ_U32(f.method(notify, 8u, {}), 0u);                // Release
    }

    cap.pump();
    g_current_case = "dsound-edge-lock-write-unlock-play-accounting";
    std::uint32_t pcm_ptr = 0;
    {
        // 引擎 0x004B5FA3 的次序：先 Play(primary,0,0,1)
        LIFT_CHECK_EQ_U32(f.method(f.primary, 48u, {0u, 0u, 1u}), 0u);

        const std::uint32_t p1 = f.placeU32(0);
        const std::uint32_t b1 = f.placeU32(0);
        const std::uint32_t p2 = f.placeU32(0);
        const std::uint32_t b2 = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 44u, {0u, kStreamBytes, p1, b1, p2, b2, 0u}), 0u);
        pcm_ptr = f.m.mem().rd32(p1);
        LIFT_CHECK_NE_U32(pcm_ptr, 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(b1), kStreamBytes);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(b2), 0u);

        // ★ 真 guest 内存：写满 176400 字节（**没有 0 字节**的图样，用来证明"引擎写进去的就是这块"）
        std::vector<std::uint8_t> pcm(kStreamBytes);
        for (std::uint32_t i = 0; i < kStreamBytes; ++i)
            pcm[i] = static_cast<std::uint8_t>(1u + (i * 7u + 3u) % 255u);
        f.m.mem().writeBytes(pcm_ptr, pcm.data(), pcm.size());

        LIFT_CHECK_EQ_U32(f.method(f.stream, 76u, {pcm_ptr, kStreamBytes, 0u, 0u}), 0u);

        // 引擎 0x004B5FB3：Play(流式, 0, 0, 1)
        LIFT_CHECK_EQ_U32(f.method(f.stream, 48u, {0u, 0u, 1u}), 0u);
        // 再 Play 一次：DS 允许（已在播时只是换 flag）⇒ 值仍是 DS_OK，但**日志不许重复**
        LIFT_CHECK_EQ_U32(f.method(f.stream, 48u, {0u, 0u, 1u}), 0u);

        const std::uint32_t status = f.placeU32(0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 36u, {status}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(status), 3u);  // PLAYING(1) | LOOPING(2)

        const std::uint32_t play_pos = f.placeU32(0);
        const std::uint32_t write_pos = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 16u, {play_pos, write_pos}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(play_pos), kStreamBytes);  // 已提交字节 = 176400
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(write_pos), kStreamBytes);

        // Set/Get 往返（静音设备只记账）
        LIFT_CHECK_EQ_U32(f.method(f.stream, 60u, {static_cast<std::uint32_t>(-1000)}), 0u);
        const std::uint32_t vol = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 24u, {vol}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(vol), static_cast<std::uint32_t>(-1000));
        LIFT_CHECK_EQ_U32(f.method(f.stream, 68u, {22050u}), 0u);
        const std::uint32_t freq = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 32u, {freq}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(freq), 22050u);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 52u, {0u}), 0u);

        // 读回来：Lock 同一块再逐字节比对（证明 Lock 给的是**持久**的 guest 内存，不是临时缓冲）
        const std::uint32_t p1b = f.placeU32(0);
        const std::uint32_t b1b = f.placeU32(0);
        const std::uint32_t p2b = f.placeU32(0);
        const std::uint32_t b2b = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 44u, {0u, 1024u, p1b, b1b, p2b, b2b, 0u}), 0u);
        const std::uint32_t ptr2 = f.m.mem().rd32(p1b);
        LIFT_CHECK_EQ_U32(ptr2, pcm_ptr);  // 同一块内存（确定性）
        std::uint32_t mismatches = 0;
        for (std::uint32_t i = 0; i < 1024u; ++i)
            if (f.m.mem().rd8(ptr2 + i) != pcm[i]) ++mismatches;
        LIFT_CHECK_EQ_U32(mismatches, 0u);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 76u, {ptr2, 1024u, 0u, 0u}), 0u);

        // Stop ⇒ 状态位清零
        LIFT_CHECK_EQ_U32(f.method(f.stream, 72u, {}), 0u);
        const std::uint32_t status2 = f.placeU32(0xFFFFFFFFu);
        LIFT_CHECK_EQ_U32(f.method(f.stream, 36u, {status2}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(status2), 0u);
    }

    cap.pump();

    // ---- ⑥ DuplicateSoundBuffer（引擎当前证据里**没调**，但它是实现过的方法 ⇒ 必须有用例）----
    g_current_case = "dsound-edge-duplicate-sound-buffer";
    std::uint32_t dup = 0;
    {
        const std::uint32_t out = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(f.device, 20u, {f.stream, out}), 0u);
        dup = f.m.mem().rd32(out);
        LIFT_CHECK_NE_U32(dup, 0u);
        LIFT_CHECK_NE_U32(dup, f.stream);
        // 格式 / 字节数照抄源缓冲
        const std::vector<std::uint8_t> zero(32, 0);
        const std::uint32_t buf = f.placeBytes(zero);
        const std::uint32_t written = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(dup, 20u, {buf, 32u, written}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(buf + 4), 44100u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 2), 2u);
        // 内容是真的**独立拷贝**（源缓冲里的 176400 字节已写过 ⇒ 副本里也必须一样）
        const std::uint32_t p1d = f.placeU32(0);
        const std::uint32_t b1d = f.placeU32(0);
        const std::uint32_t p2d = f.placeU32(0);
        const std::uint32_t b2d = f.placeU32(0);
        LIFT_CHECK_EQ_U32(f.method(dup, 44u, {0u, kStreamBytes, p1d, b1d, p2d, b2d, 0u}), 0u);
        const std::uint32_t dptr = f.m.mem().rd32(p1d);
        LIFT_CHECK_NE_U32(dptr, 0u);
        LIFT_CHECK_NE_U32(dptr, pcm_ptr);  // 不是共享同一块
        std::uint32_t mismatch = 0;
        for (std::uint32_t i = 0; i < kStreamBytes; ++i)
            if (f.m.mem().rd8(dptr + i) != static_cast<std::uint8_t>(1u + (i * 7u + 3u) % 255u)) ++mismatch;
        LIFT_CHECK_EQ_U32(mismatch, 0u);
        LIFT_CHECK_EQ_U32(f.method(dup, 76u, {dptr, kStreamBytes, 0u, 0u}), 0u);
    }

    cap.pump();

    // ---- ⑦ 卸掉 buffer 之后再拿它当源 ⇒ 不许静默（真实失败码 + 一行日志）----
    LIFT_CHECK_EQ_U32(f.method(f.stream, 72u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(f.stream, 8u, {}), 0u);  // 释放流式缓冲
    g_current_case = "dsound-edge-call-after-release-is-loud";
    {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        // this=device（还活着），源 = 已释放的缓冲 ⇒ bufferState 查不到 ⇒ DSERR_INVALIDPARAM
        LIFT_CHECK_EQ_U32(f.method(f.device, 20u, {f.stream, out}), 0x80070057u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);  // 失败必须清空出参
    }

    // ---- ⑧ 引擎清理路径 sub_4B6440 的次序：缓冲（Stop + Release）→ 最后设备 ----
    LIFT_CHECK_EQ_U32(f.method(f.primary, 72u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(f.primary, 8u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(dup, 8u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(f.device, 8u, {}), 0u);  // 最后一个引用 ⇒ 0 + 账本行

    // 收尾：把剩下的一段搬完并还原 fd 2（`pump()` 已经把每一步的现场回显过 ⇒ 这里只取全文做断言）
    const std::string log = cap.finish();

    // ---- ⑨ 日志判据（"silent 但也记账 + 首次调用打一行、重复不打"）----
    g_current_case = "dsound-edge-silent-ledger-log";
    {
        // ① 动态导出两步都留痕
        LIFT_CHECK(contains(log, "LoadLibraryA: 装载 \"DSOUND.DLL\""));
        LIFT_CHECK(contains(log, "GetProcAddress: dsound!DirectSoundCreate ⇒ thunk 0x"));
        LIFT_CHECK(contains(log, "DirectSoundCreate: pcGuidDevice=0x00000000 ⇒ DS_OK(0)"));
        // ② 两个 buffer 的 WAVEFORMATEX 原值（primary 没有格式；流式那份逐字段）
        LIFT_CHECK(contains(log, "CreateSoundBuffer: #1 ⇒ DS_OK"));
        LIFT_CHECK(contains(log, "CreateSoundBuffer: #2 ⇒ DS_OK"));
        LIFT_CHECK(contains(log, "tag=1(PCM) ch=2 rate=44100Hz bits=16 blockAlign=4 avgBytes=176400 cbSize=0"));
        // ③ 账本：累计 PCM 字节 / 非 0 字节
        LIFT_CHECK(contains(log, "累计 PCM 字节 176400"));
        LIFT_CHECK(contains(log, "本区间非 0 字节 176400"));
        // ④ "首次调用打、重复不打"：Play 调了 4 次（primary 1 + stream 2 + dup 0）但只许打 2 行——
        //    primary 与 stream 各一行；stream 的第二次 Play 是重复调用 ⇒ 不许再打
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(countOccurrences(log, "Play: dwFlags=0x00000001")), 2u);
        // ⑤ 缓冲的最终账本（Release 时打）：pcmBytes / plays / 非 0 字节
        LIFT_CHECK(contains(log, "IDirectSoundBuffer8 最终账本"));
        LIFT_CHECK(contains(log, "pcmBytes=176400"));
        LIFT_CHECK(contains(log, "plays=1"));
        LIFT_CHECK(contains(log, "nonzeroBytes=176400"));
        // ⑥ 设备的最终账本（Release 时打）
        LIFT_CHECK(contains(log, "IDirectSound8 最终账本"));
        LIFT_CHECK(contains(log, "cooperativeLevel=2"));
        // ⑦ 释放后再用 ⇒ 有一行日志（⛔ 不是静默返回 0）
        LIFT_CHECK(contains(log, "不是本模拟器建的 buffer"));
    }

    if (lift_test::g_failures != 0)
        std::fprintf(stderr, "（失败见上面的 FAIL 行；日志判据相关用例名 %s）\n",
                     "dsound-edge-silent-ledger-log");
    return lift_test::finish("dsound");
}
