// emulator-lift · guest CPU 状态 + 寄存器 / 操作数 / 栈 helper + 块缓存
//
// ★ 执行单位是 **basic block**，不是 `sub_`（README §2.2）：`jmp/call/ret` 只改 `eip` 并回 dispatcher。
// ★ 块由**运行时解码**填充（`table[EA]`），本线默认**不产出任何生成代码**（D2）。
// ★ guest 内存是独立地址空间（`GuestMemory`）；⛔ 宿主指针不进 guest 槽。
// ★ x87 寄存器存 **softfloat extFloat80 的原位字节**（10 字节有效 + 6 字节填充）——
//   这是 D3 的落点：算术走 vendor 的 SoftFloat，我们只负责栈 / tag / 控制字 / 条件码。
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/clock.h"
#include "runtime/decode.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/insn.h"
#include "runtime/memory.h"
#include "runtime/regs.h"
#include "runtime/shims.h"

namespace lift {

struct X86;
class BlockCache;
class LstIndex;

// x87 状态（8 寄存器栈 + 控制/状态字 + TOP + tag）
struct X87State {
    // 每个寄存器 16 字节：softfloat `extFloat80_t`（signExp 2 字节 + significand 8 字节）原位放
    std::uint8_t regs[8][16] = {};
    std::uint16_t control = 0x037Fu;  // 缺省控制字（x87 reset 值）
    std::uint16_t status = 0;         // 状态字（异常位 + C0..C3 + TOP）
    std::uint8_t top = 0;             // 0..7
    std::uint16_t tag = 0xFFFFu;      // 每 2 位一个寄存器（8 个 = 16 位）：0=valid 1=zero 2=special 3=empty

    void setEmpty(int i) { tag = static_cast<std::uint16_t>((tag & ~(3u << (2 * i))) | (3u << (2 * i))); }
    void setValid(int i) { tag = static_cast<std::uint16_t>(tag & ~(3u << (2 * i))); }
    void setZero(int i) { tag = static_cast<std::uint16_t>((tag & ~(3u << (2 * i))) | (1u << (2 * i))); }
    bool empty(int i) const { return ((tag >> (2 * i)) & 3u) == 3u; }
    int phys(int i) const { return (top + i) & 7; }  // ST(i) → 物理槽
    void reset();
};

// shim 需要跨调用保存的**进程级**状态（⛔ 不要用 C++ 静态变量：一台机器一份）。
struct ShimState {
    std::uint32_t last_error = 0;        // GetLastError / SetLastError
    std::uint32_t process_id = 0x00001000u;
    std::uint32_t thread_id = 0x00001004u;
    std::uint32_t process_handle = 0xFFFFFFFFu;   // 伪句柄
    std::uint32_t thread_handle = 0xFFFFFFFEu;
    std::uint32_t heap_handle = 0x00010000u;      // GetProcessHeap 的返回
    std::uint32_t module_handle = 0;              // 主模块（= image_base）
    std::uint32_t cmdline_ea = 0;                 // guest 里的命令行
    std::uint32_t exe_path_ea = 0;                // guest 里的 exe 全路径
    std::uint32_t va_cursor = 0;                  // VirtualAlloc 的 bump 指针
    std::uint32_t va_limit = 0;
    std::uint32_t exit_code = 0;
    std::uint32_t critical_section_count = 0;      // 统计用
    std::uint32_t unimplemented_calls = 0;         // 缺口计数（A5）
};

struct X86 {
    // ---- 通用寄存器（下标见 GprIndex）----
    std::uint32_t r[8] = {};   // EAX ECX EDX EBX ESP EBP ESI EDI
    std::uint32_t eip = 0;
    std::uint32_t eflags = 0x202u;  // IF=1
    std::uint16_t seg[6] = {};      // ES CS SS DS FS GS（选择子）
    std::uint32_t fs_base = 0;      // guest TEB（⛔ 不是宿主 fs）
    std::uint32_t gs_base = 0;
    X87State fpu;
    ShimState shim;

    // ---- 服务（由 Machine 装配；shim 直接从这里取）----
    GuestMemory* mem = nullptr;
    GuestHeap* heap = nullptr;
    HandleTable* handles = nullptr;
    VirtualClock* clock = nullptr;
    ShimRegistry* shims = nullptr;
    BlockCache* cache = nullptr;
    LstIndex* lst = nullptr;

    // ---- 最近执行过的指令（诊断用环形缓冲；停在 eip=0 这类地方时能看见"从哪来"）----
    static constexpr int kLastInsns = 24;
    std::uint32_t last_eip[kLastInsns] = {};
    const char* last_mnemonic[kLastInsns] = {};
    std::uint32_t last_flags[kLastInsns] = {};
    int last_count = 0;
    int last_head = 0;
    void noteInsn(std::uint32_t ea, const char* mn) {
        last_eip[last_head] = ea;
        last_mnemonic[last_head] = mn;
        last_flags[last_head] = eflags;
        last_head = (last_head + 1) % kLastInsns;
        if (last_count < kLastInsns) ++last_count;
    }

    // ---- 运行状态 ----
    bool running = true;
    std::uint64_t insns = 0;      // 已执行指令数（确定性 trace / bench 用）
    const char* stop_why = nullptr;

    // ---- 命名寄存器便捷访问 ----
    std::uint32_t& eax() { return r[kEax]; }
    std::uint32_t& ecx() { return r[kEcx]; }
    std::uint32_t& edx() { return r[kEdx]; }
    std::uint32_t& ebx() { return r[kEbx]; }
    std::uint32_t& esp() { return r[kEsp]; }
    std::uint32_t& ebp() { return r[kEbp]; }
    std::uint32_t& esi() { return r[kEsi]; }
    std::uint32_t& edi() { return r[kEdi]; }
    std::uint32_t eax() const { return r[kEax]; }
    std::uint32_t ecx() const { return r[kEcx]; }
    std::uint32_t edx() const { return r[kEdx]; }
    std::uint32_t ebx() const { return r[kEbx]; }
    std::uint32_t esp() const { return r[kEsp]; }
    std::uint32_t ebp() const { return r[kEbp]; }
    std::uint32_t esi() const { return r[kEsi]; }
    std::uint32_t edi() const { return r[kEdi]; }

    // ---- 标志位 ----
    bool flag(std::uint32_t f) const { return (eflags & f) != 0; }
    void set(std::uint32_t f, bool on) { if (on) eflags |= f; else eflags &= ~f; }
    bool cf() const { return flag(FLAG_CF); }
    bool pf() const { return flag(FLAG_PF); }
    bool af() const { return flag(FLAG_AF); }
    bool zf() const { return flag(FLAG_ZF); }
    bool sf() const { return flag(FLAG_SF); }
    bool df() const { return flag(FLAG_DF); }
    bool of() const { return flag(FLAG_OF); }
    void setCf(bool v) { set(FLAG_CF, v); }
    void setPf(bool v) { set(FLAG_PF, v); }
    void setAf(bool v) { set(FLAG_AF, v); }
    void setZf(bool v) { set(FLAG_ZF, v); }
    void setSf(bool v) { set(FLAG_SF, v); }
    void setDf(bool v) { set(FLAG_DF, v); }
    void setOf(bool v) { set(FLAG_OF, v); }

    // ---- 寄存器读写（8 位高半 / 16 位合并规则统一在这里）----
    std::uint32_t readReg(Reg reg) const;
    void writeReg(Reg reg, std::uint32_t value);

    // ---- 操作数读写（含 guest 内存；`fs:` / `gs:` 前缀走 fs_base / gs_base）----
    std::uint32_t eaOf(const MemOperand& m) const;
    std::uint32_t readMem(const MemOperand& m) const;
    void writeMem(const MemOperand& m, std::uint32_t value);
    std::uint32_t readOp(const Operand& op) const;
    void writeOp(const Operand& op, std::uint32_t value);

    // 有符号读到 64 位（imul / idiv / movsx 用）
    std::int32_t readOpS32(const Operand& op) const;
    std::int32_t readMemS32(const MemOperand& m) const;

    // ---- 栈 ----
    void push32(std::uint32_t v);
    std::uint32_t pop32();
    void pushFlags();
    void popFlags();
    // 第 i 个栈参数（返回地址在 `[esp]`）
    std::uint32_t arg(int i) const { return mem->rd32(esp() + 4 + 4u * static_cast<std::uint32_t>(i)); }

    // inline 缓存命中 / 未命中统计（bench 用）
};

// ---- 块缓存（懒解码；`table[EA]` 的实体）----
class BlockCache {
public:
    BlockCache(Decoder* dec, GuestMemory* mem) : dec_(dec), mem_(mem) {}

    // 取块；没有就解码并缓存。失败 ⇒ nullptr（`lastError()` 说明原因）
    Block* get(std::uint32_t ea);
    // 安装一个 shim 块（S5：导入的唯一 guest 地址）
    Block* installShim(std::uint32_t ea, std::uint32_t shim_id);
    // 是否已缓存
    Block* peek(std::uint32_t ea) const;

    std::size_t blockCount() const { return blocks_.size(); }
    std::size_t insnCount() const { return insn_count_; }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }
    const char* lastError() const { return last_error_; }
    std::uint32_t lastErrorEa() const { return last_error_ea_; }
    void clear();

private:
    Decoder* dec_ = nullptr;
    GuestMemory* mem_ = nullptr;
    std::unordered_map<std::uint32_t, std::unique_ptr<Block>> blocks_;
    std::size_t insn_count_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    char last_error_[192] = {};
    std::uint32_t last_error_ea_ = 0;
};

}  // namespace lift
