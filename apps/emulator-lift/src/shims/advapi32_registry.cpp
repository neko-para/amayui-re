// emulator-lift · advapi32 注册表（SCOPE §1：整体 no-op）
//
// ★ 用户口径（2026-11-04）：注册表与菜单 / 模态一样按 **no-op** 处理，不是硬 trap。
//   判据（SCOPE §1）：
//     · `RegOpenKeyExA` / `RegCreateKeyExA` → `ERROR_SUCCESS` + 非空假句柄
//       （`RegOpenKeyExA` 的 `phkResult == NULL` 例外 ⇒ 87，见下"参数校验"）；
//     · `RegQueryValueExA` → `ERROR_FILE_NOT_FOUND(2)`（调用方**回落到默认值**），
//       `lpType` / `lpcbData` 清零；
//     · `RegSetValueExA` → `ERROR_SUCCESS`（接受调用，但不落盘）；
//     · `RegEnumValueA` → `ERROR_NO_MORE_ITEMS(259)`（空枚举）；
//     · `RegCloseKey` → `ERROR_SUCCESS`。
// ★ 每一条都打一行 `[shim]`（SCOPE §3 第 3 条：⛔ 不许静默 no-op），并登记为 `addStub`
//   ⇒ `data/lift/overrides.json` 里 `kind=stub` + reason；`lift check` 的派生缺口文件会把它们移出。
// ★ 它们不属于 D6 七族，所以不触发 `seven-family-no-stub`。
//
// ★ 参数校验（2026-11-14，REQ-01M7ATN3WPB14TVZJWZXTPQW3V）——"参数非法时返回什么"是 **API 合同**，
//   不是取值取舍；⛔ 不改注册表 no-op 的**取值**口径（SCOPE §1：合法参数下各条返回值逐字不变）。
//   · `RegOpenKeyExA(..., phkResult=NULL)` ⇒ `ERROR_INVALID_PARAMETER(87)` 且**不写任何内存**；
//   · `RegQueryValueExA(..., lpData!=NULL, lpcbData=NULL)` ⇒ 87；
//     `lpData==NULL` 时 `lpcbData==NULL` **合法**（不许拒）。
//   ★ 依据 = **本机实测**（Win11 x64 / P/Invoke 打真 advapi32，2026-11-14），读数见
//     `tests/advapi32_registry.cpp` 的文件头；**文档只给约束、没写返回码** ——
//     `RegOpenKeyExA` 对 `phkResult` 只写 "A pointer to a variable that receives a handle"
//     （没说不能为 NULL、也没说返回什么）；`RegQueryValueExA` 只写 "lpcbData can be NULL only
//     if lpData is NULL"（没说返回什么）⇒ ⛔ 不要引"文档要求返回 87"当依据。
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
    if (out == 0) {
        // 实测 87（Win11 x64 / P/Invoke）；参数非法路径**一个字节都不写**。
        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER
        shimLogWarn("advapi32", "RegOpenKeyExA",
                    "phkResult 为 NULL ⇒ ERROR_INVALID_PARAMETER(87)，不写任何内存"
                    "（实测 Win11 x64 / P/Invoke；文档只写 phkResult 是接收句柄的指针）");
        call.result = 87;
        return;
    }
    cpu.mem->wr32(out, kFakeKey);
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
    // 实测（Win11 x64 / P/Invoke）：`lpData != NULL && lpcbData == NULL` ⇒ 87；
    // 而 `lpData == NULL` 时 `lpcbData == NULL` **合法**（实测回 2 = 文件不存在，不是 87）。
    // 参数非法路径**不写任何输出**（lpType / lpcbData 都保持调用方原值）。
    if (data != 0 && size == 0) {
        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER
        shimLogWarn("advapi32", "RegQueryValueExA",
                    "lpData=0x%08X 非 NULL 但 lpcbData 为 NULL ⇒ ERROR_INVALID_PARAMETER(87)"
                    "（实测 Win11 x64 / P/Invoke；文档只写 lpcbData 在 lpData 非 NULL 时不能为 NULL）",
                    data);
        call.result = 87;
        return;
    }
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
