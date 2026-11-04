// emulator-lift · 文件与路径族（pure：在 $env:TEMP 下自建目录，不碰游戏 / LFS / `--game-dir`）
//
// 判据（plan §6 S6 / §9 R2）：
// ① `CreateFileA` / `WriteFile` / `CloseHandle` 之后，**宿主盘上**真的出现了那个文件，且内容逐字节相同；
// ② 大小写不敏感路径解析：`SUB\A.TXT` 命中宿主上的 `sub/a.txt`，内容逐字节读回（含 `SetFilePointer`
//    之后的位置语义、EOF 的"TRUE + 0 字节"）；
// ③ `GetFileSize` 的高低 32 位、`FindFirstFileA` / `FindNextFileA` 的名字与大小、
//    枚举结束码 `ERROR_NO_MORE_FILES(18)`、`FindClose` 回收；
// ④ 属性（文件 = ARCHIVE、目录 = DIRECTORY）、`GetCurrentDirectoryA` 的"缺省 = guestRoot"、
//    `SetCurrentDirectoryA("sub")` 之后相对路径确实落在 `sub` 下；
// ⑤ 越界：`..\..\escape.txt` ⇒ `INVALID_HANDLE_VALUE` + 非 0 错误码，且**宿主上任何地方都没有**
//    那个文件（⛔ 也没"夹回根里"）；
// ⑥ 删除后 `GetFileAttributesA` 走真实失败码 2。
//
// ★ 为什么每条断言都要看宿主盘：本族最坏的失败形态是"guest 以为成功了、盘上什么都没有"
//   （见 overrides.md 的"⛔ 没有静默 no-op 这一档"）⇒ 只断言返回值等于 1 是不够的。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/machine.h"
#include "shims/register.h"

// 父代理在 `shims/register.h` 里补 `void registerFileShims(ShimRegistry&)` 的声明；
// 这里再声明一次，让本夹具**不依赖**那次改动（C++ 允许重复的相同声明）。
namespace lift {
void registerFileShims(ShimRegistry& shims);
}

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kDataEa = 0x00402000u;
constexpr std::uint32_t kStubEa = 0x7FFE0200u;  // 文件族的桩地址从 0x7FFE0200 起（4 字节步进）

constexpr std::uint32_t kInvalidHandleValue = 0xFFFFFFFFu;

// Win32 常量（与 kernel32_file.cpp 的同一个名字对应）
constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kGenericWrite = 0x40000000u;
constexpr std::uint32_t kCreateAlways = 2u;
constexpr std::uint32_t kOpenExisting = 3u;
constexpr std::uint32_t kAttrDirectory = 0x00000010u;
constexpr std::uint32_t kAttrArchive = 0x00000020u;

// WIN32_FIND_DATAA（x86，0x140 字节）的字段偏移
constexpr std::uint32_t kFindDataSize = 0x140u;
constexpr std::uint32_t kFdAttributes = 0x00u;
constexpr std::uint32_t kFdSizeHigh = 0x1Cu;
constexpr std::uint32_t kFdSizeLow = 0x20u;
constexpr std::uint32_t kFdFileName = 0x2Cu;

std::uint32_t processId() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// 直接读宿主文件（判据要求"逐字节相等"，⛔ 不读 guest 里的副本自证）
std::string readHostFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::string("<打不开>");
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 最小机器码装配器（照 tests/strfmt.cpp）
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
        const auto at = static_cast<std::int32_t>(currentEa());
        const auto rel = static_cast<std::int32_t>(target) - (at + 5);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void ret() { u8(0xC3); }
};

struct Stub {
    std::string name;
    std::uint32_t ea = 0;
    std::uint32_t id = 0;
};

struct Fixture {
    lift::Machine m;
    std::string err;
    std::uint32_t data_cursor = kDataEa;
    bool ok = true;
    std::vector<Stub> stubs;

    explicit Fixture(const std::string& game_dir) {
        ok = m.initBare(&err) && m.prepareStack(&err);
        if (!ok) return;
        lift::registerFileShims(m.shims());
        lift::registerSyncShims(m.shims());  // CloseHandle（文件句柄由它调 destroy 回收）

        const char* const names[] = {
            "CreateFileA",     "ReadFile",          "WriteFile",        "SetFilePointer",
            "GetFileSize",     "CloseHandle",       "FindFirstFileA",   "FindNextFileA",
            "FindClose",       "CreateDirectoryA",  "DeleteFileA",      "GetFileAttributesA",
            "GetCurrentDirectoryA", "SetCurrentDirectoryA",
        };
        std::uint32_t ea = kStubEa;
        for (const char* name : names) {
            lift::ShimDef* def = m.shims().findMutable("kernel32", name);
            if (!def) {
                ok = false;
                err = std::string("shim 没注册：kernel32!") + name;
                return;
            }
            def->stub_ea = ea;
            stubs.push_back(Stub{name, ea, def->id});
            ea += 4u;
        }
        m.vfs().setGameDir(game_dir);
        // ★ 用 ASCII 的 guest 根：A 版返回的是 **CP932 字节**，而 std::string 的期望值是 UTF-8 ——
        //   日文根会让"编码正确"看起来像"路径错误"。CP932 的编解码本身由 strfmt 夹具守。
        m.vfs().setGuestRoot("C:\\lift-game");
        m.vfs().setGuestCwd("C:\\lift-game");
        for (const Stub& s : stubs) m.cache().installShim(s.ea, s.id);
    }

    std::uint32_t stubEa(const std::string& name) const {
        for (const Stub& s : stubs) {
            if (s.name == name) return s.ea;
        }
        return 0;
    }

    std::uint32_t put(const void* p, std::size_t n) {
        const std::uint32_t ea = data_cursor;
        data_cursor = (data_cursor + static_cast<std::uint32_t>(n) + 15u) & ~15u;
        ok = ok && m.placeData(ea, p, n, &err);
        return ea;
    }
    std::uint32_t cstr(const std::string& s) { return put(s.c_str(), s.size() + 1); }
    std::uint32_t zeroBuf(std::size_t n = 512) {
        std::vector<std::uint8_t> z(n, 0);
        return put(z.data(), z.size());
    }

    // 一次 guest 调用：push（逆序）+ call + ret。
    // ★ stdcall：被调方在 dispatcher 里已经弹过 `argc*4` 字节 ⇒ 这里**只** `ret`
    //   （再 `add esp` 会多弹一次，`ret` 就吃到栈外的垃圾 —— 见 tests/strfmt.cpp 的 cdecl 版差异）。
    std::uint32_t call(const std::string& shim, const std::vector<std::uint32_t>& args) {
        if (!ok) return 0;
        const std::uint32_t target = stubEa(shim);
        if (target == 0) {
            lift_test::failf(__FILE__, __LINE__, "shim \"%s\" 在本夹具里没有桩地址", shim.c_str());
            return 0;
        }
        // ★ 每次调用都清块缓存：不同实参个数会生成不同的机器码，同一个 EA 不能复用旧块。
        //   `clear()` 会连 shim 块一起清掉 ⇒ 清完再把它们装回去。
        m.cache().clear();
        for (const Stub& s : stubs) m.cache().installShim(s.ea, s.id);
        // ★ 复位栈到哨兵：上一次 `ret` 之后 esp 已经抬过 4 字节，不复位会覆盖 0xFFFFFFFF 哨兵。
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), 0xFFFFFFFFu);

        Asm a;
        for (auto it = args.rbegin(); it != args.rend(); ++it) a.pushImm(*it);
        a.callRel(target);
        a.ret();
        ok = ok && m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err);
        m.cpu().eip = kCodeEa;
        if (!ok) return 0;

        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 10000;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        return m.cpu().eax();
    }
};

}  // namespace

int main() {
    using namespace lift_test;

    // ---------------- 唯一临时目录：$env:TEMP\lift-file-io-<pid>（结束时删掉）----------------
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path(ec) / ("lift-file-io-" + std::to_string(processId()));
    if (ec) {
        std::fprintf(stderr, "[file-io] 取宿主临时目录失败：%s\n", ec.message().c_str());
        return 1;
    }
    fs::remove_all(tmp, ec);
    ec.clear();
    if (!fs::create_directories(tmp, ec) || ec) {
        std::fprintf(stderr, "[file-io] 建临时目录 \"%s\" 失败：%s\n", pathToUtf8(tmp).c_str(),
                     ec.message().c_str());
        return 1;
    }
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{tmp};
    const std::string game = pathToUtf8(tmp);  // 宿主 game_dir（UTF-8）——本族的"唯一根"

    Fixture f(game);
    LIFT_CHECK(f.ok);
    if (!f.ok) {
        std::fprintf(stderr, "[file-io] 夹具构造失败：%s\n", f.err.c_str());
        return finish("file-io");
    }

    // ---------------- ① CreateDirectoryA + CreateFileA(CREATE_ALWAYS) + WriteFile + CloseHandle ----------------
    {
        g_current_case = "create-write-close";
        const std::uint32_t p_sub = f.cstr("sub");
        LIFT_CHECK_EQ_U32(f.call("CreateDirectoryA", {p_sub, 0u}), 1u);
        LIFT_CHECK(fs::is_directory(tmp / "sub"));  // 宿主盘上真的建出来了

        const std::uint32_t p_file = f.cstr("sub\\a.txt");
        const std::uint32_t h =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        LIFT_CHECK(fs::exists(tmp / "sub" / "a.txt"));  // 宿主盘上真的出现了

        const std::uint32_t data = f.cstr("hello");
        const std::uint32_t nwritten = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {h, data, 5u, nwritten, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nwritten), 5u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);

        std::error_code size_ec;
        const auto host_size = fs::file_size(tmp / "sub" / "a.txt", size_ec);
        LIFT_CHECK(!size_ec && host_size == 5u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");  // 逐字节
        std::printf("[case] create-write-close: CreateDirectoryA(\"sub\") 建出宿主目录；"
                    "CreateFileA(\"sub\\\\a.txt\", GENERIC_WRITE, CREATE_ALWAYS) ⇒ 句柄 0x%08X；"
                    "WriteFile 写 5 字节；CloseHandle 后宿主文件恰好是 \"hello\"\n",
                    h);
    }

    // ---------------- ② 大小写不敏感 + ReadFile + GetFileSize + SetFilePointer ----------------
    {
        g_current_case = "case-insensitive-read";
        const std::uint32_t p_file = f.cstr("SUB\\A.TXT");
        const std::uint32_t h =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);  // 大小写不敏感命中了 sub/a.txt

        const std::uint32_t hi = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("GetFileSize", {h, hi}), 5u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(hi), 0u);

        const std::uint32_t buf = f.zeroBuf(64);
        const std::uint32_t nread = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 16u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 5u);
        LIFT_CHECK(f.m.mem().readCString(buf) == "hello");

        // SetFilePointer(1, FILE_BEGIN) ⇒ 从第 2 个字节开始
        LIFT_CHECK_EQ_U32(f.call("SetFilePointer", {h, 1u, 0u, 0u}), 1u);
        f.m.mem().fillBytes(buf, 0, 64);  // 清掉上一次的字节：读到 "ello" 才算数
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 16u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 4u);
        LIFT_CHECK(f.m.mem().readCString(buf) == "ello");

        // EOF：真机是 TRUE + 0 字节（⛔ 不是 FALSE）
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 16u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 0u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);
        std::printf("[case] case-insensitive-read: CreateFileA(\"SUB\\\\A.TXT\", GENERIC_READ, "
                    "OPEN_EXISTING) 命中宿主 sub/a.txt；GetFileSize=5（hi=0）；ReadFile=\"hello\"；"
                    "SetFilePointer(1) 后 ReadFile=\"ello\"（4 字节）；EOF=TRUE+0\n");
    }

    // ---------------- ③ FindFirstFileA / FindNextFileA / FindClose ----------------
    {
        g_current_case = "find-first-next-close";
        const std::uint32_t pat = f.cstr("sub\\*.*");
        const std::uint32_t fd = f.zeroBuf(kFindDataSize);
        const std::uint32_t h = f.call("FindFirstFileA", {pat, fd});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(fd + kFdAttributes), kAttrArchive);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(fd + kFdSizeHigh), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(fd + kFdSizeLow), 5u);
        LIFT_CHECK(f.m.mem().readCString(fd + kFdFileName) == "a.txt");

        // sub 里此刻只有 a.txt ⇒ 下一次必须结束（FALSE + ERROR_NO_MORE_FILES(18)）
        const std::uint32_t more = f.call("FindNextFileA", {h, fd});
        LIFT_CHECK_EQ_U32(more, 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 18u);
        LIFT_CHECK_EQ_U32(f.call("FindClose", {h}), 1u);
        std::printf("[case] find-first-next-close: FindFirstFileA(\"sub\\\\*.*\") ⇒ 0x%08X，"
                    "cFileName=\"a.txt\"（attributes=0x%02X，size=5）；FindNextFileA=FALSE + "
                    "ERROR_NO_MORE_FILES(18)；FindClose=TRUE\n",
                    h, kAttrArchive);
    }

    // ---------------- ④ 属性 + 当前目录（相对路径落在 sub 下）----------------
    {
        g_current_case = "attributes-and-cwd";
        const std::uint32_t p_file = f.cstr("SUB\\a.txt");
        const std::uint32_t attrs = f.call("GetFileAttributesA", {p_file});
        LIFT_CHECK_NE_U32(attrs, kInvalidHandleValue);
        LIFT_CHECK((attrs & kAttrArchive) != 0u);   // 文件：ARCHIVE
        LIFT_CHECK((attrs & kAttrDirectory) == 0u);  // 不是目录
        const std::uint32_t p_dir = f.cstr("sub");
        LIFT_CHECK_EQ_U32(f.call("GetFileAttributesA", {p_dir}), kAttrDirectory);

        const std::uint32_t cdbuf = f.zeroBuf(260);
        LIFT_CHECK(f.call("GetCurrentDirectoryA", {260u, cdbuf}) > 0u);
        LIFT_CHECK(f.m.mem().readCString(cdbuf) == f.m.vfs().guestRoot());  // 缺省 = guestRoot

        LIFT_CHECK_EQ_U32(f.call("SetCurrentDirectoryA", {p_dir}), 1u);
        const std::string want_cwd = f.m.vfs().guestRoot() + "\\sub";
        // ★ 不断言"A 版返回的长度 == want_cwd.size()"：A 版按 **CP932 字节**返回长度，
        //   而 want_cwd 是 UTF-8（`天結` 在两种编码下字节数不同）。只要求"长度合理且小于缓冲"。
        const std::uint32_t n2 = f.call("GetCurrentDirectoryA", {260u, cdbuf});
        LIFT_CHECK(n2 > 0u && n2 < 260u);
        LIFT_CHECK(f.m.mem().readCString(cdbuf) == want_cwd);  // 读回来的是**新** cwd

        // 相对路径必须落在 sub 下（而不是 game_dir 根）
        const std::uint32_t p_b = f.cstr("b.txt");
        const std::uint32_t h =
            f.call("CreateFileA", {p_b, kGenericWrite, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);
        LIFT_CHECK(fs::exists(tmp / "sub" / "b.txt"));
        LIFT_CHECK(!fs::exists(tmp / "b.txt"));  // ⛔ 没有落在根上
        std::printf("[case] attributes-and-cwd: GetFileAttributesA(\"SUB\\\\a.txt\")=0x%02X"
                    "（ARCHIVE、非目录）、\"sub\"=0x%02X；GetCurrentDirectoryA=\"%s\"；"
                    "SetCurrentDirectoryA(\"sub\") 后建 \"b.txt\" ⇒ 落在宿主 sub/b.txt\n",
                    kAttrArchive, kAttrDirectory, want_cwd.c_str());
    }

    // ---------------- ⑤ 越界：..\..\ 必须响亮失败，且不许夹回根里 ----------------
    {
        g_current_case = "escape-root-rejected";
        // cwd 现在是 <root>\sub ⇒ `..\..` 会越过唯一根
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t p_escape = f.cstr("..\\..\\escape.txt");
        const std::uint32_t h =
            f.call("CreateFileA", {p_escape, kGenericWrite, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK_EQ_U32(h, kInvalidHandleValue);
        LIFT_CHECK_NE_U32(f.m.cpu().shim.last_error, 0u);

        // ⛔ 两个方向都要查：没有逃出根（上级目录），也没有被"夹回根里"（tmp 自己）
        LIFT_CHECK(!fs::exists(tmp / "escape.txt"));
        LIFT_CHECK(!fs::exists(tmp.parent_path() / "escape.txt"));
        LIFT_CHECK(!fs::exists(tmp.parent_path().parent_path() / "escape.txt"));
        std::printf("[case] escape-root-rejected: CreateFileA(\"..\\\\..\\\\escape.txt\", CREATE_ALWAYS) "
                    "⇒ INVALID_HANDLE_VALUE + last_error=%u；宿主 tmp / tmp 的上一级 / 上两级都没有 "
                    "escape.txt（既没逃出根，也没夹回根里）\n",
                    f.m.cpu().shim.last_error);
    }

    // ---------------- ⑥ DeleteFileA + 失败码 2 ----------------
    {
        g_current_case = "delete-and-missing";
        // 先退回根（顺带验 SetCurrentDirectoryA("..") 的折叠）
        const std::uint32_t p_dotdot = f.cstr("..");
        LIFT_CHECK_EQ_U32(f.call("SetCurrentDirectoryA", {p_dotdot}), 1u);
        const std::uint32_t cdbuf = f.zeroBuf(260);
        LIFT_CHECK(f.call("GetCurrentDirectoryA", {260u, cdbuf}) > 0u);
        LIFT_CHECK(f.m.mem().readCString(cdbuf) == f.m.vfs().guestRoot());

        LIFT_CHECK(fs::exists(tmp / "sub" / "b.txt"));  // 前提（④ 建的）
        const std::uint32_t p_b = f.cstr("sub\\b.txt");
        LIFT_CHECK_EQ_U32(f.call("DeleteFileA", {p_b}), 1u);
        LIFT_CHECK(!fs::exists(tmp / "sub" / "b.txt"));

        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("GetFileAttributesA", {p_b}), kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 2u);  // ERROR_FILE_NOT_FOUND
        std::printf("[case] delete-and-missing: SetCurrentDirectoryA(\"..\") 回到根；"
                    "DeleteFileA(\"sub\\\\b.txt\")=TRUE（宿主文件消失）；"
                    "再 GetFileAttributesA ⇒ 0xFFFFFFFF + last_error=2\n");
    }

    std::printf("[case] tmp = %s\n", game.c_str());
    return finish("file-io");
}
