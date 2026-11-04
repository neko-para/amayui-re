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
struct PeImage;
class BlockCache;
class LstIndex;
class Vfs;
class ComRegistry;
class GuestCallStack;
class WindowManager;
class FontEngine;

// ★ SMC 钩子：`X86::writeMem` 在类定义之前，不能直接调 `BlockCache::noteWrite`（不完整类型）。
//   `cache == nullptr` 时（测试里没装缓存）是 no-op。
void noteGuestWrite(BlockCache* cache, std::uint32_t ea, std::size_t n);

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

// guest 回调的续延状态（目前只有 `EnumSystemLocalesA` 用；一台 Machine 一份 ⇒ 放进 ShimState）
//
// ★ 机制（为什么需要它）：shim 是"被 dispatcher 调用的宿主函数"，它**不能**在栈里直接调 guest
//   代码。所以把控制权交回去：
//     ① shim 把 guest 栈铺成回调帧（`[esp]` = 续延桩地址，`[esp+4..]` = 参数），把 `cpu.eip`
//        指向 guest 回调，并置 `ShimCall::take_control` ⇒ `invoke()` 不再改写 esp/eip；
//     ② 回调 `ret` 落到**续延桩**（一块 `BlockCache::installShim` 出来的块），它的 shim 读这份状态，
//        决定"下一个回调"还是"恢复原始 esp/eip 返回调用点"。
//   ⛔ 不要用它做"通用异步"：本模拟器没有线程 / APC，这里只是"同步枚举回调"的一条受控通道。
struct GuestCallbackState {
    bool active = false;             // 有未完成的枚举
    std::uint32_t ret_ea = 0;        // 原始调用点的返回地址
    std::uint32_t saved_esp = 0;     // 原始调用点的 esp（指向 ret_ea）
    std::uint32_t cleanup_bytes = 0; // 收尾时要抬的栈字节（= 4 + argc*4，原本由 invoke() 做）
    std::uint32_t callback_ea = 0;   // guest 回调地址
    std::uint32_t scratch_ea = 0;    // 参数缓冲区（guest 堆；由发起方分配）
    std::uint32_t scratch_len = 0;
    std::vector<std::uint32_t> args; // 每次回调的第 0 个参数（指向 scratch 里的一个串）
    std::size_t next = 0;            // 下一个要传的参数下标
    const char* api = nullptr;       // 诊断用（"EnumSystemLocalesA"）
};

// ★ winmm 多媒体定时器的**协作式**实现（D5 单线程：没有定时器服务线程）
//
// 机制：`timeSetEvent` 只把定时器登记进 `ShimState::timers`（到期时刻用**虚拟时钟**算）。
// dispatcher 在**块边界**检查"有没有到期的"；到期就把 guest 栈铺成回调帧、把 eip 指向回调：
//     [esp]     = kTimerCallbackReturnEa（回调的返回地址）
//     [esp+4..] = uID / uMsg(0) / dwUser / dw1(0) / dw2(0)     ← 文档的 TimeProc 五个参数
// 回调 `ret 20` 回到 `kTimerCallbackReturnEa` ⇒ dispatcher 认出它、恢复被打断的 esp/eip，
// 周期定时器重新排队。
// ★ 与真机的差别只有一条：**回调不会抢占一条 basic block**（真机由 winmm 的线程抢占）。
//   在块边界触发 ⇒ 同一个虚拟时钟输入下**结果确定**（A4 的前提），且比"永不触发"诚实。
constexpr std::uint32_t kTimerCallbackReturnEa = 0x7FFEF004u;

struct GuestTimer {
    std::uint32_t id = 0;
    std::uint64_t due_100ns = 0;     // 虚拟时钟的到期时刻
    std::uint32_t period_ms = 0;     // 0 = 一次性
    std::uint32_t callback = 0;      // guest TimeProc
    std::uint32_t user = 0;          // dwUser
    std::uint32_t resolution = 0;
    bool oneshot = true;
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
    std::uint32_t module_size = 0;                // 主模块的映射长度（SizeOfImage；GetModuleHandleEx(FROM_ADDRESS) 用）
    // ★ 2026-11-04 改口径：AGERC.DLL 是**真实装载的 guest 模块**（见 `Machine::loadAgercModule`）。
    //   `agerc_base` 是它真实的 HINSTANCE / ImageBase（节已映射进 guest）；`agerc_image` 指向
    //   那份 `PeImage`（资源 / 版本从这里解析，⛔ 不重读宿主文件）。
    //   两者为 0 / nullptr ⇒ **没有装载 AGERC 镜像**（LoadLibrary 返回 0 + 126，⛔ 不假装成功）。
    const PeImage* agerc_image = nullptr;
    std::uint32_t agerc_base = 0;
    std::uint32_t agerc_instance = 0;             // 旧口径的假实例（2026-11-04 起不再使用；真实 HINSTANCE = agerc_base）
    bool agerc_loaded = false;                    // guest 调过 LoadLibrary("AGERC.DLL") 吗（GetProcAddress 的前提）
    std::uint32_t cmdline_ea = 0;                 // guest 里的命令行
    std::uint32_t exe_path_ea = 0;                // guest 里的 exe 全路径
    std::uint32_t env_ea = 0;                     // guest 里的环境块（UTF-16）
    std::uint32_t va_cursor = 0;                  // VirtualAlloc 的 bump 指针
    std::uint32_t va_limit = 0;
    std::uint32_t exit_code = 0;
    std::uint32_t critical_section_count = 0;      // 统计用
    std::uint32_t unimplemented_calls = 0;         // 缺口计数（A5）
    // ★ alertable 等待的语义缺口只提示一次（无 APC 队列 ⇒ 永不返回 WAIT_IO_COMPLETION）
    bool warned_alertable = false;
    GuestCallbackState callback;                   // guest 回调续延（EnumSystemLocalesA）
    // ★ winmm 定时器（协作式；见上面的 GuestTimer / kTimerCallbackReturnEa）
    std::vector<GuestTimer> timers;
    std::uint32_t next_timer_id = 1;
    bool timer_firing = false;                     // 正在跑某个定时器回调（不嵌套触发）
    std::uint32_t timer_resume_ea = 0;             // 被打断的 eip
    std::uint32_t timer_saved_esp = 0;             // 被打断的 esp
    std::uint32_t timer_firing_id = 0;
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
    // MMX 复用 x87 寄存器槽的**低 64 位**（Intel：MMX 与 x87 共用同一组物理寄存器）；
    // SSE 的 XMM 是独立寄存器文件（32 位 guest 只有 xmm0..7）。按 32 位小端 lane 存：
    // `xmm[i][0]` = 最低 dword。⛔ 不要动已有寄存器 / 内存接口。
    std::uint32_t xmm[8][4] = {};
    std::uint8_t* xmmBytes(int i) { return reinterpret_cast<std::uint8_t*>(xmm[i & 7]); }
    const std::uint8_t* xmmBytes(int i) const { return reinterpret_cast<const std::uint8_t*>(xmm[i & 7]); }
    ShimState shim;

    // ---- 服务（由 Machine 装配；shim 直接从这里取）----
    GuestMemory* mem = nullptr;
    GuestHeap* heap = nullptr;
    HandleTable* handles = nullptr;
    VirtualClock* clock = nullptr;
    ShimRegistry* shims = nullptr;
    Vfs* vfs = nullptr;
    BlockCache* cache = nullptr;
    LstIndex* lst = nullptr;
    const PeImage* image = nullptr;
    ComRegistry* com = nullptr;
    GuestCallStack* calls = nullptr;
    WindowManager* windows = nullptr;
    // ★ P2：宿主字体层（FreeType 桥 + 合成字形接缝）；gdi32 的字体 / 文本 shim 从这里取。
    FontEngine* fonts = nullptr;

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

    std::size_t blockCount() const { return blocks_.size() + shim_blocks_.size(); }
    std::size_t insnCount() const { return insn_count_; }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }
    const char* lastError() const { return last_error_; }
    std::uint32_t lastErrorEa() const { return last_error_ea_; }
    void clear();
    // ★ SMC（自修改代码）：guest 写到"已解码过的代码页"时清空**guest 指令块**，下一次从新字节重新解码。
    //   `X86::writeMem` 每写一次调一次；实现是**一次位图查询**（最多 0x100000 位 = 128 KiB），
    //   堆 / 栈的写会落在"未标记"分支 ⇒ 不影响热路径的量级。
    //   ⛔ shim 块不参与失效（它不在 `blocks_` 里，是导入的唯一地址、不是解出来的代码）。
    void noteWrite(std::uint32_t ea, std::size_t n);

private:
    void markCodePage(std::uint32_t page);
    bool codePageMarked(std::uint32_t page) const;
    void dropBlocks();

    Decoder* dec_ = nullptr;
    GuestMemory* mem_ = nullptr;
    // ★ 两类块**分开两张表**（2026-11-04 修 P0）：`blocks_` = 从 guest 字节解出来的指令块，
    //   可以被 SMC 整张丢掉；`shim_blocks_` = `installShim` 声明的 shim 块，与"哪一页被写过"无关。
    //   放进同一张表就必须在失效时逐条挑出来（O(n)），而且一漏就静默丢 shim（decode-error 误归因）。
    std::unordered_map<std::uint32_t, std::unique_ptr<Block>> blocks_;
    std::unordered_map<std::uint32_t, std::unique_ptr<Block>> shim_blocks_;
    bool stale_ = false;  // 有代码页被写过 ⇒ 下一个 get 先清 guest 块（不能当场清：当前块还在执行）
    std::vector<std::uint64_t> code_page_bits_;  // 已解码块覆盖的 guest 页（4 KiB 粒度）
    std::size_t insn_count_ = 0;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    char last_error_[192] = {};
    std::uint32_t last_error_ea_ = 0;
};

}  // namespace lift
