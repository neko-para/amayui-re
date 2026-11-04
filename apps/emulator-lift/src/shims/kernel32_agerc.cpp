// emulator-lift · AGERC.DLL 的三个导出（模态 no-op；AGERC 镜像本身是**真实装载**的）
//
// ★ 口径（用户 2026-11-04 改）：AGERC.DLL **作为真实 guest 模块装载** —— PE 节映射进 guest、
//   真实 ImageBase 当 HINSTANCE、资源 / 版本从该镜像解析（见 `Machine::loadAgercModule`）。
//   **只有**这三个导出仍是 `SCOPE.md` §1 的「模态 no-op」stub：
//     · `_GetInstance@0`  → **真实模块基址**（= HINSTANCE；guest 拿它当令牌传给 LoadIconA）
//     · `_ShowDialog@12`  → 1（IDOK：用户没改设置）
//     · `_OperateMenu@16` → 0
//   ⛔ 不再造"假实例 / 假句柄"（2026-11-04 前的口径已废止）。
// ★ 它们分到唯一 guest 地址（shim 桩页；见 ShimRegistry::bind 的 extras 段），
//   所以 guest 的 `call eax`（GetProcAddress 的返回值）与 `call [IAT]` 走同一条 dispatcher 路径。
// ★ ⛔ 不是"静默 no-op"：每个 thunk 都打一行 `[shim] agerc!...` 日志（SCOPE §3 第 3 条）。
#include <cstdint>

#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr const char* kAgerc = "agerc";

void shimAgercGetInstance(X86& cpu, ShimCall& call) {
    const std::uint32_t base = cpu.shim.agerc_base;
    if (base != 0) {
        shimLog(kAgerc, "_GetInstance@0",
                "返回**真实 AGERC 模块基址 0x%08X**（HINSTANCE；PE 已映射进 guest，资源从该镜像解析）",
                base);
        call.result = base;
        return;
    }
    // 没有镜像 ⇒ 真机上这个模块根本不存在 ⇒ 返回 0（⛔ 不再造一个假堆对象）。
    shimLog(kAgerc, "_GetInstance@0",
            "没有装载 AGERC 镜像（agerc_base == 0）⇒ 返回 0（⛔ 不造假实例；用 --agerc 指向拆壳件）");
    call.result = 0;
}

void shimAgercShowDialog(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hwndOwner / lpTemplateName / hInstance（3 个 = 12 字节，stdcall）
    shimLog(kAgerc, "_ShowDialog@12", "模态 no-op ⇒ 返回 1（IDOK，用户没改设置；SCOPE §1）");
    call.result = 1;
}

void shimAgercOperateMenu(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 参数：hwnd / uMsg / wParam / lParam（4 个 = 16 字节，stdcall）
    shimLog(kAgerc, "_OperateMenu@16", "AGERC 菜单操作 no-op ⇒ 返回 0（SCOPE §1）");
    call.result = 0;
}

}  // namespace

void registerAgercShims(ShimRegistry& s) {
    // ★ stdcall 的弹参字节数由 argc 算（0/3/4 ⇒ 0/12/16），与导出名里的 @N 一致。
    s.addStub("agerc", "_GetInstance@0", Conv::Stdcall, 0, &shimAgercGetInstance,
              "SCOPE §1 模态 no-op：AGERC 设置对话框整块不做；_GetInstance 返回**真实模块基址**（HINSTANCE）");
    s.addStub("agerc", "_ShowDialog@12", Conv::Stdcall, 3, &shimAgercShowDialog,
              "SCOPE §1 模态 no-op：AGERC 设置对话框整块不做 ⇒ 返回 IDOK(1)（\"用户没改设置\"）");
    s.addStub("agerc", "_OperateMenu@16", Conv::Stdcall, 4, &shimAgercOperateMenu,
              "SCOPE §1 模态 no-op：AGERC 菜单操作 ⇒ 返回 0");
}

}  // namespace lift
