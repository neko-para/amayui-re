// emulator-lift · 机制夹具（B1：动态导出转发 / COM 对象 vtable / refcount）
//
// ★ 判据（plan §5 B1 / §6 P0 的 G1·G2）：
//   ① `ShimRegistry::addRuntime` 拿到的是桩页里的**唯一**地址（第二条不重复、同名幂等、耗尽即失败）；
//   ② guest 侧 `call dword ptr [eax+off]` 真的能把 `this` + 参数送到方法 shim，EAX 是返回值；
//      未填的槽是 **loud trap**（不是静默返回 0）；`release` 到 0 后对象不再可用且 guest 堆归还；
//   ③ `GetProcAddress` 走**通用路径**：句柄 → 模块 key → shim 注册表 → 返回 thunk，guest `call eax`
//      能真的调到运行期登记的导出；找不到 = 127、句柄不认得 = 126（两条日志分开）。
//
// ★ 为什么这些用例放在 bare 机器上：它们测的是**机制**，不需要游戏 / PE。
//   真链路上 `Machine::prepare()` 的 `bind()` 会给 ShimRegistry 桩页；bare 夹具没有 bind，
//   所以显式 `configureStubPage()` 给一页（`hPrepare` 的桩页是 0x7FFE0000，这里用 0x7FF80000 分开）。
//
// ★ 本文件也顺带钉住 AGERC 的**新口径**（2026-11-04：真实 guest 模块；没有镜像 ⇒ 装不上）。
//   "有镜像 ⇒ 同一 base / thunk / _GetInstance=base" 由 tests/agerc_module.cpp 覆盖。
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/com.h"
#include "runtime/handles.h"
#include "runtime/log.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;   // 同一台机器里放第 n 段代码（块缓存按 EA 记）
constexpr std::uint32_t kDataEa = 0x00410000u;   // guest 里的字符串（placeData）
constexpr std::uint32_t kStubPage = 0x7FF80000u; // bare 夹具的桩页（⛔ 与 GuestLayout::shim_page 分开）
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcStubEa = 0x7FF81000u;  // LoadLibraryA / GetProcAddress 在本夹具里的地址
constexpr std::uint32_t kFixtureHeap = 0x00100000u; // 1 MiB 足够，别为夹具 commit 128 MiB

// ---- stderr **tee**（"默认日志级别看得见"的判据用；形态照 `tests/dsound.cpp`）----
// ★ 为什么是 tee 而不是"最后一次性读出来"：本夹具的其他用例会故意走到 loud trap / abort，
//   只捕获不回显的话，一旦中途停住现场就全丢了。`pump()` 把新增的一段同时写进内存 log 与真 stderr。
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
    std::size_t moved = 0;  // 已经从 tmp 搬走多少字节
    std::string log;

    bool begin() {
        tmp = std::tmpfile();
        if (!tmp) return false;
        std::fflush(stderr);
        saved = static_cast<FdT>(dupFd(2));
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;
        active = true;
        return true;
    }

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

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// 只做本文件要用的几条编码；位移由 `currentEa()` 算，⛔ 不手填
struct Asm {
    std::vector<std::uint8_t> b;

    std::uint32_t currentEa(std::uint32_t base) const {
        return base + static_cast<std::uint32_t>(b.size());
    }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void pushEax() { u8(0x50); }  // push eax
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);  // mov eax, imm32
        u32(v);
    }
    // call rel32（target 是本段的绝对 EA；base 是这一段放在哪个 EA）
    void callRel(std::uint32_t base, std::uint32_t target) {
        const std::uint32_t at = currentEa(base);
        const std::int32_t rel = static_cast<std::int32_t>(target - (at + 5));
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void callEax() { u8(0xFF); u8(0xD0); }  // call eax（GetProcAddress 的返回值）
    // call dword ptr [eax+disp8]：FF /2，modrm=01 010 000（COM 方法槽）
    void callMemEaxDisp8(std::uint8_t disp) {
        u8(0xFF);
        u8(0x50);
        u8(disp);
    }
    void ret() { u8(0xC3); }
};

// 一台 bare 机器：栈 + guest 堆 + 桩页 + 第一段代码
bool boot(lift::Machine& m, const Asm& code, std::uint32_t code_ea, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeap)) return false;
    m.shims().configureStubPage(kStubPage, kStubSlots);
    if (!m.placeCode(code_ea, code.b.data(), code.b.size(), err)) return false;
    m.cpu().eip = code_ea;
    return true;
}

// 同一台机器上跑第二段代码：栈顶哨兵要重新铺（上一段用掉了）
void reprimeStack(lift::Machine& m) {
    m.cpu().esp() = m.layout().stack_top - 4u;
    m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);
    m.cpu().running = true;
}

lift::RunResult run(lift::Machine& m) {
    lift::RunOptions o;
    o.until_ea = 0xFFFFFFFFu;
    o.max_insns = 10000;
    return m.run(o);
}

// 把一条**已经登记过**的 def 挂到指定 EA 上（bare 夹具没有 bind，地址得自己给）
bool hook(lift::Machine& m, const char* module, const char* name, std::uint32_t ea) {
    lift::ShimDef* d = m.shims().findMutable(module, name);
    if (!d) return false;
    d->stub_ea = ea;
    m.cache().installShim(ea, d->id);
    return true;
}

bool putString(lift::Machine& m, std::uint32_t ea, const char* s, std::string* err) {
    return m.placeData(ea, s, std::string(s).size() + 1, err);
}

// ---------------------------------------------------------------------------------------
// ① addRuntime：唯一地址 / 幂等 / 耗尽
// ---------------------------------------------------------------------------------------
void shimPing(lift::X86&, lift::ShimCall& call) {
    call.result = 0xABCD0001u;
}
void shimPong(lift::X86&, lift::ShimCall& call) {
    call.result = 0xABCD0002u;
}
// ---------------------------------------------------------------------------------------
// ② COM 对象：方法调用 / 参数顺序 / 未填槽 loud trap / refcount
// ---------------------------------------------------------------------------------------
std::uint32_t g_com_this = 0;
std::uint32_t g_com_arg0 = 0;
std::uint32_t g_com_arg1 = 0;
int g_get_value_calls = 0;
int g_add_calls = 0;

void shimGetValue(lift::X86&, lift::ShimCall& call) {
    ++g_get_value_calls;
    g_com_this = call.u32(0);  // ★ COM 的 this 是第 0 个栈参数
    call.result = 0x1111u;
}

void shimAdd(lift::X86&, lift::ShimCall& call) {
    ++g_add_calls;
    g_com_this = call.u32(0);
    g_com_arg0 = call.u32(1);
    g_com_arg1 = call.u32(2);
    call.result = g_com_arg0 + g_com_arg1;
}

// 模拟 `ExitProcess` 那类"实现里主动停"：只置 running/stop_why，**不**写 `stop_kind`
// （⇒ 汇入点必须归 `Stopped`）。用于钉"停机种类按轮重置"。
void shimNormalStop(lift::X86& cpu, lift::ShimCall& call) {
    call.result = 0;
    cpu.running = false;
    cpu.stop_why = "fixture: normal stop";
}

// ---------------------------------------------------------------------------------------
// ③ 运行期导出（模拟 ddraw!DirectDrawCreate）
// ---------------------------------------------------------------------------------------
std::uint32_t g_ddraw_guid = 0;
int g_ddraw_calls = 0;

void shimDirectDrawCreate(lift::X86&, lift::ShimCall& call) {
    ++g_ddraw_calls;
    g_ddraw_guid = call.u32(0);  // lpGUID
    call.result = 0x88760000u;   // 假 HRESULT（非 0 ⇒ 调用方会走失败分支，但这里只看它被调到）
}

}  // namespace

int main() {
    using namespace lift_test;

    // =====================================================================================
    // ① addRuntime：每个条目一条**唯一**地址；同名幂等；槽位耗尽返回 nullptr
    // =====================================================================================
    {
        g_current_case = "addRuntime-unique-stub";
        lift::Machine m;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        m.shims().configureStubPage(kStubPage, kStubSlots);
        LIFT_CHECK_EQ_U32(m.shims().nextStubSlot(), 0u);
        LIFT_CHECK_EQ_U32(m.shims().stubPage(), kStubPage);

        lift::ShimDef* a = m.shims().addRuntime("test", "Ping", lift::Conv::Stdcall, 0, &shimPing);
        LIFT_CHECK(a != nullptr);
        // ⚠ 之后再加 def 会让这个指针可能失效（defs_ 是 vector）⇒ 先把要断言的都取成局部值
        const std::uint32_t ea_a = a ? a->stub_ea : 0;
        const std::uint32_t id_a = a ? a->id : 0;
        if (a) {
            LIFT_CHECK_NE_U32(a->stub_ea, 0u);
            LIFT_CHECK_EQ_U32(a->stub_ea, kStubPage);  // 第一个空闲槽 = 槽 0
            LIFT_CHECK(a->declared);                   // host 侧显式登记过
            LIFT_CHECK(!a->imported);                  // IAT 里没有它
            LIFT_CHECK(a->reason == nullptr);           // 不是"未实现的导入"
            LIFT_CHECK(a->fn == &shimPing);
            LIFT_CHECK(a->module == "test" && a->name == "Ping");
        }
        LIFT_CHECK_EQ_U32(m.shims().nextStubSlot(), 1u);
        // byStubEa 能反查回来（dispatcher 走的就是这条）
        const lift::ShimDef* found = m.shims().byStubEa(ea_a);
        LIFT_CHECK(found != nullptr);
        if (found) {
            LIFT_CHECK(found->name == "Ping");
            LIFT_CHECK_EQ_U32(found->id, id_a);
        }
        LIFT_CHECK(m.shims().byId(id_a) != nullptr);

        // 第二条：EA 不重复（桩页按 4 字节步进）
        lift::ShimDef* b = m.shims().addRuntime("test", "Pong", lift::Conv::Stdcall, 1, &shimPong);
        LIFT_CHECK(b != nullptr);
        if (b) {
            LIFT_CHECK_NE_U32(b->stub_ea, ea_a);
            LIFT_CHECK_EQ_U32(b->stub_ea, ea_a + 4u);
        }
        LIFT_CHECK_EQ_U32(m.shims().nextStubSlot(), 2u);

        // 幂等：同名再登记 ⇒ 同一个 stub（⛔ 不覆盖已有 EA、不占新槽）
        lift::ShimDef* again = m.shims().addRuntime("test", "Ping", lift::Conv::Stdcall, 0, &shimPing);
        LIFT_CHECK(again != nullptr);
        if (again) LIFT_CHECK_EQ_U32(again->stub_ea, ea_a);
        LIFT_CHECK_EQ_U32(m.shims().nextStubSlot(), 2u);
        // 模块名 / 方法名归一化：`TEST!ping` 也查得到
        LIFT_CHECK(m.shims().find("TEST", "ping") != nullptr);
        LIFT_CHECK(m.shims().find("test", "nope") == nullptr);
        // 运行期 def 不该出现在"导入表里有、但没实现"的缺口清单里
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.shims().importedButUnimplemented().size()), 0u);

        // 槽位耗尽：返回 nullptr，⛔ 不覆盖已发的 EA
        lift::ShimRegistry tiny;
        tiny.configureStubPage(0x7FF90000u, 2u);
        // ⚠ addRuntime 返回的指针会被下一次 addRuntime 作废（defs_ 是 vector）⇒ 立刻取 EA
        lift::ShimDef* t1 = tiny.addRuntime("test", "A", lift::Conv::Stdcall, 0, &shimPing);
        const std::uint32_t t1_ea = t1 ? t1->stub_ea : 0;
        lift::ShimDef* t2 = tiny.addRuntime("test", "B", lift::Conv::Stdcall, 0, &shimPing);
        const std::uint32_t t2_ea = t2 ? t2->stub_ea : 0;
        LIFT_CHECK(t1 != nullptr && t2 != nullptr);
        LIFT_CHECK_EQ_U32(t1_ea, 0x7FF90000u);
        LIFT_CHECK_EQ_U32(t2_ea, 0x7FF90004u);
        LIFT_CHECK(tiny.addRuntime("test", "C", lift::Conv::Stdcall, 0, &shimPing) == nullptr);
        // 耗尽之后再失败，也不许动别人已占的地址
        const lift::ShimDef* still_a = tiny.byStubEa(t1_ea);
        LIFT_CHECK(still_a != nullptr);
        if (still_a) LIFT_CHECK(still_a->name == "A");
        LIFT_CHECK(tiny.byStubEa(0x7FF90008u) == nullptr);  // 第三个槽从没发出去

        // 没有桩页（bind 没跑过、也没 configureStubPage）⇒ 响亮失败，不发地址
        lift::ShimRegistry no_page;
        LIFT_CHECK(no_page.addRuntime("test", "A", lift::Conv::Stdcall, 0, &shimPing) == nullptr);
        LIFT_CHECK_EQ_U32(no_page.nextStubSlot(), 0u);
    }

    // =====================================================================================
    // ② COM 对象：vtable / this / 参数顺序 / 未填槽 loud trap / refcount / 释放
    // =====================================================================================
    {
        g_current_case = "com-object-vtable";
        lift::Machine m;
        lift::ComRegistry com;
        std::string err;
        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        LIFT_CHECK(m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeap));
        m.shims().configureStubPage(kStubPage, kStubSlots);
        // ★ 装配点：x86.h 里是 `X86::com`（不是 ShimState::com）；主代理会指向 Machine 持有的实例
        m.cpu().com = &com;

        const std::uint32_t heap_before = m.heap().bytesInUse();
        const std::vector<lift::ComMethod> methods = {
            {0u, "GetValue", lift::Conv::Stdcall, 0, &shimGetValue},
            {8u, "Add", lift::Conv::Stdcall, 2, &shimAdd},
            // ⚠ +4 故意不填 ⇒ 应该是 loud trap（见下面的 "com-unsupported-slot-traps"）
        };
        const std::uint32_t obj = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_NE_U32(obj, 0u);
        LIFT_CHECK(com.find(obj));
        LIFT_CHECK_EQ_U32(com.refCount(obj), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 1u);
        LIFT_CHECK(m.heap().bytesInUse() > heap_before);  // 对象 + vtable 都在 guest 堆上

        // 布局：obj+0 = vtable EA，obj+4 = refcount
        const std::uint32_t vtbl = m.mem().rd32(obj);
        LIFT_CHECK_NE_U32(vtbl, 0u);
        LIFT_CHECK_EQ_U32(vtbl, com.vtableOf(obj));
        LIFT_CHECK_EQ_U32(m.mem().rd32(obj + 4), 1u);
        LIFT_CHECK(com.ifaceOf(obj) != nullptr);
        if (const char* face = com.ifaceOf(obj)) LIFT_CHECK(std::string(face) == "ITest");

        // 方法槽 = 桩页里的唯一地址；+4 是未填槽（也是唯一地址，指向 loud trap）
        const std::uint32_t get_ea = m.mem().rd32(vtbl + 0u);
        const std::uint32_t hole_ea = m.mem().rd32(vtbl + 4u);
        const std::uint32_t add_ea = m.mem().rd32(vtbl + 8u);
        LIFT_CHECK_NE_U32(get_ea, 0u);
        LIFT_CHECK_NE_U32(hole_ea, 0u);
        LIFT_CHECK_NE_U32(add_ea, 0u);
        LIFT_CHECK(get_ea != add_ea && get_ea != hole_ea && hole_ea != add_ea);
        LIFT_CHECK(m.shims().byStubEa(get_ea) != nullptr);
        LIFT_CHECK(m.shims().byStubEa(add_ea) != nullptr);
        LIFT_CHECK(m.shims().byStubEa(hole_ea) != nullptr);
        // ★ 必须装进块缓存（否则 guest 的 call [vtbl+off] 会去解码桩页字节）
        LIFT_CHECK(m.cache().peek(get_ea) != nullptr);
        LIFT_CHECK(m.cache().peek(add_ea) != nullptr);
        LIFT_CHECK(m.cache().peek(hole_ea) != nullptr);
        // 注册名：`com!<iface>::<method>` / `com!<iface>::unsupported+0xNN`
        if (const lift::ShimDef* d = m.shims().byStubEa(get_ea)) {
            LIFT_CHECK(d->module == "com");
            LIFT_CHECK(d->name == "ITest::GetValue");
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(d->argc), 1u);  // 0 参数 + this
        }
        if (const lift::ShimDef* d = m.shims().byStubEa(add_ea)) {
            LIFT_CHECK(d->name == "ITest::Add");
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(d->argc), 3u);  // 2 参数 + this
        }
        if (const lift::ShimDef* d = m.shims().byStubEa(hole_ea)) {
            LIFT_CHECK(d->name == "ITest::unsupported+0x4");
            LIFT_CHECK(d->fn != nullptr);  // 它也有一条**会响**的 thunk（不是"不存在"）
        }

        // ---- guest：mov eax, vtbl / push this / call [eax+0] / ret ----
        Asm a;
        a.movEaxImm(vtbl);
        a.pushImm(obj);  // this
        a.callMemEaxDisp8(0);
        a.ret();
        LIFT_CHECK(m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err));
        m.cpu().eip = kCodeEa;
        lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_get_value_calls, 1);
        LIFT_CHECK_EQ_U32(g_com_this, obj);  // ★ 方法 shim 收到的是对象自己的 EA
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0x1111u);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), m.layout().stack_top);  // 被调方弹 this + ret 弹哨兵

        // ---- guest：Add(this, 1, 2)（stdcall 从右往左 push ⇒ arg0 在最低地址）----
        Asm b;
        b.movEaxImm(vtbl);
        b.pushImm(2);    // arg1
        b.pushImm(1);    // arg0
        b.pushImm(obj);  // this
        b.callMemEaxDisp8(8);
        b.ret();
        const std::uint32_t code2 = kCodeEa + kCodeStride;
        LIFT_CHECK(m.placeCode(code2, b.b.data(), b.b.size(), &err));
        reprimeStack(m);
        m.cpu().eip = code2;
        r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(g_add_calls, 1);
        LIFT_CHECK_EQ_U32(g_com_this, obj);
        LIFT_CHECK_EQ_U32(g_com_arg0, 1u);
        LIFT_CHECK_EQ_U32(g_com_arg1, 2u);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 3u);
        LIFT_CHECK_EQ_U32(m.cpu().esp(), m.layout().stack_top);

        // ---- 未填的 +4 槽：loud trap（⛔ 不是"静默返回 0"）----
        g_current_case = "com-unsupported-slot-traps";
        Asm c;
        c.movEaxImm(vtbl);
        c.pushImm(obj);
        c.callMemEaxDisp8(4);
        c.ret();
        const std::uint32_t code3 = kCodeEa + 2u * kCodeStride;
        LIFT_CHECK(m.placeCode(code3, c.b.data(), c.b.size(), &err));
        reprimeStack(m);
        const std::uint32_t traps_before = m.cpu().shim.unimplemented_calls;
        m.cpu().eip = code3;
        r = run(m);
        // ★ 已注册的槽（thunk 的 fn 就是 comUnsupported）⇒ **非正常结局**：`Trap`（非 ok）。
        //   本单之前这里落 `Stopped`(ok) ⇒ 进程 exit 0，正是要消掉的缝。
        LIFT_CHECK(r.reason == lift::StopReason::Trap);
        LIFT_CHECK(!r.ok());
        LIFT_CHECK(!m.cpu().running);
        LIFT_CHECK_EQ_U32(m.cpu().shim.unimplemented_calls, traps_before + 1u);
        LIFT_CHECK(m.cpu().stop_why != nullptr);

        // ---- 同一台 Machine 的**下一轮**：宿主停机种类必须按轮重置 ----
        //   ★ 上一轮是 `Trap`。若 `Machine::run` 开头不重置 `stop_kind`，这一轮的正常主动停
        //     （模拟 ExitProcess：shim 只置 running=false + stop_why）会被误判成 `Trap` ⇒ 红。
        g_current_case = "stop-kind-reset-per-run";
        lift::ShimDef* stop_def = m.shims().addRuntime("test", "NormalStop", lift::Conv::Stdcall, 0, &shimNormalStop);
        LIFT_CHECK(stop_def != nullptr);
        if (stop_def != nullptr) {
            const std::uint32_t stop_ea = stop_def->stub_ea;
            LIFT_CHECK(hook(m, "test", "NormalStop", stop_ea));
            const std::uint32_t code4 = kCodeEa + 3u * kCodeStride;
            Asm d;
            d.callRel(code4, stop_ea);
            d.ret();
            LIFT_CHECK(m.placeCode(code4, d.b.data(), d.b.size(), &err));
            reprimeStack(m);
            m.cpu().eip = code4;
            r = run(m);
            LIFT_CHECK(r.reason == lift::StopReason::Stopped);  // 正常主动停，不是上一轮的 Trap
            LIFT_CHECK(r.ok());
        }

        // ---- refcount：addRef / release / 到 0 释放 ----
        g_current_case = "com-refcount-release";
        LIFT_CHECK(com.addRef(m.cpu(), obj));
        LIFT_CHECK_EQ_U32(com.refCount(obj), 2u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(obj + 4), 2u);  // guest 可见的副本同步
        LIFT_CHECK(com.release(m.cpu(), obj));
        LIFT_CHECK(com.find(obj));                     // 还没到 0
        LIFT_CHECK_EQ_U32(com.refCount(obj), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(obj + 4), 1u);
        LIFT_CHECK(com.release(m.cpu(), obj));          // 到 0 ⇒ 释放 vtable + 对象
        LIFT_CHECK(!com.find(obj));
        LIFT_CHECK_EQ_U32(com.refCount(obj), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 0u);
        LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);  // guest 堆归还（vtable + 对象）
        LIFT_CHECK_EQ_U32(m.mem().rd32(obj), 0u);               // 对象头被抹掉（残留指针看得出"不是对象"）
        // 重复 release / 未知对象 / addRef 未知对象 ⇒ false（⛔ 不静默），且不再动堆
        LIFT_CHECK(!com.release(m.cpu(), obj));
        LIFT_CHECK(!com.addRef(m.cpu(), obj));
        LIFT_CHECK(!com.release(m.cpu(), 0x1234u));
        LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);

        // ---- 同一 iface 的第二个对象：复用同一批 stub（⛔ 不占新槽、不串线）----
        g_current_case = "com-second-object-reuses-stubs";
        const std::uint32_t slots_before = m.shims().nextStubSlot();
        const std::uint32_t obj2 = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_NE_U32(obj2, 0u);
        // ⚠ 地址可能与 obj 相同：guest 堆是首次适配，刚释放的块会被复用（确定性，不是 bug）
        const std::uint32_t vtbl2 = m.mem().rd32(obj2);
        LIFT_CHECK_NE_U32(vtbl2, 0u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(vtbl2 + 0u), get_ea);
        LIFT_CHECK_EQ_U32(m.mem().rd32(vtbl2 + 8u), add_ea);
        LIFT_CHECK_EQ_U32(m.mem().rd32(vtbl2 + 4u), hole_ea);
        LIFT_CHECK_EQ_U32(m.shims().nextStubSlot(), slots_before);  // 没有新占槽
        LIFT_CHECK_EQ_U32(com.refCount(obj2), 1u);
        LIFT_CHECK(com.release(m.cpu(), obj2));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 0u);
        LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);

        // ---- 对象表代际：EA 被复用后，**陈旧持有者**的释放不许动新对象 ----
        //   （REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）guest 堆是首次适配 ⇒ `create A → release A → create B`
        //   之后 B **一定**拿到 A 的 EA（同一个 8 B 对象块 + 同一个 vtable 块，见上面那条 ⚠）。
        //   Release shim 只拿得到 `this` 这个 EA ⇒ guest 路径**不可校验**（那是 guest 自己的
        //   use-after-release，真机同样 UB）；宿主侧缓存必须自己带代际。
        g_current_case = "com-generation-stale-holder";
        const std::uint32_t gen_obj_a = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_NE_U32(gen_obj_a, 0u);
        const std::uint32_t gen_a = com.generationOf(gen_obj_a);
        LIFT_CHECK_NE_U32(gen_a, 0u);  // 代际非 0（0 保留给"没有代际"）
        LIFT_CHECK(com.release(m.cpu(), gen_obj_a));  // 到 0 ⇒ 删表 + 归还 guest 堆
        LIFT_CHECK(!com.find(gen_obj_a));
        LIFT_CHECK_EQ_U32(com.generationOf(gen_obj_a), 0u);  // 表里没有 ⇒ 0（不是"记住旧代际"）

        const std::uint32_t gen_obj_b = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_NE_U32(gen_obj_b, 0u);
        // ★ 前提必须**硬断言**：否则下面的 releaseIf 会因为"表里没这个 EA"而返回 false，
        //   代际校验被短路也照样绿（恒真守卫）。★ 前提红了 ⇒ 先按 `runtime/heap.cpp` 的分配策略
        //   重造夹具，⛔ 不是删断言。
        LIFT_CHECK_EQ_U32(gen_obj_b, gen_obj_a);  // EA 被复用（判据本身）
        const std::uint32_t gen_b = com.generationOf(gen_obj_b);
        LIFT_CHECK_NE_U32(gen_b, 0u);
        LIFT_CHECK_NE_U32(gen_b, gen_a);  // 同一个 EA、**不同的代际**

        // ① 陈旧持有者按 (EA, 旧代际) 释放 ⇒ 拒绝，且新对象一个引用都不掉
        LIFT_CHECK(!com.releaseIf(m.cpu(), gen_obj_b, gen_a));
        LIFT_CHECK(com.find(gen_obj_b));
        LIFT_CHECK_EQ_U32(com.refCount(gen_obj_b), 1u);
        LIFT_CHECK_EQ_U32(m.mem().rd32(gen_obj_b + 4), 1u);  // guest 可见的镜像也没被动
        LIFT_CHECK_EQ_U32(com.ifaceOf(gen_obj_b) != nullptr ? 1u : 0u, 1u);
        // ② 代际对得上 ⇒ 真的减（校验不是"永远拒绝"）
        LIFT_CHECK(com.releaseIf(m.cpu(), gen_obj_b, gen_b));
        LIFT_CHECK(!com.find(gen_obj_b));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 0u);
        LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);

        // ③ `owner`（宿主表里唯一**不持引用**的 EA 缓存）把"建它时的父代际"一起固化
        const std::uint32_t gen_parent = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_NE_U32(gen_parent, 0u);
        const std::uint32_t gen_child = com.createObject(m.cpu(), "ITest", methods, /*user=*/0u,
                                                         /*on_zero=*/nullptr, /*owner=*/gen_parent);
        LIFT_CHECK_NE_U32(gen_child, 0u);
        LIFT_CHECK_EQ_U32(com.ownerOf(gen_child), gen_parent);
        LIFT_CHECK_EQ_U32(com.ownerGenerationOf(gen_child), com.generationOf(gen_parent));
        LIFT_CHECK_NE_U32(com.ownerGenerationOf(gen_child), 0u);
        LIFT_CHECK_EQ_U32(com.ownerGenerationOf(gen_parent), 0u);  // 没有 owner ⇒ 0（不编一个）
        // 父对象被复用后，子对象固化的代际**仍然指向建它时那个**（这正是 releaseIf 的判据）
        const std::uint32_t gen_parent_gen = com.generationOf(gen_parent);
        LIFT_CHECK(com.release(m.cpu(), gen_child));
        LIFT_CHECK(com.find(gen_parent));  // 释放子对象**不**释放 owner（不是所有权转移）
        LIFT_CHECK(com.release(m.cpu(), gen_parent));
        const std::uint32_t gen_parent2 = com.createObject(m.cpu(), "ITest", methods);
        LIFT_CHECK_EQ_U32(gen_parent2, gen_parent);       // 同 EA
        LIFT_CHECK_NE_U32(com.generationOf(gen_parent2), gen_parent_gen);  // 不同代际
        LIFT_CHECK(com.release(m.cpu(), gen_parent2));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 0u);
        LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);

        // ---- ④ 响亮拒绝的**默认级别可见性**（`REQ-01M70FP71PWDFKGQ06QE37EQJY` #2）----
        // 缺陷形态：两处"响亮拒绝"走 `comLog`（`LogLevel::Info`），而默认 `--log-level` 是 `warn`
        //   ⇒ 默认跑法下这两行**根本看不见**（`ddrawWarn` / `d3dxWarn` / `pipeWarn` / `spriteWarn`
        //   都是 Warn，只有它低一级）。判据：在**默认级别**（warn）下造出该拒绝，stderr 里必须有那行。
        // ★ 变异（把 `comWarn` 改回 `logV(LogLevel::Info, …)`）⇒ 捕获到的 log 里没有这行 ⇒ 红。
        g_current_case = "com-rejection-visible-at-default-level";
        {
            lift::logSetLevel("warn");  // 默认级别；⛔ 不许用 info 把断言混过去
            LIFT_CHECK(lift::logConfig().level == lift::LogLevel::Warn);
            StderrTee cap;
            LIFT_CHECK(cap.begin());
            // `owns_owner=true` 但 `owner=0` ⇒ 没有可持引用的父对象 ⇒ 拒绝建对象（两处之一）
            LIFT_CHECK_EQ_U32(com.createObject(m.cpu(), "ITest", methods, /*user=*/0u,
                                               /*on_zero=*/nullptr, /*owner=*/0u, /*owns_owner=*/true),
                              0u);
            const std::string log = cap.finish();
            LIFT_CHECK(contains(log, "[warn] [shim] com!ITest:"));
            LIFT_CHECK(contains(log, "owns_owner=true 但 owner=0"));
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(com.objectCount()), 0u);
            LIFT_CHECK_EQ_U32(m.heap().bytesInUse(), heap_before);  // 拒绝路径不分配
        }
    }

    // =====================================================================================
    // ③ GetProcAddress 的通用路径（guest 侧真的走一遍）
    // =====================================================================================
    {
        g_current_case = "getprocaddress-runtime-export";
        lift::Machine m;
        std::string err;
        const std::uint32_t mod_ea = kDataEa;
        const std::uint32_t proc_ea = kDataEa + 0x20u;

        Asm a;
        a.pushImm(mod_ea);                                  // LoadLibraryA("DDRAW.DLL")
        a.callRel(kCodeEa, kProcStubEa);
        a.pushImm(proc_ea);                                 // lpProcName
        a.pushEax();                                        // hModule
        a.callRel(kCodeEa, kProcStubEa + 4u);               // eax = DirectDrawCreate 的 thunk
        a.pushImm(0);                                       // pUnkOuter
        a.pushImm(0);                                       // lplpDD
        a.pushImm(0x1234u);                                 // lpGUID（不透明值，只看它被送到 shim）
        a.callEax();                                        // ★ 动态导出：call eax
        a.ret();

        // boot 里先 configureStubPage ⇒ 下面的 addRuntime 才有桩页可用
        LIFT_CHECK(boot(m, a, kCodeEa, &err));
        lift::registerProcessShims(m.shims());
        LIFT_CHECK(hook(m, "kernel32", "LoadLibraryA", kProcStubEa));
        LIFT_CHECK(hook(m, "kernel32", "GetProcAddress", kProcStubEa + 4u));

        // 运行期登记一个"导出"（真链路上这是 ddraw.cpp 自己 addRuntime 的东西）
        lift::ShimDef* exp = m.shims().addRuntime("ddraw", "DirectDrawCreate", lift::Conv::Stdcall, 3,
                                                 &shimDirectDrawCreate);
        LIFT_CHECK(exp != nullptr);
        if (exp) {
            LIFT_CHECK_NE_U32(exp->stub_ea, 0u);
            m.cache().installShim(exp->stub_ea, exp->id);  // 裸 addRuntime 的调用方负责装块
        }
        // 同一模块键的**另一个**名字不存在 ⇒ 127（模块认得、导出没有）
        LIFT_CHECK(m.shims().find("ddraw", "Nope") == nullptr);

        LIFT_CHECK(putString(m, mod_ea, "DDRAW.DLL", &err));
        LIFT_CHECK(putString(m, proc_ea, "DirectDrawCreate", &err));
        m.cpu().eip = kCodeEa;
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g_ddraw_calls), 1u);  // ★ 运行期导出真的被调到了
        LIFT_CHECK_EQ_U32(g_ddraw_guid, 0x1234u);                          // 参数按 stdcall 顺序送到
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0x88760000u);
        LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 0u);  // 成功路径不该改 GetLastError
    }

    // ---- ③b：句柄认得、但模块没有这个导出 ⇒ 0 + 127 ----
    {
        g_current_case = "getprocaddress-proc-not-found";
        lift::Machine m;
        std::string err;
        const std::uint32_t mod_ea = kDataEa;
        const std::uint32_t proc_ea = kDataEa + 0x20u;
        Asm a;
        a.pushImm(mod_ea);
        a.callRel(kCodeEa, kProcStubEa);
        a.pushImm(proc_ea);
        a.pushEax();
        a.callRel(kCodeEa, kProcStubEa + 4u);
        a.ret();

        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerProcessShims(m.shims());
        LIFT_CHECK(hook(m, "kernel32", "LoadLibraryA", kProcStubEa));
        LIFT_CHECK(hook(m, "kernel32", "GetProcAddress", kProcStubEa + 4u));
        LIFT_CHECK(putString(m, mod_ea, "DDRAW.DLL", &err));
        LIFT_CHECK(putString(m, proc_ea, "NotAnExport", &err));
        LIFT_CHECK(m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err));
        m.cpu().eip = kCodeEa;
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0u);
        LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 127u);  // ERROR_PROC_NOT_FOUND
    }

    // ---- ③c：句柄不认得 ⇒ 0 + 126（与"没有这个导出"分开）----
    {
        g_current_case = "getprocaddress-bad-handle";
        lift::Machine m;
        std::string err;
        const std::uint32_t proc_ea = kDataEa;
        Asm a;
        a.pushImm(proc_ea);
        a.pushImm(0x00DEAD00u);  // 不是任何模块句柄
        a.callRel(kCodeEa, kProcStubEa + 4u);
        a.ret();

        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerProcessShims(m.shims());
        LIFT_CHECK(hook(m, "kernel32", "GetProcAddress", kProcStubEa + 4u));
        LIFT_CHECK(putString(m, proc_ea, "DirectDrawCreate", &err));
        LIFT_CHECK(m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err));
        m.cpu().eip = kCodeEa;
        const lift::RunResult r = run(m);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0u);
        LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 126u);  // ERROR_MOD_NOT_FOUND
    }

    // ---- ③d：AGERC 没有镜像 ⇒ 按**真实模型**失败（⛔ 不假装成功）----
    // ★ 2026-11-04 改口径：AGERC.DLL 是**真实 guest 模块**；没有镜像时 LoadLibrary 返回 0 + 126，
    //   GetProcAddress(0, …) 也返回 0 + 126（句柄不认得）。
    //   真实的"有镜像 ⇒ 同一 base / thunk / _GetInstance=base"由 tests/agerc_module.cpp 覆盖。
    {
        g_current_case = "getprocaddress-agerc-no-image";
        lift::Machine m;
        std::string err;
        const std::uint32_t mod_ea = kDataEa;
        const std::uint32_t proc_ea = kDataEa + 0x20u;

        LIFT_CHECK(m.initBare(&err));
        LIFT_CHECK(m.prepareStack(&err));
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerProcessShims(m.shims());
        lift::registerAgercShims(m.shims());  // 三个导出登记了也没用：镜像不在就装不上
        LIFT_CHECK(hook(m, "kernel32", "LoadLibraryA", kProcStubEa));
        LIFT_CHECK(hook(m, "kernel32", "GetProcAddress", kProcStubEa + 4u));
        LIFT_CHECK(putString(m, mod_ea, "AGERC.DLL", &err));
        LIFT_CHECK(putString(m, proc_ea, "_GetInstance@0", &err));

        // ① LoadLibraryA("AGERC.DLL") ⇒ 0 + ERROR_MOD_NOT_FOUND
        Asm a;
        a.pushImm(mod_ea);
        a.callRel(kCodeEa, kProcStubEa);
        a.ret();
        LIFT_CHECK(m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err));
        m.cpu().eip = kCodeEa;
        LIFT_CHECK(run(m).reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0u);
        LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 126u);
        LIFT_CHECK(!m.cpu().shim.agerc_loaded);

        // ② GetProcAddress(0, "_GetInstance@0") ⇒ 0 + 126（句柄不认得）
        Asm b;
        b.pushImm(proc_ea);
        b.pushImm(0u);
        b.callRel(kCodeEa + kCodeStride, kProcStubEa + 4u);
        b.ret();
        LIFT_CHECK(m.placeCode(kCodeEa + kCodeStride, b.b.data(), b.b.size(), &err));
        reprimeStack(m);
        m.cpu().eip = kCodeEa + kCodeStride;
        LIFT_CHECK(run(m).reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(m.cpu().eax(), 0u);
        LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 126u);
    }

    // ---- ④ 句柄表：**下标查找的语义**（值 = kFirst + kStep*下标；槽位只增不减）----
    //   ★ 为什么这条要存在：`get()` 从"线性扫描"改成"按下标反算"是性能修复（DeleteObject 的
    //     O(N²) 热点），但**判据是行为等价** —— 合法 / 已关闭 / 非法形状 / 越界 四类句柄必须
    //     与旧实现给出同一答案。⛔ 这条不测快慢（墙钟断言会变成 flaky）。
    {
        g_current_case = "handle-table-index-lookup";
        lift::HandleTable table;
        const std::uint32_t a = table.add(lift::HandleKind::Event, "a", nullptr);
        const std::uint32_t b = table.add(lift::HandleKind::File, "b", nullptr);
        const std::uint32_t c = table.add(lift::HandleKind::Timer, "c", nullptr);
        LIFT_CHECK_EQ_U32(lift::HandleTable::kFirst, 0x100u);
        LIFT_CHECK_EQ_U32(a, 0x100u);
        LIFT_CHECK_EQ_U32(b, 0x104u);
        LIFT_CHECK_EQ_U32(c, 0x108u);
        LIFT_CHECK(table.get(a) != nullptr);
        LIFT_CHECK(table.get(b) != nullptr);
        LIFT_CHECK(table.get(c) != nullptr);
        LIFT_CHECK(table.get(a)->kind == lift::HandleKind::Event);
        LIFT_CHECK(table.get(c)->kind == lift::HandleKind::Timer);
        LIFT_CHECK(table.get(b)->name == "b");
        // 关闭一个 ⇒ 只有它消失，别的槽仍是它自己的那一条（下标反算不许串号）
        LIFT_CHECK(table.close(b));
        LIFT_CHECK(table.get(b) == nullptr);
        LIFT_CHECK(table.get(a) != nullptr);
        LIFT_CHECK(table.get(c) != nullptr);
        // 非法形状：kFirst 以下 / 不是 kStep 的整数倍 / 超出已分配槽数 ⇒ 一律 nullptr
        LIFT_CHECK(table.get(0u) == nullptr);
        LIFT_CHECK(table.get(lift::HandleTable::kFirst - 4u) == nullptr);
        LIFT_CHECK(table.get(lift::HandleTable::kFirst + 1u) == nullptr);
        LIFT_CHECK(table.get(c + 4u) == nullptr);
        LIFT_CHECK(table.get(0xFFFFFFFCu) == nullptr);  // 不因为 (h - kFirst) 溢出而回绕进表
        // 新加的槽接在末尾，旧下标仍然命中自己那一条
        const std::uint32_t d = table.add(lift::HandleKind::Mutex, "d", nullptr);
        LIFT_CHECK_EQ_U32(d, 0x10Cu);
        LIFT_CHECK(table.get(a)->kind == lift::HandleKind::Event);
        LIFT_CHECK(table.get(c)->kind == lift::HandleKind::Timer);
        LIFT_CHECK(table.get(d)->kind == lift::HandleKind::Mutex);
        LIFT_CHECK(table.get(b) == nullptr);
    }

    // =====================================================================================
    // ⑤ 退出码映射：`main` 的兜底（`exitCodeForRun`）必须把两个新停机原因判成非 0
    //   ★ 这是**映射级**判据，不是端到端进程退出码：真黑盒要能从 CLI 造出 OOM，而兄弟单已裁
    //     "⛔ 不把夹具造 OOM 做成产品参数"。`main.cpp` 的出口就是调用这个函数 ⇒ 构造即保证。
    // =====================================================================================
    {
        g_current_case = "exit-code-for-run";
        lift::RunResult r;
        r.reason = lift::StopReason::ResourceExhausted;
        LIFT_CHECK(!r.ok());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::exitCodeForRun(r)), 2u);
        LIFT_CHECK(std::string(lift::stopReasonName(r.reason)) == "resource-exhausted");
        r.reason = lift::StopReason::Trap;
        LIFT_CHECK(!r.ok());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::exitCodeForRun(r)), 2u);
        LIFT_CHECK(std::string(lift::stopReasonName(r.reason)) == "unimplemented-slot");
        r.reason = lift::StopReason::Stopped;
        LIFT_CHECK(r.ok());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::exitCodeForRun(r)), 0u);
    }

    return finish("mechanism");
}
