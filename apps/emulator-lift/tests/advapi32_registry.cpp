// emulator-lift · advapi32 注册表 no-op 的**参数校验**（pure：自造调用序列）
//
// 判据（REQ-01M7ATN3WPB14TVZJWZXTPQW3V；**依据是本机实测**，不是文档原话）：
//   ① `RegOpenKeyExA(..., phkResult=NULL)` ⇒ 87（`ERROR_INVALID_PARAMETER`）+ `LastError=87`，
//      且**不写任何内存**（哨兵槽必须原样）；
//   ② `RegOpenKeyExA(..., 合法指针)` ⇒ 0 + 写入非 0 假句柄（既有行为，⛔ 不能因为①而变）；
//   ③ `RegQueryValueExA(..., lpData!=NULL, lpcbData=NULL)` ⇒ 87 + `LastError=87`；
//   ④ `RegQueryValueExA(..., lpData=NULL, lpcbData=NULL)` ⇒ **不是 87**
//      （`lpData==NULL` 时 `lpcbData==NULL` 合法）。
//
// ★ 读数（2026-11-14 / Win11 x64 / P/Invoke 打真 advapi32）：
//   · `RegOpenKeyExA(HKEY_CURRENT_USER, "Software", 0, KEY_READ, NULL)` ⇒ 87；
//   · 合法缓冲 ⇒ 0；
//   · `RegQueryValueExA(h, "foo", NULL, NULL, NULL, NULL)` ⇒ 2（FILE_NOT_FOUND，
//     证明 lpData==NULL 时 lpcbData==NULL **不是**参数错误）；
//   · `RegQueryValueExA(h, "foo", NULL, NULL, buf, NULL)` ⇒ 87。
//   ⚠ 原单正文把依据写成"微软文档要求返回 87"是误引：learn.microsoft.com 的
//   `RegOpenKeyExA` 对 `phkResult` 只写 "A pointer to a variable that receives a handle"（没写不能为
//   NULL、也没写返回码）；`RegQueryValueExA` 只写 "lpcbData can be NULL only if lpData is NULL"
//   （没写返回码）。⇒ 这两条按**实测**立，文档只作约束。
//
// ⛔ 不断言"值不存在返回 2"这类**取值口径**：本线注册表整体 no-op（SCOPE §1），取值另说；
//   本夹具只锁"参数非法时返回什么"（API 合同）与"合法路径不被误伤"。
#include <cstdint>

#include "check.h"
#include "fixture.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kErrInvalidParameter = 87u;
// 哨兵：既当"NULL 调用不许写"的见证，也当②的合法输出位置（同一格 ⇒ 对照有意义）
constexpr std::uint32_t kSentinel = 0xA5A5A5A5u;

}  // namespace

int main() {
    using namespace lift_test;
    using lift_fixture::Fixture;

    // ① phkResult=NULL ⇒ 87 且不写内存；② 合法指针 ⇒ 0 + 假句柄
    {
        g_current_case = "reg-open-null-phkresult";
        Fixture f;
        lift::registerAdvapi32RegistryShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t open = f.use("advapi32", "RegOpenKeyExA");
        const std::uint32_t subkey = f.cstr("Software");
        const std::uint32_t out = f.u32data(kSentinel);  // 哨兵槽（= ② 的输出位置）
        constexpr std::uint32_t kHkeyCurrentUser = 0x80000001u;
        constexpr std::uint32_t kKeyRead = 0x20019u;

        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(open, {kHkeyCurrentUser, subkey, 0u, kKeyRead, 0u}),
                          kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_MEM_U32(f.m.mem(), out, kSentinel);  // 参数非法 ⇒ 一个字节都不写

        // 合法指针：既有行为逐字不变（0 + 非 0 假句柄）
        LIFT_CHECK_EQ_U32(f.call(open, {kHkeyCurrentUser, subkey, 0u, kKeyRead, out}), 0u);
        LIFT_CHECK_NE_U32(f.m.mem().rd32(out), 0u);
        LIFT_CHECK(f.ok);
    }

    // ③ lpData!=NULL && lpcbData=NULL ⇒ 87；④ 两者都 NULL ⇒ 合法（不是 87）
    {
        g_current_case = "reg-query-lpcbdata-null";
        Fixture f;
        lift::registerAdvapi32RegistryShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t query = f.use("advapi32", "RegQueryValueExA");
        const std::uint32_t name = f.cstr("foo");
        const std::uint32_t data = f.u32data(0x12345678u);
        // 入参 hKey 只当透传：本线不解析句柄（取值口径不在这里判）
        constexpr std::uint32_t kAnyKey = 0x00020001u;

        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(query, {kAnyKey, name, 0u, 0u, data, 0u}), kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);

        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_NE_U32(f.call(query, {kAnyKey, name, 0u, 0u, 0u, 0u}), kErrInvalidParameter);
        LIFT_CHECK_NE_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    return finish("advapi32-registry");
}
