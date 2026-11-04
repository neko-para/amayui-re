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
constexpr std::uint32_t kInvalidHandleValue = 0xFFFFFFFFu;
constexpr std::uint32_t kErrInvalidParameter = 87u;

// 2020-01-02T03:04:05Z 的 FILETIME（100 ns 自 1601）= (1577934245 + 11644473600) * 10^7
constexpr std::uint64_t kTargetFileTime = 132224078450000000ull;

std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
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

    fs::remove_all(dir, ec);
    return finish("file-path-ext");
}
