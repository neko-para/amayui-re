// emulator-lift · guest 状态 + 块表接口（骨架）
//
// ★ 执行单位是 **basic block**，不是 `sub_`：
//   - `.lst` 里有跨函数 `jmp sub_*`、function chunk、跳转表 / 间接 jmp；
//   - 所以 `jmp/call/ret` 只改 `eip` 并回 dispatcher，C 函数不跨函数 `goto`。
//   - `sub_` 只是"入口块 + 内部块 + 外部入口块"的聚合视图（报告 / 覆盖边界用）。
//
// ★ 生成器将来产出 `bb_<EA>(X86&)` 与 `table[EA]`；本文件只钉接口，不实现 lift。
//
// ★ guest 内存是**独立地址空间**：宿主指针一律不写进 guest 槽（重定位 / 64 位指针都不成立）。
//   真实实现用分页 / 稀疏映射；骨架先用一块连续缓冲，够 smoke 用。

#pragma once

#include <cstdint>
#include <vector>

namespace lift {

struct X86 {
    using Block = void (*)(X86&);

    // guest 寄存器（32 位；宿主是 64 位也不影响）
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    std::uint32_t esi = 0, edi = 0, ebp = 0, esp = 0, eip = 0;

    // x86 标志位（生成代码显式读写；不依赖宿主 flags）
    std::uint32_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0;

    bool running = true;

    // guest 内存（骨架：连续 16 MiB；真实实现 = 4 GB 预留 + 按需提交，见 README §2.6）
    std::vector<std::uint8_t> mem;

    // EA -> block（生成器产出；nullptr = 还没接上）
    Block (*lookup)(std::uint32_t) = nullptr;

    X86();

    std::uint32_t rd32(std::uint32_t a) const;
    void wr32(std::uint32_t a, std::uint32_t v);

    // stdcall / cdecl 的第 i 个栈参数（i 从 0 起；返回地址在 esp）
    std::uint32_t arg(int i) const { return rd32(esp + 4 + 4 * static_cast<std::uint32_t>(i)); }

    void push(std::uint32_t v);
    std::uint32_t pop();

    // `call`：压返回地址、把 eip 指向目标；目标由 dispatcher 执行。
    void call(std::uint32_t ea);
    // `ret`：弹返回地址。
    void ret();
};

// dispatcher：只做 `lookup(eip) -> block`，block 内部不许跨函数 goto。
void run(X86& cpu);

}  // namespace lift
