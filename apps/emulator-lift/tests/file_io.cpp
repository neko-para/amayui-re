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
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
constexpr std::uint32_t kGenericAll = 0x10000000u;
constexpr std::uint32_t kFileReadData = 0x00000001u;
constexpr std::uint32_t kFileWriteData = 0x00000002u;
constexpr std::uint32_t kShareRead = 0x00000001u;   // FILE_SHARE_READ
constexpr std::uint32_t kShareWrite = 0x00000002u;  // FILE_SHARE_WRITE
constexpr std::uint32_t kCreateNew = 1u;
constexpr std::uint32_t kCreateAlways = 2u;
constexpr std::uint32_t kOpenExisting = 3u;
constexpr std::uint32_t kOpenAlways = 4u;
constexpr std::uint32_t kTruncateExisting = 5u;
constexpr std::uint32_t kAttrDirectory = 0x00000010u;
constexpr std::uint32_t kAttrArchive = 0x00000020u;

// dwMoveMethod
constexpr std::uint32_t kFileBegin = 0u;
constexpr std::uint32_t kFileCurrent = 1u;
constexpr std::uint32_t kFileEnd = 2u;

// 错误码（S6-1 / S6-2 / S6-6 的断言用）
constexpr std::uint32_t kErrAccessDenied = 5u;
constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrSharingViolation = 32u;
constexpr std::uint32_t kErrFileExists = 80u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrNegativeSeek = 131u;
constexpr std::uint32_t kErrAlreadyExists = 183u;

// 标准句柄三元组（GetFileType / GetConsoleMode / SetStdHandle）
constexpr std::uint32_t kFileTypeDisk = 1u;     // FILE_TYPE_DISK
constexpr std::uint32_t kFileTypeChar = 2u;     // FILE_TYPE_CHAR
constexpr std::uint32_t kStdInput = 0xFFFFFFF6u;   // (DWORD)-10
constexpr std::uint32_t kStdOutput = 0xFFFFFFF5u;  // (DWORD)-11
constexpr std::uint32_t kStdError = 0xFFFFFFF4u;   // (DWORD)-12

// WIN32_FIND_DATAA（x86，0x140 字节）的字段偏移
constexpr std::uint32_t kFindDataSize = 0x140u;
constexpr std::uint32_t kFdAttributes = 0x00u;
constexpr std::uint32_t kFdCreationTime = 0x04u;
constexpr std::uint32_t kFdLastAccessTime = 0x0Cu;
constexpr std::uint32_t kFdLastWriteTime = 0x14u;
constexpr std::uint32_t kFdSizeHigh = 0x1Cu;
constexpr std::uint32_t kFdSizeLow = 0x20u;
constexpr std::uint32_t kFdReserved0 = 0x24u;
constexpr std::uint32_t kFdReserved1 = 0x28u;
constexpr std::uint32_t kFdFileName = 0x2Cu;
constexpr std::uint32_t kFdAltName = 0x130u;

// 1601-01-01 → 1970-01-01 的 100 ns 数（与 kernel32_file.cpp:152 同一个常数）
constexpr std::uint64_t kFileTimeEpochDelta = 116444736000000000ull;

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

// 读 guest 里的 FILETIME（lo, hi 两个 DWORD）
std::uint64_t readFileTime(lift::Machine& m, std::uint32_t ea) {
    return static_cast<std::uint64_t>(m.mem().rd32(ea)) |
           (static_cast<std::uint64_t>(m.mem().rd32(ea + 4u)) << 32);
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
        // ★ 标准句柄三元组（GetStdHandle / SetStdHandle / GetFileType / GetConsoleMode）在
        //   bootstrap 那一族里注册（REQ-01M6K69K9HM3ETCDNMJWPHGXJC 的新用例要用）
        lift::registerBootstrapShims(m.shims());

        const char* const names[] = {
            "CreateFileA",     "ReadFile",          "WriteFile",        "SetFilePointer",
            "SetFilePointerEx", "GetFileSize",      "CloseHandle",      "FindFirstFileA",
            "FindNextFileA",   "FindClose",         "CreateDirectoryA", "DeleteFileA",
            "GetFileAttributesA", "GetCurrentDirectoryA", "SetCurrentDirectoryA",
            "GetFileTime",     "SetFileTime",
            "GetStdHandle",    "SetStdHandle",      "GetFileType",      "GetConsoleMode",
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

    // ---------------- ⑦ S6-1：`dwDesiredAccess = 0` 只查属性 ⇒ 不授予读/写，也不许截断 ----------------
    // 判据（Windows 文档）：access=0 = "neither"；`ReadFile`/`WriteFile` 必须失败（真实失败码
    // `ERROR_ACCESS_DENIED(5)` 是对文档的自洽选择，`<待核>`：该码本身未在真机采样）。
    // ⛔ 断言同时看**宿主盘**：最坏的失败形态是"返回 TRUE 但把文件清了"。
    {
        g_current_case = "access-zero-open";
        const std::uint32_t p_file = f.cstr("sub\\a.txt");

        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h =
            f.call("CreateFileA", {p_file, 0u, 0u, 0u, kOpenExisting, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");  // 没被顺手截断

        const std::uint32_t buf = f.zeroBuf(64);
        const std::uint32_t nread = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 5u, nread, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAccessDenied);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 0u);

        const std::uint32_t data = f.cstr("XX");
        const std::uint32_t nwritten = f.zeroBuf(4);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {h, data, 2u, nwritten, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAccessDenied);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nwritten), 0u);

        // 文档：`SetFilePointer` / `SetFilePointerEx` 的 hFile 必须带 GENERIC_READ 或 GENERIC_WRITE
        // ⇒ access=0 的句柄不许定位（FALSE + ERROR_ACCESS_DENIED）
        const std::uint32_t out8 = f.zeroBuf(8);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("SetFilePointer", {h, 0u, 0u, kFileBegin}), kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAccessDenied);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 0u, 0u, out8, kFileBegin}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAccessDenied);

        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);

        // 宿主盘：内容与长度都没变（上面两次失败的副作用为零）
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");
        std::error_code size_ec;
        const auto host_size = fs::file_size(tmp / "sub" / "a.txt", size_ec);
        LIFT_CHECK(!size_ec && host_size == 5u);

        // access=0 + CREATE_ALWAYS（已存在）⇒ **只保证不截断**。
        // ⚠ `<待核>`：真机在 access=0 时是否允许 CREATE_ALWAYS 打开/创建没有采样
        //   ⇒ 这里**不把返回码钉死**，只断言宿主内容（若将来改语义，这条仍成立）。
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h2 =
            f.call("CreateFileA", {p_file, 0u, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");
        if (h2 != kInvalidHandleValue) LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h2}), 1u);

        // 顺带把 S6-1 的另一半也钉住：具体的 FILE_*_DATA 位与 GENERIC_ALL 必须被解出来
        const std::uint32_t p_bits = f.cstr("sub\\bits.txt");
        const std::uint32_t hb =
            f.call("CreateFileA", {p_bits, kGenericAll, 0u, 0u, kCreateAlways, 0u, 0u});
        LIFT_CHECK_NE_U32(hb, kInvalidHandleValue);
        const std::uint32_t word = f.cstr("bit");
        const std::uint32_t nw = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {hb, word, 3u, nw, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nw), 3u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {hb}), 1u);

        const std::uint32_t hr =
            f.call("CreateFileA", {p_bits, kFileReadData, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(hr, kInvalidHandleValue);
        const std::uint32_t rb = f.zeroBuf(16);
        const std::uint32_t nr = f.zeroBuf(4);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {hr, rb, 16u, nr, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nr), 3u);
        LIFT_CHECK(f.m.mem().readCString(rb) == "bit");
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {hr}), 1u);

        // FILE_WRITE_DATA（具体的写位）也必须被解出来
        const std::uint32_t hw =
            f.call("CreateFileA", {p_bits, kFileWriteData, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(hw, kInvalidHandleValue);
        const std::uint32_t word2 = f.cstr("zip");
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {hw, word2, 3u, nw, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {hw}), 1u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "bits.txt") == "zip");  // 宿主盘真的被改写

        std::printf("[case] access-zero-open: CreateFileA(\"sub\\\\a.txt\", 0, 0, OPEN_EXISTING) "
                    "⇒ 句柄 0x%08X；ReadFile=FALSE+5、WriteFile=FALSE+5（宿主仍是 \"hello\"/5 字节）；"
                    "access=0+CREATE_ALWAYS 后宿主内容不变；GENERIC_ALL 可写、FILE_READ_DATA 可读\n",
                    h);
    }

    // ---------------- ⑧ S6-1：截断已有文件需要写权 ----------------
    {
        g_current_case = "truncate-needs-write";
        const std::uint32_t p_file = f.cstr("sub\\a.txt");
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h = f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u,
                                                       kTruncateExisting, 0x80u, 0u});
        LIFT_CHECK_EQ_U32(h, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAccessDenied);

        // 宿主盘：长度与内容都没变（⛔ 只读意图不许把文件清空）
        std::error_code size_ec;
        const auto host_size = fs::file_size(tmp / "sub" / "a.txt", size_ec);
        LIFT_CHECK(!size_ec && host_size == 5u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");
        std::printf("[case] truncate-needs-write: CreateFileA(\"sub\\\\a.txt\", GENERIC_READ, "
                    "TRUNCATE_EXISTING) ⇒ INVALID_HANDLE_VALUE + last_error=%u；宿主仍是 "
                    "\"hello\"/5 字节（没有真截断）\n",
                    f.m.cpu().shim.last_error);
    }

    // ---------------- ⑨ S6-2：共享冲突是**双向**的，且只由访问位定义 ----------------
    {
        g_current_case = "share-mode-conflict";
        const std::uint32_t p_file = f.cstr("sub\\a.txt");

        // a) 两个 access=0 / share=0 的"属性探针"必须都能开（真机口径；旧实现误报 32）
        const std::uint32_t a1 =
            f.call("CreateFileA", {p_file, 0u, 0u, 0u, kOpenExisting, 0x80u, 0u});
        const std::uint32_t a2 =
            f.call("CreateFileA", {p_file, 0u, 0u, 0u, kOpenExisting, 0x80u, 0u});
        LIFT_CHECK_NE_U32(a1, kInvalidHandleValue);
        LIFT_CHECK_NE_U32(a2, kInvalidHandleValue);
        LIFT_CHECK_NE_U32(a1, a2);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {a1}), 1u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {a2}), 1u);

        // b) 已有 GENERIC_READ + SHARE_READ，新请求 GENERIC_WRITE + share=0
        //    ⇒ 新 access 撞已有 share ⇒ 32
        const std::uint32_t b1 =
            f.call("CreateFileA", {p_file, kGenericRead, kShareRead, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(b1, kInvalidHandleValue);
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t b2 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_EQ_U32(b2, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrSharingViolation);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {b1}), 1u);

        // c) 已有 GENERIC_READ + SHARE_READ|SHARE_WRITE，新请求 GENERIC_READ + share=0
        //    ⇒ **已有 access 撞新 share**（第二个方向；旧实现完全不看这一维）
        const std::uint32_t c1 = f.call("CreateFileA", {p_file, kGenericRead,
                                                        kShareRead | kShareWrite, 0u,
                                                        kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(c1, kInvalidHandleValue);
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t c2 =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_EQ_U32(c2, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrSharingViolation);
        LIFT_CHECK(readHostFile(tmp / "sub" / "a.txt") == "hello");  // 被拒的打开没有副作用
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {c1}), 1u);

        // d) 判定跟着**活句柄**走（不是缓存）：关掉之后同样的打开成功
        const std::uint32_t d =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(d, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {d}), 1u);

        std::printf("[case] share-mode-conflict: a) 两个 access=0/share=0 都能开（0x%08X/0x%08X）；"
                    "b) 已有 READ+SHARE_READ 后 WRITE+share=0 ⇒ 0xFFFFFFFF+%u；"
                    "c) 已有 READ+SHARE_READ|WRITE 后 READ+share=0 ⇒ 0xFFFFFFFF+%u；"
                    "d) 关句柄后同样的打开成功 0x%08X\n",
                    a1, a2, kErrSharingViolation, kErrSharingViolation, d);
    }

    // ---------------- ⑩ S6-4：FIND_DATA / GetFileTime 的四个时间是宿主真值 ----------------
    {
        g_current_case = "find-data-times";
        const std::uint32_t p_dir = f.cstr("timecheck");
        LIFT_CHECK_EQ_U32(f.call("CreateDirectoryA", {p_dir, 0u}), 1u);
        const std::uint32_t p_file = f.cstr("timecheck\\t.dat");
        const std::uint32_t h =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kCreateAlways, 0u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);

        // 把 access / write 设成两个**不同**的过去时间 ⇒ 若实现仍拿 mtime 填三格，
        // access == write 必然成立，下面的"两两不等"会红。
        const std::int64_t now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count();
        const std::int64_t want_creation = now_seconds - 5000;
        const std::int64_t want_access = now_seconds - 3000;
        const std::int64_t want_write = now_seconds - 1000;
        auto putFileTime = [&](std::uint32_t ea, std::int64_t unix_seconds) {
            const auto v = static_cast<std::uint64_t>(
                unix_seconds * 10000000ll + static_cast<std::int64_t>(kFileTimeEpochDelta));
            f.m.mem().wr32(ea, static_cast<std::uint32_t>(v & 0xFFFFFFFFull));
            f.m.mem().wr32(ea + 4u, static_cast<std::uint32_t>(v >> 32));
        };
        const std::uint32_t ct = f.zeroBuf(8);
        const std::uint32_t at = f.zeroBuf(8);
        const std::uint32_t wt = f.zeroBuf(8);
        putFileTime(ct, want_creation);
        putFileTime(at, want_access);
        putFileTime(wt, want_write);
        LIFT_CHECK_EQ_U32(f.call("SetFileTime", {h, ct, at, wt}), 1u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);
        LIFT_CHECK(fs::exists(tmp / "timecheck" / "t.dat"));

        const std::uint32_t pat = f.cstr("timecheck\\*.*");
        const std::uint32_t fd = f.zeroBuf(kFindDataSize);
        const std::uint32_t fh = f.call("FindFirstFileA", {pat, fd});
        LIFT_CHECK_NE_U32(fh, kInvalidHandleValue);
        LIFT_CHECK(f.m.mem().readCString(fd + kFdFileName) == "t.dat");

        const std::uint64_t creation = readFileTime(f.m, fd + kFdCreationTime);
        const std::uint64_t access = readFileTime(f.m, fd + kFdLastAccessTime);
        const std::uint64_t write = readFileTime(f.m, fd + kFdLastWriteTime);
        auto unixOf = [](std::uint64_t ft) {
            return (static_cast<std::int64_t>(ft) - static_cast<std::int64_t>(kFileTimeEpochDelta)) /
                   10000000ll;
        };
        // ① 三个被 SetFileTime 设过的值必须真的落盘（±2s 容忍 100ns → 秒的截断）。
        //   ★ creation：Windows 用 ftCreationTime，macOS 用 setattrlist(ATTR_CMN_CRTIME)；
        //     其它 POSIX 写不了 birthtime ⇒ 保持"刚建"的值（只断言非 0 / 附近）。
#if defined(_WIN32) || defined(__APPLE__)
        LIFT_CHECK(std::llabs(unixOf(creation) - want_creation) <= 2);
#else
        LIFT_CHECK(unixOf(creation) > now_seconds - 600);
        LIFT_CHECK(unixOf(creation) < now_seconds + 600);
#endif
        LIFT_CHECK(std::llabs(unixOf(access) - want_access) <= 2);
        LIFT_CHECK(std::llabs(unixOf(write) - want_write) <= 2);
        // ② 三格不是同一个投影（creation / access / write 三个不同过去值）
        LIFT_CHECK(creation != access);
        LIFT_CHECK(creation != write);
        // ③ 不是"文件系统不支持 ⇒ 填 0"
        LIFT_CHECK(creation != 0u);

        // ④ GetFileTime 与 FIND_DATA 是**同一份**宿主真值（两处各算一次，结果应逐值相等）
        const std::uint32_t h2 =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(h2, kInvalidHandleValue);
        const std::uint32_t gfc = f.zeroBuf(8);
        const std::uint32_t gfa = f.zeroBuf(8);
        const std::uint32_t gfw = f.zeroBuf(8);
        LIFT_CHECK_EQ_U32(f.call("GetFileTime", {h2, gfc, gfa, gfw}), 1u);
        LIFT_CHECK_EQ_U32(readFileTime(f.m, gfc), creation);
        LIFT_CHECK_EQ_U32(readFileTime(f.m, gfa), access);
        LIFT_CHECK_EQ_U32(readFileTime(f.m, gfw), write);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h2}), 1u);

        // 另外两条：dwReserved0/1 恒 0；8.3 短名（cAlternateFileName）不模拟
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(fd + kFdReserved0), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(fd + kFdReserved1), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(fd + kFdAltName), 0u);
        LIFT_CHECK_EQ_U32(f.call("FindClose", {fh}), 1u);

        std::printf("[case] find-data-times: creation=%lld access=%lld write=%lld（Unix 秒；"
                    "access/write 由 SetFileTime 设成两个不同过去值，creation 仍是 now 附近）；"
                    "GetFileTime 与 FIND_DATA 逐值相等；dwReserved0/1=0；cAlternateFileName 为空\n",
                    static_cast<long long>(unixOf(creation)), static_cast<long long>(unixOf(access)),
                    static_cast<long long>(unixOf(write)));
    }

    // ---------------- ⑪ S6-6：SetFilePointerEx 七条（正/负/相对/越界/131/失败不动指针/87） ----------------
    {
        g_current_case = "set-file-pointer-ex";
        const std::uint32_t p_file = f.cstr("sub\\a.txt");
        const std::uint32_t h = f.call("CreateFileA", {p_file, kGenericRead | kGenericWrite, 0u,
                                                       0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        const std::uint32_t out = f.zeroBuf(8);
        const std::uint32_t hi_size = f.zeroBuf(4);
        const std::uint32_t buf = f.zeroBuf(64);
        const std::uint32_t nread = f.zeroBuf(4);

        // ① 0 / FILE_BEGIN ⇒ 新位置 0
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 0u, 0u, out, kFileBegin}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out + 4u), 0u);

        // ② 2 / FILE_BEGIN ⇒ 2；随后读到 "llo"
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 2u, 0u, out, kFileBegin}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 2u);
        f.m.mem().fillBytes(buf, 0, 64);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 3u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 3u);
        LIFT_CHECK(f.m.mem().readCString(buf) == "llo");

        // ③ -1 / FILE_CURRENT ⇒ 4（负相对移动）；随后读到 "o"
        LIFT_CHECK_EQ_U32(
            f.call("SetFilePointerEx", {h, 0xFFFFFFFFu, 0xFFFFFFFFu, out, kFileCurrent}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 4u);
        f.m.mem().fillBytes(buf, 0, 64);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 1u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 1u);
        LIFT_CHECK(f.m.mem().readCString(buf) == "o");

        // ④ 0 / FILE_END ⇒ 5，且等于 GetFileSize 的低 32 位（两者配合）
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 0u, 0u, out, kFileEnd}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 5u);
        LIFT_CHECK_EQ_U32(f.call("GetFileSize", {h, hi_size}), 5u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(hi_size), 0u);

        // ⑤ 100 / FILE_BEGIN ⇒ 100（越界不是错误），文件没有变大
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 100u, 0u, out, kFileBegin}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 100u);
        LIFT_CHECK_EQ_U32(f.call("GetFileSize", {h, hi_size}), 5u);
        std::error_code pos_ec;
        const auto pos_size = fs::file_size(tmp / "sub" / "a.txt", pos_ec);
        LIFT_CHECK(!pos_ec && pos_size == 5u);  // 宿主盘：越界 seek 没有把文件变大

        // ⑥ 回到 0，再 -100 / FILE_CURRENT ⇒ FALSE + ERROR_NEGATIVE_SEEK(131)，**指针不动**
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 0u, 0u, out, kFileBegin}), 1u);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(
            f.call("SetFilePointerEx", {h, 0xFFFFFF9Cu, 0xFFFFFFFFu, out, kFileCurrent}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNegativeSeek);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);  // 失败不写出参
        f.m.mem().fillBytes(buf, 0, 64);
        LIFT_CHECK_EQ_U32(f.call("ReadFile", {h, buf, 5u, nread, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(nread), 5u);
        LIFT_CHECK(f.m.mem().readCString(buf) == "hello");  // 位置仍在 0

        // ⑦ 非法 dwMoveMethod ⇒ FALSE + ERROR_INVALID_PARAMETER(87)
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("SetFilePointerEx", {h, 0u, 0u, out, 3u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);

        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);
        std::printf("[case] set-file-pointer-ex: ① 0/BEGIN=0 ② 2/BEGIN=2→\"llo\" "
                    "③ -1/CURRENT=4→\"o\" ④ 0/END=5==GetFileSize ⑤ 100/BEGIN=100（大小仍 5）"
                    " ⑥ -100/CURRENT=FALSE+131（仍读到 \"hello\"）⑦ method=3 ⇒ FALSE+87\n");
    }

    // ---------------- ⑫ CREATE_ALWAYS / OPEN_ALWAYS 命中已存在 ⇒ 成功 + last_error=183 ----------------
    // 文档（CreateFileA `dwCreationDisposition` 表）：这两个 disposition 在"文件已存在"时
    // **函数成功**，同时 last-error = ERROR_ALREADY_EXISTS(183)；其余 disposition 不置。
    // ⛔ 断言同时看**宿主盘**：183 只改 last_error，不许改变截断 / 不截断的既定语义。
    {
        g_current_case = "create-always-last-error";
        const std::uint32_t p_file = f.cstr("sub\\reuse.txt");
        const std::uint32_t nwritten = f.zeroBuf(4);

        // ① 首次 CREATE_ALWAYS（文件不存在）⇒ 句柄有效，且 last_error **没有**被写成 183
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h1 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h1, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 0u);
        const std::uint32_t first = f.cstr("first");
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {h1, first, 5u, nwritten, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h1}), 1u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "reuse.txt") == "first");

        // ② 第二次 CREATE_ALWAYS（已存在）⇒ 句柄有效 + 183；并按 disposition 语义**截断**
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h2 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kCreateAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h2, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAlreadyExists);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h2}), 1u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "reuse.txt").empty());
        {
            std::error_code size_ec;
            const auto host_size = fs::file_size(tmp / "sub" / "reuse.txt", size_ec);
            LIFT_CHECK(!size_ec && host_size == 0u);
        }

        // ③ OPEN_ALWAYS（已存在）⇒ 句柄有效 + 183；但**不**截断（写进去的内容留下）
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h3 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kOpenAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h3, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrAlreadyExists);
        const std::uint32_t keep = f.cstr("keep");
        LIFT_CHECK_EQ_U32(f.call("WriteFile", {h3, keep, 4u, nwritten, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h3}), 1u);
        LIFT_CHECK(readHostFile(tmp / "sub" / "reuse.txt") == "keep");

        // ④ OPEN_ALWAYS（不存在）⇒ 句柄有效，last_error **不是** 183（新建空文件）
        const std::uint32_t p_fresh = f.cstr("sub\\fresh.txt");
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h4 =
            f.call("CreateFileA", {p_fresh, kGenericWrite, 0u, 0u, kOpenAlways, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h4, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 0u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h4}), 1u);
        {
            std::error_code size_ec;
            const auto host_size = fs::file_size(tmp / "sub" / "fresh.txt", size_ec);
            LIFT_CHECK(!size_ec && host_size == 0u);
        }

        // ⑤ 其余 disposition 命中已存在文件**不置** 183（OPEN_EXISTING / TRUNCATE_EXISTING）
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h5 =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(h5, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 0u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h5}), 1u);

        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h6 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kTruncateExisting, 0x80u, 0u});
        LIFT_CHECK_NE_U32(h6, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 0u);
        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h6}), 1u);

        // ⑥ 失败的 disposition 也不许"顺手"写成 183：CREATE_NEW 命中已存在 ⇒ 80
        f.m.cpu().shim.last_error = 0;
        const std::uint32_t h7 =
            f.call("CreateFileA", {p_file, kGenericWrite, 0u, 0u, kCreateNew, 0x80u, 0u});
        LIFT_CHECK_EQ_U32(h7, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrFileExists);

        std::printf("[case] create-always-last-error: ① 首次 CREATE_ALWAYS ⇒ 0x%08X + last_error=0；"
                    "② 再次 CREATE_ALWAYS ⇒ 0x%08X + last_error=%u（宿主被截断成 0 字节）；"
                    "③ OPEN_ALWAYS(存在) ⇒ 0x%08X + %u 且内容保留 \"keep\"；"
                    "④ OPEN_ALWAYS(不存在) ⇒ 新建空文件 + 非 183；"
                    "⑤ OPEN_EXISTING / TRUNCATE_EXISTING 均非 183；⑥ CREATE_NEW(存在) ⇒ 0xFFFFFFFF+%u\n",
                    h1, h2, kErrAlreadyExists, h3, kErrAlreadyExists, kErrFileExists);
    }

    // ---------------- ⑬ 标准句柄三元组：SetStdHandle 存值 / GetFileType 按真实种类 / GetConsoleMode 只认控制台 ----------------
    // 判据（REQ-01M6K69K9HM3ETCDNMJWPHGXJC，文档 = MSDN 的 GetFileType / GetConsoleMode / SetStdHandle）：
    //   · `CreateFileA` 打开的真文件 ⇒ `GetFileType` = FILE_TYPE_DISK(1)（⛔ 旧实现恒 2）；
    //   · 同一个文件句柄 ⇒ `GetConsoleMode` = 0 + ERROR_INVALID_HANDLE(6)（⛔ 旧实现恒成功）；
    //   · `SetStdHandle(STD_OUTPUT, h)` 之后 `GetStdHandle(STD_OUTPUT)` 返回 h；恢复原值也成立；
    //   · 没动过的标准槽仍是"字符设备"（启动路径现状：`GetFileType` = 2、`GetConsoleMode` 成功）。
    {
        g_current_case = "std-handles-triple";
        const std::uint32_t p_file = f.cstr("sub\\a.txt");
        const std::uint32_t h =
            f.call("CreateFileA", {p_file, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);

        // ① 真文件 ≠ 字符设备；也不是控制台
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {h}), kFileTypeDisk);
        const std::uint32_t mode = f.zeroBuf(4);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("GetConsoleMode", {h, mode}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidHandle);

        // ② 没动过的标准槽：字符设备 + 控制台模式（启动路径的现状，⛔ 不许因为这次修正而改变）
        const std::uint32_t in = f.call("GetStdHandle", {kStdInput});
        const std::uint32_t out = f.call("GetStdHandle", {kStdOutput});
        const std::uint32_t err = f.call("GetStdHandle", {kStdError});
        LIFT_CHECK_NE_U32(in, 0u);
        LIFT_CHECK_NE_U32(out, 0u);
        LIFT_CHECK_NE_U32(err, 0u);
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {in}), kFileTypeChar);
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {out}), kFileTypeChar);
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {err}), kFileTypeChar);
        LIFT_CHECK_EQ_U32(f.call("GetConsoleMode", {out, mode}), 1u);

        // ③ SetStdHandle 真的存值（⛔ 旧实现只返回 1、什么都不存）
        LIFT_CHECK_EQ_U32(f.call("SetStdHandle", {kStdOutput, h}), 1u);
        LIFT_CHECK_EQ_U32(f.call("GetStdHandle", {kStdOutput}), h);
        // 换到那一槽的仍是**文件**句柄 ⇒ 类型跟着句柄走（不是"标准槽就一定是字符设备"）
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {h}), kFileTypeDisk);
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("GetConsoleMode", {h, mode}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidHandle);
        // 别的槽不受影响
        LIFT_CHECK_EQ_U32(f.call("GetStdHandle", {kStdInput}), in);
        LIFT_CHECK_EQ_U32(f.call("GetStdHandle", {kStdError}), err);

        // ④ 恢复原值也成立（进程级表就是"最后一次写的值"）
        LIFT_CHECK_EQ_U32(f.call("SetStdHandle", {kStdOutput, out}), 1u);
        LIFT_CHECK_EQ_U32(f.call("GetStdHandle", {kStdOutput}), out);
        LIFT_CHECK_EQ_U32(f.call("GetFileType", {out}), kFileTypeChar);
        LIFT_CHECK_EQ_U32(f.call("GetConsoleMode", {out, mode}), 1u);

        // ⑤ 非法 nStdHandle ⇒ 0（真实失败值 + 错误码），⛔ 不是静默成功
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call("GetStdHandle", {kStdOutput + 100u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidHandle);

        LIFT_CHECK_EQ_U32(f.call("CloseHandle", {h}), 1u);
        std::printf("[case] std-handles-triple: CreateFileA ⇒ 0x%08X；GetFileType=1"
                    "（FILE_TYPE_DISK）、GetConsoleMode=FALSE+%u；默认标准槽 0x%08X/0x%08X/0x%08X "
                    "⇒ GetFileType=2 且 GetConsoleMode=TRUE；SetStdHandle(STD_OUTPUT, h) 后 "
                    "GetStdHandle 返回 h，恢复后回到 0x%08X\n",
                    h, kErrInvalidHandle, in, out, err, out);
    }

    std::printf("[case] tmp = %s\n", game.c_str());
    return finish("file-io");
}
