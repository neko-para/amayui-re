// emulator-lift · S7（D53）：结构翻译判据的实现（见 `lift_struct.h` 的口径与理由）
//
// ★ 这里是"**按函数性质**"的机械判定：每一条都只看 `Insn` 的 op / 操作数形态与 CFG 数据流，
//   ⛔ 不看 EA 的数值、⛔ 不看助记符字符串、⛔ 不查 `.lst`。⇒ 换一份 PE 也只按同一套判据跑。
// ★ 单调性：判据只回答"能不能证明"，证明不了就**不结构化**（回退成今天的 `ret i32` 派发语义）。
#include "aot/lift_struct.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace lift {
namespace aot {
namespace {

// esp / ebp 相对**入口**的偏移（字节）。`UNKNOWN` = 证明不了（保守）。
constexpr std::int32_t kUnknown = 0x7FFFFFFF;

struct EspState {
    std::int32_t esp = 0;
    std::int32_t ebp = kUnknown;
};

bool isEsp32(const Operand& o) {
    return o.kind == OpKind::Reg && regSpec(o.reg).cls == RegClass::Gpr32 &&
           regSpec(o.reg).index == kEsp;
}
bool isEbp32(const Operand& o) {
    return o.kind == OpKind::Reg && regSpec(o.reg).cls == RegClass::Gpr32 &&
           regSpec(o.reg).index == kEbp;
}
// 16 位的 SP / BP 也动同一格（只改低 16 位）⇒ 一律当"不可静态跟踪"（保守）。
bool isSpOrBp16(const Operand& o) {
    if (o.kind != OpKind::Reg || regSpec(o.reg).cls != RegClass::Gpr16) return false;
    const std::uint8_t i = regSpec(o.reg).index;
    return i == kEsp || i == kEbp;
}

// 这条指令会不会**写** op0（op0 是内存操作数时才是"写返回槽"的候选）。
// 判据 = 该 op 的目的操作数是 op0 且可以落到内存上。⛔ 不按助记符字符串判。
bool writesOp0Memory(Op op) {
    switch (op) {
        case Op::Mov:
        case Op::Movzx:
        case Op::Movsx:
        case Op::Add:
        case Op::Or:
        case Op::And:
        case Op::Sub:
        case Op::Xor:
        case Op::Inc:
        case Op::Dec:
        case Op::Neg:
        case Op::Not:
        case Op::Adc:
        case Op::Sbb:
        case Op::Shl:
        case Op::Shr:
        case Op::Sar:
        case Op::Rol:
        case Op::Ror:
        case Op::Rcl:
        case Op::Rcr:
        case Op::Shld:
        case Op::Shrd:
        case Op::Xadd:
        case Op::Cmpxchg:
        case Op::Bt:  // `bt` 只读；这里保守算写（bt 不写内存，但 `bts/btr/btc` 写）
        case Op::Bts:
        case Op::Btr:
        case Op::Btc:
        case Op::Pop:
        case Op::Packed:
        case Op::Sse:
        case Op::Movs:
        case Op::Stos:
            return true;
        default: return false;
    }
}

// 返回地址那一格（**语法**判据，D53 §3）：`[esp+0]`（esp 尚未被本函数改动时）或 `[ebp+4]`。
// ⚠ `[esp+0]` 在函数中段可能只是"刚 push 的实参槽"（D54 §2.1 的 false positive 正例）⇒ 保守：
//   只要**任何**一条写 `[esp+0]` 就把整函数降级（单调、可回退）。
bool isReturnSlotForm(const MemOperand& m) {
    if (m.absolute || m.seg_fs || m.seg_gs) return false;
    if (m.index != Reg::None) return false;
    if (m.base == Reg::ESP && m.disp == 0) return true;
    if (m.base == Reg::EBP && m.disp == 4) return true;
    return false;
}

// 一条指令对 esp/ebp 的转移。返回 false = 证明不了（该函数不可结构化），`why` 给首因。
bool transferEsp(const Insn& in, EspState* s, StructReason* why) {
    // ---- 写 SP / BP 的 16 位形态：一律不可跟踪 ----
    if (isSpOrBp16(in.op0())) {
        *why = StructReason::VarStack;
        return false;
    }
    // ---- xchg 碰到 esp/ebp ----
    if (in.op == Op::Xchg && (isEsp32(in.op0()) || isEsp32(in.op1()))) {
        *why = StructReason::VarStack;
        return false;
    }
    // ---- 目的操作数是 ESP 的各种形态 ----
    if (isEsp32(in.op0())) {
        switch (in.op) {
            case Op::Mov:
                if (in.op1().kind == OpKind::Reg && regSpec(in.op1().reg).cls == RegClass::Gpr32) {
                    const std::uint8_t src = regSpec(in.op1().reg).index;
                    if (src == kEsp) return true;  // mov esp,esp = 恒等（`lea esp,[esp+0]` 的兄弟）
                    if (src == kEbp) {             // 帧恢复：esp = ebp
                        if (s->ebp == kUnknown) {
                            *why = StructReason::DynamicReturn;
                            return false;
                        }
                        s->esp = s->ebp;
                        return true;
                    }
                }
                *why = StructReason::VarStack;  // mov esp, imm / [mem] / 其它寄存器
                return false;
            case Op::Lea: {
                const MemOperand& m = in.op1().mem;
                if (m.absolute || m.seg_fs || m.seg_gs || m.index != Reg::None) {
                    *why = StructReason::VarStack;
                    return false;
                }
                const std::int32_t d = static_cast<std::int32_t>(m.disp);
                if (m.base == Reg::ESP) {
                    s->esp = static_cast<std::int32_t>(s->esp + d);
                    return true;
                }
                if (m.base == Reg::EBP) {
                    if (s->ebp == kUnknown) {
                        *why = StructReason::DynamicReturn;
                        return false;
                    }
                    s->esp = static_cast<std::int32_t>(s->ebp + d);
                    return true;
                }
                *why = StructReason::VarStack;  // lea esp,[其它寄存器+…]
                return false;
            }
            case Op::Add:
            case Op::Sub:
                if (in.op1().kind == OpKind::Imm) {
                    const std::int32_t d = static_cast<std::int32_t>(
                        static_cast<std::uint32_t>(in.op1().imm));
                    s->esp = static_cast<std::int32_t>(
                        s->esp + (in.op == Op::Add ? d : -d));
                    return true;
                }
                *why = StructReason::VarStack;  // add/sub esp,reg / [mem]
                return false;
            default:
                // and/or/xor/inc/dec/shl/… 写 esp ⇒ esp 不再可静态跟踪（对齐 / 运行期大小）
                *why = StructReason::VarStack;
                return false;
        }
    }
    // ---- 目的操作数是 EBP（只为帧恢复跟踪）----
    if (isEbp32(in.op0())) {
        switch (in.op) {
            case Op::Mov:
                if (in.op1().kind == OpKind::Reg && regSpec(in.op1().reg).cls == RegClass::Gpr32 &&
                    regSpec(in.op1().reg).index == kEsp) {
                    s->ebp = s->esp;
                    return true;
                }
                break;
            case Op::Lea: {
                const MemOperand& m = in.op1().mem;
                if (!m.absolute && !m.seg_fs && !m.seg_gs && m.index == Reg::None) {
                    const std::int32_t d = static_cast<std::int32_t>(m.disp);
                    if (m.base == Reg::ESP) {
                        s->ebp = static_cast<std::int32_t>(s->esp + d);
                        return true;
                    }
                    if (m.base == Reg::EBP && s->ebp != kUnknown) {
                        s->ebp = static_cast<std::int32_t>(s->ebp + d);
                        return true;
                    }
                }
                break;
            }
            default: break;
        }
        s->ebp = kUnknown;  // 其它写 EBP 的形态 ⇒ 帧基准没了（保守）
        return true;
    }
    // ---- 隐式改 esp 的形态 ----
    switch (in.op) {
        case Op::Push: s->esp = static_cast<std::int32_t>(s->esp - 4); return true;
        case Op::Pushfd: s->esp = static_cast<std::int32_t>(s->esp - 4); return true;
        case Op::Pushad: s->esp = static_cast<std::int32_t>(s->esp - 32); return true;
        case Op::Pop: {
            if (isEsp32(in.op0())) {  // pop esp：值来自栈 ⇒ 不可跟踪
                *why = StructReason::VarStack;
                return false;
            }
            s->esp = static_cast<std::int32_t>(s->esp + 4);
            if (isEbp32(in.op0())) s->ebp = kUnknown;
            return true;
        }
        case Op::Popfd: s->esp = static_cast<std::int32_t>(s->esp + 4); return true;
        case Op::Popad: s->esp = static_cast<std::int32_t>(s->esp + 32); return true;
        case Op::Leave: {
            // leave = mov esp,ebp; pop ebp（D53 明确：帧恢复不算失效）
            if (s->ebp == kUnknown) {
                *why = StructReason::DynamicReturn;
                return false;
            }
            s->esp = s->ebp;
            s->ebp = kUnknown;
            return true;
        }
        default: return true;
    }
}

// 被调方 `retn imm16` 的立即数（= caller 在 `call` 之后要多加的字节）。
// 口径：该单元里所有 `ret` 的立即数**唯一** ⇒ 用它；不唯一（共享 epilogue / 多个退出点）
// ⇒ 0（cdecl 口径）—— 函数序言/尾声的 `mov esp,ebp` 会把偏移**重新对齐**，所以 0 是安全的
// 兜底（错了只会让"证明不了"落在 ret 处 ⇒ 降级，⛔ 不会把它判成可结构化）。
const char* structReasonNameImpl(StructReason r) {
    switch (r) {
        case StructReason::None: return "none";
        case StructReason::VarStack: return "可变长栈";
        case StructReason::ReturnSlotWrite: return "写返回槽";
        case StructReason::IndirectCall: return "间接 call";
        case StructReason::DynamicReturn: return "动态返回（ret 的 esp 偏移不可知/非 0）";
        case StructReason::UnknownCallee: return "静态 call 目标不是已发射单元";
        case StructReason::CalleeCascade: return "调用图传染";
    }
    return "?";
}

}  // namespace

const char* structReasonName(StructReason r) { return structReasonNameImpl(r); }

StructResult analyzeStructure(const std::vector<GenComponent>& comps,
                              const std::map<std::uint32_t, std::uint32_t>& entry_owner,
                              const std::set<std::uint32_t>& dropped, std::uint32_t shim_begin,
                              std::uint32_t shim_end) {
    StructResult out;
    StructStats& st = out.stats;
    // `dropped` 目前只用于"跨单元边"的语义（`c.blocks.count(t) == 0` 已覆盖）；
    // 显式留参是为了让调用方**必须**把"哪些 EA 没有块"这份事实递进来（将来收严判据时用）。
    (void)dropped;

    // ---- ① 每个单元的 `retn imm16`（call 清理建模的输入）----
    std::map<std::uint32_t, std::int32_t> ret_cleanup;
    for (const GenComponent& c : comps) {
        std::optional<std::uint16_t> uniq;
        bool multi = false;
        for (const auto& kv : c.blocks) {
            for (const Insn& in : kv.second.insns) {
                if (in.op != Op::Ret) continue;
                const std::uint16_t imm =
                    in.op0().kind == OpKind::Imm
                        ? static_cast<std::uint16_t>(static_cast<std::uint32_t>(in.op0().imm) & 0xFFFFu)
                        : 0u;
                if (!uniq.has_value()) uniq = imm;
                else if (*uniq != imm) multi = true;
            }
        }
        ret_cleanup[c.root] = (uniq.has_value() && !multi) ? *uniq : 0;
    }

    // ---- ② 自身分析：esp 数据流 + 返回槽写 + 间接 call + 调用图边 ----
    std::map<std::uint32_t, std::set<std::uint32_t>> callees;  // root → 静态 callee roots
    std::map<std::uint32_t, StructReason> self_reason;

    for (const GenComponent& c : comps) {
        ++st.functions;
        StructReason why = StructReason::None;
        std::uint32_t why_ea = 0;
        std::set<std::uint32_t> cs;
        bool self_ok = true;

        // ---- ②-a 语法判据（返回槽写 / 间接 call / 识别不了的 esp 形态）----
        //   注意：`ret` 的 esp 偏移由 ②-b 的数据流判，不在这里。
        for (const auto& kv : c.blocks) {
            for (const Insn& in : kv.second.insns) {
                if (!self_ok) break;
                switch (in.op) {
                    case Op::Call:
                        if (in.op0().kind != OpKind::Imm) {
                            self_ok = false;
                            why = StructReason::IndirectCall;
                            why_ea = in.ea;
                        }
                        break;
                    default: break;
                }
                if (!self_ok) break;
                // 写返回地址那一格（op0 是内存目的操作数；`xchg` 的 op1 也算）
                if (writesOp0Memory(in.op) && in.op0().kind == OpKind::Mem &&
                    isReturnSlotForm(in.op0().mem)) {
                    self_ok = false;
                    why = StructReason::ReturnSlotWrite;
                    why_ea = in.ea;
                    break;
                }
                if (in.op == Op::Xchg && in.op1().kind == OpKind::Mem &&
                    isReturnSlotForm(in.op1().mem)) {
                    self_ok = false;
                    why = StructReason::ReturnSlotWrite;
                    why_ea = in.ea;
                    break;
                }
            }
            if (!self_ok) break;
        }

        // ---- ②-b esp / ebp 数据流（CFG；块按 `genBlockEdges` 连边）----
        if (self_ok) {
            std::map<std::uint32_t, EspState> state;
            std::vector<std::uint32_t> work;
            const auto root_it = c.blocks.find(c.root);
            if (root_it == c.blocks.end()) {
                // 根没有块（不该发生：`claim(root)` 成功才成单元）⇒ 保守降级
                self_ok = false;
                why = StructReason::DynamicReturn;
                why_ea = c.root;
            } else {
                state[c.root] = EspState{0, kUnknown};
                work.push_back(c.root);
            }
            while (self_ok && !work.empty()) {
                const std::uint32_t ea = work.back();
                work.pop_back();
                const auto bit = c.blocks.find(ea);
                if (bit == c.blocks.end()) continue;
                EspState s = state[ea];
                bool block_ok = true;
                // 顺序执行块内指令；遇到 `call` 时把它的栈清理并进状态（D54 §1 的方法学）。
                for (const Insn& in : bit->second.insns) {
                    if (in.op == Op::Call) {
                        if (in.op0().kind == OpKind::Imm) {
                            const std::uint32_t target = genBranchTarget(in);
                            // shim 桩：清理由宿主 shim 做，静态看不见 ⇒ 0（帧恢复兜底）
                            if (!(target >= shim_begin && target < shim_end)) {
                                const auto oi = entry_owner.find(target);
                                if (oi == entry_owner.end()) {
                                    self_ok = false;
                                    why = StructReason::UnknownCallee;
                                    why_ea = in.ea;
                                    block_ok = false;
                                    break;
                                }
                                cs.insert(oi->second);
                                const auto cl = ret_cleanup.find(oi->second);
                                s.esp = static_cast<std::int32_t>(
                                    s.esp + (cl != ret_cleanup.end() ? cl->second : 0));
                            }
                        }
                        continue;
                    }
                    if (in.op == Op::Ret) {
                        if (s.esp == kUnknown || s.esp != 0) {
                            self_ok = false;
                            why = StructReason::DynamicReturn;
                            why_ea = in.ea;
                            block_ok = false;
                            break;
                        }
                        continue;
                    }
                    if (!transferEsp(in, &s, &why)) {
                        self_ok = false;
                        why_ea = in.ea;
                        block_ok = false;
                        break;
                    }
                }
                if (!self_ok || !block_ok) break;
                // 后继：块图的静态边（`genBlockEdges` = 分区与发射共用的唯一判据）。
                std::vector<std::uint32_t> succ;
                if (!bit->second.terminated) {
                    const Insn& last = bit->second.insns.back();
                    succ.push_back(last.ea + last.length);
                } else {
                    for (const GenEdge& e : genBlockEdges(bit->second)) {
                        if (e.dynamic) continue;
                        succ.push_back(e.ea);
                    }
                }
                for (const std::uint32_t t : succ) {
                    if (c.blocks.count(t) == 0) continue;  // 入口 / dropped：跨单元（ret i32 / 尾巴调用）
                    const auto si = state.find(t);
                    if (si == state.end()) {
                        state[t] = s;
                        work.push_back(t);
                    } else {
                        // 合并：不一致 ⇒ UNKNOWN（不做路径特判）
                        const std::int32_t esp =
                            (si->second.esp != kUnknown && si->second.esp == s.esp) ? s.esp : kUnknown;
                        const std::int32_t ebp =
                            (si->second.ebp != kUnknown && si->second.ebp == s.ebp) ? s.ebp : kUnknown;
                        if (esp != si->second.esp || ebp != si->second.ebp) {
                            si->second.esp = esp;
                            si->second.ebp = ebp;
                            work.push_back(t);
                        }
                    }
                }
            }
        }

        callees[c.root] = std::move(cs);
        if (self_ok) {
            ++st.self_ok;
        } else {
            ++st.downgraded_self;
            self_reason[c.root] = why;
            out.reason_ea[c.root] = why_ea;
            switch (why) {
                case StructReason::VarStack: ++st.reason_varstack; break;
                case StructReason::ReturnSlotWrite: ++st.reason_return_slot; break;
                case StructReason::IndirectCall: ++st.reason_indirect_call; break;
                case StructReason::DynamicReturn: ++st.reason_dynamic_ret; break;
                case StructReason::UnknownCallee: ++st.reason_unknown_callee; break;
                default: break;
            }
        }

        // ---- ②-c 站点读数（判据不需要，但覆盖率要按**发射形状**给出；只数一次，⛔ 不放在数据流里）----
        for (const auto& kv : c.blocks) {
            for (const Insn& in : kv.second.insns) {
                if (in.op == Op::Ret) {
                    ++st.ret_sites;
                    continue;
                }
                if (in.op == Op::Call) {
                    ++st.call_sites;
                    if (in.op0().kind == OpKind::Imm) ++st.call_sites_static;
                    else ++st.call_sites_indirect;
                    continue;
                }
                if (in.op == Op::Jmp) {
                    ++st.jmp_sites;
                    if (in.op0().kind == OpKind::Imm) ++st.jmp_sites_static;
                    else ++st.jmp_sites_indirect;
                }
            }
        }
    }

    // ---- ③ 传递闭包（fixpoint）：自身通过 && 所有 callee 都可结构化 ----
    std::set<std::uint32_t> structured;
    for (const GenComponent& c : comps) {
        if (self_reason.count(c.root) == 0) structured.insert(c.root);
    }
    for (;;) {
        bool changed = false;
        for (const std::uint32_t root : std::vector<std::uint32_t>(structured.begin(), structured.end())) {
            const auto it = callees.find(root);
            if (it == callees.end()) continue;
            for (const std::uint32_t callee : it->second) {
                if (structured.count(callee) != 0) continue;
                structured.erase(root);
                out.reason[root] = StructReason::CalleeCascade;
                out.contagious_from[root] = callee;
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    // 首因优先：自身不过的原因保留（传染只在"自身过"的根上记）
    for (const auto& kv : self_reason) out.reason[kv.first] = kv.second;
    st.structured = structured.size();
    st.downgraded_callee = comps.size() - st.structured - st.downgraded_self;
    out.structured = std::move(structured);

    // ---- ④ 站点账（按发射形状）----
    //   ret：在可结构化单元里 ⇒ 宿主 ret；否则 ⇒ 回派发器。
    //   call：可结构化 caller + 静态目标 + 目标可结构化 ⇒ 宿主 call；间接 ⇒ lift_dyn；
    //         shim 桩 ⇒ ret i32；其余 ⇒ ret i32。
    //   jmp：跨单元 + 目标可结构化 ⇒ 尾巴调用；间接 ⇒ lift_dyn；其余 ⇒ ret i32。
    for (const GenComponent& c : comps) {
        const bool is_struct = out.structured.count(c.root) != 0;
        for (const auto& kv : c.blocks) {
            for (const Insn& in : kv.second.insns) {
                if (in.op == Op::Ret) {
                    if (is_struct) ++st.ret_sites_structured;
                    continue;
                }
                if (in.op == Op::Call) {
                    if (in.op0().kind != OpKind::Imm) {
                        ++st.call_sites_lift_dyn;
                        continue;
                    }
                    const std::uint32_t target = genBranchTarget(in);
                    if (target >= shim_begin && target < shim_end) {
                        ++st.call_sites_shim;
                        continue;
                    }
                    const auto oi = entry_owner.find(target);
                    if (is_struct && oi != entry_owner.end() &&
                        out.structured.count(oi->second) != 0) {
                        ++st.call_sites_host;
                    } else {
                        ++st.call_sites_dispatch;
                    }
                    continue;
                }
                if (in.op == Op::Jmp) {
                    if (in.op0().kind != OpKind::Imm) {
                        ++st.jmp_sites_lift_dyn;
                        continue;
                    }
                    const std::uint32_t target = genBranchTarget(in);
                    if (c.blocks.count(target) != 0) continue;  // 单元内 ⇒ br（不算跨函数）
                    const auto oi = entry_owner.find(target);
                    if (oi != entry_owner.end() && out.structured.count(oi->second) != 0)
                        ++st.jmp_sites_tail;
                    else
                        ++st.jmp_sites_dispatch;
                }
            }
        }
    }
    // 降级清单由调用方（`lift-gen`）按 `reason` / `reason_ea` / `contagious_from` 打印（确定性顺序）。
    return out;
}

}  // namespace aot
}  // namespace lift
