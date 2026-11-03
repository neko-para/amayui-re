// emulator-lift · 入口（骨架）
//
// 现在只做三件事：
//   1. 初始化 SDL3（证明宿主层链接可用；--smoke 不建窗口）；
//   2. 打印版本；
//   3. 跑一次 dispatcher（空块表 / 一个 demo block），证明 guest 状态与块调度能转起来。
//
// 真正的 boot / Win32 shim / 块表在后续实现单里接。

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

#include "runtime/x86.h"

namespace {

void demo_block(lift::X86& cpu) {
    // 骨架里的唯一一个 block：把 eip 停在原地并结束 dispatcher。
    cpu.running = false;
}

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);

    if (!SDL_Init(smoke ? 0 : SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "emulator-lift: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const int linked = SDL_GetVersion();
    std::printf("emulator-lift 0.1.0 (SDL %d.%d.%d)\n",
                SDL_VERSIONNUM_MAJOR(linked), SDL_VERSIONNUM_MINOR(linked), SDL_VERSIONNUM_MICRO(linked));

    lift::X86 cpu;
    cpu.lookup = [](std::uint32_t) -> lift::X86::Block { return &demo_block; };
    lift::run(cpu);

    SDL_Quit();

    if (smoke) std::printf("smoke ok\n");
    return 0;
}
