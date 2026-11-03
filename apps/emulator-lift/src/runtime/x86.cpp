// emulator-lift · guest 寄存器 / 操作数 / 栈 的公共实现 + 块缓存
//
// ★ 8 位高半（AH/CH/DH/BH）与 16 位的**合并规则**只在这里写一次：
//   写 AL 保留高 24 位，写 AH 只改 bits 8..15，写 AX 保留高 16 位。
//   ⛔ 语义层（exec_*.cpp）不要自己拼字节 —— 那样六条路径会各错一次。
// ★ 有效地址 = base + index*scale + disp（`fs:` / `gs:` 前缀加的是 fs_base / gs_base，
//   ⛔ 不是宿主段寄存器：guest 的 fs 指向 guest TEB）。
#include "runtime/x86.h"

#include <cstdio>
#include <cstring>

#include "runtime/exec.h"

namespace lift {

void X87State::reset() {
    std::memset(regs, 0, sizeof(regs));
    control = 0x037Fu;
    status = 0;
    top = 0;
    tag = 0xFFFFu;
}

std::uint32_t X86::readReg(Reg reg) const {
    const RegSpec spec = regSpec(reg);
    switch (spec.cls) {
        case RegClass::Gpr32: return r[spec.index];
        case RegClass::Gpr16: return r[spec.index] & 0xFFFFu;
        case RegClass::Gpr8: return (r[spec.index] >> (8 * spec.byte_off)) & 0xFFu;
        case RegClass::Seg: return seg[spec.index];
        case RegClass::X87: {
            // 只有 fstsw / fxch 之类会读 x87 寄存器的"整数值"；这里返回低 4 字节的原始位
            // （真正需要 80 位值的语义在 exec_fpu.cpp 里走 fpu_bridge）
            const std::uint8_t* p = fpu.regs[fpu.phys(spec.index)];
            std::uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        case RegClass::None:
        default: return 0;
    }
}

void X86::writeReg(Reg reg, std::uint32_t value) {
    const RegSpec spec = regSpec(reg);
    switch (spec.cls) {
        case RegClass::Gpr32: r[spec.index] = value; break;
        case RegClass::Gpr16: r[spec.index] = (r[spec.index] & 0xFFFF0000u) | (value & 0xFFFFu); break;
        case RegClass::Gpr8: {
            const unsigned shift = 8u * spec.byte_off;
            r[spec.index] = (r[spec.index] & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
            break;
        }
        case RegClass::Seg: seg[spec.index] = static_cast<std::uint16_t>(value & 0xFFFFu); break;
        case RegClass::X87: {
            std::uint8_t* p = fpu.regs[fpu.phys(spec.index)];
            std::memcpy(p, &value, 4);  // 只覆盖低 32 位：调用方要用 80 位时走 fpu_bridge
            break;
        }
        case RegClass::None:
        default: break;
    }
}

std::uint32_t X86::eaOf(const MemOperand& m) const {
    std::uint32_t ea = m.disp;
    if (m.base != Reg::None) ea += r[regSpec(m.base).index];
    if (m.index != Reg::None) ea += r[regSpec(m.index).index] * static_cast<std::uint32_t>(m.scale);
    if (m.seg_fs && fs_base) ea += fs_base;
    if (m.seg_gs && gs_base) ea += gs_base;
    return ea;
}

std::uint32_t X86::readMem(const MemOperand& m) const {
    const std::uint32_t a = eaOf(m);
    switch (m.size) {
        case 1: return mem->rd8(a);
        case 2: return mem->rd16(a);
        case 4: return mem->rd32(a);
        default:
            if (m.size == 8) return static_cast<std::uint32_t>(mem->rd64(a) & 0xFFFFFFFFu);
            return mem->rd32(a);
    }
}

void X86::writeMem(const MemOperand& m, std::uint32_t value) {
    const std::uint32_t a = eaOf(m);
    switch (m.size) {
        case 1: mem->wr8(a, static_cast<std::uint8_t>(value & 0xFFu)); break;
        case 2: mem->wr16(a, static_cast<std::uint16_t>(value & 0xFFFFu)); break;
        case 8: mem->wr64(a, value); break;
        case 4:
        default: mem->wr32(a, value); break;
    }
}

std::uint32_t X86::readOp(const Operand& op) const {
    switch (op.kind) {
        case OpKind::Reg: return readReg(op.reg);
        case OpKind::Imm: return static_cast<std::uint32_t>(op.imm);
        case OpKind::Mem: return readMem(op.mem);
        case OpKind::None:
        default: return 0;
    }
}

void X86::writeOp(const Operand& op, std::uint32_t value) {
    switch (op.kind) {
        case OpKind::Reg: writeReg(op.reg, value); break;
        case OpKind::Mem: writeMem(op.mem, value); break;
        case OpKind::Imm: break;  // 写立即数非法；上游 decode 不会产生
        case OpKind::None:
        default: break;
    }
}

std::int32_t X86::readOpS32(const Operand& op) const {
    const std::uint32_t v = readOp(op);
    switch (op.size) {
        case 1: return static_cast<std::int32_t>(static_cast<std::int8_t>(v & 0xFFu));
        case 2: return static_cast<std::int32_t>(static_cast<std::int16_t>(v & 0xFFFFu));
        default: return static_cast<std::int32_t>(v);
    }
}

std::int32_t X86::readMemS32(const MemOperand& m) const {
    const std::uint32_t v = readMem(m);
    switch (m.size) {
        case 1: return static_cast<std::int32_t>(static_cast<std::int8_t>(v & 0xFFu));
        case 2: return static_cast<std::int32_t>(static_cast<std::int16_t>(v & 0xFFFFu));
        default: return static_cast<std::int32_t>(v);
    }
}

void X86::push32(std::uint32_t v) {
    r[kEsp] -= 4;
    mem->wr32(r[kEsp], v);
}

std::uint32_t X86::pop32() {
    const std::uint32_t v = mem->rd32(r[kEsp]);
    r[kEsp] += 4;
    return v;
}

void X86::pushFlags() { push32(eflags); }
void X86::popFlags() { eflags = pop32() | FLAG_IF; }

// ---------------------------------------------------------------------------------------
// BlockCache
// ---------------------------------------------------------------------------------------
Block* BlockCache::get(std::uint32_t ea) {
    const auto it = blocks_.find(ea);
    if (it != blocks_.end()) {
        ++hits_;
        return it->second.get();
    }
    ++misses_;
    auto block = std::make_unique<Block>();
    std::string why;
    if (!dec_->decodeBlock(*mem_, ea, block.get(), &why)) {
        last_error_ea_ = ea;
        std::snprintf(last_error_, sizeof(last_error_), "%s", why.c_str());
        return nullptr;
    }
    insn_count_ += block->insns.size();
    Block* raw = block.get();
    blocks_.emplace(ea, std::move(block));
    return raw;
}

Block* BlockCache::installShim(std::uint32_t ea, std::uint32_t shim_id) {
    if (const auto it = blocks_.find(ea); it != blocks_.end()) return it->second.get();
    auto block = std::make_unique<Block>();
    block->ea = ea;
    block->is_shim = true;
    block->shim_id = shim_id;
    block->terminated = true;
    Block* raw = block.get();
    blocks_.emplace(ea, std::move(block));
    return raw;
}

Block* BlockCache::peek(std::uint32_t ea) const {
    const auto it = blocks_.find(ea);
    return it == blocks_.end() ? nullptr : it->second.get();
}

void BlockCache::clear() {
    blocks_.clear();
    insn_count_ = 0;
    hits_ = 0;
    misses_ = 0;
}

}  // namespace lift
