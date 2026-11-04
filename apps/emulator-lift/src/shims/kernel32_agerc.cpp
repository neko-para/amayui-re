// emulator-lift · AGERC.DLL 的三个导出（**整体 stub**，用户 2026-11-03 明示）
//
// ★ 口径（用户裁决）：**不要**实现真模块装载 / 解析 AGERC 的 PE。
//   做法：`LoadLibraryA/W("AGERC.DLL")` 返回**确定性假句柄** + 一行日志；
//   `GetProcAddress` 对它 3 个导出返回**三个 stub thunk 的 guest 地址**；
//   thunk 按 `SCOPE.md` §1「模态 no-op」返回合理值：
//     · `_GetInstance@0`  → 非空假指针（guest 只当不透明令牌；真去解引用它会 fault = 响亮）
//     · `_ShowDialog@12`  → 1（IDOK：用户没改设置）
//     · `_OperateMenu@16` → 0
//   每条都在 `data/lift/overrides.json` 登记 `kind: stub` + reason（`addStub` ⇒ 清单自动出现）。
// ★ 它们分到唯一 guest 地址（shim 桩页；见 ShimRegistry::bind 的 extras 段），
//   所以 guest 的 `call eax`（GetProcAddress 的返回值）与 `call [IAT]` 走同一条 dispatcher 路径。
// ★ ⛔ 不是"静默 no-op"：每个 thunk 都打一行 `[shim] agerc!...` 日志（SCOPE §3 第 3 条）。
#include <cstdint>
#include <string>

#include "runtime/cp932.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr const char* kAgerc = "agerc";

// 返回一个**非空但无意义**的 guest 对象。16 字节清零、从 guest 堆分配 ⇒ 确定性地址
// （纯档夹具里 heap 未初始化时退化为固定的、已提交的 TEB 页后面那块哨兵地址）。
std::uint32_t fakeInstance(X86& cpu) {
    if (cpu.shim.agerc_instance) return cpu.shim.agerc_instance;
    if (cpu.heap && cpu.heap->ready()) {
        cpu.shim.agerc_instance = cpu.heap->alloc(16, true);
    }
    if (cpu.shim.agerc_instance == 0) {
        // 兜底：栈顶下面那 16 字节（已提交、不会被 guest 的 esp 用到）；只作为不透明令牌。
        cpu.shim.agerc_instance = cpu.esp() - 0x40u;
        if (cpu.mem->committed(cpu.shim.agerc_instance, 16)) {
            cpu.mem->fillBytes(cpu.shim.agerc_instance, 0, 16);
        } else {
            cpu.shim.agerc_instance = 0;
        }
    }
    return cpu.shim.agerc_instance;
}

void shimAgercGetInstance(X86& cpu, ShimCall& call) {
    const std::uint32_t p = fakeInstance(cpu);
    if (p == 0) shimAbort(kAgerc, "_GetInstance@0", "拿不到一个已提交的假实例地址");
    shimLog(kAgerc, "_GetInstance@0", "返回非空假实例 0x%08X（SCOPE §1 模态 no-op；不加载 AGERC 的 PE）", p);
    call.result = p;
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
              "SCOPE §1 模态 no-op：AGERC 设置对话框整块不做；_GetInstance 返回非空假指针（不透明令牌）");
    s.addStub("agerc", "_ShowDialog@12", Conv::Stdcall, 3, &shimAgercShowDialog,
              "SCOPE §1 模态 no-op：AGERC 设置对话框整块不做 ⇒ 返回 IDOK(1)（\"用户没改设置\"）");
    s.addStub("agerc", "_OperateMenu@16", Conv::Stdcall, 4, &shimAgercOperateMenu,
              "SCOPE §1 模态 no-op：AGERC 菜单操作 ⇒ 返回 0");
}

}  // namespace lift
