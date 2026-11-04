// emulator-lift · kernel32 文件与路径族（S6：plan §6 S6 / §9 R2）
//
// ★ 这一族的口径：
//   · **路径一律先过 `cpu.vfs`**（唯一根 = `--game-dir`）：A 版入参是 **CP932 字节**（本作的码页），
//     W 版是 UTF-16 ⇒ 两者都先转成 **UTF-8** 再交给 `Vfs::resolve` / `Vfs::find`。
//     相对路径相对 `cpu.vfs->guestCwd()`（解析本身在 vfs.cpp 里做，见那里的 tag 说明）。
//   · **失败必须是 Windows 的真实失败值**：`INVALID_HANDLE_VALUE(0xFFFFFFFF)` / `FALSE` +
//     `cpu.shim.last_error`。vfs 的错误串带 tag（`[notfound]` 等）⇒ 本文件用
//     `vfsErrorToWin32()` 一张表映射；⛔ 不许"看起来成功"。
//   · **句柄值只来自 `HandleTable`**（确定性，0x100 起 4 字节步进）。`HandleKind::File` 的 payload
//     是 `FileState`（持有 `std::FILE*`），`HandleKind::Find` 的是 `FindState`（向量 + 游标）；
//     两者都装 `HandleEntry::destroy`，由 sync 族的 `CloseHandle` 或本族的 `FindClose` 回收。
//   · **`ReadFile` / `WriteFile` 走 `std::fread` / `std::fwrite`**；每次传输前做一次
//     `fseek(0, SEEK_CUR)`（C 标准要求"输出→输入"之间有一个定位函数，否则是未定义行为）。
//   · **枚举顺序 = `Vfs::find` 排序后的顺序**（plan §8.3-4 的确定性要求），⛔ 不用宿主目录顺序。
//   · **时间**：`GetFileTime` / `WIN32_FIND_DATAA` 的三个时间都是**宿主真值**（vfs 的
//     `hostFileTimes` 走 SDL3 `SDL_GetPathInfo`，含创建 / 访问时间）；只有 SDL 失败时才退回
//     `last_write_time` + "此刻两钟差值"的近似（三值相同、同一次运行内单调）。
//   · **日志**：每一次"成功但不真做"（卷信息 / 时间近似 / 不建模的 FILE_FLAG_*）留一行 `shimLog`；
//     参数非法 / 未提交缓冲区 / 无效句柄 ⇒ `shimAbort` / `HandleTable::invalidHandle`（响亮失败）。
//
// ⚠ 偏差（逐条；M1 的边界，按 plan §9 R2 "先只实现游戏实际用到的形态"）：
//   1. **`SetFilePointerEx` 的 `argc` 是 5 不是 4**：x86 stdcall 下 `LARGE_INTEGER`（8 字节）
//      占两个栈槽 ⇒ 真机导出是 `SetFilePointerEx@20`（hFile / lo / hi / lpNew / method）。
//      写 4 会让**每次调用漏弹 4 字节栈**（几百次后炸在别处）。同样地，
//      `GetFileTime`/`SetFileTime` 的 4 个参数都是指针 ⇒ argc=4 没问题。
//   2. **`dwShareMode` 按"每一对同路径句柄双向"判定**：新请求的 access 位撞已有句柄的 share、
//      已有句柄的 access 位撞新请求的 share ⇒ `ERROR_SHARING_VIOLATION(32)`；冲突**只由访问位
//      定义**，`access = 0` 的打开（只查属性的探针）不请求任何访问位 ⇒ 两个方向都不参与冲突。
//      ⛔ 仍**不实现真共享锁**，也**不建模** `FILE_SHARE_DELETE` 与 `DELETE` 访问位
//      （`0x00010000`；本模拟器只有一个进程、没有别的进程能删/锁文件）—— 请求了 DELETE 会留日志。
//   3. **`dwFlagsAndAttributes` 基本忽略**：`FILE_FLAG_BACKUP_SEMANTICS`（开目录）/
//      `FILE_FLAG_DELETE_ON_CLOSE` / `FILE_ATTRIBUTE_TEMPORARY` / `FILE_FLAG_OVERLAPPED` 都不建模；
//      请求了 `FILE_FLAG_BACKUP_SEMANTICS` 就留一行日志。**目录仍然打不开**（真机不带这个标志也打不开）。
//   4. **`lpSecurityAttributes` / `hTemplateFile` 忽略**：本模拟器没有安全描述符与文件模板。
//   5. **`SetFileAttributesA` 只模拟 READONLY**（宿主权限位）；HIDDEN / SYSTEM / ARCHIVE 等属性位
//      只回读成"文件恒有 ARCHIVE"，⛔ 不落盘。请求了别的位会留一行日志。
//   6. **`GetVolumeInformationA` 是固定值**：卷名 `"AGE"`、序列号常量、文件系统 `"NTFS"`、
//      `FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK`（**不**报 CASE_SENSITIVE —— 我们的 VFS
//      确实是大小写不敏感的）。每次调用留一行日志。
//   7. **`CopyFileA` 用宿主 `copy_file`**：跨卷复制由宿主负责（Windows 的 `CopyFile` 也是）；
//      文件属性 / 时间戳的复制不在本模拟器保证范围内。
//   8. **`GetTempPathA` / `GetTempFileNameA` 的落点 = `<--appdata>/Local/Temp`**（guest 文本
//      `C:\Users\Amayui\AppData\Local\Temp`，与 `TEMP` / `TMP` / 环境块**同源**；2026-11-05 改）。
//      没有 `--appdata` ⇒ 二者都**响亮失败**（⛔ 不再退回 `--game-dir/.lift-tmp`：那是对安装树的写）。
//      `GetTempFileNameA` 的 `uUnique != 0` 按文档**用调用方的值**（只看低 16 位、`%04X` 大写）、
//      **不创建文件**、返回 `uUnique` 本身；`uUnique == 0` 时**有意偏差**：从 0 起找第一个空号
//      （真机从系统时间起，这里确定性优先）并创建那个空文件。前缀一律按字节截到 **3** 个字符，
//      扩展名 `.TMP`（文档形态，大写）。
//   9. **`FindFirstFileA` 的 `cAlternateFileName`（8.3 短名）恒为 0**（plan §9 R2 点名不做）。
//      `WIN32_FIND_DATAA` / `GetFileTime` 的三个时间是**宿主真值**（vfs 的 `hostFileTimes`：
//      Windows 走 SDL3 `SDL_GetPathInfo`；macOS 走 `stat` 的 `st_birthtimespec`（SDL 在 POSIX 给的是
//      ctime）；其它 POSIX 接受 ctime 回退）；只有取时间失败时才三值一起退回 mtime 近似。
//  10. **`SetFileTime` 真写宿主时间戳**（Windows 走 `CreateFileW(FILE_WRITE_ATTRIBUTES)` +
//      `::SetFileTime`，三个时间都能写；macOS 走 `utimensat`（access/write）+
//      `setattrlist(ATTR_CMN_CRTIME)`（creation）；其它 POSIX 走 `utimensat`，creation 写不了 ⇒
//      留一行日志（文档也明说"not all file systems can record creation and last access times"）。
//      `lpXxxTime` 为 NULL 或两个 DWORD 全 0 ⇒ 不改那一个；
//      两个 DWORD 全 0xFFFFFFFF（文档的"禁止后续更新"哨兵）⇒ 只记日志、不改（宿主时间戳没有这个
//      开关）。⛔ 不再"返回成功但什么都不做"。
//  11. **`GetCurrentDirectoryW` 是 W 版里唯一多做的一条**（`GetCurrentDirectoryA/W` 同一份实现），
//      其余 W 版只做任务点名的 `CreateFileW`。
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#include <unistd.h>  // fseeko / ftello / off_t（Windows 上走 _fseeki64 / _ftelli64）
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // SetFileTime 的宿主侧实现（CreateFileW + ::SetFileTime）
#else
#include <fcntl.h>      // AT_FDCWD
#include <sys/stat.h>   // utimensat / timespec
#if defined(__APPLE__)
#include <sys/attr.h>   // setattrlist / ATTR_CMN_CRTIME（macOS 的 birthtime 写入口）
#endif
#endif

#include "runtime/cp932.h"
#include "runtime/vfs.h"   // HostDirEntry / kAttr* / Vfs 的完整定义（x86.h 只前置声明 Vfs）
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------
// 错误码（只列本族用到的；数值与 Win32 一致）
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kErrFileNotFound = 2u;
constexpr std::uint32_t kErrPathNotFound = 3u;
constexpr std::uint32_t kErrAccessDenied = 5u;
constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrNoMoreFiles = 18u;
constexpr std::uint32_t kErrWriteFault = 29u;
constexpr std::uint32_t kErrReadFault = 30u;
constexpr std::uint32_t kErrSharingViolation = 32u;
constexpr std::uint32_t kErrFileExists = 80u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrCallNotImplemented = 120u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrInvalidName = 123u;
constexpr std::uint32_t kErrNegativeSeek = 131u;
constexpr std::uint32_t kErrDirNotEmpty = 145u;
constexpr std::uint32_t kErrAlreadyExists = 183u;
constexpr std::uint32_t kErrDirectory = 267u;

// 返回值
constexpr std::uint32_t kInvalidHandleValue = 0xFFFFFFFFu;
constexpr std::uint32_t kInvalidSetFilePointer = 0xFFFFFFFFu;

// 访问位 / disposition / seek method（Win32 常量）
constexpr std::uint32_t kGenericRead = 0x80000000u;
constexpr std::uint32_t kGenericWrite = 0x40000000u;
constexpr std::uint32_t kGenericAll = 0x10000000u;
constexpr std::uint32_t kFileReadData = 0x00000001u;
constexpr std::uint32_t kFileWriteData = 0x00000002u;
constexpr std::uint32_t kFileAppendData = 0x00000004u;
constexpr std::uint32_t kFileDelete = 0x00010000u;  // DELETE：不建模（见文件头偏差 2）

// 本族内部的"读 / 写"两位：数值**故意**与 FILE_SHARE_READ / FILE_SHARE_WRITE 对齐，
// 这样共享冲突可以直接按位比（`access & ~share`），不需要再做一次映射。
constexpr std::uint32_t kAccessRead = 0x00000001u;   // FILE_SHARE_READ
constexpr std::uint32_t kAccessWrite = 0x00000002u;  // FILE_SHARE_WRITE
constexpr std::uint32_t kAccessMask = kAccessRead | kAccessWrite;

constexpr std::uint32_t kCreateNew = 1u;
constexpr std::uint32_t kCreateAlways = 2u;
constexpr std::uint32_t kOpenExisting = 3u;
constexpr std::uint32_t kOpenAlways = 4u;
constexpr std::uint32_t kTruncateExisting = 5u;

constexpr std::uint32_t kFileBegin = 0u;
constexpr std::uint32_t kFileCurrent = 1u;
constexpr std::uint32_t kFileEnd = 2u;

constexpr std::uint32_t kFileFlagBackupSemantics = 0x02000000u;

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
constexpr std::uint32_t kFdFileNameChars = 260u;
constexpr std::uint32_t kFdAltName = 0x130u;
constexpr std::uint32_t kFdAltNameChars = 14u;

// 1601-01-01 → 1970-01-01 的 100 ns 数（与 kernel32_time.cpp / vfs.cpp 同一个常数）
constexpr std::int64_t kFileTimeEpochDelta = 116444736000000000ll;

// 卷信息的固定值（偏差 6）
constexpr std::uint32_t kFixedVolumeSerial = 0x41474531u;   // "AGE1"
constexpr std::uint32_t kMaxComponentLength = 255u;
constexpr std::uint32_t kFileSystemFlags = 0x00000002u | 0x00000004u;  // CASE_PRESERVED | UNICODE_ON_DISK

// ---------------------------------------------------------------------------------------
// UTF-8 ↔ UTF-16
// ★ 实现在 `runtime/cp932.cpp`（`utf8ToUtf16` / `utf16ToUtf8`）：W 版 API 的多个 shim 都要用，
//   放一处免得每族各写一份（行为与原先的局部版本逐行相同）。
// ---------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------
// 宿主路径 / 目录项的小工具
// ---------------------------------------------------------------------------------------
fs::path pathFromUtf8(std::string_view s) {
    const std::u8string u(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return fs::path(u);
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ★ 为什么不是 `std::fopen`：MSVC 的窄字符 `fopen` 按 **ANSI 代码页**解释路径，而本仓的路径
//   一律 UTF-8（游戏目录本身是日文名）⇒ Windows 上必须走宽字符版（`_wfopen_s`，避免 C4996）。
std::FILE* openHostFile(const std::string& host_utf8, const char* mode) {
#if defined(_WIN32)
    const std::wstring wp = pathFromUtf8(host_utf8).wstring();
    const std::wstring wm(mode, mode + std::strlen(mode));
    std::FILE* fp = nullptr;
    if (::_wfopen_s(&fp, wp.c_str(), wm.c_str()) != 0) return nullptr;
    return fp;
#else
    return std::fopen(host_utf8.c_str(), mode);
#endif
}

// errno 的可读文本（`strerror` 在 MSVC 上已弃用 ⇒ 走 `strerror_s`）
std::string errnoText(int e) {
#if defined(_WIN32)
    char buf[160] = {};
    ::strerror_s(buf, sizeof(buf), e);
    return std::string(buf);
#else
    return std::string(std::strerror(e));
#endif
}

// 64 位 seek（std::fseek 的 long 在 Windows 上是 32 位 ⇒ 超过 2 GiB 会失效）
int seekFile64(std::FILE* fp, std::int64_t offset, int whence) {
#if defined(_WIN32)
    return ::_fseeki64(fp, offset, whence);
#else
    return ::fseeko(fp, static_cast<off_t>(offset), whence);
#endif
}

std::int64_t tellFile64(std::FILE* fp) {
#if defined(_WIN32)
    return ::_ftelli64(fp);
#else
    return static_cast<std::int64_t>(::ftello(fp));
#endif
}

// `file_time_type` → FILETIME 100 ns：**只在 SDL 取不到真值时**兜底（近似口径同 vfs.cpp 的
// fileTime100ns；三值相同、同一次运行内单调），不是默认路径。
std::int64_t fileTime100ns(fs::file_time_type t) {
    using hundred_ns = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    const auto now_file = fs::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now();
    const auto delta = t - now_file;
    const auto sys = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return std::chrono::duration_cast<hundred_ns>(sys.time_since_epoch()).count() + kFileTimeEpochDelta;
}

// 宿主上的属性近似（文件恒有 ARCHIVE；只读由宿主权限位推）
std::uint32_t attributesOfStatus(const fs::file_status& st) {
    if (fs::is_directory(st)) return kAttrDirectory;
    std::uint32_t a = kAttrArchive;
    if ((st.permissions() & fs::perms::owner_write) == fs::perms::none) a |= kAttrReadOnly;
    return a;
}

// vfs 的 error tag → Win32 错误码（映射表与 vfs.cpp 文件头的 tag 清单一一对应）
std::uint32_t vfsErrorToWin32(const std::string& e) {
    if (e.rfind("[notfound]", 0) == 0) return kErrFileNotFound;
    if (e.rfind("[notdir]", 0) == 0) return kErrPathNotFound;
    if (e.rfind("[escape]", 0) == 0) return kErrAccessDenied;
    if (e.rfind("[ambiguous]", 0) == 0) return kErrInvalidName;
    if (e.rfind("[invalid]", 0) == 0) return kErrInvalidName;
    if (e.rfind("[io]", 0) == 0) return kErrAccessDenied;
    return kErrPathNotFound;
}

std::uint32_t errnoToWin32(int e) {
    switch (e) {
        case ENOENT: return kErrFileNotFound;
        case EACCES:
        case EPERM: return kErrAccessDenied;
        case EEXIST: return kErrFileExists;
        case ENOSPC: return 39u;  // ERROR_HANDLE_DISK_FULL
        default: return kErrAccessDenied;
    }
}

// ---------------------------------------------------------------------------------------
// 现场检查 / 诊断
// ---------------------------------------------------------------------------------------
void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

void requireHandles(X86& cpu, const char* api) {
    requireMem(cpu, api);
    if (!cpu.handles) shimAbort("kernel32", api, "cpu.handles 为空（Machine 没装配句柄表）");
}

void requireVfs(X86& cpu, const char* api) {
    requireMem(cpu, api);
    if (!cpu.vfs) shimAbort("kernel32", api, "cpu.vfs 为空（Machine 没装配 VFS）");
    if (!cpu.vfs->ready()) {
        shimAbort("kernel32", api, "VFS 没有根（--game-dir 未设置）—— 文件与路径族无法回答任何路径");
    }
}

void requireCommitted(X86& cpu, std::uint32_t ea, std::size_t n, const char* api, const char* what) {
    if (ea == 0) shimAbort("kernel32", api, "%s 为 NULL", what);
    if (n == 0) return;
    if (!cpu.mem->committed(ea, n)) {
        shimAbort("kernel32", api, "%s = 0x%08X 起 %u 字节未提交（guest 缓冲区非法）", what, ea,
                  static_cast<unsigned>(n));
    }
}

// 路径类失败：设 last_error + 一行日志（返回值由调用方给）
void pathFailure(X86& cpu, const char* api, std::uint32_t code, const std::string& guest_path,
                 const std::string& why) {
    cpu.shim.last_error = code;
    shimLog("kernel32", api, "\"%s\" ⇒ 失败 %u（%s）", guest_path.c_str(), code, why.c_str());
}

// ---------------------------------------------------------------------------------------
// 路径入参（A 版 = CP932 字节；W 版 = UTF-16）⇒ UTF-8
// ---------------------------------------------------------------------------------------
std::string pathFromCp932(const std::string& bytes, const char* api) {
    if (bytes.empty()) return {};
    std::size_t bad = 0;
    const std::u16string w = cp932ToWide(bytes, &bad);
    if (bad != 0) {
        shimLog("kernel32", api,
                "路径里有 %u 字节不是合法 CP932 ⇒ 已按 Windows 的宽松口径跳过（见文件头）",
                static_cast<unsigned>(bad));
    }
    std::size_t bad_utf16 = 0;
    return utf16ToUtf8(w, &bad_utf16);
}

std::string pathFromWide(const std::u16string& w) {
    std::size_t bad = 0;
    return utf16ToUtf8(w, &bad);
}

// ★ UTF-8 文本 → CP932 字节：唯一实现在 `runtime/cp932`（`utf8ToCp932`）。
//   本文件原有一份同义的局部 `cp932FromUtf8` —— 两处各拼一遍必然漂（2026-11-05 缺陷单：
//   日文 guest 路径乱码就是"A 版字节 / UTF-8 文本"在某一处混用了）。

// 把 CP932 字节写进 guest 缓冲：截断到 cap-1、保证 0 结尾，返回写入字节数（不含结尾 0）。
// ★ 截断走 `cp932Truncate`：**成对**丢双字节字符（⛔ 不留半个字符 —— 半个字符写进
//   `lpTempFileName` / `cFileName` 这类定长字段就是非法 CP932）。
std::uint32_t writeAnsiBuffer(X86& cpu, std::uint32_t ea, std::uint32_t cap,
                              const std::string& bytes, const char* api, const char* what) {
    const std::string clipped = cp932Truncate(bytes, static_cast<std::size_t>(cap) - 1u);
    const std::size_t n = clipped.size();
    requireCommitted(cpu, ea, n + 1u, api, what);
    if (n > 0) cpu.mem->writeBytes(ea, clipped.data(), n);
    cpu.mem->wr8(ea + static_cast<std::uint32_t>(n), 0);
    return static_cast<std::uint32_t>(n);
}

// ---------------------------------------------------------------------------------------
// 文件对象
// ---------------------------------------------------------------------------------------
struct FileState {
    std::FILE* fp = nullptr;
    std::string host_path;
    std::string guest_path;
    std::uint32_t share_mode = 0;
    // ★ 打开时 `dwDesiredAccess` 归一出来的读/写位（kAccessRead / kAccessWrite）。
    //   `access = 0`（只查属性的探针）⇒ 0：既不授予读/写，也不参与共享冲突。
    std::uint32_t access_mask = 0;
    // 下面两位是 access_mask 的缓存（ReadFile / WriteFile 的唯一判据；创建后不再改）
    bool readable = false;
    bool writable = false;
};

struct FindState {
    std::vector<HostDirEntry> entries;
    std::size_t cursor = 0;
};

void destroyFile(void* p) {
    auto* st = static_cast<FileState*>(p);
    if (!st) return;
    if (st->fp) {
        std::fclose(st->fp);  // 折回/关闭都会 flush（CloseHandle 之后重开能读到写进去的字节）
        st->fp = nullptr;
    }
    delete st;
}

void destroyFind(void* p) { delete static_cast<FindState*>(p); }

FileState* fileOf(X86& cpu, std::uint32_t h, const char* api) {
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) cpu.handles->invalidHandle(h, api);  // [[noreturn]]：打印后停
    if (e->kind != HandleKind::File || !e->payload) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 不是文件句柄（kind=%s）⇒ 失败", h,
                handleKindName(e->kind));
        return nullptr;
    }
    return static_cast<FileState*>(e->payload);
}

FindState* findOf(X86& cpu, std::uint32_t h, const char* api) {
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) cpu.handles->invalidHandle(h, api);
    if (e->kind != HandleKind::Find || !e->payload) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 不是查找句柄（kind=%s）⇒ 失败", h,
                handleKindName(e->kind));
        return nullptr;
    }
    return static_cast<FindState*>(e->payload);
}

// 当前大小（不改文件指针）
bool sizeOfFile(const FileState& st, std::uint64_t* size) {
    if (!st.fp) return false;
    const std::int64_t cur = tellFile64(st.fp);
    if (cur < 0) return false;
    if (seekFile64(st.fp, 0, SEEK_END) != 0) return false;
    const std::int64_t end = tellFile64(st.fp);
    if (seekFile64(st.fp, cur, SEEK_SET) != 0) return false;
    if (end < 0) return false;
    *size = static_cast<std::uint64_t>(end);
    return true;
}

// 解析一条 guest 路径；失败时 why 带 vfs 的 tag（调用方用它映射错误码）
bool resolveGuest(X86& cpu, const std::string& guest_path, std::string* host, std::string* why,
                  bool must_exist, bool resolve_case) {
    host->clear();
    why->clear();
    if (guest_path.empty()) {
        *why = "[invalid] 空路径";
        return false;
    }
    return cpu.vfs->resolve(guest_path, host, why, must_exist, resolve_case);
}

// 共享冲突判定（Windows 文档：**两个方向**都要看，且**只由访问位定义**）：
//   ① 新请求的 access 位里有哪一位是**已有句柄**的 share_mode 不允许的；
//   ② 已有句柄的 access 位里有哪一位是**新请求的** share_mode 不允许的。
// `access = 0` 的打开不请求任何访问位 ⇒ 两个方向都不产生冲突（真机里"只查属性"的探针可以并开）。
// ⚠ 只比**本族句柄表**里同宿主路径的活句柄：没有别的进程、也没有真文件锁；DELETE 位不建模。
bool shareViolation(X86& cpu, const std::string& host, std::uint32_t new_access,
                    std::uint32_t new_share, std::uint32_t skip_handle) {
    // 句柄值 = kFirst + kStep*下标 ⇒ 上界由 entries 数量给出（**已关闭**的槽也会被 get 成 nullptr，
    // 所以不能"碰到 nullptr 就 break"）。
    const std::uint32_t end =
        HandleTable::kFirst + HandleTable::kStep * static_cast<std::uint32_t>(cpu.handles->size());
    for (std::uint32_t h = HandleTable::kFirst; h < end; h += HandleTable::kStep) {
        if (h == skip_handle) continue;
        const HandleEntry* e = cpu.handles->get(h);
        if (!e || !e->used) continue;
        if (e->kind != HandleKind::File || !e->payload) continue;
        const auto* other = static_cast<const FileState*>(e->payload);
        if (other->host_path != host) continue;
        // ① 新 access × 已有 share
        if ((new_access & ~other->share_mode & kAccessMask) != 0) return true;
        // ② 已有 access × 新 share
        if ((other->access_mask & ~new_share & kAccessMask) != 0) return true;
    }
    return false;
}

std::string guestFormOf(X86& cpu, const std::string& host, const std::string& fallback) {
    std::string g;
    if (cpu.vfs->toGuest(host, &g) && !g.empty()) return g;
    return fallback;
}

// ---------------------------------------------------------------------------------------
// CreateFileA / CreateFileW
// ---------------------------------------------------------------------------------------
void createFile(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "CreateFileW" : "CreateFileA";
    requireHandles(cpu, api);
    requireVfs(cpu, api);
    const std::string guest = wide ? pathFromWide(call.wstr(0)) : pathFromCp932(call.str(0), api);
    const std::uint32_t access = call.u32(1);
    const std::uint32_t share = call.u32(2);
    const std::uint32_t disposition = call.u32(4);
    const std::uint32_t flags = call.u32(5);
    // arg3 = lpSecurityAttributes / arg6 = hTemplateFile：不建模（偏差 4）

    if (guest.empty()) {
        pathFailure(cpu, api, kErrPathNotFound, guest, "[invalid] 空文件名");
        call.result = kInvalidHandleValue;
        return;
    }
    if (disposition < kCreateNew || disposition > kTruncateExisting) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "dwCreationDisposition=%u 非法 ⇒ 返回 INVALID_HANDLE_VALUE",
                disposition);
        call.result = kInvalidHandleValue;
        return;
    }
    if ((flags & kFileFlagBackupSemantics) != 0) {
        shimLog("kernel32", api,
                "dwFlagsAndAttributes=0x%08X 里的 FILE_FLAG_BACKUP_SEMANTICS 不建模：目录仍然打不开",
                flags);
    }

    // dwDesiredAccess 的完整解码：GENERIC_* 与具体的 FILE_*_DATA 位都要认。
    // ★ 文档："read, write, both or 0 to indicate neither" ⇒ `access = 0` 的句柄**什么都不授予**
    //   （只允许查询元数据），⛔ 不许再"按可读处理"。
    const bool want_append = (access & kFileAppendData) != 0;
    const bool want_read = (access & (kGenericRead | kGenericAll | kFileReadData)) != 0;
    const bool want_write =
        (access & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData)) != 0;
    const std::uint32_t access_mask =
        (want_read ? kAccessRead : 0u) | (want_write ? kAccessWrite : 0u);
    if ((access & kFileDelete) != 0) {
        // 偏差 2：DELETE 访问位不建模（没有别的进程能删文件）
        shimLog("kernel32", api, "dwDesiredAccess=0x%08X 含 DELETE(0x%08X)：不建模（见文件头偏差 2）",
                access, kFileDelete);
    }
    const bool must_exist = (disposition == kOpenExisting || disposition == kTruncateExisting);

    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, must_exist, /*resolve_case=*/true)) {
        // ★ 把 dwCreationDisposition 一起打出来（2026-11-05）：引擎的"探测临时文件"与"真的要建文件"
        //   在日志里长得一模一样（都是 `[notfound] 段 ... 里不存在`），只凭路径判断不出是哪种意图
        //   ⇒ 定位"引擎为什么放弃并 MessageBox+ExitProcess"时会被引到错的方向。
        shimLog("kernel32", api, "%s 解析失败：dwCreationDisposition=%u（%s）", guest.c_str(),
                static_cast<unsigned>(disposition),
                must_exist ? "必须已存在" : "允许不存在（可新建）");
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = kInvalidHandleValue;
        return;
    }

    std::error_code ec;
    const fs::path hp = pathFromUtf8(host);
    const bool exists = fs::exists(hp, ec) && !ec;
    ec.clear();
    const bool is_dir = fs::is_directory(hp, ec) && !ec;
    if (is_dir) {
        // 真机：不带 FILE_FLAG_BACKUP_SEMANTICS 打开目录 ⇒ ERROR_ACCESS_DENIED（偏差 3）
        pathFailure(cpu, api, kErrAccessDenied, guest, "目标是目录，CreateFile 打不开目录");
        call.result = kInvalidHandleValue;
        return;
    }

    bool truncate = false;
    switch (disposition) {
        case kCreateNew:
            if (exists) {
                pathFailure(cpu, api, kErrFileExists, guest, "CREATE_NEW 但文件已存在");
                call.result = kInvalidHandleValue;
                return;
            }
            truncate = true;  // 新建 ⇒ 必然是空文件
            break;
        case kCreateAlways:
            truncate = true;
            break;
        case kOpenExisting:
            if (!exists) {
                pathFailure(cpu, api, kErrFileNotFound, guest, "OPEN_EXISTING 但文件不存在");
                call.result = kInvalidHandleValue;
                return;
            }
            break;
        case kOpenAlways:
            // 已存在时**不**截断（真机 OPEN_ALWAYS 也不截断；指针在 0）；不存在 ⇒ 新建空文件
            truncate = !exists;
            break;
        case kTruncateExisting:
        default:
            if (!exists) {
                pathFailure(cpu, api, kErrFileNotFound, guest, "TRUNCATE_EXISTING 但文件不存在");
                call.result = kInvalidHandleValue;
                return;
            }
            truncate = true;
            break;
    }

    // 文档：`CREATE_ALWAYS` / `OPEN_ALWAYS` 命中**已存在**的文件 ⇒ 函数仍**成功**，但
    // last-error 置 `ERROR_ALREADY_EXISTS(183)`（guest 用 `GetLastError()` 区分"首次运行 / 已有存档"；
    // 本作有 8 处 CREATE_ALWAYS + 1 处 OPEN_ALWAYS）。⛔ 其余 disposition 不置 183。
    const bool already_existed =
        exists && (disposition == kCreateAlways || disposition == kOpenAlways);

    // 文档：`TRUNCATE_EXISTING` 要求 `dwDesiredAccess` 含 `GENERIC_WRITE`；`CREATE_ALWAYS` 命中
    // **已存在**的文件也是"截断"，同样要求写权 ⇒ 只读意图不许把文件清空。
    // ⚠ `<待核>`：`access = 0` × {CREATE_ALWAYS 且文件不存在, CREATE_NEW, OPEN_ALWAYS} 的真机行为
    //   没有采样 ⇒ 这里只在"**真的会截断已有数据**"时拒绝（新建/不存在的分支不拦），
    //   夹具也只钉"宿主内容不变"，不把这个返回码当成真机语义。
    const bool truncates_existing = truncate && exists;
    if (truncates_existing && !want_write) {
        pathFailure(cpu, api, kErrAccessDenied, guest,
                    "要截断已有文件但 dwDesiredAccess 没有写权（需要 GENERIC_WRITE）");
        call.result = kInvalidHandleValue;
        return;
    }

    // 共享冲突：**双向**判定，且 `access = 0` 不参与（见 shareViolation 的说明与偏差 2）
    if (shareViolation(cpu, host, access_mask, share, 0)) {
        pathFailure(cpu, api, kErrSharingViolation, guest,
                    "新请求的访问位/共享位与同一路径上已打开句柄的共享模式冲突（双向判定）");
        call.result = kInvalidHandleValue;
        return;
    }

    // ★ 宿主 `FILE*` **一律用可读模式**打开：`GetFileSize` / `GetFileTime` / `SetFilePointer(Ex)`
    //   都要靠它 seek/tell；guest 可见的读/写权限由 `access_mask` 卡（见 readFile / writeFile）。
    const char* mode = nullptr;
    if (want_append) {
        mode = "a+b";
    } else if (truncate) {
        mode = "w+b";
    } else if (want_write) {
        mode = "r+b";
    } else {
        mode = "rb";  // 只查属性（access=0）或只读：宿主侧可读即可
    }
    std::FILE* fp = openHostFile(host, mode);
    if (!fp) {
        const int e = errno;
        pathFailure(cpu, api, errnoToWin32(e), guest,
                    std::string("宿主打开失败（") + mode + "，errno=" + std::to_string(e) + "）");
        call.result = kInvalidHandleValue;
        return;
    }

    auto* st = new FileState{};
    st->fp = fp;
    st->host_path = host;
    st->guest_path = guestFormOf(cpu, host, guest);
    st->share_mode = share;
    st->access_mask = access_mask;
    st->readable = want_read;  // ⛔ 不再有 "|| (!want_write)"：access=0 不授予读
    st->writable = want_write;

    const std::uint32_t h = cpu.handles->add(HandleKind::File, st->guest_path.c_str(), st);
    HandleEntry* entry = cpu.handles->get(h);
    if (!entry) {
        std::fclose(fp);
        delete st;
        shimAbort("kernel32", api, "句柄表拒绝分配文件句柄（add 后立刻取不到 0x%08X）", h);
    }
    entry->destroy = &destroyFile;
    call.result = h;
    // ★ 成功路径上**唯一**写 last_error 的地方：文档明写这两个 disposition 命中已存在文件时置 183。
    //   其余情况**不碰** `last_error`（Win32 成功时也不清零，guest 见到的仍是上一次的遗留值）。
    if (already_existed) cpu.shim.last_error = kErrAlreadyExists;
    if (cpu.shims && cpu.shims->trace()) {
        // trace 已由 shim 框架打；这里只补一句"落到了哪个宿主文件"（确定性：宿主路径在 trace 里有意义）
        shimLog("kernel32", api, "0x%08X ⇒ \"%s\"（mode=%s）", h, host.c_str(), mode);
    }
}

void shimCreateFileA(X86& cpu, ShimCall& call) { createFile(cpu, call, false); }
void shimCreateFileW(X86& cpu, ShimCall& call) { createFile(cpu, call, true); }

// ---------------------------------------------------------------------------------------
// ReadFile / WriteFile / FlushFileBuffers
// ---------------------------------------------------------------------------------------
void readFile(X86& cpu, ShimCall& call) {
    const char* api = "ReadFile";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t n = call.u32(2);
    const std::uint32_t out_ea = call.u32(3);
    const std::uint32_t overlapped = call.u32(4);
    if (overlapped != 0) {
        // 真机：非重叠句柄上 lpOverlapped 被忽略、位置仍取当前文件指针（本模拟器只开非重叠句柄）
        shimLog("kernel32", api,
                "lpOverlapped=0x%08X 非 NULL ⇒ 按非重叠句柄处理（位置仍取当前文件指针）", overlapped);
    }
    if (n == 0) {
        if (out_ea != 0) {
            requireCommitted(cpu, out_ea, 4, api, "lpNumberOfBytesRead");
            cpu.mem->wr32(out_ea, 0);
        }
        call.result = 1;  // 真机：0 字节读直接成功
        return;
    }
    if (!st->readable) {
        cpu.shim.last_error = kErrAccessDenied;
        shimLog("kernel32", api,
                "句柄 0x%08X 的访问位 0x%08X 不含读位（access=0 的只查属性句柄也不许读）⇒ 失败",
                call.u32(0), st->access_mask);
        call.result = 0;
        return;
    }
    requireCommitted(cpu, buf, n, api, "lpBuffer");
    if (out_ea != 0) requireCommitted(cpu, out_ea, 4, api, "lpNumberOfBytesRead");

    // C 标准：输出 → 输入之间必须有一个定位函数（否则是未定义行为）
    if (seekFile64(st->fp, 0, SEEK_CUR) != 0) {
        cpu.shim.last_error = kErrReadFault;
        shimLog("kernel32", api, "宿主定位失败（文件 0x%08X）⇒ 失败", call.u32(0));
        call.result = 0;
        return;
    }
    std::vector<std::uint8_t> tmp(n, 0);
    const std::size_t got = std::fread(tmp.data(), 1, n, st->fp);
    if (got == 0 && std::ferror(st->fp)) {
        cpu.shim.last_error = kErrReadFault;
        shimLog("kernel32", api, "宿主读失败（%s）⇒ 失败", errnoText(errno).c_str());
        call.result = 0;
        return;
    }
    if (got > 0) cpu.mem->writeBytes(buf, tmp.data(), got);
    if (out_ea != 0) cpu.mem->wr32(out_ea, static_cast<std::uint32_t>(got));
    call.result = 1;  // EOF 也是 TRUE + 0 字节
}

void writeFile(X86& cpu, ShimCall& call) {
    const char* api = "WriteFile";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t n = call.u32(2);
    const std::uint32_t out_ea = call.u32(3);
    const std::uint32_t overlapped = call.u32(4);
    if (overlapped != 0) {
        shimLog("kernel32", api,
                "lpOverlapped=0x%08X 非 NULL ⇒ 按非重叠句柄处理（位置仍取当前文件指针）", overlapped);
    }
    if (n == 0) {
        if (out_ea != 0) {
            requireCommitted(cpu, out_ea, 4, api, "lpNumberOfBytesWritten");
            cpu.mem->wr32(out_ea, 0);
        }
        call.result = 1;
        return;
    }
    if (!st->writable) {
        cpu.shim.last_error = kErrAccessDenied;
        shimLog("kernel32", api,
                "句柄 0x%08X 的访问位 0x%08X 不含写位（access=0 的只查属性句柄也不许写）⇒ 失败",
                call.u32(0), st->access_mask);
        call.result = 0;
        return;
    }
    requireCommitted(cpu, buf, n, api, "lpBuffer");
    if (out_ea != 0) requireCommitted(cpu, out_ea, 4, api, "lpNumberOfBytesWritten");

    if (seekFile64(st->fp, 0, SEEK_CUR) != 0) {  // 输入 → 输出之间的定位函数
        cpu.shim.last_error = kErrWriteFault;
        shimLog("kernel32", api, "宿主定位失败（文件 0x%08X）⇒ 失败", call.u32(0));
        call.result = 0;
        return;
    }
    std::vector<std::uint8_t> tmp(n, 0);
    cpu.mem->readBytes(buf, tmp.data(), n);
    const std::size_t put = std::fwrite(tmp.data(), 1, n, st->fp);
    if (put != n) {
        cpu.shim.last_error = kErrWriteFault;
        shimLog("kernel32", api, "宿主写失败（写入 %u/%u 字节，%s）⇒ 失败",
                static_cast<unsigned>(put), static_cast<unsigned>(n), errnoText(errno).c_str());
        call.result = 0;
        return;
    }
    if (std::fflush(st->fp) != 0) {
        cpu.shim.last_error = kErrWriteFault;
        shimLog("kernel32", api, "宿主 flush 失败（%s）⇒ 失败", errnoText(errno).c_str());
        call.result = 0;
        return;
    }
    if (out_ea != 0) cpu.mem->wr32(out_ea, n);
    call.result = 1;
}

void shimFlushFileBuffers(X86& cpu, ShimCall& call) {
    const char* api = "FlushFileBuffers";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    if (std::fflush(st->fp) != 0) {
        cpu.shim.last_error = kErrWriteFault;
        shimLog("kernel32", api, "宿主 flush 失败（%s）⇒ 失败", errnoText(errno).c_str());
        call.result = 0;
        return;
    }
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// SetFilePointer / SetFilePointerEx / GetFileSize
// ---------------------------------------------------------------------------------------
// 自己算"基准 + distance"（⛔ 不靠宿主 `fseeko` 的 `errno == EINVAL` 判定负位置：
// POSIX 的 EINVAL 只在部分实现上覆盖"结果为负"，Windows 的 `_fseeki64` 没有承诺）。
//   返回 false ⇒ `why` 是给日志的理由，失败码由 `error` 带回（131 = 负位置，87 = 其它）。
// ★ 失败时**不调用宿主 seek** ⇒ 文件指针一定不动（与 `SetFilePointer` 文档的"not moved"一致）。
bool seekTarget(const FileState& st, std::int64_t distance, std::uint32_t method,
                std::int64_t* target, std::uint32_t* error, const char** why) {
    std::int64_t base = 0;
    if (method == kFileBegin) {
        base = 0;  // 文档把 liDistanceToMove 当 unsigned；本实现按二补码还原（与真机实现一致）
    } else if (method == kFileCurrent) {
        base = tellFile64(st.fp);
        if (base < 0) {
            *error = kErrInvalidParameter;
            *why = "宿主 tell 失败";
            return false;
        }
    } else if (method == kFileEnd) {
        std::uint64_t size = 0;
        if (!sizeOfFile(st, &size)) {
            *error = kErrInvalidParameter;
            *why = "宿主取文件大小失败（FILE_END 的基准拿不到）";
            return false;
        }
        base = static_cast<std::int64_t>(size);
    } else {
        *error = kErrInvalidParameter;
        *why = "dwMoveMethod 非法";
        return false;
    }
    // 溢出守卫：正负两个方向都先判，避免 `base + distance` 有符号溢出（UB）
    if (distance > 0 && base > INT64_MAX - distance) {
        *error = kErrInvalidParameter;
        *why = "目标位置溢出 64 位";
        return false;
    }
    if (distance < 0 && base < INT64_MIN - distance) {
        *error = kErrInvalidParameter;
        *why = "目标位置溢出 64 位";
        return false;
    }
    const std::int64_t want = base + distance;
    if (want < 0) {
        // 文档（SetFilePointer）："If a new file pointer is a negative value, the function fails,
        // the file pointer is not moved" ⇒ ERROR_NEGATIVE_SEEK(131)
        *error = kErrNegativeSeek;
        *why = "目标位置为负";
        return false;
    }
    *target = want;
    return true;
}

// 文档（SetFilePointer / SetFilePointerEx）：hFile 必须是带 `GENERIC_READ` 或 `GENERIC_WRITE` 的句柄。
// `access = 0` 的"只查属性"句柄不授予任何访问位 ⇒ 不许定位（FALSE + ERROR_ACCESS_DENIED(5)）。
bool seekAccessDenied(X86& cpu, const FileState* st, std::uint32_t h, const char* api) {
    if (st->access_mask != 0) return false;
    cpu.shim.last_error = kErrAccessDenied;
    shimLog("kernel32", api, "句柄 0x%08X 的访问位为 0（只查属性）⇒ 不许定位（错误 5）", h);
    return true;
}

void shimSetFilePointer(X86& cpu, ShimCall& call) {
    const char* api = "SetFilePointer";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = kInvalidSetFilePointer;
        return;
    }
    if (seekAccessDenied(cpu, st, call.u32(0), api)) {
        call.result = kInvalidSetFilePointer;
        return;
    }
    const std::uint32_t lo = call.u32(1);
    const std::uint32_t hi_ea = call.u32(2);
    const std::uint32_t method = call.u32(3);
    if (method > kFileEnd) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "dwMoveMethod=%u 非法（只支持 0/1/2）⇒ 返回 0xFFFFFFFF", method);
        call.result = kInvalidSetFilePointer;
        return;
    }
    std::int64_t distance = 0;
    if (hi_ea == 0) {
        // 没有高 32 位：只有 32 位、**按有符号**解释（真机：> 2 GiB 的偏移必须给 lpDistanceToMoveHigh）
        distance = static_cast<std::int64_t>(static_cast<std::int32_t>(lo));
    } else {
        requireCommitted(cpu, hi_ea, 4, api, "lpDistanceToMoveHigh");
        const std::uint32_t hi = cpu.mem->rd32(hi_ea);
        distance = static_cast<std::int64_t>((static_cast<std::uint64_t>(hi) << 32) |
                                            static_cast<std::uint64_t>(lo));
    }
    std::int64_t pos = 0;
    std::uint32_t err = kErrInvalidParameter;
    const char* why = "";
    if (!seekTarget(*st, distance, method, &pos, &err, &why)) {
        cpu.shim.last_error = err;
        shimLog("kernel32", api, "偏移 %lld（method=%u）不可用：%s ⇒ 返回 0xFFFFFFFF（指针不动）",
                static_cast<long long>(distance), method, why);
        call.result = kInvalidSetFilePointer;
        return;
    }
    // 目标位置已由自己算好且非负：宿主 seek 只负责落位（失败仍按 errno 兜底，见下）
    errno = 0;
    if (seekFile64(st->fp, pos, SEEK_SET) != 0) {
        const int e = errno;
        cpu.shim.last_error = (e == EINVAL) ? kErrNegativeSeek : kErrInvalidParameter;
        shimLog("kernel32", api,
                "宿主定位失败（目标 %lld，method=%u，errno=%d）⇒ 返回 0xFFFFFFFF（宿主兜底判定）",
                static_cast<long long>(pos), method, e);
        call.result = kInvalidSetFilePointer;
        return;
    }
    if (hi_ea != 0) {
        cpu.mem->wr32(hi_ea, static_cast<std::uint32_t>(static_cast<std::uint64_t>(pos) >> 32));
    }
    call.result = static_cast<std::uint32_t>(static_cast<std::uint64_t>(pos) & 0xFFFFFFFFull);
}

// ★ argc = 5（偏差 1）：x86 stdcall 下 LARGE_INTEGER 占两个栈槽，真机是 `SetFilePointerEx@20`。
//   栈布局：[0] hFile [1] li.lo [2] li.hi [3] lpNewFilePointer [4] dwMoveMethod
// ★ 文档要求 hFile 带 `GENERIC_READ` 或 `GENERIC_WRITE`：`access = 0` 的只查属性句柄 ⇒
//   FALSE + ERROR_ACCESS_DENIED(5)（与 `SetFilePointer` 共用 `seekAccessDenied`）。
void shimSetFilePointerEx(X86& cpu, ShimCall& call) {
    const char* api = "SetFilePointerEx";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    if (seekAccessDenied(cpu, st, call.u32(0), api)) {
        call.result = 0;
        return;
    }
    const std::uint32_t lo = call.u32(1);
    const std::uint32_t hi = call.u32(2);
    const std::uint32_t new_ptr = call.u32(3);
    const std::uint32_t method = call.u32(4);
    if (method > kFileEnd) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "dwMoveMethod=%u 非法（只支持 0/1/2）⇒ 返回 FALSE", method);
        call.result = 0;
        return;
    }
    if (new_ptr != 0) requireCommitted(cpu, new_ptr, 8, api, "lpNewFilePointer");
    const auto distance = static_cast<std::int64_t>((static_cast<std::uint64_t>(hi) << 32) |
                                                     static_cast<std::uint64_t>(lo));
    std::int64_t pos = 0;
    std::uint32_t err = kErrInvalidParameter;
    const char* why = "";
    if (!seekTarget(*st, distance, method, &pos, &err, &why)) {
        cpu.shim.last_error = err;
        shimLog("kernel32", api, "偏移 %lld（method=%u）不可用：%s ⇒ 返回 FALSE（指针不动）",
                static_cast<long long>(distance), method, why);
        call.result = 0;
        return;
    }
    errno = 0;
    if (seekFile64(st->fp, pos, SEEK_SET) != 0) {
        const int e = errno;
        cpu.shim.last_error = (e == EINVAL) ? kErrNegativeSeek : kErrInvalidParameter;
        shimLog("kernel32", api,
                "宿主定位失败（目标 %lld，method=%u，errno=%d）⇒ 返回 FALSE（宿主兜底判定）",
                static_cast<long long>(pos), method, e);
        call.result = 0;
        return;
    }
    if (new_ptr != 0) {
        const auto u = static_cast<std::uint64_t>(pos);
        cpu.mem->wr32(new_ptr, static_cast<std::uint32_t>(u & 0xFFFFFFFFull));
        cpu.mem->wr32(new_ptr + 4u, static_cast<std::uint32_t>(u >> 32));
    }
    call.result = 1;
}

void shimGetFileSize(X86& cpu, ShimCall& call) {
    const char* api = "GetFileSize";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = kInvalidHandleValue;
        return;
    }
    const std::uint32_t hi_ea = call.u32(1);
    if (hi_ea != 0) requireCommitted(cpu, hi_ea, 4, api, "lpFileSizeHigh");
    std::uint64_t size = 0;
    if (!sizeOfFile(*st, &size)) {
        cpu.shim.last_error = kErrAccessDenied;
        shimLog("kernel32", api, "宿主取大小失败（文件 0x%08X）⇒ 返回 0xFFFFFFFF", call.u32(0));
        call.result = kInvalidHandleValue;
        return;
    }
    if (hi_ea != 0) cpu.mem->wr32(hi_ea, static_cast<std::uint32_t>(size >> 32));
    call.result = static_cast<std::uint32_t>(size & 0xFFFFFFFFull);
}

// ---------------------------------------------------------------------------------------
// 属性 / 删除 / 改名 / 复制 / 目录
// ---------------------------------------------------------------------------------------
void shimGetFileAttributesA(X86& cpu, ShimCall& call) {
    const char* api = "GetFileAttributesA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = kInvalidHandleValue;
        return;
    }
    std::error_code ec;
    const fs::file_status st = fs::status(pathFromUtf8(host), ec);
    if (ec || !fs::exists(st)) {
        pathFailure(cpu, api, kErrFileNotFound, guest, "[notfound] 宿主查状态失败或不存在");
        call.result = kInvalidHandleValue;
        return;
    }
    call.result = attributesOfStatus(st);
}

void shimSetFileAttributesA(X86& cpu, ShimCall& call) {
    const char* api = "SetFileAttributesA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    const std::uint32_t attrs = call.u32(1);
    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = 0;
        return;
    }
    const fs::path hp = pathFromUtf8(host);
    std::error_code ec;
    fs::file_status st = fs::status(hp, ec);
    if (ec || !fs::exists(st)) {
        pathFailure(cpu, api, kErrFileNotFound, guest, "[notfound] 宿主查状态失败或不存在");
        call.result = 0;
        return;
    }
    // 偏差 5：只模拟 READONLY（宿主写权限位），其余属性位不落盘
    const std::uint32_t modeled = kAttrReadOnly | kAttrArchive | kAttrNormal | kAttrDirectory;
    if ((attrs & ~modeled) != 0u) {
        shimLog("kernel32", api,
                "属性位 0x%08X 里的非 READONLY 部分不落盘（本模拟器只模拟只读位）",
                attrs & ~modeled);
    }
    fs::perms perms = st.permissions();
    if ((attrs & kAttrReadOnly) != 0u) {
        perms &= ~(fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write);
    } else {
        perms |= fs::perms::owner_write;
    }
    ec.clear();
    fs::permissions(hp, perms, ec);
    if (ec) {
        pathFailure(cpu, api, kErrAccessDenied, guest, "[io] 宿主改权限失败：" + ec.message());
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimDeleteFileA(X86& cpu, ShimCall& call) {
    const char* api = "DeleteFileA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = 0;
        return;
    }
    const fs::path hp = pathFromUtf8(host);
    std::error_code ec;
    if (fs::is_directory(hp, ec) && !ec) {
        pathFailure(cpu, api, kErrAccessDenied, guest, "目标是目录（DeleteFile 只删文件）");
        call.result = 0;
        return;
    }
    ec.clear();
    if (!fs::remove(hp, ec) || ec) {
        const std::uint32_t code = ec ? kErrAccessDenied : kErrFileNotFound;
        const std::string detail =
            ec ? ("[io] 宿主删除失败：" + ec.message()) : "[notfound] 什么都没删掉";
        pathFailure(cpu, api, code, guest, detail);
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimMoveFileA(X86& cpu, ShimCall& call) {
    const char* api = "MoveFileA";
    requireVfs(cpu, api);
    const std::string src_guest = pathFromCp932(call.str(0), api);
    const std::string dst_guest = pathFromCp932(call.str(1), api);
    std::string src;
    std::string dst;
    std::string why;
    if (!resolveGuest(cpu, src_guest, &src, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), src_guest, why);
        call.result = 0;
        return;
    }
    // 目标：可以不存在（不存在时 vfs 保留调用方拼写 = 创建语义）；已存在时按大小写不敏感命中
    if (!resolveGuest(cpu, dst_guest, &dst, &why, /*must_exist=*/false, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), dst_guest, why);
        call.result = 0;
        return;
    }
    std::error_code ec;
    if (fs::exists(pathFromUtf8(dst), ec) && !ec) {
        pathFailure(cpu, api, kErrAlreadyExists, dst_guest, "目标已存在（MoveFile 不覆盖）");
        call.result = 0;
        return;
    }
    ec.clear();
    fs::rename(pathFromUtf8(src), pathFromUtf8(dst), ec);
    if (ec) {
        pathFailure(cpu, api, kErrAccessDenied, src_guest, "[io] 宿主改名失败：" + ec.message());
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimCopyFileA(X86& cpu, ShimCall& call) {
    const char* api = "CopyFileA";
    requireVfs(cpu, api);
    const std::string src_guest = pathFromCp932(call.str(0), api);
    const std::string dst_guest = pathFromCp932(call.str(1), api);
    const bool fail_if_exists = call.u32(2) != 0;
    std::string src;
    std::string dst;
    std::string why;
    if (!resolveGuest(cpu, src_guest, &src, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), src_guest, why);
        call.result = 0;
        return;
    }
    // 目标：可以不存在（不存在时 vfs 保留调用方拼写 = 创建语义）；已存在时按大小写不敏感命中
    if (!resolveGuest(cpu, dst_guest, &dst, &why, /*must_exist=*/false, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), dst_guest, why);
        call.result = 0;
        return;
    }
    if (!cpu.vfs->isFile(src_guest)) {
        pathFailure(cpu, api, kErrFileNotFound, src_guest, "[notfound] 源不是文件");
        call.result = 0;
        return;
    }
    std::error_code ec;
    const fs::copy_options opt =
        fail_if_exists ? fs::copy_options::none : fs::copy_options::overwrite_existing;
    fs::copy_file(pathFromUtf8(src), pathFromUtf8(dst), opt, ec);
    if (ec) {
        const bool existed = fs::exists(pathFromUtf8(dst), ec) && !ec;
        const std::uint32_t code = (fail_if_exists && existed) ? kErrFileExists : kErrAccessDenied;
        pathFailure(cpu, api, code, src_guest, "[io] 宿主复制失败：" + ec.message());
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimCreateDirectoryA(X86& cpu, ShimCall& call) {
    const char* api = "CreateDirectoryA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    std::string host;
    std::string why;
    // 目录名：可以不存在（不存在时 vfs 保留调用方拼写 = 创建语义）；已存在时按大小写不敏感命中
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/false, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = 0;
        return;
    }
    const fs::path hp = pathFromUtf8(host);
    std::error_code ec;
    if (fs::exists(hp, ec) && !ec) {
        pathFailure(cpu, api, kErrAlreadyExists, guest, "目标已存在（CreateDirectory 不覆盖）");
        call.result = 0;
        return;
    }
    ec.clear();
    if (!fs::create_directory(hp, ec) || ec) {
        pathFailure(cpu, api, kErrPathNotFound, guest,
                    ec ? ("[io] 宿主建目录失败：" + ec.message()) : "[notdir] 父目录不存在");
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimRemoveDirectoryA(X86& cpu, ShimCall& call) {
    const char* api = "RemoveDirectoryA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = 0;
        return;
    }
    const fs::path hp = pathFromUtf8(host);
    std::error_code ec;
    if (!fs::is_directory(hp, ec) || ec) {
        pathFailure(cpu, api, kErrDirectory, guest, "[notdir] 不是目录");
        call.result = 0;
        return;
    }
    ec.clear();
    if (!fs::is_empty(hp, ec) || ec) {
        pathFailure(cpu, api, kErrDirNotEmpty, guest, "[io] 目录非空（RemoveDirectory 只删空目录）");
        call.result = 0;
        return;
    }
    ec.clear();
    if (!fs::remove(hp, ec) || ec) {
        pathFailure(cpu, api, kErrAccessDenied, guest, "[io] 宿主删目录失败");
        call.result = 0;
        return;
    }
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// 当前目录
// ---------------------------------------------------------------------------------------
void getCurrentDirectory(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "GetCurrentDirectoryW" : "GetCurrentDirectoryA";
    requireMem(cpu, api);
    requireVfs(cpu, api);
    const std::uint32_t cap = call.u32(0);
    const std::uint32_t buf = call.u32(1);
    // 缺省 cwd = guestRoot（vfs.h 的合同）⇒ 这里读 vfs 的 getter，空就退回根
    const std::string guest =
        cpu.vfs->guestCwd().empty() ? cpu.vfs->guestRoot() : cpu.vfs->guestCwd();

    if (wide) {
        const std::u16string w = utf8ToUtf16(guest, nullptr);
        const std::uint32_t need = static_cast<std::uint32_t>(w.size()) + 1u;
        if (cap < need) {
            cpu.shim.last_error = kErrInsufficientBuffer;
            shimLog("kernel32", api, "缓冲区 %u 个字符不够（需要 %u）⇒ 返回所需长度 + 错误 122", cap,
                    need);
            call.result = need;
            return;
        }
        requireCommitted(cpu, buf, need * 2u, api, "lpBuffer");
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(w.size()); ++i) {
            cpu.mem->wr16(buf + i * 2u, static_cast<std::uint16_t>(w[i]));
        }
        cpu.mem->wr16(buf + static_cast<std::uint32_t>(w.size()) * 2u, 0);
        call.result = static_cast<std::uint32_t>(w.size());
        return;
    }

    std::size_t bad = 0;
    const std::string bytes = utf8ToCp932(guest, &bad);
    if (bad != 0) {
        shimLog("kernel32", api, "当前目录 \"%s\" 里有 %u 个字符不能编成 CP932（已写 '?'）",
                guest.c_str(), static_cast<unsigned>(bad));
    }
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size()) + 1u;
    if (cap < need) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", api, "缓冲区 %u 字节不够（需要 %u）⇒ 返回所需长度 + 错误 122", cap, need);
        call.result = need;
        return;
    }
    writeAnsiBuffer(cpu, buf, cap, bytes, api, "lpBuffer");
    call.result = static_cast<std::uint32_t>(bytes.size());
}

void shimGetCurrentDirectoryA(X86& cpu, ShimCall& call) { getCurrentDirectory(cpu, call, false); }
void shimGetCurrentDirectoryW(X86& cpu, ShimCall& call) { getCurrentDirectory(cpu, call, true); }

void shimSetCurrentDirectoryA(X86& cpu, ShimCall& call) {
    const char* api = "SetCurrentDirectoryA";
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    std::string host;
    std::string why;
    if (!resolveGuest(cpu, guest, &host, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = 0;
        return;
    }
    std::error_code ec;
    if (!fs::is_directory(pathFromUtf8(host), ec) || ec) {
        pathFailure(cpu, api, kErrPathNotFound, guest, "[notdir] 目标不是目录");
        call.result = 0;
        return;
    }
    cpu.vfs->setGuestCwd(cpu.vfs->absoluteGuest(guest));
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// 临时目录 / 临时文件名
// ---------------------------------------------------------------------------------------
// ★ 2026-11-05：临时目录 = guest 的 `C:\Users\Amayui\AppData\Local\Temp`（与 `TEMP` / `TMP`
//   环境变量、`GetTempPathA` **同源**，见 vfs.h），宿主落在 `<--appdata>/Local/Temp`。
//   ⛔ 不再退回 `--game-dir/.lift-tmp`：那是对**安装树**的写（验收红线）。
//   没有 `--appdata` ⇒ 响亮失败（没有 TEMP 就没有临时目录；⛔ 不指向宿主真实 Temp）。
bool tempDirHost(X86& cpu, const char* api, std::string* host, std::string* guest) {
    if (!cpu.vfs || cpu.vfs->appDataDir().empty()) {
        shimLog("kernel32", api, "没有 --appdata ⇒ 没有临时目录（⛔ 不写 --game-dir，也不指向宿主 Temp）");
        return false;
    }
    const std::string guest_tmp = lift::guestTempDirText();
    std::string why;
    // 首次使用时把 `<--appdata>/Local/Temp` 建出来（`resolve` 只解析、不建目录）
    if (!cpu.vfs->makeSureDirectoryPathExists(guest_tmp + "\\", &why)) {
        shimLog("kernel32", api, "临时目录 \"%s\" 建不出来：%s", guest_tmp.c_str(), why.c_str());
        return false;
    }
    std::string h;
    if (!cpu.vfs->resolve(guest_tmp, &h, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
        shimLog("kernel32", api, "临时目录 \"%s\" 解析失败：%s", guest_tmp.c_str(), why.c_str());
        return false;
    }
    *host = h;
    *guest = guest_tmp;
    return true;
}

void shimGetTempPathA(X86& cpu, ShimCall& call) {
    const char* api = "GetTempPathA";
    requireMem(cpu, api);
    requireVfs(cpu, api);
    const std::uint32_t cap = call.u32(0);
    const std::uint32_t buf = call.u32(1);
    std::string host;
    std::string guest;
    if (!tempDirHost(cpu, api, &host, &guest)) {
        cpu.shim.last_error = kErrPathNotFound;
        call.result = 0;
        return;
    }
    std::size_t bad = 0;
    const std::string bytes = utf8ToCp932(guest + "\\", &bad);
    if (bad != 0) {
        shimLog("kernel32", api, "临时目录路径里有 %u 个字符不能编成 CP932（已写 '?'）",
                static_cast<unsigned>(bad));
    }
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size()) + 1u;
    if (buf == 0) {
        // 两个参数分开看：`nBufferLength == 0` 是文档认可的"问大小"形态（返回值 > nBufferLength ⇒
        // 所需长度），`nBufferLength != 0` 而 `lpBuffer == NULL` 在真机上是写空指针（AV）；
        // 本模拟器按宿主安全口径改成 0 + ERROR_INVALID_PARAMETER（与 GetFullPathNameA 一致）。
        if (cap == 0) {
            call.result = need;
            return;
        }
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpBuffer 为 NULL 但 nBufferLength=%u ≠ 0 ⇒ 返回 0 + 错误 87", cap);
        call.result = 0;
        return;
    }
    if (cap < need) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", api, "缓冲区 %u 字节不够（需要 %u）⇒ 返回所需长度 + 错误 122", cap, need);
        call.result = need;
        return;
    }
    writeAnsiBuffer(cpu, buf, cap, bytes, api, "lpBuffer");
    call.result = static_cast<std::uint32_t>(bytes.size());  // 真机：长度**不含**结尾 0
}

void shimGetTempFileNameA(X86& cpu, ShimCall& call) {
    const char* api = "GetTempFileNameA";
    requireMem(cpu, api);
    requireVfs(cpu, api);
    const std::uint32_t dir_ea = call.u32(0);
    const std::string prefix = pathFromCp932(call.str(1), api);
    // ★ uUnique 是**入参语义**（不是"忽略"）：非 0 ⇒ 用调用方的值、不创建、返回原值。
    //   uUnique == 0 才有"从空号里找一个"的行为（起点 0 是本模拟器的**有意偏差**，见文件头偏差 8）。
    const std::uint32_t u_unique = call.u32(2);
    const std::uint32_t out_ea = call.u32(3);
    requireCommitted(cpu, out_ea, kFdFileNameChars, api, "lpTempFileName");

    // lpPathName 能用就用（通常是 GetTempPathA 的返回值）；不能就退回 .lift-tmp
    std::string host_dir;
    std::string guest_dir;
    std::string why;
    bool used_arg = false;
    const std::string dir_arg = call.str(0);
    if (dir_ea != 0) {
        const std::string dir_guest = pathFromCp932(dir_arg, api);
        if (!dir_guest.empty() &&
            resolveGuest(cpu, dir_guest, &host_dir, &why, /*must_exist=*/true, /*resolve_case=*/true)) {
            std::error_code ec;
            if (fs::is_directory(pathFromUtf8(host_dir), ec) && !ec) {
                used_arg = true;
                cpu.vfs->toGuest(host_dir, &guest_dir);
            } else {
                why = "[notdir] 不是目录";
            }
        } else if (why.empty()) {
            why = "[invalid] 空路径";
        }
        if (!used_arg) {
            // ★ 日志写**转码后**的文本（`dir_guest`），⛔ 不把 guest 的 CP932 字节直接塞进宿主日志：
            //   日志流是 UTF-8，塞 CP932 字节会打出乱码（2026-11-05 缺陷单的同一根因）。
            shimLog("kernel32", api, "lpPathName \"%s\" 不可用（%s）⇒ 退回 TEMP（<--appdata>/Local/Temp）",
                    dir_guest.empty() ? "<空>" : dir_guest.c_str(), why.c_str());
        }
    }
    if (!used_arg && !tempDirHost(cpu, api, &host_dir, &guest_dir)) {
        cpu.shim.last_error = kErrPathNotFound;
        call.result = 0;
        return;
    }
    if (guest_dir.empty() && !cpu.vfs->toGuest(host_dir, &guest_dir)) guest_dir = cpu.vfs->guestRoot();

    // 文档：`<pre>` 只取 lpPrefixString 的**前 3 个字符**；空前缀给 "tmp"
    const std::string safe_prefix = (prefix.empty() ? std::string("tmp") : prefix).substr(0, 3);

    // 把 `<guest_dir>\<name>` 写进 lpTempFileName（CP932，超过 MAX_PATH 留日志）
    const auto writeTempName = [&](const std::string& name) {
        const std::string guest_path = guest_dir + (guest_dir.empty() ? "" : "\\") + name;
        std::size_t bad = 0;
        const std::string bytes = utf8ToCp932(guest_path, &bad);
        if (bad != 0) {
            shimLog("kernel32", api, "临时文件名里有 %u 个字符不能编成 CP932（已写 '?'）",
                    static_cast<unsigned>(bad));
        }
        if (bytes.size() >= kFdFileNameChars) {
            shimLog("kernel32", api, "临时文件全路径 %u 字节超过 MAX_PATH(260) ⇒ 截断",
                    static_cast<unsigned>(bytes.size()));
        }
        writeAnsiBuffer(cpu, out_ea, kFdFileNameChars, bytes, api, "lpTempFileName");
    };

    if (u_unique != 0) {
        // 文档：uUnique != 0 ⇒ **只生成名字、不创建文件**（"GetTempFileName is not able to
        // guarantee that the file name is unique"），返回 uUnique 本身；只有低 16 位进名字。
        char num[8] = {};
        std::snprintf(num, sizeof(num), "%04X", static_cast<unsigned>(u_unique & 0xFFFFu));
        const std::string name = safe_prefix + num + ".TMP";
        writeTempName(name);
        shimLog("kernel32", api,
                "uUnique=0x%08X != 0 ⇒ 名字 \"%s\"（不创建文件）、返回原值（低 16 位进名字）",
                u_unique, name.c_str());
        call.result = u_unique;
        return;
    }

    // uUnique == 0：从 0 起找第一个空号（**有意偏差**：真机从系统时间起），并创建那个空文件
    for (std::uint32_t i = 0; i <= 0xFFFFu; ++i) {
        char num[8] = {};
        std::snprintf(num, sizeof(num), "%04X", static_cast<unsigned>(i));
        const std::string name = safe_prefix + num + ".TMP";
        const fs::path hp = pathFromUtf8(host_dir) / pathFromUtf8(name);
        std::error_code ec;
        if (fs::exists(hp, ec) && !ec) continue;
        // 真机 GetTempFileName 在 uUnique==0 时**创建**这个空文件
        std::FILE* fp = openHostFile(pathToUtf8(hp), "wb");
        if (!fp) {
            cpu.shim.last_error = errnoToWin32(errno);
            shimLog("kernel32", api, "创建 \"%s\" 失败（errno=%d）⇒ 返回 0", name.c_str(), errno);
            call.result = 0;
            return;
        }
        std::fclose(fp);
        writeTempName(name);
        call.result = i;
        return;
    }
    cpu.shim.last_error = kErrFileExists;
    shimLog("kernel32", api, "0x10000 个候选名全被占用 ⇒ 返回 0");
    call.result = 0;
}

// ---------------------------------------------------------------------------------------
// 卷 / 盘
// ---------------------------------------------------------------------------------------
void shimGetVolumeInformationA(X86& cpu, ShimCall& call) {
    const char* api = "GetVolumeInformationA";
    requireMem(cpu, api);
    requireVfs(cpu, api);
    // 偏差 6：全是固定值
    shimLog("kernel32", api,
            "卷信息是固定值（卷名 \"AGE\"、序列号 0x%08X、文件系统 \"NTFS\"、大小写不敏感）",
            kFixedVolumeSerial);

    const std::uint32_t name_buf = call.u32(1);
    const std::uint32_t name_size = call.u32(2);
    if (name_buf != 0 && name_size != 0) {
        writeAnsiBuffer(cpu, name_buf, name_size, "AGE", api, "lpVolumeNameBuffer");
    }
    const std::uint32_t serial_ea = call.u32(3);
    if (serial_ea != 0) {
        requireCommitted(cpu, serial_ea, 4, api, "lpVolumeSerialNumber");
        cpu.mem->wr32(serial_ea, kFixedVolumeSerial);
    }
    const std::uint32_t max_comp_ea = call.u32(4);
    if (max_comp_ea != 0) {
        requireCommitted(cpu, max_comp_ea, 4, api, "lpMaximumComponentLength");
        cpu.mem->wr32(max_comp_ea, kMaxComponentLength);
    }
    const std::uint32_t flags_ea = call.u32(5);
    if (flags_ea != 0) {
        requireCommitted(cpu, flags_ea, 4, api, "lpFileSystemFlags");
        cpu.mem->wr32(flags_ea, kFileSystemFlags);
    }
    const std::uint32_t fs_buf = call.u32(6);
    const std::uint32_t fs_size = call.u32(7);
    if (fs_buf != 0 && fs_size != 0) {
        writeAnsiBuffer(cpu, fs_buf, fs_size, "NTFS", api, "lpFileSystemNameBuffer");
    }
    call.result = 1;
}

void shimGetDriveTypeA(X86& cpu, ShimCall& call) {
    requireVfs(cpu, "GetDriveTypeA");
    // 本模拟器只有一张盘：game_dir 所在的宿主盘；对 guest 一律回答"固定盘"
    call.result = 3u;  // DRIVE_FIXED
}

void shimGetLogicalDrives(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 只有根盘（bit0 = A:）
    call.result = 1u;
}

// ---------------------------------------------------------------------------------------
// GetFullPathNameA / GetSystemDirectoryA / GetWindowsDirectoryA
// ---------------------------------------------------------------------------------------
// ★ 固定的 **guest** 路径（⛔ 不是宿主路径：本模拟器不把宿主的 C:\WINDOWS 暴露给 guest）。
//   取值与本作年代一致（本模拟器报 XP 5.1.2600，系统盘是 C:）。
constexpr const char* kGuestWindowsDir = "C:\\WINDOWS";
constexpr const char* kGuestSystemDir = "C:\\WINDOWS\\SYSTEM32";

// 返回"所需大小（含结尾 0）"或"写入长度（不含结尾 0）"——文档的两种口径：
//   · 成功 ⇒ 长度，**不含**结尾 0；
//   · 缓冲太小（含 uSize==0 的"问大小"）⇒ 所需大小，**含**结尾 0。
void answerFixedDirectory(X86& cpu, ShimCall& call, const char* api, const char* guest_dir) {
    const std::uint32_t buf = call.u32(0);
    const std::uint32_t cap = call.u32(1);
    const std::string bytes = guest_dir;  // 全 ASCII ⇒ CP932 字节与 UTF-8 字节一致
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size()) + 1u;
    if (cap == 0) {  // "问大小"（允许 lpBuffer == NULL）
        call.result = need;
        return;
    }
    if (buf == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpBuffer 为 NULL 但 uSize=%u ≠ 0 ⇒ 返回 0 + 错误 87", cap);
        call.result = 0;
        return;
    }
    if (cap < need) {
        call.result = need;  // 太小：返回所需大小（含结尾 0），不写缓冲
        return;
    }
    writeAnsiBuffer(cpu, buf, cap, bytes, api, "lpBuffer");
    call.result = static_cast<std::uint32_t>(bytes.size());
}

void shimGetSystemDirectoryA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetSystemDirectoryA");
    answerFixedDirectory(cpu, call, "GetSystemDirectoryA", kGuestSystemDir);
}

void shimGetWindowsDirectoryA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetWindowsDirectoryA");
    answerFixedDirectory(cpu, call, "GetWindowsDirectoryA", kGuestWindowsDir);
}

// ★ 文档（fileapi.h）：成功 ⇒ 长度**不含**结尾 0；缓冲太小 ⇒ 所需大小**含**结尾 0；
//   其它失败 ⇒ 0。相对路径相对当前目录（`SetCurrentDirectoryA` 的状态）；**不查文件是否存在**、
//   **不改调用方的大小写**（文档："GetFullPathName does not convert the specified file name"）。
//   ⇒ 规范化走 `Vfs::canonicalGuest`（纯文本；`..` 到根即止），⛔ 不走 `resolve()`（那会查宿主盘）。
void shimGetFullPathNameA(X86& cpu, ShimCall& call) {
    const char* api = "GetFullPathNameA";
    requireMem(cpu, api);
    requireVfs(cpu, api);
    const std::string guest_in = pathFromCp932(call.str(0), api);
    const std::uint32_t cap = call.u32(1);
    const std::uint32_t buf = call.u32(2);
    const std::uint32_t part_ptr = call.u32(3);
    if (guest_in.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpFileName 为 NULL / 空 ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    const std::string full = cpu.vfs->canonicalGuest(guest_in);
    if (full.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "\"%s\" 规范化结果为空 ⇒ 返回 0 + 错误 87", guest_in.c_str());
        call.result = 0;
        return;
    }
    std::size_t bad = 0;
    const std::string bytes = utf8ToCp932(full, &bad);
    if (bad != 0) {
        shimLog("kernel32", api, "\"%s\" 里有 %u 个字符编不成 CP932（已写 '?'）", full.c_str(),
                static_cast<unsigned>(bad));
    }
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size()) + 1u;
    if (cap == 0) {  // "问大小"（允许 lpBuffer == NULL）
        call.result = need;
        return;
    }
    if (buf == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpBuffer 为 NULL 但 nBufferLength=%u ≠ 0 ⇒ 返回 0 + 错误 87", cap);
        call.result = 0;
        return;
    }
    if (cap < need) {  // 太小：返回所需大小（含结尾 0），不写缓冲、不写 lpFilePart
        call.result = need;
        return;
    }
    writeAnsiBuffer(cpu, buf, cap, bytes, api, "lpBuffer");

    if (part_ptr != 0) {
        requireCommitted(cpu, part_ptr, 4, api, "lpFilePart");
        // 文档："If lpBuffer refers to a directory and not a file, lpFilePart receives zero."
        // ⇒ 调用方给的是目录形态（以分隔符结尾）或结果本身就是根 ⇒ 0；否则指向最后一个分量。
        const char last_in = guest_in.back();
        const bool dir_form = last_in == '\\' || last_in == '/' || full.back() == '\\';
        std::uint32_t part = 0;
        if (!dir_form) {
            const std::size_t slash = bytes.find_last_of("\\/");
            const std::size_t off = (slash == std::string::npos) ? 0u : slash + 1u;
            part = buf + static_cast<std::uint32_t>(off);
        }
        cpu.mem->wr32(part_ptr, part);
    }
    call.result = static_cast<std::uint32_t>(bytes.size());
}

// ---------------------------------------------------------------------------------------
// 文件时间
// ---------------------------------------------------------------------------------------
void shimGetFileTime(X86& cpu, ShimCall& call) {
    const char* api = "GetFileTime";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    const std::uint32_t creation_ea = call.u32(1);
    const std::uint32_t access_ea = call.u32(2);
    const std::uint32_t write_ea = call.u32(3);
    if (creation_ea != 0) requireCommitted(cpu, creation_ea, 8, api, "lpCreationTime");
    if (access_ea != 0) requireCommitted(cpu, access_ea, 8, api, "lpLastAccessTime");
    if (write_ea != 0) requireCommitted(cpu, write_ea, 8, api, "lpLastWriteTime");

    // 三个时间取宿主真值（SDL_GetPathInfo；见 vfs 的 hostFileTimes）。SDL 失败才退回 mtime 近似
    // （三值相同）——那是**退路**，不是默认口径。
    HostFileTimes times;
    if (!hostFileTimes(st->host_path, &times)) {
        std::error_code ec;
        const auto wt = fs::last_write_time(pathFromUtf8(st->host_path), ec);
        if (ec) {
            pathFailure(cpu, api, kErrFileNotFound, st->guest_path,
                        "[io] 宿主取文件时间失败：" + ec.message());
            call.result = 0;
            return;
        }
        const auto t = static_cast<std::uint64_t>(fileTime100ns(wt));
        times.create_100ns = t;
        times.access_100ns = t;
        times.write_100ns = t;
        shimLog("kernel32", api, "SDL_GetPathInfo 失败 ⇒ creation/access/write 退回宿主 mtime 近似");
    }
    const std::uint64_t values[3] = {times.create_100ns, times.access_100ns, times.write_100ns};
    const std::uint32_t slots[3] = {creation_ea, access_ea, write_ea};
    for (int i = 0; i < 3; ++i) {
        if (slots[i] == 0) continue;  // 三个出参都可为 NULL（真机允许）
        cpu.mem->wr32(slots[i], static_cast<std::uint32_t>(values[i] & 0xFFFFFFFFull));
        cpu.mem->wr32(slots[i] + 4u, static_cast<std::uint32_t>(values[i] >> 32));
    }
    call.result = 1;
}

// FILETIME 的"禁止后续更新"哨兵（两个 DWORD 全 0xFFFFFFFF）
constexpr std::uint64_t kFileTimeFreezeSentinel = 0xFFFFFFFFFFFFFFFFull;

// 读一个 FILETIME 出参（100 ns 自 1601）。返回 false = "这个时间不改"：
//   · 指针为 NULL（文档允许）；
//   · 两个 DWORD 全 0（文档："If the application does not need to change this information,
//     set this parameter either to NULL or to a pointer to a FILETIME ... set to 0"）；
//   · 两个 DWORD 全 0xFFFFFFFF（文档的"禁止后续更新"哨兵；宿主时间戳没有这个开关 ⇒ 记日志、不改）。
bool readOptionalFileTime(X86& cpu, std::uint32_t ea, const char* api, const char* what,
                          std::uint64_t* out) {
    if (ea == 0) return false;
    requireCommitted(cpu, ea, 8, api, what);
    const std::uint64_t v =
        static_cast<std::uint64_t>(cpu.mem->rd32(ea)) |
        (static_cast<std::uint64_t>(cpu.mem->rd32(ea + 4u)) << 32);
    if (v == 0) return false;
    if (v == kFileTimeFreezeSentinel) {
        shimLog("kernel32", api, "%s 是全 0xFFFFFFFF（禁止后续更新的哨兵）⇒ 本模拟器不改它", what);
        return false;
    }
    *out = v;
    return true;
}

// ★ 真写宿主时间戳（偏差 10）：Windows 三个都能写；POSIX 只有 access / write。
//   ⛔ 不"成功但什么都不做"：宿主侧失败 ⇒ FALSE + 真实错误码。
void shimSetFileTime(X86& cpu, ShimCall& call) {
    const char* api = "SetFileTime";
    requireHandles(cpu, api);
    FileState* st = fileOf(cpu, call.u32(0), api);
    if (!st) {
        call.result = 0;
        return;
    }
    std::uint64_t creation = 0;
    std::uint64_t access = 0;
    std::uint64_t write = 0;
    const bool has_creation = readOptionalFileTime(cpu, call.u32(1), api, "lpCreationTime", &creation);
    const bool has_access = readOptionalFileTime(cpu, call.u32(2), api, "lpLastAccessTime", &access);
    const bool has_write = readOptionalFileTime(cpu, call.u32(3), api, "lpLastWriteTime", &write);
    if (!has_creation && !has_access && !has_write) {
        // 文档：三个都可以不改（NULL / 全 0）⇒ 成功返回
        call.result = 1;
        return;
    }

#if defined(_WIN32)
    auto toFileTime = [](std::uint64_t v) {
        FILETIME ft;
        ft.dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFull);
        ft.dwHighDateTime = static_cast<DWORD>(v >> 32);
        return ft;
    };
    FILETIME ft_creation{};
    FILETIME ft_access{};
    FILETIME ft_write{};
    if (has_creation) ft_creation = toFileTime(creation);
    if (has_access) ft_access = toFileTime(access);
    if (has_write) ft_write = toFileTime(write);
    const fs::path p = pathFromUtf8(st->host_path);
    const std::wstring wpath = p.wstring();
    HANDLE h = ::CreateFileW(wpath.c_str(), FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = ::GetLastError();
        cpu.shim.last_error = static_cast<std::uint32_t>(e);
        shimLog("kernel32", api, "\"%s\" 以 FILE_WRITE_ATTRIBUTES 打开失败（宿主错误 %lu）⇒ 返回 0",
                st->guest_path.c_str(), static_cast<unsigned long>(e));
        call.result = 0;
        return;
    }
    const BOOL ok = ::SetFileTime(h, has_creation ? &ft_creation : nullptr,
                                  has_access ? &ft_access : nullptr,
                                  has_write ? &ft_write : nullptr);
    const DWORD err = ok ? 0u : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        cpu.shim.last_error = static_cast<std::uint32_t>(err);
        shimLog("kernel32", api, "\"%s\" 的宿主 SetFileTime 失败（宿主错误 %lu）⇒ 返回 0",
                st->guest_path.c_str(), static_cast<unsigned long>(err));
        call.result = 0;
        return;
    }
#else
    // POSIX：`utimensat` 写 atime/mtime；macOS 额外用 `setattrlist(ATTR_CMN_CRTIME)` 写 birthtime。
    // 其它 POSIX / 文件系统不支持 birthtime ⇒ 只留一行日志（用户 2026-11-04 口径：能改就改，不能就算了）。
    const std::string& host = st->host_path;
    timespec ts[2];
    ts[0].tv_nsec = has_access ? 0 : UTIME_OMIT;
    ts[1].tv_nsec = has_write ? 0 : UTIME_OMIT;
    if (has_access) {
        const std::int64_t ns = static_cast<std::int64_t>((access - kFileTimeEpochDelta) * 100ull);
        ts[0].tv_sec = static_cast<time_t>(ns / 1000000000ll);
        ts[0].tv_nsec = static_cast<long>(ns % 1000000000ll);
    }
    if (has_write) {
        const std::int64_t ns = static_cast<std::int64_t>((write - kFileTimeEpochDelta) * 100ull);
        ts[1].tv_sec = static_cast<time_t>(ns / 1000000000ll);
        ts[1].tv_nsec = static_cast<long>(ns % 1000000000ll);
    }
    if ((has_access || has_write) && ::utimensat(AT_FDCWD, host.c_str(), ts, 0) != 0) {
        const int e = errno;
        cpu.shim.last_error = errnoToWin32(e);
        shimLog("kernel32", api, "\"%s\" 的 utimensat 失败（errno=%d %s）⇒ 返回 0",
                st->guest_path.c_str(), e, errnoText(e).c_str());
        call.result = 0;
        return;
    }
#if defined(__APPLE__)
    if (has_creation) {
        struct attrlist al{};
        al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_CRTIME;
        struct timespec birth{};
        const std::int64_t ns = static_cast<std::int64_t>((creation - kFileTimeEpochDelta) * 100ull);
        birth.tv_sec = static_cast<time_t>(ns / 1000000000ll);
        birth.tv_nsec = static_cast<long>(ns % 1000000000ll);
        if (::setattrlist(host.c_str(), &al, &birth, sizeof(birth), 0) != 0) {
            const int e = errno;
            // 不是失败路径：atime/mtime 已经写成功，creation 是"能改就改"的额外项。
            shimLog("kernel32", api,
                    "\"%s\" 的 creation time 写入失败（setattrlist errno=%d %s）⇒ 跳过，其余时间已写",
                    st->guest_path.c_str(), e, errnoText(e).c_str());
        }
    }
#else
    if (has_creation) {
        shimLog("kernel32", api,
                "\"%s\" 的 creation time 在 POSIX 上不可移植 ⇒ 未写入（文档允许文件系统不记录它）",
                st->guest_path.c_str());
    }
#endif
#endif
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// FindFirstFileA / FindNextFileA / FindClose
// ---------------------------------------------------------------------------------------
// ★ CP932 的安全截断只有一份实现（`runtime/cp932` 的 `cp932Truncate`）：`cFileName` 定长 260 字节，
//   截断时必须成对丢双字节字符（⛔ 不留半个字符）。

void fillFindData(X86& cpu, std::uint32_t ea, const HostDirEntry& e, const char* api) {
    requireCommitted(cpu, ea, kFindDataSize, api, "lpFindFileData");
    cpu.mem->fillBytes(ea, 0, kFindDataSize);
    cpu.mem->wr32(ea + kFdAttributes, e.attributes);
    // 三个时间各写各的（宿主真值；见 vfs 的 hostFileTimes）
    const std::uint64_t times[3] = {e.create_time_100ns, e.access_time_100ns, e.write_time_100ns};
    const std::uint32_t offs[3] = {kFdCreationTime, kFdLastAccessTime, kFdLastWriteTime};
    for (int i = 0; i < 3; ++i) {
        cpu.mem->wr32(ea + offs[i], static_cast<std::uint32_t>(times[i] & 0xFFFFFFFFull));
        cpu.mem->wr32(ea + offs[i] + 4u, static_cast<std::uint32_t>(times[i] >> 32));
    }
    cpu.mem->wr32(ea + kFdSizeHigh, static_cast<std::uint32_t>(e.size >> 32));
    cpu.mem->wr32(ea + kFdSizeLow, static_cast<std::uint32_t>(e.size & 0xFFFFFFFFull));
    cpu.mem->wr32(ea + kFdReserved0, 0u);
    cpu.mem->wr32(ea + kFdReserved1, 0u);

    std::size_t bad = 0;
    const std::string bytes = utf8ToCp932(e.name, &bad);
    if (bad != 0) {
        shimLog("kernel32", api, "文件名 \"%s\" 里有 %u 个字符不能编成 CP932（已写 '?'）",
                e.name.c_str(), static_cast<unsigned>(bad));
    }
    const std::string clipped = cp932Truncate(bytes, kFdFileNameChars - 1u);
    if (clipped.size() != bytes.size()) {
        shimLog("kernel32", api, "文件名 \"%s\" 超过 %u 字节 ⇒ 截断到 \"%s\"", e.name.c_str(),
                static_cast<unsigned>(kFdFileNameChars - 1u), clipped.c_str());
    }
    if (!clipped.empty()) cpu.mem->writeBytes(ea + kFdFileName, clipped.data(), clipped.size());
    // cAlternateFileName 保持 0（偏差 9：8.3 短名不模拟；plan §9 R2）
}

void shimFindFirstFileA(X86& cpu, ShimCall& call) {
    const char* api = "FindFirstFileA";
    requireHandles(cpu, api);
    requireVfs(cpu, api);
    const std::string guest = pathFromCp932(call.str(0), api);
    const std::uint32_t data_ea = call.u32(1);
    requireCommitted(cpu, data_ea, kFindDataSize, api, "lpFindFileData");

    std::vector<HostDirEntry> entries;
    std::string why;
    if (!cpu.vfs->find(guest, &entries, &why)) {
        pathFailure(cpu, api, vfsErrorToWin32(why), guest, why);
        call.result = kInvalidHandleValue;
        return;
    }
    if (entries.empty()) {
        pathFailure(cpu, api, kErrFileNotFound, guest, "[notfound] 没有匹配的条目");
        call.result = kInvalidHandleValue;
        return;
    }
    // 时间戳来源：每次 FindFirst 留一行（plan 的"不许静默"；见文件头偏差 9）
    shimLog("kernel32", api,
            "枚举顺序 = Vfs::find 排序后的顺序（%u 条）；时间戳走 hostFileTimes（SDL 真值，失败才回退 mtime）",
            static_cast<unsigned>(entries.size()));

    auto* fs_state = new FindState{};
    fs_state->entries = std::move(entries);
    fs_state->cursor = 0;
    const std::uint32_t h = cpu.handles->add(HandleKind::Find, guest.c_str(), fs_state);
    HandleEntry* entry = cpu.handles->get(h);
    if (!entry) {
        delete fs_state;
        shimAbort("kernel32", api, "句柄表拒绝分配查找句柄（add 后立刻取不到 0x%08X）", h);
    }
    entry->destroy = &destroyFind;
    fillFindData(cpu, data_ea, fs_state->entries[0], api);
    fs_state->cursor = 1;
    call.result = h;
}

void shimFindNextFileA(X86& cpu, ShimCall& call) {
    const char* api = "FindNextFileA";
    requireHandles(cpu, api);
    FindState* fs_state = findOf(cpu, call.u32(0), api);
    if (!fs_state) {
        call.result = 0;
        return;
    }
    const std::uint32_t data_ea = call.u32(1);
    requireCommitted(cpu, data_ea, kFindDataSize, api, "lpFindFileData");
    if (fs_state->cursor >= fs_state->entries.size()) {
        cpu.shim.last_error = kErrNoMoreFiles;  // ERROR_NO_MORE_FILES(18)
        call.result = 0;
        return;
    }
    fillFindData(cpu, data_ea, fs_state->entries[fs_state->cursor], api);
    ++fs_state->cursor;
    call.result = 1;
}

void shimFindClose(X86& cpu, ShimCall& call) {
    const char* api = "FindClose";
    requireHandles(cpu, api);
    const std::uint32_t h = call.u32(0);
    HandleEntry* e = cpu.handles->get(h);
    if (!e || !e->used) cpu.handles->invalidHandle(h, api);
    if (e->kind != HandleKind::Find) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄 0x%08X 不是查找句柄（kind=%s）⇒ 返回 FALSE", h,
                handleKindName(e->kind));
        call.result = 0;
        return;
    }
    // ★ 与 CloseHandle 同一个回收点（`DuplicateHandle` 可能还有别的句柄指着这个 FindState）
    releasePayload(*cpu.handles, h);
    if (!cpu.handles->close(h)) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", api, "句柄表拒绝关闭 0x%08X ⇒ 返回 FALSE", h);
        call.result = 0;
        return;
    }
    call.result = 1;
}

}  // namespace

// =======================================================================================
// 注册（唯一导出，见 register.h）
// =======================================================================================
void registerFileShims(ShimRegistry& s) {
    // ---- 打开 / 读写 / 定位 ----
    s.add("kernel32", "CreateFileA", Conv::Stdcall, 7, shimCreateFileA);
    s.add("kernel32", "CreateFileW", Conv::Stdcall, 7, shimCreateFileW);
    s.add("kernel32", "ReadFile", Conv::Stdcall, 5, readFile);
    s.add("kernel32", "WriteFile", Conv::Stdcall, 5, writeFile);
    s.add("kernel32", "FlushFileBuffers", Conv::Stdcall, 1, shimFlushFileBuffers);
    s.add("kernel32", "SetFilePointer", Conv::Stdcall, 4, shimSetFilePointer);
    // ★ argc=5（偏差 1：LARGE_INTEGER 占两个栈槽，真机是 SetFilePointerEx@20）
    s.add("kernel32", "SetFilePointerEx", Conv::Stdcall, 5, shimSetFilePointerEx);
    s.add("kernel32", "GetFileSize", Conv::Stdcall, 2, shimGetFileSize);

    // ---- 属性 / 删除 / 改名 / 复制 / 目录 ----
    s.add("kernel32", "GetFileAttributesA", Conv::Stdcall, 1, shimGetFileAttributesA);
    s.add("kernel32", "SetFileAttributesA", Conv::Stdcall, 2, shimSetFileAttributesA);
    s.add("kernel32", "DeleteFileA", Conv::Stdcall, 1, shimDeleteFileA);
    s.add("kernel32", "MoveFileA", Conv::Stdcall, 2, shimMoveFileA);
    s.add("kernel32", "CopyFileA", Conv::Stdcall, 3, shimCopyFileA);
    s.add("kernel32", "CreateDirectoryA", Conv::Stdcall, 2, shimCreateDirectoryA);
    s.add("kernel32", "RemoveDirectoryA", Conv::Stdcall, 1, shimRemoveDirectoryA);

    // ---- 当前目录 ----
    s.add("kernel32", "GetCurrentDirectoryA", Conv::Stdcall, 2, shimGetCurrentDirectoryA);
    s.add("kernel32", "GetCurrentDirectoryW", Conv::Stdcall, 2, shimGetCurrentDirectoryW);
    s.add("kernel32", "SetCurrentDirectoryA", Conv::Stdcall, 1, shimSetCurrentDirectoryA);

    // ---- 枚举 ----
    s.add("kernel32", "FindFirstFileA", Conv::Stdcall, 2, shimFindFirstFileA);
    s.add("kernel32", "FindNextFileA", Conv::Stdcall, 2, shimFindNextFileA);
    s.add("kernel32", "FindClose", Conv::Stdcall, 1, shimFindClose);

    // ---- 临时目录 / 临时文件 / 卷 / 盘 ----
    s.add("kernel32", "GetTempPathA", Conv::Stdcall, 2, shimGetTempPathA);
    s.add("kernel32", "GetTempFileNameA", Conv::Stdcall, 4, shimGetTempFileNameA);
    s.add("kernel32", "GetVolumeInformationA", Conv::Stdcall, 8, shimGetVolumeInformationA);
    s.add("kernel32", "GetDriveTypeA", Conv::Stdcall, 1, shimGetDriveTypeA);
    s.add("kernel32", "GetLogicalDrives", Conv::Stdcall, 0, shimGetLogicalDrives);

    // ---- 文件时间（读：近似；写：真写宿主时间戳，见偏差 10）----
    s.add("kernel32", "GetFileTime", Conv::Stdcall, 4, shimGetFileTime);
    s.add("kernel32", "SetFileTime", Conv::Stdcall, 4, shimSetFileTime);

    // ---- 路径规范化 / 固定目录（⛔ 固定 guest 路径，不暴露宿主路径）----
    s.add("kernel32", "GetFullPathNameA", Conv::Stdcall, 4, shimGetFullPathNameA);
    s.add("kernel32", "GetSystemDirectoryA", Conv::Stdcall, 2, shimGetSystemDirectoryA);
    s.add("kernel32", "GetWindowsDirectoryA", Conv::Stdcall, 2, shimGetWindowsDirectoryA);
}

}  // namespace lift
