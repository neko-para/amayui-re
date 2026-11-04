// emulator-lift · advapi32 注册表（SCOPE §1：整体 no-op）
//
// ★ 用户口径（2026-11-04）：注册表与菜单 / 模态一样按 **no-op** 处理，不是硬 trap。
//   判据（SCOPE §1）：
//     · `RegOpenKeyExA` / `RegCreateKeyExA` → `ERROR_SUCCESS` + 非空假句柄；
//     · `RegQueryValueExA` → `ERROR_FILE_NOT_FOUND(2)`（调用方**回落到默认值**），
//       `lpType` / `lpcbData` 清零；
//     · `RegSetValueExA` → `ERROR_SUCCESS`（接受调用，但不落盘）；
//     · `RegEnumValueA` → `ERROR_NO_MORE_ITEMS(259)`（空枚举）；
//     · `RegCloseKey` → `ERROR_SUCCESS`。
// ★ 每一条都打一行 `[shim]`（SCOPE §3 第 3 条：⛔ 不许静默 no-op），并登记为 `addStub`
//   ⇒ `data/lift/overrides.json` 里 `kind=stub` + reason；`lift check` 的派生缺口文件会把它们移出。
// ★ 它们不属于 D6 七族，所以不触发 `seven-family-no-stub`。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// 假注册表句柄：非 0、稳定、一眼能看出不是真 HKEY 的值。
constexpr std::uint32_t kFakeKey = 0x00020001u;

void shimRegOpenKeyExA(X86& cpu, ShimCall& call) {
    // 参数：hKey / lpSubKey / ulOptions / samDesired / phkResult
    const std::uint32_t out = call.u32(4);
    if (out) cpu.mem->wr32(out, kFakeKey);
    shimLog("advapi32", "RegOpenKeyExA",
            "注册表整体 no-op ⇒ 成功 + 假句柄 0x%08X（SCOPE §1；不读真实注册表）", kFakeKey);
    call.result = 0;  // ERROR_SUCCESS
}

void shimRegCreateKeyExA(X86& cpu, ShimCall& call) {
    // 参数：hKey / lpSubKey / Reserved / lpClass / dwOptions / samDesired /
    //       lpSecurityAttributes / phkResult / lpdwDisposition
    const std::uint32_t out = call.u32(7);
    const std::uint32_t disposition = call.u32(8);
    if (out) cpu.mem->wr32(out, kFakeKey);
    if (disposition) cpu.mem->wr32(disposition, 1);  // REG_CREATED_NEW_KEY
    shimLog("advapi32", "RegCreateKeyExA",
            "注册表整体 no-op ⇒ 成功 + 假句柄 0x%08X（SCOPE §1；不写真实注册表）", kFakeKey);
    call.result = 0;
}

void shimRegQueryValueExA(X86& cpu, ShimCall& call) {
    // 参数：hKey / lpValueName / lpReserved / lpType / lpData / lpcbData
    const std::uint32_t type = call.u32(3);
    const std::uint32_t data = call.u32(4);
    const std::uint32_t size = call.u32(5);
    (void)data;
    if (type) cpu.mem->wr32(type, 0);  // REG_NONE
    if (size) cpu.mem->wr32(size, 0);
    shimLog("advapi32", "RegQueryValueExA",
            "注册表整体 no-op ⇒ 恒返回 ERROR_FILE_NOT_FOUND(2)，调用方回落到默认值（SCOPE §1）");
    call.result = 2;  // ERROR_FILE_NOT_FOUND
}

void shimRegSetValueExA(X86&, ShimCall& call) {
    shimLog("advapi32", "RegSetValueExA",
            "注册表整体 no-op ⇒ 返回 ERROR_SUCCESS（接受调用，但不落盘；SCOPE §1）");
    call.result = 0;
}

void shimRegEnumValueA(X86&, ShimCall& call) {
    shimLog("advapi32", "RegEnumValueA",
            "注册表整体 no-op ⇒ 恒返回 ERROR_NO_MORE_ITEMS(259)（空枚举；SCOPE §1）");
    call.result = 259;  // ERROR_NO_MORE_ITEMS
}

void shimRegCloseKey(X86&, ShimCall& call) {
    shimLog("advapi32", "RegCloseKey", "注册表整体 no-op ⇒ 返回 ERROR_SUCCESS（SCOPE §1）");
    call.result = 0;
}

}  // namespace

void registerAdvapi32RegistryShims(ShimRegistry& s) {
    // 顺序 = 原来的 5 条 addUnimplemented 的顺序（trace 的 id 顺序不变）；RegCloseKey 仍保留
    // （本 EXE 没导入它，但它是同族 defensive registration，保持整族口径一致）。
    s.addStub("advapi32", "RegOpenKeyExA", Conv::Stdcall, 5, &shimRegOpenKeyExA,
              "SCOPE §1 注册表整体 no-op：RegOpenKeyExA 返回 ERROR_SUCCESS + 非空假句柄（不读真实注册表）");
    s.addStub("advapi32", "RegQueryValueExA", Conv::Stdcall, 6, &shimRegQueryValueExA,
              "SCOPE §1 注册表整体 no-op：RegQueryValueExA 恒返回 ERROR_FILE_NOT_FOUND(2)，调用方回落默认值");
    s.addStub("advapi32", "RegSetValueExA", Conv::Stdcall, 6, &shimRegSetValueExA,
              "SCOPE §1 注册表整体 no-op：RegSetValueExA 返回成功但不落盘");
    s.addStub("advapi32", "RegCreateKeyExA", Conv::Stdcall, 9, &shimRegCreateKeyExA,
              "SCOPE §1 注册表整体 no-op：RegCreateKeyExA 返回成功 + 假句柄（不写真实注册表）");
    s.addStub("advapi32", "RegEnumValueA", Conv::Stdcall, 8, &shimRegEnumValueA,
              "SCOPE §1 注册表整体 no-op：RegEnumValueA 恒返回 ERROR_NO_MORE_ITEMS(259)（空枚举）");
    s.addStub("advapi32", "RegCloseKey", Conv::Stdcall, 1, &shimRegCloseKey,
              "SCOPE §1 注册表整体 no-op：RegCloseKey 返回成功");
}

}  // namespace lift
