// emulator-lift · 文件与路径族的本轮新增（pure：$env:TEMP 下自建目录，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "文件与路径族"）：
//   ① `GetFullPathNameA` 走 **vfs 的文本规范化**（相对 cwd、折叠 `.` / `..`、`..` 到根即止），
//      返回值的两种口径（成功 = 长度**不含**结尾 0；缓冲太小 = 所需大小**含**结尾 0）、
//      `lpFilePart` 的"目录形态 ⇒ 0"规则；
//   ② `GetSystemDirectoryA` / `GetWindowsDirectoryA` 是**固定的 guest 路径**
//      （⛔ 不含宿主路径的任何片段），返回值口径同上；
//   ③ `SetFileTime` **真的写宿主时间戳**（写完 `GetFileTime` 读回来对得上，而不是"返回 TRUE 什么都没做"）；
//   ④ `GetTempFileNameA` 的 `uUnique`：非 0 ⇒ 用调用方的值（低 16 位、大写十六进制）+ **不创建**
//      + 返回原值；0 ⇒ 从前缀 + 递增序号找一个空号、**创建 0 字节文件**。前缀只取前 3 个字符、
//      扩展名 `.TMP`（文档形态）。断言要落在**宿主盘**上（存在 / 不存在 / 长度）。
//   ⑤ ★ **guest 的路径编码**（2026-11-05 缺陷单 `🐞S2 日文 guest 路径乱码`）：
//      A 版 API 的入参 / 出参是 **CP932 字节**、W 版 API 是 **UTF-16**、宿主 / VFS 内部是 UTF-8。
//      判据用**日文 guest 根**（`E:\Games\Eushully\天結` = 真实缺省值）逐字节断言：
//      `GetModuleFileNameA` == CP932（`天結` = 93 56 8C 8B）、⛔ 不是它的 UTF-8（E5 A4 A9 E7 B5 90）、
//      把这份字节**原样**喂回 `CreateFileA` 必须命中 VFS overlay（引擎就是这么用 exe 路径的）。
//   ⑥ ★ **`Vfs::resolve` 认 guest 根前缀**（2026-11-05 缺陷单
//      `🐞S2 VFS 不认 guest 根前缀：引擎读自己的版本失败 ⇒ MessageBox ⇒ ExitProcess(0)`）：
//      `<guest_root>\…`（UTF-8 / CP932 两种字节、大小写 / 分隔符变体）⇒ `--game-dir` 根并命中
//      overlay；不是根 / 根只出现在中间 / `..` 越界 ⇒ **失败**。用例 `guest-root-absolute`。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;

constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kGenericWrite = 0x40000000u;
constexpr std::uint32_t kCreateAlways = 2u;
constexpr std::uint32_t kOpenExisting = 3u;
constexpr std::uint32_t kInvalidHandleValue = 0xFFFFFFFFu;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrPathNotFound = 3u;
constexpr std::uint32_t kErrNoMoreFiles = 18u;

// WIN32_FIND_DATAA（x86，0x140 字节）的字段偏移（与 kernel32_file.cpp 的常量同值）
constexpr std::uint32_t kFindDataSize = 0x140u;
constexpr std::uint32_t kFdSizeHigh = 0x1Cu;
constexpr std::uint32_t kFdSizeLow = 0x20u;
constexpr std::uint32_t kFdFileName = 0x2Cu;

// 2020-01-02T03:04:05Z 的 FILETIME（100 ns 自 1601）= (1577934245 + 11644473600) * 10^7
constexpr std::uint64_t kTargetFileTime = 132224078450000000ull;

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ★ UTF-8 文本 → 宿主 `std::filesystem::path`：⛔ 不能用 `fs::path(std::string)` —— 那在 Windows 上
//   按**本地 ANSI 码页**（这台机器是 GBK）解释，日文名会变成另一个不存在的东西。
std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::uint32_t processId() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

// 读 guest 的 ANSI（CP932/ASCII）串 —— 本夹具的路径全是 ASCII
std::string guestStr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 512) {
    std::string s;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const char c = static_cast<char>(m.mem().rd8(ea + i));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// 读 guest 的 UTF-16 串（**原样**，不做 ASCII 收窄 —— 日文路径要逐码元比）
std::u16string guestWstr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 512) {
    std::u16string s;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const char16_t c = static_cast<char16_t>(m.mem().rd16(ea + i * 2u));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// 红的时候要能一眼看出"实际拿到的是哪一套字节"（CP932 / UTF-8 / 别的）
std::string hexBytes(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        char b[4] = {};
        std::snprintf(b, sizeof(b), "%02X ", c);
        out += b;
    }
    return out;
}

// 读 guest 的 UTF-16 串（本夹具的路径 / 环境块全是 ASCII）
std::string guestWideStr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 512) {
    std::string s;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const std::uint16_t w = m.mem().rd16(ea + i * 2u);
        if (w == 0) break;
        s.push_back(w < 0x80 ? static_cast<char>(w) : '?');
    }
    return s;
}

// guest 环境块（UTF-16，`NAME=VALUE\0…\0\0`）→ 条目列表
std::vector<std::string> envBlockItems(lift::Machine& m, std::uint32_t ea) {
    std::vector<std::string> out;
    std::uint32_t off = 0;
    for (;;) {
        std::string item;
        bool any = false;
        for (;;) {
            const std::uint16_t w = m.mem().rd16(ea + off);
            off += 2;
            if (w == 0) break;
            any = true;
            item.push_back(w < 0x80 ? static_cast<char>(w) : '?');
        }
        if (!any) break;
        out.push_back(item);
    }
    return out;
}

bool hasEnvItem(const std::vector<std::string>& items, const std::string& prefix) {
    for (const std::string& s : items) {
        if (s.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

int main() {
    using namespace lift_test;

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("lift-file-path-" + std::to_string(processId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "无法建临时目录：%s\n", ec.message().c_str());
        return 1;
    }

    // ① GetFullPathNameA
    {
        g_current_case = "get-full-path-name";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerSyncShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        LIFT_CHECK(f.ok);
        const std::uint32_t gfp = f.use("GetFullPathNameA");

        const std::uint32_t buf = f.zeroData(512);
        const std::uint32_t part = f.u32data(0xDEADBEEFu);
        auto call_gfp = [&](const char* in, std::uint32_t cap) {
            const std::uint32_t name = f.cstr(in);
            return f.call(gfp, {name, cap, buf, part});
        };

        // 相对路径相对 guest cwd（本夹具没有 SetCurrentDirectoryA ⇒ 缺省 = guestRoot）
        LIFT_CHECK_EQ_U32(call_gfp("sub\\file.txt", 512), 20u);  // "D:\GAME\sub\file.txt" = 20
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\sub\\file.txt");
        LIFT_CHECK_MEM_U32(f.m.mem(), part, buf + 12u);  // 指向 "file.txt"（20 - 8）

        // 折叠 `.` 与 `..`；`..` 在根上就停在根（⛔ 不报错、也不逃出根）
        LIFT_CHECK_EQ_U32(call_gfp(".\\x", 512), 9u);
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\x");
        LIFT_CHECK_EQ_U32(call_gfp("..\\..\\..\\a.txt", 512), 13u);
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\a.txt");

        // 带盘符的绝对路径：所有盘符都映射到同一个 guest 根（vfs 的口径）
        LIFT_CHECK_EQ_U32(call_gfp("C:\\foo\\bar.dll", 512), 19u);
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\foo\\bar.dll");

        // 目录形态（以分隔符结尾）⇒ lpFilePart = 0
        LIFT_CHECK_EQ_U32(call_gfp("sub\\", 512), 11u);
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\sub");
        LIFT_CHECK_MEM_U32(f.m.mem(), part, 0u);

        // 缓冲太小 ⇒ 返回**所需大小（含结尾 0）**，且不写缓冲
        f.m.mem().fillBytes(buf, 0xCD, 64);
        LIFT_CHECK_EQ_U32(call_gfp("sub\\file.txt", 5u), 21u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf), 0xCDu);
        // cap == 0 ⇒ "问大小"（含结尾 0）
        LIFT_CHECK_EQ_U32(call_gfp("sub\\file.txt", 0u), 21u);

        // NULL / 空文件名 ⇒ 0 + ERROR_INVALID_PARAMETER
        LIFT_CHECK_EQ_U32(f.call(gfp, {0u, 512u, buf, part}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // ② GetSystemDirectoryA / GetWindowsDirectoryA（固定 guest 路径）
    {
        g_current_case = "fixed-directories";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t sys = f.use("GetSystemDirectoryA");
        const std::uint32_t win = f.use("GetWindowsDirectoryA");
        const std::uint32_t buf = f.zeroData(512);
        const std::string expect_sys = "C:\\WINDOWS\\SYSTEM32";
        const std::string expect_win = "C:\\WINDOWS";

        LIFT_CHECK_EQ_U32(f.call(sys, {buf, 512u}),
                          static_cast<std::uint32_t>(expect_sys.size()));
        LIFT_CHECK(guestStr(f.m, buf) == expect_sys);
        LIFT_CHECK_EQ_U32(f.call(win, {buf, 512u}),
                          static_cast<std::uint32_t>(expect_win.size()));
        LIFT_CHECK(guestStr(f.m, buf) == expect_win);

        // 太小 ⇒ 所需大小（含结尾 0）；cap==0 ⇒ 同样问大小
        LIFT_CHECK_EQ_U32(f.call(sys, {buf, 4u}),
                          static_cast<std::uint32_t>(expect_sys.size() + 1u));
        LIFT_CHECK_EQ_U32(f.call(sys, {0u, 0u}),
                          static_cast<std::uint32_t>(expect_sys.size() + 1u));

        // ⛔ 固定路径里不许出现宿主的任何片段（临时目录名 / 盘符）
        const std::string got = guestStr(f.m, buf);
        LIFT_CHECK(got.find("lift-file-path") == std::string::npos);
        LIFT_CHECK(f.ok);
    }

    // ③ SetFileTime 真写宿主时间戳
    {
        g_current_case = "set-file-time-writes-host";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerSyncShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        LIFT_CHECK(f.ok);
        const std::uint32_t create = f.use("CreateFileA");
        const std::uint32_t write = f.use("WriteFile");
        const std::uint32_t setft = f.use("SetFileTime");
        const std::uint32_t getft = f.use("GetFileTime");
        const std::uint32_t close = f.use("CloseHandle");

        const std::uint32_t name = f.cstr("times.dat");
        const std::uint32_t data = f.data("hello", 5u);
        const std::uint32_t written = f.u32data(0xDEADBEEFu);
        const std::uint32_t h = f.call(create, {name, kGenericRead | kGenericWrite, 0u, 0u,
                                                kCreateAlways, 0u, 0u});
        LIFT_CHECK_NE_U32(h, kInvalidHandleValue);
        LIFT_CHECK_EQ_U32(f.call(write, {h, data, 5u, written, 0u}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), written, 5u);

        // 宿主上确实出现了这个文件（先证明写路径通）
        const fs::path host_path = dir / "times.dat";
        LIFT_CHECK(fs::exists(host_path, ec));

        // 只改 last-write（creation / access 传 NULL）
        const auto before_time = fs::last_write_time(host_path, ec);
        LIFT_CHECK(!ec);
        const std::uint32_t ft = f.data(&kTargetFileTime, 8u);
        LIFT_CHECK_EQ_U32(f.call(setft, {h, 0u, 0u, ft}), 1u);

        // 读回来（GetFileTime 的 write 出参）
        const std::uint32_t out = f.zeroData(8);
        LIFT_CHECK_EQ_U32(f.call(getft, {h, 0u, 0u, out}), 1u);
        const std::uint64_t back =
            static_cast<std::uint64_t>(f.m.mem().rd32(out)) |
            (static_cast<std::uint64_t>(f.m.mem().rd32(out + 4u)) << 32);
        const std::int64_t diff = static_cast<std::int64_t>(back) -
                                  static_cast<std::int64_t>(kTargetFileTime);
        // 容差 2 秒：FAT 的写时间分辨率是 2 秒；NTFS 是 100 ns（这里应当精确）
        LIFT_CHECK(diff > -20000000ll && diff < 20000000ll);
        // 而且绝不等于"文件刚创建时的 now"（那说明根本没写）
        LIFT_CHECK(back != 0u);

        // 三个都传 NULL ⇒ 成功返回，不改任何东西
        LIFT_CHECK_EQ_U32(f.call(setft, {h, 0u, 0u, 0u}), 1u);

        LIFT_CHECK_EQ_U32(f.call(close, {h}), 1u);
        // 宿主侧独立复核：文件 mtime 真的变了（用 std::filesystem 读，不信 shim 自己的转换）
        const auto after_time = fs::last_write_time(host_path, ec);
        LIFT_CHECK(!ec);
        LIFT_CHECK(before_time != after_time);
        LIFT_CHECK(f.ok);
    }

    // ④ GetTempFileNameA：uUnique != 0 ⇒ 原值 + 不创建；uUnique == 0 ⇒ 递增 + 创建 0 字节文件
    {
        g_current_case = "temp-file-name";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        f.m.vfs().setGuestCwd("D:\\GAME");
        LIFT_CHECK(f.ok);
        const std::uint32_t gtn = f.use("GetTempFileNameA");
        const std::uint32_t p_dir = f.cstr("D:\\GAME");
        const std::uint32_t p_age = f.cstr("AGE");
        const std::uint32_t p_long = f.cstr("AGELONG");  // 前缀必须截到 3 个字符
        const std::uint32_t buf = f.zeroData(512);
        const std::string guest_dir = "D:\\GAME\\";
        std::error_code ec2;
        fs::remove(dir / "AGE1234.TMP", ec2);

        // a) uUnique = 0x1234 ⇒ 返回原值、名字 AGE1234.TMP、**宿主上不存在这个文件**
        LIFT_CHECK_EQ_U32(f.call(gtn, {p_dir, p_age, 0x1234u, buf}), 0x1234u);
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\AGE1234.TMP");
        LIFT_CHECK(!fs::exists(dir / "AGE1234.TMP", ec2));

        // b) uUnique = 0 + 长前缀 ⇒ 前缀截 3、序号 4 位大写十六进制、扩展名 .TMP、
        //    而且**真的创建**了那个 0 字节文件
        const std::uint32_t r1 = f.call(gtn, {p_dir, p_long, 0u, buf});
        const std::string n1 = guestStr(f.m, buf);
        LIFT_CHECK(n1.size() == guest_dir.size() + 11u);  // "AGE" + 4 + ".TMP"
        LIFT_CHECK(n1.rfind(guest_dir + "AGE", 0) == 0);
        LIFT_CHECK(n1.compare(n1.size() - 4u, 4u, ".TMP") == 0);
        // 返回的序号必须就是名字里的那 4 位十六进制（⛔ 不钉"一定从 0 起"：那是本模拟器的
        // **有意偏差**——真机从系统时间起；这里只钉"与名字一致"）
        const std::string num1 = n1.substr(guest_dir.size() + 3u, 4u);
        LIFT_CHECK_EQ_U32(r1, static_cast<std::uint32_t>(std::stoul(num1, nullptr, 16)));
        for (std::size_t i = 0; i < 4u; ++i) {  // 序号是大写十六进制
            const char c = n1[n1.size() - 8u + i];
            LIFT_CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
        }
        const fs::path host1 = dir / n1.substr(guest_dir.size());
        LIFT_CHECK(fs::exists(host1, ec2));
        std::error_code sz1;
        const auto size1 = fs::file_size(host1, sz1);
        LIFT_CHECK(!sz1 && size1 == 0u);  // 建出来的是**空**文件

        // c) 再调一次 uUnique = 0 ⇒ 名字不同（序号递增），同样创建 0 字节文件
        const std::uint32_t r2 = f.call(gtn, {p_dir, p_long, 0u, buf});
        const std::string n2 = guestStr(f.m, buf);
        LIFT_CHECK(n2 != n1);
        LIFT_CHECK_EQ_U32(r2, r1 + 1u);  // 序号递增（同一个 uUnique=0 循环的下一个空号）
        LIFT_CHECK(n2.rfind(guest_dir + "AGE", 0) == 0);
        LIFT_CHECK(n2.compare(n2.size() - 4u, 4u, ".TMP") == 0);
        const fs::path host2 = dir / n2.substr(guest_dir.size());
        LIFT_CHECK(fs::exists(host2, ec2));
        std::error_code sz2;
        const auto size2 = fs::file_size(host2, sz2);
        LIFT_CHECK(!sz2 && size2 == 0u);

        // 清掉本用例的产物（dir 会被后面的运行复用）
        fs::remove(host1, ec2);
        fs::remove(host2, ec2);
        std::printf("[case] temp-file-name: uUnique=0x1234 ⇒ 返回 0x1234、\"%s\"、宿主不存在；"
                    "uUnique=0（前缀 \"AGELONG\"）⇒ \"%s\"（截到 AGE、4 位大写十六进制、.TMP）、"
                    "宿主存在且 0 字节；再调 ⇒ \"%s\"（序号递增）\n",
                    (guest_dir + "AGE1234.TMP").c_str(), n1.c_str(), n2.c_str());
    }

    // ④b A4：`setAppDataDir` 清掉上一轮的 `<--appdata>/Local/Temp`（否则 `GetTempFileNameA(
    //     uUnique=0)` 的序号跨运行累积，同一条命令两次会得到不同名字 / 不同 trace）。
    //     ★ 2026-11-05：清理点从 `--game-dir/.lift-tmp` 移到 `--appdata`（⛔ 不再写安装树）。
    {
        g_current_case = "temp-dir-cleared";
        std::error_code tec;
        const fs::path tempdir = dir / "appdata" / "Local" / "Temp";
        fs::create_directories(tempdir, tec);
        std::FILE* tf = std::fopen(pathToUtf8(tempdir / "AGE0000.TMP").c_str(), "wb");
        LIFT_CHECK(tf != nullptr);
        if (tf) std::fclose(tf);
        LIFT_CHECK(fs::exists(tempdir / "AGE0000.TMP", tec));
        Fixture g;
        g.m.vfs().setGameDir(pathToUtf8(dir));  // 触发清理的是 setAppDataDir，不是 setGameDir
        LIFT_CHECK(fs::exists(tempdir / "AGE0000.TMP", tec));  // 还没设 --appdata ⇒ 不能删
        g.m.vfs().setAppDataDir(pathToUtf8(dir / "appdata"));
        LIFT_CHECK(!fs::exists(tempdir / "AGE0000.TMP", tec));
    }

    // ⑤ shlwapi!PathRemoveFileSpecA（P4 小族）：去掉最后一段 + 一个分隔符
    //    判据 = ① 结果串（四个边界逐条）② 返回值 = **是否真的移除了东西**
    //           ③ 只写回"新串 + 结尾 0"，旧串尾巴不清（真机也不清；调用方常紧跟 strcat_s）。
    {
        g_current_case = "path-remove-file-spec";
        Fixture f;
        lift::registerShlwapiShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t prfs = f.use("shlwapi", "PathRemoveFileSpecA");
        LIFT_CHECK_NE_U32(prfs, 0u);

        struct Case {
            const char* in;
            const char* out;
            std::uint32_t ret;
        };
        const Case cases[] = {
            // 文件名 + 它前面那个分隔符一起去掉
            {"D:\\GAME\\save\\file.dat", "D:\\GAME\\save", 1},
            // 结尾是分隔符 ⇒ 只去掉那个分隔符（最后一段本来就是目录）
            {"D:\\GAME\\save\\", "D:\\GAME\\save", 1},
            // 一级路径 ⇒ 保留**根的**尾部反斜杠
            {"D:\\GAME", "D:\\", 1},
            // 根目录保持原样 + FALSE（⛔ 不把根的 `\` 也啃掉）
            {"D:\\", "D:\\", 0},
            {"\\", "\\", 0},
            // 没有分隔符 ⇒ 变空串 + FALSE（整串就是一个文件名）
            {"file.dat", "", 0},
            {"", "", 0},
            // `/` 也是分隔符
            {"D:/GAME/save/file.dat", "D:/GAME/save", 1},
            // 多级
            {"D:\\a\\b\\c", "D:\\a\\b", 1},
            // 盘符根下的一级
            {"\\a", "\\", 1},
        };
        for (const Case& c : cases) {
            const std::size_t in_len = std::strlen(c.in);
            const std::size_t out_len = std::strlen(c.out);
            std::vector<std::uint8_t> blob(0x40, 0xAA);  // 尾部是哨兵
            std::memcpy(blob.data(), c.in, in_len + 1u);
            const std::uint32_t p = f.data(blob.data(), blob.size());
            LIFT_CHECK_EQ_U32(f.call(prfs, {p}), c.ret);
            LIFT_CHECK(guestStr(f.m, p) == c.out);
            // 新 NUL 之后的那一个字节必须**原样**（只写了 out_len+1 个字节）
            const std::size_t probe = out_len + 1u;
            std::uint8_t expect = 0xAAu;  // 哨兵：既不是输入的一部分、也没被写过
            if (probe < in_len) {
                expect = static_cast<std::uint8_t>(c.in[probe]);  // 输入里原本的字节
            } else if (probe == in_len) {
                expect = 0x00u;  // 输入自己的结尾 0（那一格本来就在字符串里）
            }
            LIFT_CHECK_EQ_U32(f.m.mem().rd8(p + static_cast<std::uint32_t>(probe)), expect);
        }

        // NULL 指针：不崩、也不假装成功（真机在这里会解引用 NULL）
        LIFT_CHECK_EQ_U32(f.call(prfs, {0u}), 0u);
        LIFT_CHECK(f.ok);
    }

    // ⑥ imagehlp!MakeSureDirectoryPathExists（P4 小族）：递归建目录，**落盘只经 VFS**
    //    判据 = ① 宿主盘上真的出现了那几层目录（`\` / `/` 都认、已存在算成功）
    //           ② 末尾分隔符 ⇒ 最后一段也是目录；**没有**末尾分隔符 ⇒ 最后一段按文件名处理，⛔ 不建
    //           ③ guest 根 / `--appdata` 前缀不被建成宿主目录（否则安装树旁边会多出一棵假树）
    //           ④ 逃出根 ⇒ FALSE（⛔ 不建到 game_dir 外面）
    //   ★ 2026-11-05 写覆盖层（`REQ-01M6KNBE5FP87DAC93ZRQ1K1DK`）：有 `--appdata` 时游戏树的目录
    //     建在**影子** `<--appdata>/Game/…` 里，`--game-dir` **一个目录都不许多**；AppData 特殊
    //     目录仍走原映射（`<--appdata>/Roaming`）。
    {
        g_current_case = "make-sure-directory-path-exists";
        Fixture f;
        lift::registerImagehlpShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        f.m.vfs().setAppDataDir(pathToUtf8(dir / "appdata"));
        LIFT_CHECK(f.ok);
        const std::uint32_t msdpe = f.use("imagehlp", "MakeSureDirectoryPathExists");
        LIFT_CHECK_NE_U32(msdpe, 0u);
        auto call_msdpe = [&](const char* guest_path) {
            const std::uint32_t p = f.cstr(guest_path);
            return f.call(msdpe, {p});
        };
        // 影子根：`<--appdata>/Game`（vfs.h 的唯一落点；⛔ 不是 Roaming / Local）
        const fs::path shadow = dir / "appdata" / "Game";

        // a) 逐段建（`\\` 结尾 = 最后一段也是目录）
        LIFT_CHECK_EQ_U32(call_msdpe("D:\\GAME\\lift-w4-msdpe\\a\\b\\"), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "lift-w4-msdpe" / "a" / "b", ec));
        // ★ 关键回归：guest 根的前缀（`D:\GAME`）在 guest 世界里本来就存在 ⇒ ⛔ 不许在宿主上
        //   造出 `game_dir/GAME`（那会让第一次启动就写脏安装树）
        LIFT_CHECK(!fs::exists(dir / "GAME", ec));
        // ★ 安装树只读：这次建目录**一个字节都不许**落到 --game-dir
        LIFT_CHECK(!fs::exists(dir / "lift-w4-msdpe", ec));

        // b) 已存在算成功（与 CreateDirectoryA 的"已存在即失败"相反）
        LIFT_CHECK_EQ_U32(call_msdpe("D:\\GAME\\lift-w4-msdpe\\a\\b\\"), 1u);

        // c) 没有末尾分隔符 ⇒ 最后一段是**文件名**，只建到它的父目录
        LIFT_CHECK_EQ_U32(call_msdpe("D:\\GAME\\lift-w4-msdpe2\\a\\file.txt"), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "lift-w4-msdpe2" / "a", ec));
        LIFT_CHECK(!fs::exists(shadow / "lift-w4-msdpe2" / "a" / "file.txt", ec));
        LIFT_CHECK(!fs::exists(dir / "lift-w4-msdpe2", ec));

        // d) 根 ⇒ TRUE（没有要建的段）
        LIFT_CHECK_EQ_U32(call_msdpe("D:\\"), 1u);

        // e) 空串 / NULL ⇒ FALSE + ERROR_INVALID_PARAMETER
        LIFT_CHECK_EQ_U32(call_msdpe(""), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(msdpe, {0u}), 0u);

        // f) `/` 也是分隔符
        LIFT_CHECK_EQ_U32(call_msdpe("D:/GAME/lift-w4-fwd/x/y/"), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "lift-w4-fwd" / "x" / "y", ec));
        LIFT_CHECK(!fs::exists(dir / "lift-w4-fwd", ec));

        // g) `\\` 前缀（UNC 形态）：与所有其它路径 API 走**同一条** VFS 单一根映射
        //    （⛔ 不假装能建网络共享；`\\server\share` 之后的部分落到影子）
        LIFT_CHECK_EQ_U32(call_msdpe("\\\\server\\share\\lift-w4-unc\\z\\"), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "server" / "share" / "lift-w4-unc" / "z", ec));
        LIFT_CHECK(!fs::exists(dir / "server", ec));

        // h) 逃出 guest 根 ⇒ FALSE，且 game_dir 外面什么都没建
        LIFT_CHECK_EQ_U32(call_msdpe("D:\\GAME\\..\\..\\lift-w4-escape\\"), 0u);
        LIFT_CHECK(!fs::exists(dir.parent_path() / "lift-w4-escape", ec));

        // i) `--appdata` 的特殊目录：映射到 `<appdata>/Roaming`，**不**落到 game_dir 的 `Users` 下
        LIFT_CHECK_EQ_U32(call_msdpe("C:\\Users\\Amayui\\AppData\\Roaming\\lift-w4-appdata\\sub\\"), 1u);
        LIFT_CHECK(fs::is_directory(dir / "appdata" / "Roaming" / "lift-w4-appdata" / "sub", ec));
        LIFT_CHECK(!fs::exists(dir / "Users", ec));

        // j) 相对路径（相对 guest cwd = guestRoot）
        LIFT_CHECK_EQ_U32(call_msdpe("lift-w4-rel\\d\\"), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "lift-w4-rel" / "d", ec));
        LIFT_CHECK(!fs::exists(dir / "lift-w4-rel", ec));

        LIFT_CHECK(f.ok);
    }

    // ⑥b 安装树红线（2026-11-05）：**VFS 装配 / 临时目录都不许写 `--game-dir`**。
    //    判据：① `setGameDir` 不再删 `game_dir/.lift-tmp`（旧实现会，且那就是往安装树里写）
    //          ② 没有 `--appdata` 时 `GetTempPathA` 响亮失败（⛔ 不退回 `--game-dir/.lift-tmp`）
    //          ③ 跑完这两步，`--game-dir` 里的东西一个都没少 / 一个都没多
    {
        g_current_case = "no-writes-into-game-dir";
        std::error_code e3;
        const fs::path sentinel = dir / ".lift-tmp" / "KEEP.TMP";
        fs::create_directories(sentinel.parent_path(), e3);
        std::FILE* sf = std::fopen(pathToUtf8(sentinel).c_str(), "wb");
        LIFT_CHECK(sf != nullptr);
        if (sf) std::fclose(sf);
        Fixture g;
        lift::registerFileShims(g.m.shims());
        LIFT_CHECK(g.ok);
        g.m.vfs().setGameDir(pathToUtf8(dir));  // 旧实现会在这一步删掉 `<game_dir>/.lift-tmp`
        LIFT_CHECK(fs::exists(sentinel, e3));
        const std::uint32_t gtp = g.use("GetTempPathA");
        const std::uint32_t buf = g.zeroData(512);
        LIFT_CHECK_EQ_U32(g.call(gtp, {512u, buf}), 0u);
        LIFT_CHECK_EQ_U32(g.m.cpu().shim.last_error, kErrPathNotFound);
        LIFT_CHECK(fs::exists(sentinel, e3));
        LIFT_CHECK(!fs::exists(dir / "Temp", e3));  // 也不许在 game_dir 里造临时目录
        fs::remove_all(dir / ".lift-tmp", e3);
    }

    // ⑦ 环境块 / 特殊目录 / 临时目录 —— **同一份真源**（任务 A 的判据）
    //    判据：① `setupProcessStructures` 写进 guest 的那一块 == `lift::guestEnvironment()`
    //          ② `GetEnvironmentVariableA/W` 取到的 == 那一块里的值
    //          ③ `SHGetSpecialFolderPathA` / `SHGetFolderPathA` 的文本与 LOCALAPPDATA / APPDATA **完全一致**
    //          ④ `TEMP` / `TMP` / `GetTempPathA` 同一处，且 VFS 真能在 `--appdata` 下建出来
    {
        g_current_case = "guest-env-block";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        lift::registerProcessShims(f.m.shims());
        lift::registerShell32Shims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        f.m.vfs().setAppDataDir(pathToUtf8(dir / "appdata"));
        LIFT_CHECK(f.ok);

        lift::ProcessSetup setup;
        std::string serr;
        LIFT_CHECK(lift::setupProcessStructures(f.m.mem(), lift::PeImage{}, f.m.layout(), f.m.vfs(),
                                                &setup, &serr));
        f.m.cpu().shim.env_ea = setup.env_ea;
        const std::vector<std::string> want = lift::guestEnvironment(true);
        LIFT_CHECK(envBlockItems(f.m, setup.env_ea) == want);
        LIFT_CHECK(setup.environment_ansi == lift::guestEnvironmentText(true));

        // 逐条点名（缺一个就红；⛔ 不是"包含某一项"就算过）
        for (const char* need : {"APPDATA=", "COMSPEC=", "HOMEDRIVE=", "HOMEPATH=", "LOCALAPPDATA=",
                                 "PATH=", "SystemDrive=", "SystemRoot=", "TEMP=", "TMP=",
                                 "USERPROFILE=", "windir="}) {
            LIFT_CHECK(hasEnvItem(want, need));
        }

        const std::uint32_t gv = f.use("GetEnvironmentVariableA");
        const std::uint32_t gvw = f.use("GetEnvironmentVariableW");
        const std::uint32_t buf = f.zeroData(512);
        auto get = [&](const char* name) {
            const std::uint32_t n = f.cstr(name);
            const std::uint32_t r = f.call(gv, {n, buf, 512u});
            return r == 0 ? std::string() : guestStr(f.m, buf);
        };
        LIFT_CHECK(get("LOCALAPPDATA") == "C:\\Users\\Amayui\\AppData\\Local");
        LIFT_CHECK(get("APPDATA") == "C:\\Users\\Amayui\\AppData\\Roaming");
        LIFT_CHECK(get("USERPROFILE") == "C:\\Users\\Amayui");
        LIFT_CHECK(get("TEMP") == lift::guestTempDirText());
        LIFT_CHECK(get("TMP") == lift::guestTempDirText());
        LIFT_CHECK(get("SystemRoot") == "C:\\Windows");
        LIFT_CHECK(get("windir") == "C:\\Windows");
        LIFT_CHECK(get("COMSPEC") == "C:\\Windows\\System32\\cmd.exe");
        LIFT_CHECK(get("HOMEDRIVE") == "C:");
        LIFT_CHECK(get("HOMEPATH") == "\\Users\\Amayui");
        LIFT_CHECK(get("PATH") == ".");
        // 不存在 ⇒ 0 + 203（⛔ 不是空串也算成功）
        LIFT_CHECK_EQ_U32(f.call(gv, {f.cstr("NOPE"), buf, 512u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 203u);

        const std::uint32_t wbuf = f.zeroData(512);
        const char16_t kLocalAppDataName[] = u"LOCALAPPDATA";
        const std::uint32_t wname = f.wideCstr(kLocalAppDataName, 13u);
        LIFT_CHECK_EQ_U32(f.call(gvw, {wname, wbuf, 512u}),
                          static_cast<std::uint32_t>(std::strlen("C:\\Users\\Amayui\\AppData\\Local")));
        LIFT_CHECK(guestWideStr(f.m, wbuf) == "C:\\Users\\Amayui\\AppData\\Local");

        // 特殊目录 == env 文本（**完全一致**，⛔ 两套各说各话）
        const std::uint32_t sf = f.use("shell32", "SHGetSpecialFolderPathA");
        const std::uint32_t gf = f.use("shell32", "SHGetFolderPathA");
        const std::uint32_t p1 = f.zeroData(512);
        const std::uint32_t p2 = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(sf, {0u, p1, 0x001Cu, 0u}), 1u);
        LIFT_CHECK(guestStr(f.m, p1) == get("LOCALAPPDATA"));
        LIFT_CHECK_EQ_U32(f.call(sf, {0u, p1, 0x001Au, 0u}), 1u);
        LIFT_CHECK(guestStr(f.m, p1) == get("APPDATA"));
        LIFT_CHECK_EQ_U32(f.call(sf, {0u, p1, 0x0005u, 0u}), 1u);
        LIFT_CHECK(guestStr(f.m, p1) == lift::guestDocumentsText());
        // SHGetFolderPathA(CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE) ⇒ S_OK + 同一份文本 + 建出脚手架
        std::error_code e2;
        fs::remove_all(dir / "appdata" / "Local", e2);
        LIFT_CHECK_EQ_U32(f.call(gf, {0u, 0x801Cu, 0u, 0u, p2}), 0u);
        LIFT_CHECK(guestStr(f.m, p2) == get("LOCALAPPDATA"));
        LIFT_CHECK(fs::is_directory(dir / "appdata" / "Local", e2));

        // TEMP / TMP / GetTempPathA 同一处；VFS 真能在 --appdata 下把它建出来
        const std::uint32_t gtp = f.use("GetTempPathA");
        LIFT_CHECK_EQ_U32(f.call(gtp, {512u, p1}),
                          static_cast<std::uint32_t>(lift::guestTempDirText().size() + 1u));
        LIFT_CHECK(guestStr(f.m, p1) == lift::guestTempDirText() + "\\");
        LIFT_CHECK(fs::is_directory(dir / "appdata" / "Local" / "Temp", e2));
        LIFT_CHECK(f.ok);
    }

    // ⑧ 没有 `--appdata`：AppData 派生的变量**一律不提供**（与 SHGetSpecialFolderPathA 的
    //    FALSE + 5 同一口径；⛔ 不指向宿主真实 AppData，也不指向一个之后必然解析失败的 guest 路径）
    {
        g_current_case = "guest-env-without-appdata";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        lift::registerProcessShims(f.m.shims());
        lift::registerShell32Shims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        LIFT_CHECK(f.ok);
        lift::ProcessSetup setup;
        std::string serr;
        LIFT_CHECK(lift::setupProcessStructures(f.m.mem(), lift::PeImage{}, f.m.layout(), f.m.vfs(),
                                                &setup, &serr));
        f.m.cpu().shim.env_ea = setup.env_ea;
        const std::vector<std::string> want = lift::guestEnvironment(false);
        LIFT_CHECK(envBlockItems(f.m, setup.env_ea) == want);
        for (const char* banned : {"APPDATA=", "LOCALAPPDATA=", "TEMP=", "TMP="}) {
            LIFT_CHECK(!hasEnvItem(want, banned));
        }
        LIFT_CHECK(hasEnvItem(want, "USERPROFILE="));
        LIFT_CHECK(hasEnvItem(want, "SystemRoot="));

        const std::uint32_t gv = f.use("GetEnvironmentVariableA");
        const std::uint32_t buf = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(gv, {f.cstr("LOCALAPPDATA"), buf, 512u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 203u);
        const std::uint32_t sf = f.use("shell32", "SHGetSpecialFolderPathA");
        const std::uint32_t gf = f.use("shell32", "SHGetFolderPathA");
        const std::uint32_t p1 = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(sf, {0u, p1, 0x001Cu, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 5u);
        LIFT_CHECK_EQ_U32(f.call(gf, {0u, 0x801Cu, 0u, 0u, p1}), 0x80070005u);
        // 没有 --appdata ⇒ 没有临时目录（⛔ 不再退回 --game-dir/.lift-tmp）
        const std::uint32_t gtp = f.use("GetTempPathA");
        LIFT_CHECK_EQ_U32(f.call(gtp, {512u, p1}), 0u);
        std::error_code e2;
        LIFT_CHECK(!fs::exists(dir / ".lift-tmp", e2));
    }

    // ⑨ guest 眼里的主模块路径 = **游戏目录里的 `AGE.EXE`**（任务 B-1）
    //    判据：① `ProcessSetup::guest_exe_path` / guest 内存里的 exe 路径 / 命令行都不含宿主路径
    //          ② `GetModuleFileNameA` / `GetCommandLineA` 返回同一份 guest 文本
    //          ③ trace 输入清单仍记宿主 `--exe` 原始串（A4 的"输入"必须可追）
    {
        g_current_case = "guest-exe-path";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        LIFT_CHECK(f.ok);
        lift::PeImage img;
        img.path = "C:\\host\\repo\\corpus\\AGE.EXE__dumped.sectfix.EXE";
        lift::ProcessSetup setup;
        std::string serr;
        LIFT_CHECK(lift::setupProcessStructures(f.m.mem(), img, f.m.layout(), f.m.vfs(), &setup, &serr));
        f.m.cpu().shim.exe_path_ea = setup.exe_path_ea;
        f.m.cpu().shim.cmdline_ea = setup.cmdline_ea;

        LIFT_CHECK(setup.guest_exe_path == "D:\\GAME\\AGE.EXE");
        LIFT_CHECK(setup.exe_path == img.path);  // A4：宿主原始串只进 trace 输入清单
        LIFT_CHECK(setup.cmdline == "\"D:\\GAME\\AGE.EXE\"");
        LIFT_CHECK(guestStr(f.m, setup.exe_path_ea) == "D:\\GAME\\AGE.EXE");
        LIFT_CHECK(setup.cmdline.find("host") == std::string::npos);
        LIFT_CHECK(setup.cmdline.find("corpus") == std::string::npos);

        const std::uint32_t gmf = f.use("GetModuleFileNameA");
        const std::uint32_t buf = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(gmf, {0u, buf, 512u}),
                          static_cast<std::uint32_t>(std::strlen("D:\\GAME\\AGE.EXE")));
        LIFT_CHECK(guestStr(f.m, buf) == "D:\\GAME\\AGE.EXE");
        const std::uint32_t gcl = f.use("GetCommandLineA");
        LIFT_CHECK(guestStr(f.m, f.call(gcl, {})) == "\"D:\\GAME\\AGE.EXE\"");
        LIFT_CHECK(f.ok);
    }

    // ⑩ VFS overlay：guest 打开 `…\AGE.EXE` **任意大小写**都拿到我们真正在跑的那份字节；
    //    大小 / 时间 / 属性（`FindFirstFileA` 的枚举项）与那份**同一个答案**。
    {
        g_current_case = "exe-overlay";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerSyncShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        LIFT_CHECK(f.ok);

        const fs::path packed = dir / "AGE.EXE";          // 安装树里的加壳原件
        const fs::path dumped = dir / "overlay" / "AGE.dumped.exe";  // 我们真正在跑的那份
        fs::create_directories(dumped.parent_path(), ec);
        auto writeFile = [](const fs::path& p, const std::string& bytes) {
            std::FILE* fp = std::fopen(pathToUtf8(p).c_str(), "wb");
            if (!fp) return false;
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), fp);
            std::fclose(fp);
            return true;
        };
        const std::string packed_bytes = "PACKED-ORIGINAL-0";
        const std::string dumped_bytes = "UNPACKED-BYTES-0123456789-DIFFERENT-LENGTH";
        LIFT_CHECK(writeFile(packed, packed_bytes));
        LIFT_CHECK(writeFile(dumped, dumped_bytes));
        LIFT_CHECK(writeFile(dir / "OTHER.DAT", "OTHER"));

        f.m.vfs().setExeOverlay("D:\\GAME\\AGE.EXE", pathToUtf8(dumped), "0123456789abcdef");
        LIFT_CHECK_EQ_U32(f.m.vfs().overlayHostPath() == pathToUtf8(dumped) ? 1u : 0u, 1u);

        const std::uint32_t create = f.use("CreateFileA");
        const std::uint32_t readf = f.use("ReadFile");
        const std::uint32_t getsize = f.use("GetFileSize");
        const std::uint32_t close = f.use("CloseHandle");
        auto readAll = [&](const char* guest_path) {
            const std::uint32_t h = f.call(create, {f.cstr(guest_path), kGenericRead, 0u, 0u,
                                                    kOpenExisting, 0u, 0u});
            if (h == kInvalidHandleValue) return std::string("<open-failed>");
            const std::uint32_t size = f.call(getsize, {h, 0u});
            const std::uint32_t nbuf = f.zeroData(256);
            const std::uint32_t got = f.u32data(0);
            f.call(readf, {h, nbuf, 256u, got, 0u});
            const std::uint32_t n = f.m.mem().rd32(got);
            LIFT_CHECK_EQ_U32(n, size);  // 可读字节数 == GetFileSize（同一份字节）
            const std::string s = guestStr(f.m, nbuf, n + 1u);
            f.call(close, {h});
            return s;
        };
        // ① 正常形态 / 大小写不同 / 相对路径（cwd = guest 根）都拿到拆壳件的字节
        LIFT_CHECK(readAll("D:\\GAME\\AGE.EXE") == dumped_bytes);
        LIFT_CHECK(readAll("d:\\game\\age.exe") == dumped_bytes);
        LIFT_CHECK(readAll("AGE.EXE") == dumped_bytes);
        // ② 别的文件**没有**被重定向
        LIFT_CHECK(readAll("D:\\GAME\\OTHER.DAT") == "OTHER");

        // ③ 枚举项（FindFirstFileA 的底层）也是同一份大小：不是加壳原件的 18 字节
        std::vector<lift::HostDirEntry> entries;
        std::string ferr;
        LIFT_CHECK(f.m.vfs().find("D:\\GAME\\*.*", &entries, &ferr));
        bool saw_age = false;
        for (const lift::HostDirEntry& e : entries) {
            if (e.name != "AGE.EXE") continue;
            saw_age = true;
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(e.size),
                              static_cast<std::uint32_t>(dumped_bytes.size()));
            LIFT_CHECK(e.size != packed_bytes.size());
        }
        LIFT_CHECK(saw_age);

        // ④ 宿主上两份字节都没被改（overlay 只是路由，⛔ 不是复制 / 改写）
        auto hostBytes = [](const fs::path& p) {
            std::FILE* fp = std::fopen(pathToUtf8(p).c_str(), "rb");
            if (!fp) return std::string();
            std::string s;
            char tmp[64];
            std::size_t n = 0;
            while ((n = std::fread(tmp, 1, sizeof(tmp), fp)) > 0) s.append(tmp, n);
            std::fclose(fp);
            return s;
        };
        LIFT_CHECK(hostBytes(packed) == packed_bytes);
        LIFT_CHECK(hostBytes(dumped) == dumped_bytes);
        LIFT_CHECK(f.ok);
    }

    // ⑪ ★ 2026-11-05 缺陷单：**guest 路径的编码**
    //    guest 根 = `E:\Games\Eushully\天結`（含日文；= `Vfs::guestRoot()` 的真实缺省值）。判据：
    //      ① `GetModuleFileNameA` 写出的字节 == **CP932**（`天結` = 93 56 8C 8B），⛔ 不是它的 UTF-8
    //         （`天結` = E5 A4 A9 E7 B5 90）—— 后者正是"引擎日志打出 `螟ｩ邨申AGE.EXE`"的成因；
    //      ② `GetCommandLineA` 指向的字节同样是 CP932（带引号）；`GetCommandLineW` 是 UTF-16；
    //      ③ `GetModuleFileNameW` 的 UTF-16 == `L"E:\Games\Eushully\天結\AGE.EXE"`（⛔ 不是把
    //         UTF-8 字节按 CP932 解出来的乱码）；
    //      ④ 把 ① 的**字节原样**喂回 `CreateFileA`（引擎就是这么用 exe 路径的）⇒ 命中 VFS overlay；
    //      ⑤ `FindFirstFileA` 的 `cFileName`（日文文件名）/ `GetFullPathNameA`（相对路径）也是 CP932。
    {
        g_current_case = "guest-exe-path-cp932";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        lift::registerFileShims(f.m.shims());
        lift::registerSyncShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));  // 宿主根随便；**guest 根**才是日文的
        f.m.vfs().setGuestRoot("E:\\Games\\Eushully\\天結");
        LIFT_CHECK(f.ok);

        lift::PeImage img;
        img.path = "C:\\host\\corpus\\AGE.EXE__dumped.sectfix.EXE";  // 宿主 `--exe` 串：⛔ 不许出现在 guest
        lift::ProcessSetup setup;
        std::string serr;
        LIFT_CHECK(lift::setupProcessStructures(f.m.mem(), img, f.m.layout(), f.m.vfs(), &setup, &serr));
        f.m.cpu().shim.exe_path_ea = setup.exe_path_ea;
        f.m.cpu().shim.cmdline_ea = setup.cmdline_ea;

        // ★ 期望的 **CP932 字节**：`天結` = 93 56 8C 8B（硬编码；与 tools/lib/cp932.mjs 的
        //   `encodeBack('天結')` 逐字节一致）。⛔ 不用 `wideToCp932` 现算 —— 那会让"编码器坏了"
        //   与"路径编码链坏了"两件事一起绿（自证）。
        const char kTenketsuCp932[4] = {'\x93', '\x56', '\x8C', '\x8B'};
        const std::string exe_cp932 = std::string("E:\\Games\\Eushully\\") +
                                      std::string(kTenketsuCp932, sizeof(kTenketsuCp932)) + "\\AGE.EXE";
        const std::string exe_utf8 = "E:\\Games\\Eushully\\天結\\AGE.EXE";
        const std::u16string exe_utf16 = u"E:\\Games\\Eushully\\天結\\AGE.EXE";
        // 两套字节真的不同（UTF-8 的 天結 是 6 字节、CP932 是 4 字节）⇒ 下面每条断言都能分开
        LIFT_CHECK(exe_cp932.size() + 2u == exe_utf8.size());
        LIFT_CHECK(exe_cp932 != exe_utf8);

        // ① GetModuleFileNameA ⇒ CP932
        const std::uint32_t gmf = f.use("GetModuleFileNameA");
        const std::uint32_t gmfw = f.use("GetModuleFileNameW");
        const std::uint32_t gcl = f.use("GetCommandLineA");
        const std::uint32_t gclw = f.use("GetCommandLineW");
        const std::uint32_t buf = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(gmf, {0u, buf, 512u}),
                          static_cast<std::uint32_t>(exe_cp932.size()));
        const std::string got_a = guestStr(f.m, buf);
        LIFT_CHECK(got_a == exe_cp932);
        if (got_a != exe_cp932) {
            // 红的时候要能一眼看出是"UTF-8 从 guest 内存漏出来了"
            std::fprintf(stderr, "  GetModuleFileNameA 实际字节: %s（期望 CP932: %s）\n",
                         hexBytes(got_a).c_str(), hexBytes(exe_cp932).c_str());
        }

        // ② GetCommandLineA（指针）/ GetCommandLineW（UTF-16）
        const std::uint32_t cl_ea = f.call(gcl, {});
        LIFT_CHECK(cl_ea != 0);
        const std::string got_cmd = guestStr(f.m, cl_ea);
        LIFT_CHECK(got_cmd == "\"" + exe_cp932 + "\"");
        if (got_cmd != "\"" + exe_cp932 + "\"") {
            std::fprintf(stderr, "  GetCommandLineA 实际字节: %s\n", hexBytes(got_cmd).c_str());
        }
        const std::uint32_t clw_ea = f.call(gclw, {});
        LIFT_CHECK(clw_ea != 0);
        LIFT_CHECK(guestWstr(f.m, clw_ea, 512) == u"\"" + exe_utf16 + u"\"");

        // ③ GetModuleFileNameW ⇒ UTF-16（W 版**不经过** CP932；见 vfs.h / kernel32_process.cpp）
        const std::uint32_t wbuf = f.zeroData(1024);  // 512 个 UTF-16 码元 = 1024 字节
        LIFT_CHECK_EQ_U32(f.call(gmfw, {0u, wbuf, 512u}),
                          static_cast<std::uint32_t>(exe_utf16.size()));
        LIFT_CHECK(guestWstr(f.m, wbuf, 512) == exe_utf16);

        // ④ 端到端：① 的字节**原样**当路径 ⇒ CreateFileA 命中 overlay（真机就是这么拿 exe 路径的）
        const fs::path packed = dir / "AGE.EXE";                     // 安装树里的加壳原件
        const fs::path dumped = dir / "overlay" / "AGE.dumped.exe";  // 真正在跑的那份
        fs::create_directories(dumped.parent_path(), ec);
        auto writeFile = [](const fs::path& p, const std::string& bytes) {
            // ★ 宿主打开走 `openHostFileUtf8`（= `_wfopen_s`）：裸 `std::fopen` 在 Windows 上按
            //   **本地 ANSI 码页**解释路径 ⇒ 日文文件名会落成另一个名字（本用例抓到的真事）。
            std::FILE* fp = lift::openHostFileUtf8(pathToUtf8(p), "wb");
            if (!fp) return false;
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), fp);
            std::fclose(fp);
            return true;
        };
        const std::string packed_bytes = "PACKED-ORIGINAL-CJK-ROOT";
        const std::string dumped_bytes = "UNPACKED-BYTES-CJK-ROOT-0123456789";
        LIFT_CHECK(writeFile(packed, packed_bytes));
        LIFT_CHECK(writeFile(dumped, dumped_bytes));
        // ★ 与 `Machine::prepare` 的装配**逐字一致**：overlay 的 guest 侧 = `vfs.guestExePath()`
        f.m.vfs().setExeOverlay(f.m.vfs().guestExePath(), pathToUtf8(dumped), "0123456789abcdef");

        const std::uint32_t create = f.use("CreateFileA");
        const std::uint32_t readf = f.use("ReadFile");
        const std::uint32_t getsize = f.use("GetFileSize");
        const std::uint32_t close = f.use("CloseHandle");
        const std::uint32_t path_ea = f.data(exe_cp932.c_str(), exe_cp932.size() + 1u);
        const std::uint32_t h = f.call(create, {path_ea, kGenericRead, 0u, 0u, kOpenExisting, 0u, 0u});
        LIFT_CHECK(h != kInvalidHandleValue);
        if (h != kInvalidHandleValue) {
            const std::uint32_t size = f.call(getsize, {h, 0u});
            LIFT_CHECK_EQ_U32(size, static_cast<std::uint32_t>(dumped_bytes.size()));
            const std::uint32_t nbuf = f.zeroData(256);
            const std::uint32_t got_ea = f.u32data(0);
            f.call(readf, {h, nbuf, 256u, got_ea, 0u});
            const std::uint32_t n = f.m.mem().rd32(got_ea);
            LIFT_CHECK(guestStr(f.m, nbuf, n + 1u) == dumped_bytes);
            f.call(close, {h});
        }
        // 加壳原件没被碰（overlay 只是路由）
        LIFT_CHECK(writeFile(packed, packed_bytes));

        // ⑤ 同一根因的其它面：FindFirstFileA 的 cFileName（日文文件名）+ GetFullPathNameA
        const std::string dat_utf8 = "天結.CJK";
        const char kTenketsuDatCp932[8] = {'\x93', '\x56', '\x8C', '\x8B', '.', 'C', 'J', 'K'};
        const std::string dat_cp932(kTenketsuDatCp932, sizeof(kTenketsuDatCp932));
        LIFT_CHECK(writeFile(dir / pathFromUtf8(dat_utf8), "CJK"));

        const std::uint32_t find = f.use("FindFirstFileA");
        const std::uint32_t findclose = f.use("FindClose");
        const std::uint32_t fdata = f.zeroData(0x140u);
        const std::string pattern_cp932 =
            std::string("E:\\Games\\Eushully\\") +
            std::string(kTenketsuCp932, sizeof(kTenketsuCp932)) + "\\*.CJK";
        const std::uint32_t pat_ea = f.data(pattern_cp932.c_str(), pattern_cp932.size() + 1u);
        const std::uint32_t fh = f.call(find, {pat_ea, fdata});
        LIFT_CHECK(fh != kInvalidHandleValue);
        if (fh != kInvalidHandleValue) {
            std::string cfile;
            for (std::uint32_t i = 0; i < 260u; ++i) {  // cFileName[260] @ WIN32_FIND_DATAA+0x2C
                const char c = static_cast<char>(f.m.mem().rd8(fdata + 0x2Cu + i));
                if (c == 0) break;
                cfile.push_back(c);
            }
            LIFT_CHECK(cfile == dat_cp932);
            if (cfile != dat_cp932) {
                std::fprintf(stderr, "  FindFirstFileA 的 cFileName 实际字节: %s\n", hexBytes(cfile).c_str());
            }
            f.call(findclose, {fh});
        }

        // GetFullPathNameA：相对路径（cwd = guest 根）⇒ CP932 全路径
        const std::uint32_t gfp = f.use("GetFullPathNameA");
        const std::uint32_t rel_ea = f.cstr("AGE.EXE");
        const std::uint32_t full_buf = f.zeroData(512);
        LIFT_CHECK_EQ_U32(f.call(gfp, {rel_ea, 512u, full_buf, 0u}),
                          static_cast<std::uint32_t>(exe_cp932.size()));
        LIFT_CHECK(guestStr(f.m, full_buf) == exe_cp932);

        fs::remove_all(dumped.parent_path(), ec);
    }

    // ⑫ ★ 2026-11-05 缺陷单（S2）：`Vfs::resolve` 认 **guest 根前缀**
    //
    //   repro（修前红）：boot 跑到 112,255,445 条指令时 `version!GetFileVersionInfoSizeA` 拿着
    //   guest 内存里的路径 `E:\Games\Eushully\天結\AGE.EXE` 解析失败（`[notdir] 段 "Games"`）
    //   ⇒ 引擎读不到自己的版本 ⇒ MessageBox ⇒ ExitProcess(0)。
    //
    //   判据：
    //     ① `<guest 根>\AGE.EXE` 的 UTF-8 文本 ⇒ 命中 overlay（= `setExeOverlay` 的那份，⛔ 不是壳里的）；
    //     ② 大小写 / 分隔符变体（`e:/games/eushully/天結/age.exe`）⇒ **同一个**宿主路径；
    //     ③ **CP932 字节**形态（guest 内存里就是 `93 56 8C 8B`）⇒ 同样命中 —— 修前这一条红：
    //        它被按 UTF-8 读 ⇒ `天結` 与根文本比不中 ⇒ `[notdir] 段 "Games"`；顺带断言
    //        `guestPathForLog` 把这条字节串还原成可读的 UTF-8（日志不再 `?V??`）；
    //     ④ `<guest 根>\SAVE\X.DAT` ⇒ `<game-dir>\SAVE\X.DAT`（根之后**照旧**走分段解析）；
    //     ⑤ 反例：`E:\Games\Other\x` ⇒ 失败（不是 guest 根，⛔ 不许剥成 `Other\x` / `Games\Other\x` 去猜）；
    //     ⑥ 反例：根的文本只出现在**中间**（`C:\backup\…\天結\AGE.EXE`）⇒ 失败
    //        （⛔ 不是"路径里含根就剥"——那正是把备份路径 / 中间段当根的模糊匹配）；
    //     ⑦ 反例：`..\..\escape` 仍被 `[escape]` 拦（剥前缀这条路**不**放宽安全检查）。
    {
        g_current_case = "guest-root-absolute";
        lift::Vfs v;
        v.setGameDir(pathToUtf8(dir));
        // ★ 不 `setGuestRoot`：判据必须覆盖**缺省根**（vfs.h 的 `E:\Games\Eushully\天結`）——
        //   真机 boot 就是没传 `--guest-root` 的那条路。
        LIFT_CHECK(v.ready());
        LIFT_CHECK(v.guestRoot() == "E:\\Games\\Eushully\\天結");

        auto writeFile = [](const fs::path& p, const std::string& bytes) {
            // ★ 宿主打开走 `openHostFileUtf8`（= `_wfopen_s`）：裸 `std::fopen` 在 Windows 上按
            //   **本地 ANSI 码页**解释日文名（见本文件 ⑪ 的实测）。
            std::FILE* fp = lift::openHostFileUtf8(pathToUtf8(p), "wb");
            if (!fp) return false;
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), fp);
            std::fclose(fp);
            return true;
        };
        const fs::path packed = dir / "AGE.EXE";                    // 安装树里的加壳原件
        const fs::path dumped = dir / "overlay" / "AGE.dumped.exe";  // 真正在跑的那份（DEBUG 件）
        fs::create_directories(dumped.parent_path(), ec);
        LIFT_CHECK(writeFile(packed, "PACKED-ORIGINAL-0"));
        LIFT_CHECK(writeFile(dumped, "UNPACKED-DEBUG-BUILD-0123456789"));
        v.setExeOverlay(v.guestExePath(), pathToUtf8(dumped), "0123456789abcdef");
        const std::string dumped_host = pathToUtf8(dumped);

        std::string host;
        std::string why;

        // ① UTF-8 文本（= guest 根的缺省文本形态）
        LIFT_CHECK(v.resolve("E:\\Games\\Eushully\\天結\\AGE.EXE", &host, &why, true));
        LIFT_CHECK(host == dumped_host);
        LIFT_CHECK(v.overlayHostPath() == dumped_host);

        // ② 大小写 / 分隔符变体 ⇒ 同一个宿主路径
        LIFT_CHECK(v.resolve("e:/games/eushully/天結/age.exe", &host, &why, true));
        LIFT_CHECK(host == dumped_host);
        LIFT_CHECK(v.resolve("e:\\GAMES\\eushully\\天結\\AGE.exe", &host, &why, true));
        LIFT_CHECK(host == dumped_host);

        // 根**本身**（末尾带分隔符）⇒ `--game-dir` 根
        LIFT_CHECK(v.resolve("E:\\Games\\Eushully\\天結\\", &host, &why, true));
        LIFT_CHECK(host == pathToUtf8(dir));

        // ③ CP932 字节（guest 内存里的真实形态；`天結` = 93 56 8C 8B）
        const char kTenketsuCp932[4] = {'\x93', '\x56', '\x8C', '\x8B'};
        const std::string root_utf8 = "E:\\Games\\Eushully\\天結";
        const std::string cp932_exe = "E:\\Games\\Eushully\\" + std::string(kTenketsuCp932, 4) + "\\AGE.EXE";
        LIFT_CHECK(cp932_exe.size() + 2u == (root_utf8 + "\\AGE.EXE").size());  // 两套字节真的不同
        LIFT_CHECK(cp932_exe != root_utf8 + "\\AGE.EXE");
        LIFT_CHECK(lift::guestPathForLog(cp932_exe) == root_utf8 + "\\AGE.EXE");
        LIFT_CHECK(v.resolve(cp932_exe, &host, &why, true));
        LIFT_CHECK(host == dumped_host);

        // ④ 根之后的段照旧走分段解析（SAVE 目录存在 ⇒ 文件可以不存在）
        fs::create_directories(dir / "SAVE", ec);
        LIFT_CHECK(v.resolve("E:\\Games\\Eushully\\天結\\SAVE\\X.DAT", &host, &why));
        LIFT_CHECK(host == pathToUtf8(dir / "SAVE" / "X.DAT"));

        // ⑤ 反例：不是 guest 根 ⇒ 失败（不许把 `E:\Games\Other\x` 剥成 `Other\x` / `Games\Other\x`）
        LIFT_CHECK(!v.resolve("E:\\Games\\Other\\x", &host, &why));
        LIFT_CHECK(host.empty());
        LIFT_CHECK(why.rfind("[", 0) == 0);  // 带 tag 前缀（shim 靠它映射错误码）

        // ⑥ 反例：根的文本只出现在**中间** ⇒ 不许剥（"任意包含"的模糊匹配会在这里绿）
        LIFT_CHECK(!v.resolve("C:\\backup\\Games\\Eushully\\天結\\AGE.EXE", &host, &why, true));
        LIFT_CHECK(host.empty());

        // ⑦ 反例：`..` 越出 guest 根 ⇒ 仍是 `[escape]`
        LIFT_CHECK(!v.resolve("..\\..\\escape", &host, &why));
        LIFT_CHECK(why.rfind("[escape]", 0) == 0);
    }

    // ⑬ ★ 2026-11-05 `REQ-01M6KNBE5FP87DAC93ZRQ1K1DK`：**写覆盖层** —— `--game-dir` 只读
    //
    //   repro（修前）：boot 跑到 104.5M 条指令时引擎写 `Eushully\天結…\$$AGE.TMP`（相对路径）+
    //   `MakeSureDirectoryPathExists` 准备目录；旧实现直接落进安装树（实测在
    //   `links/install/Eushully/…` 建过 `SAVE`）。本用例钉住新口径：
    //     ① `CreateFileA(CREATE_ALWAYS)` 对 `--game-dir` 相对路径写成功 ⇒ 字节落在
    //        `<--appdata>/Game/…`（影子目录），安装树里**没有**这个文件 / 目录；
    //     ② 同一 guest 路径随后 `OPEN_EXISTING` 读 ⇒ 读回**影子**内容（先影子、再安装树）；
    //     ③ `MakeSureDirectoryPathExists` 在影子目录建出目录（安装树里没有）；
    //     ④ 只读回归：安装树里本来就有的文件照常读；改写它 = copy-up 到影子（安装树字节不变）；
    //     ⑤ 影子 ∪ 安装树合并枚举（`FindFirstFileA` 两边都看得见、同名影子优先且只出现一次）；
    //     ⑥ 没有 whiteout：`DeleteFileA` 对"只在安装树里"的文件响亮失败（5），安装树文件还在。
    {
        g_current_case = "game-dir-write-overlay";
        Fixture f;
        lift::registerFileShims(f.m.shims());
        lift::registerSyncShims(f.m.shims());
        lift::registerImagehlpShims(f.m.shims());
        f.m.vfs().setGameDir(pathToUtf8(dir));
        f.m.vfs().setGuestRoot("D:\\GAME");
        f.m.vfs().setAppDataDir(pathToUtf8(dir / "appdata"));
        LIFT_CHECK(f.ok);

        const fs::path shadow = dir / "appdata" / "Game";  // vfs.h 的唯一落点
        const fs::path game = dir;
        auto readHost = [](const fs::path& p) -> std::string {
            std::FILE* fp = lift::openHostFileUtf8(pathToUtf8(p), "rb");
            if (!fp) return "<missing>";
            std::string s;
            char tmp[64];
            std::size_t n = 0;
            while ((n = std::fread(tmp, 1, sizeof(tmp), fp)) > 0) s.append(tmp, n);
            std::fclose(fp);
            return s;
        };
        auto writeHost = [](const fs::path& p, const std::string& bytes) -> bool {
            std::FILE* fp = lift::openHostFileUtf8(pathToUtf8(p), "wb");
            if (!fp) return false;
            if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), fp);
            std::fclose(fp);
            return true;
        };

        // 安装树里的既有文件（引擎"先读再改"的形态）：字节必须保持不变
        LIFT_CHECK(writeHost(game / "config.ini", "ORIGINAL-CONFIG"));
        LIFT_CHECK(writeHost(game / "install-only.dat", "INSTALL-ONLY"));

        const std::uint32_t create = f.use("CreateFileA");
        const std::uint32_t writef = f.use("WriteFile");
        const std::uint32_t readf = f.use("ReadFile");
        const std::uint32_t close = f.use("CloseHandle");
        const std::uint32_t msdpe = f.use("imagehlp", "MakeSureDirectoryPathExists");
        const std::uint32_t find = f.use("FindFirstFileA");
        const std::uint32_t findnext = f.use("FindNextFileA");
        const std::uint32_t findclose = f.use("FindClose");
        const std::uint32_t del = f.use("DeleteFileA");
        LIFT_CHECK_NE_U32(msdpe, 0u);

        auto readGuest = [&](const char* guest_path) -> std::string {
            const std::uint32_t h = f.call(create, {f.cstr(guest_path), kGenericRead, 0u, 0u,
                                                    kOpenExisting, 0u, 0u});
            if (h == kInvalidHandleValue) return "<open-failed>";
            const std::uint32_t buf = f.zeroData(256);
            const std::uint32_t got = f.u32data(0);
            f.call(readf, {h, buf, 256u, got, 0u});
            const std::uint32_t n = f.m.mem().rd32(got);
            std::string s = guestStr(f.m, buf, n + 1u);
            f.call(close, {h});
            return s;
        };
        auto writeGuest = [&](const char* guest_path, const std::string& bytes) -> std::uint32_t {
            const std::uint32_t h = f.call(create, {f.cstr(guest_path), kGenericWrite, 0u, 0u,
                                                    kCreateAlways, 0u, 0u});
            if (h == kInvalidHandleValue) return h;
            const std::uint32_t data = f.data(bytes.data(), bytes.size());
            const std::uint32_t written = f.u32data(0);
            f.call(writef, {h, data, static_cast<std::uint32_t>(bytes.size()), written, 0u});
            f.call(close, {h});
            return h;
        };

        // ① 引擎的形态：**相对路径**（cwd = guest 根）+ 两段安装树里不存在的目录
        const char* kTempGuest = "Eushully\\Tenketsu\\$$AGE.TMP";
        LIFT_CHECK_NE_U32(writeGuest(kTempGuest, "SHADOW-TEMP"), kInvalidHandleValue);
        LIFT_CHECK(fs::exists(shadow / "Eushully" / "Tenketsu" / "$$AGE.TMP", ec));
        LIFT_CHECK(!fs::exists(game / "Eushully", ec));  // 安装树里一个目录都不许多
        LIFT_CHECK(readHost(shadow / "Eushully" / "Tenketsu" / "$$AGE.TMP") == "SHADOW-TEMP");

        // ② 同一 guest 路径随后读 ⇒ 影子内容（先影子、再安装树）
        LIFT_CHECK(readGuest(kTempGuest) == "SHADOW-TEMP");

        // ③ MakeSureDirectoryPathExists 在影子目录建目录（安装树里没有）
        LIFT_CHECK_EQ_U32(f.call(msdpe, {f.cstr("D:\\GAME\\Eushully\\Tenketsu\\SAVE\\")}), 1u);
        LIFT_CHECK(fs::is_directory(shadow / "Eushully" / "Tenketsu" / "SAVE", ec));
        LIFT_CHECK(!fs::exists(game / "Eushully", ec));

        // ④ 只读回归 + copy-up：安装树里已有的 `config.ini` 覆盖写 ⇒ 只改**影子副本**，
        //    安装树字节不变；随后读回影子内容
        LIFT_CHECK_NE_U32(writeGuest("config.ini", "SHADOW-CONFIG"), kInvalidHandleValue);
        LIFT_CHECK(readHost(game / "config.ini") == "ORIGINAL-CONFIG");  // 安装树不变
        LIFT_CHECK(readHost(shadow / "config.ini") == "SHADOW-CONFIG");
        LIFT_CHECK(readGuest("config.ini") == "SHADOW-CONFIG");

        // ⑤ 影子 ∪ 安装树：枚举根目录同时看得见 `install-only.dat`（安装树）与影子项；
        //    同名的 `config.ini` 只出现一次、大小取影子那份
        {
            std::vector<std::string> names;
            std::uint64_t config_size = 0;
            bool saw_install_only = false;
            std::uint32_t config_hits = 0;
            const std::uint32_t fd = f.zeroData(kFindDataSize);
            const std::uint32_t h = f.call(find, {f.cstr("D:\\GAME\\*.*"), fd});
            LIFT_CHECK(h != kInvalidHandleValue);
            if (h != kInvalidHandleValue) {
                for (;;) {
                    std::string cfile;
                    for (std::uint32_t i = 0; i < 260u; ++i) {
                        const char c = static_cast<char>(f.m.mem().rd8(fd + kFdFileName + i));
                        if (c == 0) break;
                        cfile.push_back(c);
                    }
                    names.push_back(cfile);
                    if (cfile == "config.ini") {
                        config_size =
                            (static_cast<std::uint64_t>(f.m.mem().rd32(fd + kFdSizeHigh)) << 32) |
                            f.m.mem().rd32(fd + kFdSizeLow);
                    }
                    if (f.call(findnext, {h, fd}) == 0) {
                        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNoMoreFiles);
                        break;
                    }
                }
                f.call(findclose, {h});
            }
            for (const std::string& n : names) {
                if (n == "install-only.dat") saw_install_only = true;
                if (n == "config.ini") ++config_hits;
            }
            LIFT_CHECK(saw_install_only);
            LIFT_CHECK_EQ_U32(config_hits, 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(config_size),
                              static_cast<std::uint32_t>(std::strlen("SHADOW-CONFIG")));
        }

        // ⑥ 没有 whiteout：只在安装树里的文件删不掉（响亮失败 5，安装树文件还在）
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(del, {f.cstr("install-only.dat")}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 5u);
        LIFT_CHECK(readHost(game / "install-only.dat") == "INSTALL-ONLY");
        LIFT_CHECK(!fs::exists(shadow / "install-only.dat", ec));

        // 收尾：清掉本用例写出来的影子目录（`dir` 会被后面的运行复用）
        fs::remove_all(shadow, ec);
        LIFT_CHECK(f.ok);
    }

    fs::remove_all(dir, ec);
    return finish("file-path-ext");
}
