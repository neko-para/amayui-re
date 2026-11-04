// emulator-lift · imagehlp 小族（P4）
//
// 本轮只做导入表里**真出现**的那一条：`MakeSureDirectoryPathExists`（`.lst` 3 个调用点，启动路径上
// 的是 0x004903A3）。引擎的用法（`sub_4900F0`，0x00490356-0x004903A3）：
//   `CreateFileA` 配置文件失败 ⇒ 把路径复制到 `DirPath` → `PathRemoveFileSpecA` 去掉文件名 →
//   `strcat_s(DirPath, "\\")` 补一个分隔符 → `MakeSureDirectoryPathExists(DirPath)`。
//   ⇒ 引擎传进来的路径**一定以分隔符结尾**（"最后一段也是目录"）。
//
// 语义（Win32 imagehlp 的文档口径）：
//   · "creates all the directories in the specified DirPath, beginning with the root"；
//     每个分隔符（`\` 或 `/`）**之前**的段都是目录；末尾分隔符把**最后一段**也标成目录。
//   · **已存在算成功**（与 `CreateDirectoryA` 的"已存在即失败"相反）。
//   · 失败 ⇒ FALSE；成功 ⇒ TRUE（文档只说 TRUE/FALSE，没有错码返回值；错码走 `last_error`）。
//   · ⛔ 不建 `\\server\share` 之类的网络根：本模拟器只有一个 guest 根（`--game-dir`），
//     与所有其它路径 API 走**同一条** VFS 映射（见 `Vfs::makeSureDirectoryPathExists`）。
//
// ★ 落盘只经 VFS（`--appdata` / `--game-dir` 的映射集中在 `runtime/vfs.cpp` 一处）：
//   ⛔ 不在这里直接 `std::filesystem::create_directory` 宿主路径 —— 那会绕过 VFS，
//   把 guest 路径的语义（根 / 大小写 / 逃逸检查）全部丢掉。
#include <cstdint>
#include <string>

#include "runtime/cp932.h"
#include "runtime/x86.h"
#include "runtime/vfs.h"
#include "shims/register.h"

namespace lift {
namespace {

// 路径入参（A 版 = CP932 字节）⇒ VFS 要的 UTF-8 guest 路径
std::string guestPathFromCp932(const std::string& bytes, const char* api) {
    if (bytes.empty()) return {};
    std::size_t bad = 0;
    const std::u16string w = cp932ToWide(bytes, &bad);
    if (bad != 0) {
        shimLog("imagehlp", api,
                "路径里有 %u 字节不是合法 CP932 ⇒ 已按 Windows 的宽松口径跳过", static_cast<unsigned>(bad));
    }
    std::size_t bad_utf16 = 0;
    return utf16ToUtf8(w, &bad_utf16);
}

// VFS 的失败原因 → Win32 错误码（与 kernel32_file.cpp 的 pathFailure 同一口径）
std::uint32_t vfsErrorToWin32(const std::string& why) {
    if (why.rfind("[escape]", 0) == 0) return 5;   // ERROR_ACCESS_DENIED
    if (why.rfind("[ready]", 0) == 0) return 5;
    if (why.rfind("[invalid]", 0) == 0) return 87;  // ERROR_INVALID_PARAMETER
    return 3;                                       // ERROR_PATH_NOT_FOUND
}

void shimMakeSureDirectoryPathExists(X86& cpu, ShimCall& call) {
    const char* api = "MakeSureDirectoryPathExists";
    if (cpu.vfs == nullptr) shimAbort("imagehlp", api, "cpu.vfs 为空（Machine 没装配 VFS）");
    if (!cpu.vfs->ready()) {
        shimAbort("imagehlp", api, "VFS 没有根（--game-dir 未设置）—— 建目录必须走 VFS");
    }
    if (call.u32(0) == 0) {
        shimLog("imagehlp", api, "DirPath 为 NULL ⇒ FALSE（Win32 失败值）");
        call.result = 0;
        return;
    }
    const std::string guest = guestPathFromCp932(call.str(0), api);
    if (guest.empty()) {
        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER
        shimLog("imagehlp", api, "DirPath 为空串 ⇒ FALSE + ERROR_INVALID_PARAMETER");
        call.result = 0;
        return;
    }
    std::string why;
    if (!cpu.vfs->makeSureDirectoryPathExists(guest, &why)) {
        cpu.shim.last_error = vfsErrorToWin32(why);
        shimLog("imagehlp", api, "\"%s\" ⇒ FALSE（%s）", guest.c_str(), why.c_str());
        call.result = 0;
        return;
    }
    shimLog("imagehlp", api, "\"%s\" ⇒ TRUE（逐段建目录；已存在算成功）", guest.c_str());
    call.result = 1;
}

}  // namespace

void registerImagehlpShims(ShimRegistry& s) {
    s.add("imagehlp", "MakeSureDirectoryPathExists", Conv::Stdcall, 1, &shimMakeSureDirectoryPathExists);
}

}  // namespace lift
