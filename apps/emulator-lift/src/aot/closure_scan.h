// emulator-lift · 闭包发现的两条**只读** PE 扫描（D26 的第二条 + 守卫共用）
//
// ★ 为什么单独一份头（inline）：`lift_gen.cpp` 的 D26 与守卫 `tests/closure_imm32.cpp`
//   **必须**用同一份判据 —— 两处各写一套扫描的话，"守卫绿"就不再说明"生成器收得到"。
//   （与 `lift_aot_text.h` 的 `execRanges` / `execRangeOf` 同一式。）
//
// ★ D26 的第一条（4 字节对齐 dword）留在 `lift_gen.cpp`：它的语义是"可执行节里的 dword 表"，
//   与这条"指令流里的 imm32"是**两条**扫描（合起来才是 D26 的候选集）。本头只放第二条。
//
// ★ 判据（哪一条是判据、哪一条只是形态证据）：
//   · **可执行节**里**任意偏移**的 `68 id`（`push imm32`）· `B8+rd id`（`mov r32, imm32`）·
//     `C7 /0 id`（`mov r/m32, imm32` 且 `mod=11 / reg=000`）⇒ imm32 按小端读出；
//   · imm32 落在**可执行节**内 ⇒ 候选目标。
//   ⛔ 不扫"任意偏移的任意 4 字节"：只认会携带 imm32 的 opcode，这是最窄的形态证据。
//   ⛔ 不在这里判"能不能解码 / 是不是已发射块"：那是 `offerSoftRoot` 的事（越界 / 解不出 /
//     重叠三档分开记账）。本头只回答"候选是谁"。
//
// ★ 取证（本语料，2026-11-17）：
//   · `0x004F80E0`：`0x4F822A` 的 `push $0x4F80E0`（imm32 在 +1，mod 4 = 3）；
//   · `0x00513B90`：`0x412295` 的 `push offset SEH_412290`（imm32 在 +1，mod 4 = 2）；
//   · `0x0041389A`：`0x413562` 的 `mov eax, offset loc_41389A`（catch(Command_Exit_Exception)
//     的续延；`B8` + imm32，mod 4 = 3）。
#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "aot/lift_aot_text.h"  // ExecRange / execRangeOf
#include "runtime/image.h"

namespace lift {
namespace aot {

struct Imm32Scan {
    std::uint64_t push_sites = 0;     // 扫过的 `68 imm32` 站点数（任意偏移）
    std::uint64_t push_in_exec = 0;   // 其中 imm32 落在可执行节内的
    std::uint64_t mov_sites = 0;      // 扫过的 `B8..BF imm32` / `C7 C0..C7 imm32` 站点数（任意偏移）
    std::uint64_t mov_in_exec = 0;    // 其中 imm32 落在可执行节内的
    std::uint64_t sites = 0;          // 合计站点数
    std::uint64_t in_exec = 0;        // 合计候选站点数
    std::set<std::uint32_t> targets;  // 不同候选目标（升序 ⇒ 确定性）
};

// 读一个 4 字节小端（`p` 必须已有 4 字节可读 —— 调用方按 `i + 5/6 <= n` 保证）。
inline std::uint32_t imm32Le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline Imm32Scan scanInstructionImmediates(const lift::PeImage& image,
                                           const std::vector<ExecRange>& exec_ranges) {
    Imm32Scan out;
    for (const lift::PeSectionInfo& sec : image.sections) {
        if (!sec.executable()) continue;  // 判据：**可执行节**
        const std::uint64_t vsize = sec.vsize != 0 ? sec.vsize : sec.raw_size;
        const std::uint64_t n = std::min<std::uint64_t>(sec.raw_size, vsize);
        if (n < 5) continue;  // 1 字节 opcode + 4 字节 imm32
        const std::uint64_t raw_end = static_cast<std::uint64_t>(sec.raw_offset) + n;
        if (raw_end > image.bytes.size()) continue;  // `loadPeFile` 已全量校验；这里只是不许越界读
        const std::uint8_t* const base = image.bytes.data() + sec.raw_offset;
        // ★ 任意偏移（`++i`，⛔ 不是 `i += 4`）：这正是"未对齐立即数"那句话的机械含义。
        for (std::uint64_t i = 0; i + 5 <= n; ++i) {
            const std::uint8_t op = base[i];
            std::uint64_t imm_at = 0;
            bool is_push = false;
            if (op == 0x68) {
                // `push imm32`（Intel SDM：68 id）
                imm_at = i + 1;
                is_push = true;
            } else if (op >= 0xB8 && op <= 0xBF) {
                // `mov r32, imm32`（Intel SDM：B8+rd id）。★ 这一类是 `catch` 续延：
                // 本语料 `0x413562` 的 `mov eax, offset loc_41389A`（catch(Command_Exit_Exception)
                // 的续延地址，由 `retn` 交给 `___CxxFrameHandler3` 再跳过去）⇒ 与 push 同一类。
                imm_at = i + 1;
            } else if (op == 0xC7 && i + 6 <= n && (base[i + 1] & 0xC0) == 0xC0 &&
                       (base[i + 1] & 0x38) == 0x00) {
                // `mov r/m32, imm32` 且 ModRM `mod=11 / reg=000`（= `mov r32, imm32` 的另一种编码）
                imm_at = i + 2;
            } else {
                continue;
            }
            const std::uint32_t value = imm32Le32(base + imm_at);
            if (is_push) {
                ++out.push_sites;
            } else {
                ++out.mov_sites;
            }
            ++out.sites;
            if (execRangeOf(exec_ranges, value) == nullptr) continue;
            if (is_push) {
                ++out.push_in_exec;
            } else {
                ++out.mov_in_exec;
            }
            ++out.in_exec;
            out.targets.insert(value);
        }
    }
    return out;
}

}  // namespace aot
}  // namespace lift
