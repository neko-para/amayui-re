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

// ---------------------------------------------------------------------------------------
// imm32 · `ImmAssociateContext`（P4 小族）
//
// ★ 裁决：`SCOPE.md` §1 —— **完整 IME 不做** ⇒ 按"这台机器没有 IME"回答：
//   返回**前一个**输入上下文 = **NULL**（真机上没有 IME 时就是这个值）。
//   ⛔ 不返回假 HIMC：那会让引擎以为输入上下文真的换过去了，IME 消息却永远不会来
//   —— 那就是"假成功"，SCOPE §3 明令禁止。
//   ⛔ 也不 trap：引擎在启动路径上必然会调它一次，trap 会把"没有 IME"变成"跑不下去"。
//   参数（HWND / HIMC）一律只记进日志、不做校验：本模拟器没有 IME 状态可改，
//   任何 hwnd / himc 的真机结果都是 NULL ⇒ 校验只会凭空造出真机没有的失败路径。
// ---------------------------------------------------------------------------------------
void shimImmAssociateContext(X86& cpu, ShimCall& call) {
    const std::uint32_t hwnd = call.u32(0);
    const std::uint32_t himc = call.u32(1);
    (void)cpu;
    shimLog("imm32", "ImmAssociateContext", "hwnd=0x%08X hIMC=0x%08X ⇒ NULL（没有 IME，SCOPE §1）",
            hwnd, himc);
    call.result = 0;
}

}  // namespace

void registerShell32Shims(ShimRegistry& s) {
    s.add("shell32", "SHGetSpecialFolderPathA", Conv::Stdcall, 4, &shimSHGetSpecialFolderPathA);
    // ★ 输入法：SCOPE §1 的显式偏差 ⇒ 走 addStub（覆盖清单里 kind=stub + reason 可回查）
    s.addStub("imm32", "ImmAssociateContext", Conv::Stdcall, 2, &shimImmAssociateContext,
              "SCOPE §1：完整 IME 不做 ⇒ 没有 IME：返回 NULL（不造假句柄）+ 一行日志");
}

}  // namespace lift
