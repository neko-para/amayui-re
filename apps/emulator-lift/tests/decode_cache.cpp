// emulator-lift · **EA → Insn 解码缓存**的等价判据（pure 档，最外层 `add_test`）
//
// 判据（红了意味着什么）：`decodeCached()` 给出的 `Insn` 必须与"**每次都重新解码**"给出的
// `Insn` **逐字段相同** —— 无论这一次是**命中**、**未命中**、还是"EA 相同但字节被改过"。
// 它守的是 `runtime/decode_cache.*` 的那条**唯一**正确性主张：
//   缓存只在"同一份字节 + 同一个 EA"上复用结果，⛔ 永不给出陈旧答案。
//
// ★ 为什么这条用例必须有（不是"测实现细节"）：AOT 臂的 `lift_note_write` 是 **no-op**
//   （`cpu.cache == nullptr`），所以"guest 写代码页"没有任何钩子。解码缓存的逐字节校验
//   是这条缺口**在解码层面**唯一的自愈机制；校验一旦被删/写错，症状是**静默跑旧指令**
//   （不崩、不黑屏、diff 也看不见）—— 正是 AGENTS.md §10 说的"值得配守卫"的那一类。
//
// ★ 三个场景（对应任务书的"命中 / 失效 / 字节被改后重解码"）：
//   ① `cache-hit`：同 EA 第二次 ⇒ 命中，且与重新解码**逐字段相同**；
//   ② `cache-miss-different-ea`：另一个 EA ⇒ 未命中、各自正确（两条不互相污染）；
//   ③ `smc-invalidate`：改字节（**连同编码长度一起变**：5 字节 `mov eax,imm32` → 1 字节 `nop`）
//      ⇒ 必须重解，得到的是**新**字节的 Insn（长度也要跟着变），且 `invalidated` +1；
//   ④ `page-window-partial`：指令落在页尾、15 字节窗口不完整 ⇒ 走 `uncached` 直通，
//      结果仍与重新解码逐字段相同（`9B` 前缀合并那类"随可读字节数变化"的形态不许被缓存）。
#include <cstdint>
#include <cstring>
#include <string>

#include "check.h"
#include "runtime/decode.h"
#include "runtime/decode_cache.h"
#include "runtime/insn.h"
#include "runtime/memory.h"

namespace {

constexpr std::uint32_t kCodeBase = 0x00400000u;
constexpr std::uint32_t kPage = 0x1000u;

// 逐字段比较两份 `Insn`（⛔ 不用 memcmp：`Insn` 有填充字节，而且"哪一字段不同"要能读懂）。
bool sameOperand(const lift::Operand& a, const lift::Operand& b) {
    if (a.kind != b.kind || a.reg != b.reg || a.size != b.size || a.imm != b.imm || a.vec != b.vec) return false;
    if (a.mem.base != b.mem.base || a.mem.index != b.mem.index || a.mem.scale != b.mem.scale ||
        a.mem.size != b.mem.size || a.mem.absolute != b.mem.absolute || a.mem.seg_fs != b.mem.seg_fs ||
        a.mem.seg_gs != b.mem.seg_gs || a.mem.disp != b.mem.disp) {
        return false;
    }
    return true;
}

bool sameInsn(const lift::Insn& a, const lift::Insn& b) {
    if (a.ea != b.ea || a.length != b.length || a.opsz != b.opsz || a.rep != b.rep || a.lock != b.lock ||
        a.operand16 != b.operand16 || a.op != b.op || a.pk != b.pk || a.sse != b.sse || a.cc != b.cc ||
        a.zydis_mnemonic != b.zydis_mnemonic) {
        return false;
    }
    // mnemonic 是静态字符串指针（规范名）；按**内容**比，避免"同一份名字两份字面量"这种假红。
    const char* ma = a.mnemonic != nullptr ? a.mnemonic : "";
    const char* mb = b.mnemonic != nullptr ? b.mnemonic : "";
    if (std::strcmp(ma, mb) != 0) return false;
    for (int i = 0; i < 3; ++i) {
        if (!sameOperand(a.ops[i], b.ops[i])) return false;
    }
    return true;
}

// "每次都重新解码"的参照答案（⛔ 这是判据的另一半，不是被测对象）。
bool freshDecode(const lift::Decoder& dec, const lift::GuestMemory& mem, std::uint32_t ea, lift::Insn* out,
                 std::string* why) {
    return dec.decode(mem, ea, out, why);
}

// 把字节写进 guest 代码页（页已提交 ⇒ `writeBytes` 走的是正常的访存路径）。
void place(lift::GuestMemory& mem, std::uint32_t ea, const std::uint8_t* bytes, std::size_t n) {
    mem.writeBytes(ea, bytes, n);
}

}  // namespace

int main() {
    using namespace lift_test;

    lift::GuestMemory mem;
    LIFT_CHECK(mem.reserve());
    // ★ 只提交**一页**：④ 要靠"页尾之后没提交"来构造"15 字节窗口不完整"（那正是 `uncached` 的判据）。
    LIFT_CHECK(mem.commitZeroed(kCodeBase, kPage, lift::RegionKind::Image, "decode-cache-test"));
    lift::Decoder dec;
    LIFT_CHECK(dec.ok());

    lift::DecodeCache cache;
    lift::Insn cached;
    lift::Insn fresh;
    std::string why_cache;
    std::string why_fresh;

    // ---- ① 同 EA 第二次 = 命中，且与重新解码逐字段相同 ----
    // `mov eax, [esp+4]`（8B 44 24 04）：4 字节，带 ModRM+SIB+disp8（操作数形态最"有内容"的常见形）。
    g_current_case = "cache-hit";
    {
        const std::uint32_t ea = kCodeBase + 0x10u;
        const std::uint8_t code[] = {0x8B, 0x44, 0x24, 0x04};
        place(mem, ea, code, sizeof(code));

        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));  // 未命中 ⇒ 真解码 + 建条目
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.hits), 0u);

        const lift::Insn first = cached;
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));  // 命中
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.hits), 1u);
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK(sameInsn(cached, first));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.invalidated), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.uncached), 0u);
    }

    // ---- ② 另一个 EA（另一个槽位）= 未命中，两条互不污染 ----
    g_current_case = "cache-miss-different-ea";
    {
        const std::uint32_t ea = kCodeBase + 0x20u;
        const std::uint8_t code[] = {0xD9, 0x00};  // `fld dword [eax]`（x87 形态：op / mem 操作数都要对上）
        place(mem, ea, code, sizeof(code));

        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cached.op), static_cast<std::uint32_t>(lift::Op::Fld));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.misses), 2u);  // ① 的那次 + 这次
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.hits), 1u);    // ② 没有蹭到 ① 的条目
    }

    // ---- ③ EA 相同、字节被改（长度也变）⇒ 重解，得到新字节的 Insn ----
    g_current_case = "smc-invalidate";
    {
        const std::uint32_t ea = kCodeBase + 0x30u;
        const std::uint8_t long_code[] = {0xB8, 0x78, 0x56, 0x34, 0x12};  // `mov eax, 0x12345678`（5 字节）
        place(mem, ea, long_code, sizeof(long_code));
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));
        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cached.length), 5u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cached.ops[1].imm), 0x12345678u);

        // ★ guest 自己改写了这段代码（SMC）：5 字节 `mov` → 1 字节 `nop` + 垃圾
        const std::uint8_t short_code[] = {0x90};
        place(mem, ea, short_code, sizeof(short_code));
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.invalidated), 1u);  // 字节校验**真的**看见了
        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(sameInsn(cached, fresh));  // ⛔ 陈旧答案会在这里露馅（长度仍 5 / imm 仍旧值 / op 仍 Mov）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cached.length), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cached.op), static_cast<std::uint32_t>(lift::Op::Nop));

        // 改回来 ⇒ 又要重解一次（校验是"逐字节"的，不是"变过一次就作废整条"）
        place(mem, ea, long_code, sizeof(long_code));
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));
        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.invalidated), 2u);
    }

    // ---- ④ 15 字节窗口不完整（页尾）⇒ 走 `uncached`，结果仍与重新解码相同 ----
    g_current_case = "page-window-partial";
    {
        const std::uint32_t ea = kCodeBase + kPage - 1u;  // 该页最后一个字节起
        const std::uint8_t code[] = {0xC3};               // `ret`
        place(mem, ea, code, sizeof(code));
        const std::uint64_t uncached0 = cache.uncached;
        LIFT_CHECK(lift::decodeCached(cache, dec, mem, ea, &cached, &why_cache));
        LIFT_CHECK(freshDecode(dec, mem, ea, &fresh, &why_fresh));
        LIFT_CHECK(sameInsn(cached, fresh));
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(cache.uncached), static_cast<std::uint32_t>(uncached0 + 1u));
    }

    // ---- ⑤ 解不出来 ⇒ 与直接 decode **同样**返回 false（失败路径不许把缓存写成"有效条目"）----
    g_current_case = "decode-failure";
    {
        const std::uint32_t ea = kCodeBase + 0x40u;
        // 15 个 `0x66` 前缀 ⇒ 超过 x86 的 15 字节编码上限，Zydis 必然拒绝（判据只看"两边一致"，
        // ⛔ 不假设它一定失败：真正的合同是"缓存与不缓存给出同一个答案"）。
        const std::uint8_t junk[15] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                                       0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
        place(mem, ea, junk, sizeof(junk));
        lift::Insn a;
        lift::Insn b;
        std::string why_a;
        std::string why_b;
        const bool ok_cached = lift::decodeCached(cache, dec, mem, ea, &a, &why_a);
        const bool ok_fresh = freshDecode(dec, mem, ea, &b, &why_b);
        LIFT_CHECK_EQ_U32(ok_cached ? 1u : 0u, ok_fresh ? 1u : 0u);
        if (ok_cached) LIFT_CHECK(sameInsn(a, b));
        LIFT_CHECK_EQ_U32(ok_cached ? 1u : 0u, 0u);  // 这条形态本来就是红的（解不出来）
    }

    return lift_test::finish("lift-decode-cache");
}
