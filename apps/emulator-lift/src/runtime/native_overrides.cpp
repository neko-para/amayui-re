// emulator-lift · guest 函数级 native override：机制 + 已知表
//
// ★ 本文件只放**机制**（表 / ABI / 回退 / 段级 / 锚校验）与 `kKnown[]` 注册；
//   各函数的 native 实现按域拆在同目录的 `native_*.cpp`（2026-11-08 拆分，纯搬运）。
//   ⛔ 这里不放具体热点实现 —— 它们由 `--profile` 选出、各自有独立的准入与守卫。
#include "runtime/native_overrides.h"

#include <utility>

#include "corpus/lst_index.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {
bool g_nested_started = false;
}

bool startNestedGuestCall(X86& cpu, std::uint32_t fn, const std::vector<std::uint32_t>& args,
                          std::uint32_t cleanup_bytes, const char* api, GuestContinuation cont, std::string* err,
                          std::uint32_t callee_ecx) {
    if (cpu.calls == nullptr) {
        if (err) *err = "native nested call：没有 GuestCallStack";
        return false;
    }
    if (!cpu.calls->start(cpu, fn, args, cleanup_bytes, api, std::move(cont), err, callee_ecx)) return false;
    g_nested_started = true;
    return true;
}

namespace {
bool frameCommitted(GuestMemory& mem, std::uint32_t esp, std::uint32_t arg_bytes) {
    return mem.committed(esp, static_cast<std::size_t>(4u + arg_bytes));
}
}  // namespace

const char* guestConvName(GuestConv c) {
    switch (c) {
        case GuestConv::Cdecl: return "cdecl";
        case GuestConv::Stdcall: return "stdcall";
        case GuestConv::Thiscall: return "thiscall";
    }
    return "?";
}

// thisCall 原语（见头文件的契约）：只读 `ECX` + `esp+4+4i`，不改任何状态。
// ★ 顺序就是 ABI 顺序：`a[0]` = 返回地址之后的第一个 dword = guest 的第 1 个形参。
ThisCallArgs thisCall(const X86& cpu, std::size_t arg_count) {
    ThisCallArgs f;
    f.self = cpu.ecx();
    if (arg_count > ThisCallArgs::kMax) arg_count = ThisCallArgs::kMax;
    for (std::size_t i = 0; i < arg_count; ++i) f.a[i] = cpu.arg(static_cast<int>(i));
    return f;
}

bool NativeOverrideTable::add(std::uint32_t ea, const std::string& name, GuestConv conv, std::uint32_t arg_bytes,
                              NativeFn fn, std::string* err) {
    if (count_ >= kMax) {
        if (err) *err = "native override 表已满（kMax=" + std::to_string(kMax) + "）";
        return false;
    }
    if (fn == nullptr) {
        if (err) *err = "native override 没有实现：" + name;
        return false;
    }
    if (ea == 0) {
        if (err) *err = "native override 的 EA 为 0：" + name;
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].ea == ea) {
            if (err) *err = "native override 重复 EA：" + name;
            return false;
        }
    }
    NativeOverride& e = entries_[count_++];
    e.ea = ea;
    e.name = name;
    e.conv = conv;
    e.arg_bytes = arg_bytes;
    e.fn = fn;
    e.hits = 0;
    e.fallbacks = 0;
    return true;
}

void NativeOverrideTable::clear() {
    for (std::size_t i = 0; i < count_; ++i) entries_[i] = NativeOverride{};
    count_ = 0;
}

NativeOverride* NativeOverrideTable::find(std::uint32_t ea) {
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].ea == ea) return &entries_[i];
    return nullptr;
}

const NativeOverride* NativeOverrideTable::find(std::uint32_t ea) const {
    for (std::size_t i = 0; i < count_; ++i)
        if (entries_[i].ea == ea) return &entries_[i];
    return nullptr;
}

bool NativeOverrideTable::invoke(NativeOverride& ov, X86& cpu, GuestMemory& mem, const char** why,
                                 bool* suspended) {
    if (suspended) *suspended = false;
    if (!ov.fn) {
        if (why) *why = "没有 native 实现";
        ++ov.fallbacks;
        return false;
    }
    const std::uint32_t esp = cpu.esp();
    if (!frameCommitted(mem, esp, ov.arg_bytes)) {
        if (why) *why = "返回地址 / 栈参数不在已提交页";
        ++ov.fallbacks;
        return false;
    }
    const std::uint32_t ret = mem.rd32(esp);
    std::uint32_t result = 0;
    g_nested_started = false;
    if (!ov.fn(cpu, mem, &result)) {
        if (g_nested_started) {
            // native 已经铺好 guest 回调帧；EIP/ESP 由 GuestCallStack 设置，不要回退。
            if (suspended) *suspended = true;
            ++ov.hits;
            return true;
        }
        if (why) *why = "native 实现要求回退（参数越界 / safe mode）";
        ++ov.fallbacks;
        return false;
    }
    cpu.eax() = result;
    cpu.eip = ret;
    cpu.esp() = esp + 4u + (ov.conv == GuestConv::Cdecl ? 0u : ov.arg_bytes);
    ++ov.hits;
    return true;
}

// ---------------------------------------------------------------------------------------
// 段级 override 表（函数中段；见头文件的口径）
// ---------------------------------------------------------------------------------------

bool NativeBlockTable::add(std::uint32_t entry_ea, std::uint32_t resume_ea, const std::string& name,
                           NativeBlockFn fn, std::string* err) {
    if (count_ >= kMax) {
        if (err) *err = "段级 native override 表已满（kMax=" + std::to_string(kMax) + "）";
        return false;
    }
    if (fn == nullptr) {
        if (err) *err = "段级 native override 没有实现：" + name;
        return false;
    }
    if (entry_ea == 0 || resume_ea == 0) {
        if (err) *err = "段级 native override 的 entry/resume EA 为 0：" + name;
        return false;
    }
    if (entry_ea == resume_ea) {
        if (err) *err = "段级 native override 的 entry == resume（会原地打转）：" + name;
        return false;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == entry_ea) {
            if (err) *err = "段级 native override 重复 entry EA：" + name;
            return false;
        }
    }
    NativeBlockOverride& e = entries_[count_++];
    e.entry_ea = entry_ea;
    e.resume_ea = resume_ea;
    e.name = name;
    e.fn = fn;
    e.hits = 0;
    e.declines = 0;
    e.fallbacks = 0;
    e.disabled = false;
    return true;
}

void NativeBlockTable::clear() {
    for (std::size_t i = 0; i < count_; ++i) entries_[i] = NativeBlockOverride{};
    count_ = 0;
}

NativeBlockOverride* NativeBlockTable::find(std::uint32_t eip) {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == eip && !entries_[i].disabled) return &entries_[i];
    }
    return nullptr;
}

const NativeBlockOverride* NativeBlockTable::find(std::uint32_t eip) const {
    for (std::size_t i = 0; i < count_; ++i) {
        if (entries_[i].entry_ea == eip && !entries_[i].disabled) return &entries_[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// 已知名字表：名字 → ABI + 实现（EA 由 `.lst` 解析，⛔ 不硬编码）
// ---------------------------------------------------------------------------------------

namespace {
const KnownNative kKnown[] = {
    {"_memcpy", GuestConv::Cdecl, 12, nativeMemcpy},
    {"_memcpy_0", GuestConv::Cdecl, 12, nativeMemcpy},
    {"_memset", GuestConv::Cdecl, 12, nativeMemset},
    {"_strlen", GuestConv::Cdecl, 4, nativeStrlen},
    // ★ 由 profile 选出的自定义 memcmp（`.lst` 里是 `sub_401000 proc near`）。
    //   语义见 `.lst` 401066 / `.c`：**只返回符号**（-1/0/+1），不是字节差。
    {"sub_401000", GuestConv::Cdecl, 12, nativeSignMemcmp},
    // ★ profile boot 第一热点：S4IC/LZSS 解压（.c: 43752；this=字典对象, a4[0]=out, a4[2]=comp）。
    {"sub_436A80", GuestConv::Thiscall, 12, nativeLzss436A80},
    // ★ `sub_48B090`（NormalTexture::UploadImage）已进入 `all`。Windows 侧曾观察到纹理问题，
    //   但 macOS 未复现；后续继续排查。可用 `all,-sub_48B090` 单独排除。
    //   已修：alpha 目标行序、24bpp/32bpp、锁走 lockRect/unlockRect、失败路径无半状态、
    //   ECX/this、cols/pitch、目标格式从设备状态判断、bpp/src 在 Create 后重读。
    //   仍显式回退：4/1bpp 源、16bpp 目标、色键≠0；时间补偿不在本轮范围。
    {"sub_48B090", GuestConv::Thiscall, 12, nativeUpload48B090},
    // ★ 2026-11-07：boot profile 第 2 热点（`sub_429020` Map::findIndex，thiscall retn 8）。
    {"sub_429020", GuestConv::Thiscall, 8, nativeFindIndex429020},
    // ★ 2026-11-07：boot profile 第 3 热点（`sub_48AE90` NormalTexture 纯色填充，thiscall retn 0Ch）。
    {"sub_48AE90", GuestConv::Thiscall, 12, nativeFill48AE90},
    // ★ 2026-11-07：影片渲染热点（LOGO 1280×720 占 guest 块时间 77.5%）。只覆盖 A8R8G8B8 / 无 alpha 面
    //   的主路径，其余回退解释器。差分守卫见 `tests/movie_logo.cpp`（开/关 override 的 `tex movie #1` hash 必须一致）。
    {"sub_487C60", GuestConv::Thiscall, 4, nativeRender487C60},
    // ★ 2026-11-08：字形位图 → 已锁定纹理表面的逐像素混合叶子（thiscall `retn 20h` = 32 B）。
    //   `.lst` 0x46D9F0..0x46DEC2；16bpp / 32bpp 两支；裁剪 = `sub_46CE20` 的成功路径。
    //   准入失败（this/mgr/texid/裁剪对象/参数跨度/级数越界）⇒ 回退解释器，⛔ 不猜。
    //   纯像素核 = `blendGlyph46D9F0`（G1 直接跑）；帧级 A/B 见需求单
    //   REQ-01M6V1KDFTE933AD3YB5P6XJK2（`all` vs `all,-sub_46D9F0` 逐帧逐字节相同）。
    {"sub_46D9F0", GuestConv::Thiscall, 32, nativeBlend46D9F0},
};
constexpr std::size_t kKnownCount = sizeof(kKnown) / sizeof(kKnown[0]);
// ★ 表容量是**硬约束**：`--native-overrides=all` 会把每个 `debug == false` 的项都 `add()` 一遍
//   （machine.cpp 的 configureNativeOverrides），装不下 ⇒ add() 失败 ⇒ configure 返回 false ⇒
//   **模拟器直接拒绝启动**。把它变成编译期错误，而不是等到运行时"all 突然起不来"。
static_assert(kKnownCount <= NativeOverrideTable::kMax,
              "kKnown[] 的项数超过 NativeOverrideTable::kMax：请同时抬高 kMax（见头文件的注释）");

// ★ 段级项（函数中段）：entry/resume 是固定的 `.lst` EA（中段没有符号），启用前用 `.lst` 校验锚。
//   `name` 就是 `--native-overrides` 里写的名字（与函数级同名不会冲突：函数级表里没有 sub_414AC0）。
const KnownBlockNative kKnownBlocks[] = {
    {"sub_414AC0", "sub_414AC0", 0x00415384u, 0x0041539Fu, nativeFillPool414AC0},
};
constexpr std::size_t kKnownBlockCount = sizeof(kKnownBlocks) / sizeof(kKnownBlocks[0]);
}  // namespace

const KnownNative* findKnownNative(const std::string& name) {
    for (std::size_t i = 0; i < kKnownCount; ++i)
        if (name == kKnown[i].name) return &kKnown[i];
    return nullptr;
}

std::size_t knownNativeCount() { return kKnownCount; }

const KnownNative& knownNativeAt(std::size_t i) { return kKnown[i]; }

const KnownBlockNative* findKnownBlockNative(const std::string& name) {
    for (std::size_t i = 0; i < kKnownBlockCount; ++i)
        if (name == kKnownBlocks[i].name) return &kKnownBlocks[i];
    return nullptr;
}

std::size_t knownBlockNativeCount() { return kKnownBlockCount; }

const KnownBlockNative& knownBlockNativeAt(std::size_t i) { return kKnownBlocks[i]; }

namespace {
// 把 `.lst` 的指令文本归一化（列宽对齐的连续空白压成一个空格）。
std::string normalizeAsm(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}
}  // namespace

bool verifyKnownBlockAnchor(const LstIndex& lst, const KnownBlockNative& k, std::string* err) {
    auto bad = [&](const std::string& msg) {
        if (err) *err = std::string("段级 override ") + k.name + " 的锚校验失败：" + msg;
        return false;
    };
    if (!lst.loaded()) return bad("没有 .lst（段级 EA 无法核验）");
    const LstFunction* fn = lst.findFunction(k.enclosing_fn);
    if (fn == nullptr) return bad(std::string("`.lst` 里找不到函数 ") + k.enclosing_fn);
    const LstFunction* owner = lst.functionAt(k.entry_ea);
    if (owner == nullptr || owner->ea != fn->ea)
        return bad("entry EA 不在 " + std::string(k.enclosing_fn) + " 里");
    const LstEntry* entry = lst.find(k.entry_ea);
    if (entry == nullptr) return bad("entry EA 不是 `.lst` 里的一条指令");
    // 首指令必须是我们取证到的那条（换镜像 / 换 EA ⇒ 拒绝启用，⛔ 不静默换一个 EA）。
    if (normalizeAsm(entry->text) != "mov ecx, [esi-10h]")
        return bad("entry EA 的首指令不是 `mov ecx, [esi-10h]`（读到：" + normalizeAsm(entry->text) + "）");
    const LstEntry* resume = lst.find(k.resume_ea);
    if (resume == nullptr) return bad("resume EA 不是 `.lst` 里的一条指令");
    if (normalizeAsm(resume->text) != "mov eax, [ebx+5D7ECh]")
        return bad("resume EA 的首指令不是 `mov eax, [ebx+5D7ECh]`（读到：" + normalizeAsm(resume->text) + "）");
    return true;
}

}  // namespace lift
