// emulator-lift · shell32 特殊目录（P0 / G5）+ 小族（P4：imm32）
//
// ★ 用户口径（2026-11-04）：需要一个传入伪造 AppData 的路径。
//   `--appdata <host-path>` 落到 Vfs；本 shim 只返回 **Windows 形状的 guest 路径**，
//   Vfs::resolve 再把 `C:\Users\Amayui\AppData\...` 映射到 `--appdata` 下的对应子目录。
// ★ 没有 --appdata 时返回 FALSE + ERROR_ACCESS_DENIED(5) 并留日志：⛔ 不悄悄回退到宿主真实 AppData。
// ★ P4 起本文件还放 `imm32` 那一条（输入法）：它和 shell32 一样是"很小的族 + 一句裁决"，
//   单独开一个 translation unit 只会让注册表更长，不会让语义更清楚。
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
constexpr std::uint32_t kCsidlFlagCreate = 0x8000u;
// HRESULT：成功 = S_OK(0)；没有 --appdata 时与 `SHGetSpecialFolderPathA` 同一口径（拒绝访问）。
constexpr std::uint32_t kSOK = 0x00000000u;
constexpr std::uint32_t kEAccessDenied = 0x80070005u;
constexpr std::uint32_t kEFail = 0x80004005u;

// ★ 文本来自 `runtime/vfs.h` 的**唯一真源**（与环境块 / GetEnvironmentVariable* 同一份）。
//   ⛔ 不在这里另写路径字符串。
const char* guestSpecialPath(std::uint32_t csidl) {
    csidl &= 0x00FFu;  // CSIDL 的低字节；高字节是 CSIDL_FLAG_*
    switch (csidl) {
        case kCsidlAppData: return lift::guestAppDataText();
        case kCsidlLocalAppData: return lift::guestLocalAppDataText();
        case kCsidlPersonal: return lift::guestDocumentsText();
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

// ---------------------------------------------------------------------------------------
// shell32 · `SHGetFolderPathA`（2026-11-05 新增）
//
// ★ 为什么必须有它：引擎在 `UseAppDataFolder` 为 1 时**只**走这条路（`sub_4900F0`：
//   major > 4 ⇒ LoadLibrary(Shell32) + GetProcAddress("SHGetFolderPathA")，**没有** A 版失败后
//   改调 `SHGetSpecialFolderPathA` 的分支）。我们报 GetVersion = XP 5.1，而 XP 的 shell32
//   **确实**导出 `SHGetFolderPathA`；此前按"老 shell32 不提供"处理 ⇒ 引擎拿不到 AppData 路径，
//   回落到配置里的相对 `set:RegFilePath`（`Eushully\天結いキャッスルマイスター`）⇒ 往安装树里写 SYS4REG.INI（验收红线）。
// ★ 语义（Win32 文档）：
//   · csidl 低字节 = 目录 id，`CSIDL_FLAG_CREATE(0x8000)` ⇒ 不存在就建（这里建 `<--appdata>/<sub>`）；
//   · 返回 **HRESULT**：S_OK(0) 成功；没有 --appdata ⇒ E_ACCESSDENIED(0x80070005) + 5，
//     ⛔ 绝不回退宿主真实 AppData。
// ---------------------------------------------------------------------------------------
void shimSHGetFolderPathA(X86& cpu, ShimCall& call) {
    (void)call.u32(0);  // hwndOwner
    const std::uint32_t csidl = call.u32(1);
    const std::uint32_t flags = call.u32(2);
    const std::uint32_t path_ea = call.u32(4);
    (void)flags;  // SHGFP_TYPE_CURRENT / DEFAULT：本模拟器只有一个当前值
    const char* guest = guestSpecialPath(csidl);
    if (!guest) {
        shimLog("shell32", "SHGetFolderPathA", "CSIDL=0x%08X 未建模 ⇒ E_FAIL（0x%08X）", csidl, kEFail);
        call.result = kEFail;
        return;
    }
    if (!cpu.vfs || cpu.vfs->appDataDir().empty()) {
        cpu.shim.last_error = 5;
        shimLog("shell32", "SHGetFolderPathA",
                "CSIDL=0x%08X 需要 --appdata，但当前为空 ⇒ E_ACCESSDENIED + 5"
                "（⛔ 不回退宿主 AppData）",
                csidl);
        call.result = kEAccessDenied;
        return;
    }
    if (path_ea == 0) {
        shimLog("shell32", "SHGetFolderPathA", "pszPath 为 NULL ⇒ E_INVALIDARG（0x80070057）");
        call.result = 0x80070057u;
        return;
    }
    // CSIDL_FLAG_CREATE：把 `<--appdata>/<sub>` 的映射脚手架建出来（guest 看着仍是 Windows 形状）
    if ((csidl & kCsidlFlagCreate) != 0) {
        std::string why;
        const std::string dir = std::string(guest) + "\\";
        if (!cpu.vfs->makeSureDirectoryPathExists(dir, &why)) {
            shimLog("shell32", "SHGetFolderPathA", "CSIDL_FLAG_CREATE 建目录 \"%s\" 失败：%s", guest,
                    why.c_str());
            call.result = kEFail;
            return;
        }
    }
    const std::size_t n = std::strlen(guest);
    cpu.mem->writeBytes(path_ea, guest, n + 1);
    shimLog("shell32", "SHGetFolderPathA",
            "CSIDL=0x%08X ⇒ guest \"%s\"（S_OK；宿主映射到 --appdata%s）", csidl, guest,
            (csidl & kCsidlFlagCreate) ? "；已按 CSIDL_FLAG_CREATE 建脚手架" : "");
    call.result = kSOK;
}

// ---------------------------------------------------------------------------------------
// imm32 · `ImmAssociateContext`（P4 小族）
//
// ★ 2026-11-13 用户裁决：**不再无条件返回 NULL**；改成**条件 trap**（仍是 `addStub`，
//   所以覆盖清单里 kind = `noop`，reason 说清两臂）：
//   · `hIMC == 0`（摘除输入上下文 / 没有 IME）⇒ 返回**前一个**上下文 = **NULL** + 一行日志。
//     真机上"没有 IME"就是 NULL；启动路径 `0x4BB0C4` 每次有窗口都会这样调一次
//     （`_WinMain@16`，`.lst 296455-296489`）⇒ 这一臂必须照跑，⛔ 不能 trap 把 boot 停掉。
//   · `hIMC != 0`（真的想装一个输入上下文）⇒ **本线不实现** ⇒ **响亮停**（共用
//     `trapUnimplemented`：reason + 返回地址 + 寄存器 + guest 栈）。
//     理由：IME 不做；真机可能在存档界面弹输入对话框来编辑存档名，本线**不实现**那条路径。
//     用户原话："无论如何先保持硬 trap 拒绝即可，发现调用到了再说。"
//   ⛔ 不返回假 HIMC：那会让引擎以为输入上下文真的换过去了，IME 消息却永远不会来 ——
//   "假成功"（SCOPE §3 明令禁止）。
// ---------------------------------------------------------------------------------------
constexpr const char* kImmAssociateContextReason =
    "SCOPE §1：IME 不做（真机可能在存档界面弹输入对话框）⇒ hIMC==0 摘除返回 NULL；"
    "hIMC!=0 硬 trap（2026-11-13 用户裁决：先拒绝，发现调用到了再说）";

void shimImmAssociateContext(X86& cpu, ShimCall& call) {
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t himc = call.u32(1);
    if (himc != 0) {
        trapUnimplemented(cpu, "imm32", "ImmAssociateContext", kImmAssociateContextReason, call.ret_ea,
                          call.def ? call.def->stub_ea : 0u, call.conv, call.argc);
        return;
    }
    shimLog("imm32", "ImmAssociateContext", "hwnd=0x%08X hIMC=0 ⇒ NULL（摘除 IME；SCOPE §1 不做 IME）", hwnd);
    call.result = 0;
}

}  // namespace

void registerShell32Shims(ShimRegistry& s) {
    s.add("shell32", "SHGetSpecialFolderPathA", Conv::Stdcall, 4, &shimSHGetSpecialFolderPathA);
    // ★ 2026-11-05：XP 的 shell32 有 `SHGetFolderPathA`（引擎在 major>4 的路径上只走它）。
    s.add("shell32", "SHGetFolderPathA", Conv::Stdcall, 5, &shimSHGetFolderPathA);
    // ★ 输入法：SCOPE §1 的显式偏差 ⇒ 走 addStub（覆盖清单里 kind=noop + reason 可回查）。
    //   ★ 2026-11-13 用户裁决：hIMC==0 返回 NULL，hIMC!=0 **硬 trap**（见上面的说明）。仍然是
    //   **一条**注册 —— ⛔ 不要为 trap 再加第二个同名条目。
    s.addStub("imm32", "ImmAssociateContext", Conv::Stdcall, 2, &shimImmAssociateContext,
              kImmAssociateContextReason);
}

}  // namespace lift
