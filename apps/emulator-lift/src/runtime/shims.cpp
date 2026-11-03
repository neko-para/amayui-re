// emulator-lift · shim 框架实现（唯一 guest 地址 + 调用约定 + trace + 缺口报告）
//
// ★ 机制回顾（shims.h）：每个导入在**桩页**里有唯一地址（4 字节步进），IAT 槽写这个地址。
//   dispatcher 见到这个 EA 就调 `invoke()`：读返回地址 → 调宿主实现 → 按约定弹参 → 回 eip。
// ★ 弹参规则只有一条：`stdcall_bytes = (conv == Stdcall) ? argc*4 : 0`。
//   ⛔ 别统一按 stdcall 弹：cdecl 每调一次会多抬 4 字节栈，几百次后炸在毫不相干的地方。
#include "runtime/shims.h"

#include <algorithm>
#include <cstring>

#include "runtime/exec.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/sha256.h"
#include "runtime/x86.h"

namespace lift {

const char* convName(Conv c) {
    switch (c) {
        case Conv::Stdcall: return "stdcall";
        case Conv::Cdecl: return "cdecl";
        case Conv::Varargs: return "varargs";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------
// ShimCall：参数都从 guest 栈上读（`[esp]` = 返回地址，`[esp+4+4i]` = 第 i 个参数）
// ---------------------------------------------------------------------------------------
std::uint32_t ShimCall::u32(int i) const {
    return cpu->mem->rd32(cpu->esp() + 4u + 4u * static_cast<std::uint32_t>(i));
}

std::uint16_t ShimCall::u16(int i) const {
    return static_cast<std::uint16_t>(u32(i) & 0xFFFFu);
}

std::string ShimCall::str(int i) const {
    const std::uint32_t ea = u32(i);
    if (ea == 0) return {};
    return cpu->mem->readCString(ea);
}

std::u16string ShimCall::wstr(int i) const {
    const std::uint32_t ea = u32(i);
    if (ea == 0) return {};
    std::u16string s;
    for (std::size_t k = 0; k < 4096; ++k) {
        const char16_t c = static_cast<char16_t>(cpu->mem->rd16(ea + static_cast<std::uint32_t>(k) * 2));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

std::vector<std::uint8_t> ShimCall::bytes(int i, std::size_t n) const {
    const std::uint32_t ea = u32(i);
    std::vector<std::uint8_t> v(n);
    if (ea == 0 || n == 0) return v;
    cpu->mem->readBytes(ea, v.data(), n);
    return v;
}

// ---------------------------------------------------------------------------------------
// ShimRegistry
// ---------------------------------------------------------------------------------------
namespace {
// ★ PE 的导入表写的是 `KERNEL32.dll`，host 侧注册时写的是 `kernel32` —— 必须归一化。
//   规则：小写 + 去掉尾部的 `.dll`。⛔ 不要靠"注册时抄对名字"，那会让漏配变成运行时缺口。
std::string normalizeModule(std::string_view s) {
    std::string o = toLower(std::string(s));
    if (o.size() > 4 && o.compare(o.size() - 4, 4, ".dll") == 0) o.resize(o.size() - 4);
    return o;
}
}  // namespace

void ShimRegistry::add(const char* module, const char* name, Conv conv, int argc, ShimFn fn) {
    ShimDef def;
    def.module = module ? module : "";
    def.name = name ? name : "";
    def.conv = conv;
    def.argc = argc;
    def.fn = fn;
    def.id = static_cast<std::uint32_t>(defs_.size());
    defs_.push_back(std::move(def));
}

void ShimRegistry::addUnimplemented(const char* module, const char* name, const char* reason) {
    ShimDef def;
    def.module = module ? module : "";
    def.name = name ? name : "";
    def.conv = Conv::Stdcall;
    def.argc = 0;
    def.fn = nullptr;
    def.reason = reason;
    def.id = static_cast<std::uint32_t>(defs_.size());
    defs_.push_back(std::move(def));
}

const ShimDef* ShimRegistry::find(const char* module, const char* name) const {
    const std::string m = normalizeModule(module ? module : "");
    for (const ShimDef& d : defs_) {
        if (normalizeModule(d.module) == m && equalsIgnoreCase(d.name, name ? name : "")) return &d;
    }
    return nullptr;
}

ShimDef* ShimRegistry::findMutable(const char* module, const char* name) {
    const std::string m = normalizeModule(module ? module : "");
    for (ShimDef& d : defs_) {
        if (normalizeModule(d.module) == m && equalsIgnoreCase(d.name, name ? name : "")) return &d;
    }
    return nullptr;
}

const ShimDef* ShimRegistry::byStubEa(std::uint32_t ea) const {
    for (const ShimDef& d : defs_)
        if (d.stub_ea == ea && ea != 0) return &d;
    return nullptr;
}

const ShimDef* ShimRegistry::byId(std::uint32_t id) const { return id < defs_.size() ? &defs_[id] : nullptr; }

bool ShimRegistry::bind(GuestMemory& mem, const PeImage& img, std::uint32_t shim_page, std::uint32_t shim_slots,
                        std::string* err) {
    std::uint32_t slot = 0;
    std::size_t unresolved = 0;
    for (const PeImportInfo& imp : img.imports) {
        std::string name = imp.by_ordinal ? ("#" + std::to_string(imp.ordinal)) : imp.name;
        ShimDef* def = findMutable(imp.module.c_str(), name.c_str());
        if (!def) {
            // 导入表里有、但我们既没实现也没登记 ⇒ 现场补一条（缺口必须可见，⛔ 不能悄悄跳过）
            ShimDef d;
            d.module = imp.module;
            d.name = name;
            d.conv = Conv::Stdcall;
            d.argc = 0;
            d.fn = nullptr;
            d.reason = "导入表里有，但既没有实现也没有 addUnimplemented 登记";
            d.id = static_cast<std::uint32_t>(defs_.size());
            defs_.push_back(std::move(d));
            def = &defs_.back();
        }
        if (slot >= shim_slots) {
            if (err) *err = "shim 桩页槽位不够（shim_slots=" + std::to_string(shim_slots) + "）";
            return false;
        }
        def->imported = true;
        if (def->stub_ea == 0) def->stub_ea = shim_page + slot * 4u;
        ++slot;
        if (!def->fn) ++unresolved;
        // IAT 槽指向桩页里的唯一地址
        const std::uint32_t iat_ea = img.image_base + imp.iat_rva;
        if (!mem.committed(iat_ea, 4)) {
            if (err) *err = "IAT 槽不在镜像里（导入表 RVA 失效）";
            return false;
        }
        mem.wr32(iat_ea, def->stub_ea);
    }
    (void)unresolved;
    return true;
}

std::vector<const ShimDef*> ShimRegistry::importedButUnimplemented() const {
    std::vector<const ShimDef*> out;
    for (const ShimDef& d : defs_)
        if (d.imported && !d.fn) out.push_back(&d);
    std::sort(out.begin(), out.end(), [](const ShimDef* a, const ShimDef* b) {
        if (a->module != b->module) return a->module < b->module;
        return a->name < b->name;
    });
    return out;
}

std::vector<const ShimDef*> ShimRegistry::calledUnimplemented() const {
    std::vector<const ShimDef*> out;
    for (const ShimDef& d : defs_)
        if (d.called && !d.fn) out.push_back(&d);
    return out;
}

std::size_t ShimRegistry::importedCount() const {
    std::size_t n = 0;
    for (const ShimDef& d : defs_)
        if (d.imported) ++n;
    return n;
}

void ShimRegistry::invoke(X86& cpu, ShimDef& def) {
    const std::uint32_t ret_ea = cpu.mem->rd32(cpu.esp());
    ShimCall call;
    call.cpu = &cpu;
    call.conv = def.conv;
    call.argc = def.argc;
    call.ret_ea = ret_ea;
    call.stdcall_bytes = (def.conv == Conv::Stdcall) ? static_cast<std::uint32_t>(def.argc) * 4u : 0u;

    def.called = true;
    ++def.call_count;

    if (!def.fn) {
        // **响亮失败**：未实现的导入。这里是 A5 缺口的执行期落点。
        std::fprintf(stderr, "\n=== unimplemented import (base gap) ===\n");
        std::fprintf(stderr, "  %s!%s  (stub 0x%08X, conv=%s, argc=%d)\n", def.module.c_str(), def.name.c_str(),
                     def.stub_ea, convName(def.conv), def.argc);
        std::fprintf(stderr, "  reason: %s\n", def.reason ? def.reason : "(未登记理由)");
        std::fprintf(stderr, "  返回地址 = 0x%08X\n", ret_ea);
        dumpRegisters(cpu, stderr);
        dumpGuestStack(cpu, stderr, 24);
        cpu.running = false;
        cpu.stop_why = "unimplemented import";
        ++cpu.shim.unimplemented_calls;
        return;
    }

    def.fn(cpu, call);
    cpu.eax() = call.result;

    if (trace_) {
        // ★ 确定性：只打参数原始字 + 返回值，不打时间、不打宿主地址
        std::fprintf(trace_out_, "[shim] %3u %s!%s(", def.id, def.module.c_str(), def.name.c_str());
        for (int i = 0; i < def.argc; ++i) {
            std::fprintf(trace_out_, "%s%08X", i ? "," : "", call.u32(i));
        }
        std::fprintf(trace_out_, ") -> %08X  ret=%08X\n", call.result, ret_ea);
    }

    // `ret N`（stdcall）或 `ret`（cdecl）：先弹返回地址，再抬 argc*4
    cpu.esp() += 4u + call.stdcall_bytes;
    cpu.eip = ret_ea;
}

}  // namespace lift
