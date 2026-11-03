#include "runtime/x86.h"

#include <cstdio>

namespace lift {

X86::X86() : mem(16u * 1024u * 1024u, 0) {}

std::uint32_t X86::rd32(std::uint32_t a) const {
    if (a > mem.size() || mem.size() - a < 4) return 0;
    return static_cast<std::uint32_t>(mem[a]) |
           (static_cast<std::uint32_t>(mem[a + 1]) << 8) |
           (static_cast<std::uint32_t>(mem[a + 2]) << 16) |
           (static_cast<std::uint32_t>(mem[a + 3]) << 24);
}

void X86::wr32(std::uint32_t a, std::uint32_t v) {
    if (a > mem.size() || mem.size() - a < 4) return;
    mem[a] = static_cast<std::uint8_t>(v & 0xFF);
    mem[a + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    mem[a + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    mem[a + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void X86::push(std::uint32_t v) {
    esp -= 4;
    wr32(esp, v);
}

std::uint32_t X86::pop() {
    const std::uint32_t v = rd32(esp);
    esp += 4;
    return v;
}

void X86::call(std::uint32_t ea) {
    push(eip);
    eip = ea;
}

void X86::ret() { eip = pop(); }

void run(X86& cpu) {
    if (!cpu.lookup) return;  // 骨架：还没接上块表就退出（smoke）
    while (cpu.running) {
        const X86::Block block = cpu.lookup(cpu.eip);
        if (!block) {
            std::fprintf(stderr, "emulator-lift: no block @ eip=0x%08X\n", cpu.eip);
            break;
        }
        block(cpu);
    }
}

}  // namespace lift
