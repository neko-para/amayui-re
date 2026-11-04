// emulator-lift · shell32 特殊目录（P0 / G5）
//
// ★ 用户口径（2026-11-04）：需要一个传入伪造 AppData 的路径。
//   `--appdata <host-path>` 落到 Vfs；本 shim 只返回 **Windows 形状的 guest 路径**，
//   Vfs::resolve 再把 `C:\Users\Amayui\AppData\...` 映射到 `--appdata` 下的对应子目录。
// ★ 没有 --appdata 时返回 FALSE + ERROR_ACCESS_DENIED(5) 并留日志：⛔ 不悄悄回退到宿主真实 AppData。
#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/x86.h"
#include "runtime/vfs.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kCsidlPersonal = 0x0005u;
constexpr std::uint32_t kCsidlAppData = 0x001Au;
constexpr std::uint32_t kCsidlLocalAppData = 0x001Cu;

const char* guestSpecialPath(std::uint32_t csidl) {
    csidl &= 0x00FFu;  // CSIDL 的低字节；高字节是 CSIDL_FLAG_*
    switch (csidl) {
        case kCsidlAppData: return "C:\\Users\\Amayui\\AppData\\Roaming";
        case kCsidlLocalAppData: return "C:\\Users\\Amayui\\AppData\\Local";
        case kCsidlPersonal: return "C:\\Users\\Amayui\\Documents";
        default: return nullptr;
    }
}

void shimSHGetSpecialFolderPathA(X86& cpu, ShimCall& call) {
    (void)call.u32(0);  // hwnd
    const std::uint32_t path_ea = call.u32(1);
    const std::uint32_t csidl = call.u32(2);
    (void)call.u32(3);  // fCreate（目录由 VFS 的写入路径负责创建）
    const char* guest = guestSpecialPath(csidl);
    if (!guest) {
        cpu.shim.last_error = 5;
        shimLog("shell32", "SHGetSpecialFolderPathA", "CSIDL=0x%08X 未建模 ⇒ FALSE + 5", csidl);
        call.result = 0;
        return;
    }
    if (!cpu.vfs || cpu.vfs->appDataDir().empty()) {
        cpu.shim.last_error = 5;
        shimLog("shell32", "SHGetSpecialFolderPathA",
                "CSIDL=0x%08X 需要 --appdata，但当前为空 ⇒ FALSE + 5（⛔ 不回退宿主 AppData）", csidl);
        call.result = 0;
        return;
    }
    if (path_ea) {
        const std::size_t n = std::strlen(guest);
        cpu.mem->writeBytes(path_ea, guest, n + 1);
    }
    shimLog("shell32", "SHGetSpecialFolderPathA",
            "CSIDL=0x%08X ⇒ guest \"%s\"（宿主映射到 --appdata；不创建目录）", csidl, guest);
    call.result = 1;
}

}  // namespace

void registerShell32Shims(ShimRegistry& s) {
    s.add("shell32", "SHGetSpecialFolderPathA", Conv::Stdcall, 4, &shimSHGetSpecialFolderPathA);
}

}  // namespace lift
