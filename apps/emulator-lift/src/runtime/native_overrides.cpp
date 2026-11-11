// emulator-lift · guest 函数级 native override：机制 + 已知表
//
// ★ 本文件只放**机制**（表 / ABI / 回退 / 段级 / 锚校验）与 `kKnown[]` 注册；
//   各函数的 native 实现按域拆在同目录的 `native_*.cpp`（2026-11-08 拆分，纯搬运）。
//   ⛔ 这里不放具体热点实现 —— 它们由 `--profile` 选出、各自有独立的准入与守卫。
#include "runtime/native_overrides.h"

#include <cstring>
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
    // ★ 条件式 override 的台账通道：fn 在运行期间能拿到"自己那一项"（见 x86.h）。
    //   fn 只有一个返回出口 ⇒ 这里紧接着复位，不会留下悬垂指向。
    cpu.current_override = &ov;
    const bool ran = ov.fn(cpu, mem, &result);
    cpu.current_override = nullptr;
    if (!ran) {
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
    // ★ 2026-11-10：每帧固定税 `sub_4BBAB0` = `Voice::Tick(now)`（thiscall `retn 4`；ECX = T_Engine+0x148A0）。
    //   只接管**三槽全惰性**的纯查询支（零写、零可变调用）⇒ 返回 0；其余（内层扫描 / 已启动 /
    //   首 tick / 到期 / 无设备）一律回退解释器。时钟补偿 k = `kVoiceTickPureInsns`（见头文件）。
    //   ⛔ 不与 F2（令牌等待）混：这一项只治"查得太勤"。
    {"sub_4BBAB0", GuestConv::Thiscall, 4, nativeVoiceTick4BBAB0},
    // ★ 2026-11-08：Live2D `BDAffine` 逐顶点仿射（stdcall `retn 1Ch` = 28 B；虚表第 6 项 @0x52F0C0）。
    //   `.lst` 0x4CCC60..0x4CCD34；x87 逐位（SoftFloat 四则 + transcendental 同路径）。
    //   ★ 首版 `debug = true`（⛔ 不进 `all`）：真字节差分见 tests/native_override_assets.cpp
    //   （解释器 vs override，比 dst / 寄存器 / x87 ST 与状态字）。进 `all` 前要补帧级 A/B。
    {"sub_4CCC60", GuestConv::Stdcall, 28, nativeAffine4CCC60, /*debug=*/true},
    // ★ 2026-11-11：退出写存档尾段的 **LZSS Encode 整函数**（`retn 8`、thiscall、EAX = `this->out`；
    //   `.lst` 0x4367B0..0x436A75）。1 次拦截覆盖尾段 92.85% 的墙钟（`incl=3328.1 ms` / `calls=1`）。
    //   契约见 `native_overrides.h` 的 `LzssEncodeOutcome` / `LzssWhy` 段。
    //   ★ 首版 `debug = true`（⛔ 不进 `all`）：真字节两臂差分见 tests/native_override_assets.cpp。
    {"sub_4367B0", GuestConv::Thiscall, 8, nativeLzss4367B0, /*debug=*/true},
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

// =======================================================================================
// `sub_4367B0` · 退出写存档的 LZSS Encode（**整函数**；REQ-01M7375DARF1QFZEFNWX3CAK6T）
//
// ★ 为什么实现落在本文件（而 `native_overrides.cpp` 的头注说"不放具体热点实现"）：本单的改动面被
//   需求单钉成"只许动 native_overrides.cpp / tests/native_override*.cpp"⇒ 纯核 + 薄包装都放这里。
//   ⛔ 不要因为它在这里就把它当成"机制"的一部分去复用；它和 `native_lzss.cpp` 的解码是**两件事**。
//
// ★ 逐字节等价的依据全是 EA（`.lst` 0x4367B0..0x436A75；子函数 0x436520 / 0x436550 / 0x4366A0）：
//   - InitTree（0x436520）= `memset32(this+0xD024, 0x1000, 0x100)` + `memset32(this+0x1018, 0x1000, 0x1000)`
//     ⇒ **`dad[0..4095] = NIL` 与 `rson[4097..4352] = NIL`**（写的是 0x1000，⛔ 不是 0）。
//   - `text_buf` 只清 **0xFEE = 4078** 字节（0x43683D）；`text_buf[4078..4095]` 由首灌写，
//     `[4096..4112]` 只在 `s < 17` 时写（0x436998）。
//   - 首灌上限 **18**（0x43686E `cmp eax,12h`），`cur < in_end` 是**32 位无符号地址比较**（0x43685D `jnb`）。
//   - InsertNode 的平局判据 **严格大于**（0x4365EA `jle`）⇒ 等长保留先入树的候选、`match_pos` 不更新。
//   - 主循环 `match_len > len` 时收紧到 `len`（0x4368B6）；`>= 3` 发匹配，否则字面量取 **`text_buf[r]`**（0x4368E0）。
//   - `mask` 是 **byte**（`shl byte [ebp-19h],1`）：左移 8 次后为 0 ⇒ flush `size` 字节并判 `comp >= in_size`。
//   - 拷回循环的上限是**进入本阶段前的 `match_len` 快照**（`0x43696D mov [ebp-30h],edx`），
//     因为循环里的 `InsertNode` 会改写 `[this+0xD428]`。
//   - 排空循环 `do { DeleteNode(s); s++; r++; if (--len) InsertNode(r); } while (--rem);`（0x4369D8..0x4369FC）。
//   - 收尾 **`size > 1` 才 flush**（0x436A0E `cmp eax,1; jle` 直接跳返回）；give_up 从 **`a2` 起点**整体重拷
//     （0x436A3B 的 `var_40` 是进入函数时存下的 a2，⛔ 不是 `cur`），并令 `a3[1] = a3[2] = a3[0]`。
// =======================================================================================

namespace {

// `Compress`（0xD430）的字段偏移。EA 是锚：`+4` = text_buf（0x43683D / 0x436862 / 0x4368E0）、
// `+0x1018` = dad（0x43659F）、`+0x501C` = lson（0x436563）、`+0x9020` = rson（0x43656A / 0x436590）、
// `+0xD424` = match_pos（0x4368F8）、`+0xD428` = match_len（0x4368B6）、`+0xD42C` = out（0x436809）。
constexpr std::uint32_t kLzoText = 0x004;
constexpr std::uint32_t kLzoDad = 0x1018;
constexpr std::uint32_t kLzoLson = 0x501C;
constexpr std::uint32_t kLzoRson = 0x9020;
constexpr std::uint32_t kLzoMatchPos = 0xD424;
constexpr std::uint32_t kLzoMatchLen = 0xD428;
constexpr std::uint32_t kLzoOut = 0xD42C;
constexpr std::uint32_t kLzoObjectBytes = 0xD430;
// N = 4096 / F = 18 / NIL = 0x1000 / r 初值 = N-F = 4078。
// ★ NIL **不是 0**：`0x43655E mov edx,1000h`、`0x4366AC cmp [ecx+edx*4+1018h],edi`。
constexpr std::uint32_t kLzoN = 4096;
constexpr std::uint32_t kLzoF = 18;
constexpr std::uint32_t kLzoNil = 4096;
constexpr std::uint32_t kLzoR0 = 4078;  // N - F
constexpr std::uint32_t kLzoCodeBytes = 20;  // `[ebp-18h] Src[20]`；flush 前 size 最多 1 + 8*2 = 17
// `this + 0xD430` 不回绕的上界（`a3[0] + 0x1000` 的溢出判据另算）。
constexpr std::uint32_t kLzoSelfLimit = static_cast<std::uint32_t>(0x100000000ull - kLzoObjectBytes);
// `a3[0] + 0x1000` **不溢出**的上界（`0xFFFFF000` 会回绕成 0 ⇒ 拒绝，见准入）。
constexpr std::uint32_t kLzoInputLimit = 0xFFFFEFFFu;
constexpr std::uint32_t kLzoAllocExtra = 0x1000u;

std::uint32_t lzoLoad(const std::uint8_t* obj, std::uint32_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, obj + off, sizeof(v));
    return v;
}
void lzoStore(std::uint8_t* obj, std::uint32_t off, std::uint32_t v) { std::memcpy(obj + off, &v, sizeof(v)); }
std::int32_t lzoLoadI32(const std::uint8_t* obj, std::uint32_t off) {
    std::int32_t v = 0;
    std::memcpy(&v, obj + off, sizeof(v));
    return v;
}
std::uint32_t lzoText(const std::uint8_t* obj, std::uint32_t i) { return obj[kLzoText + i]; }

// `sub_436550` InsertNode(r)：把 ring 位置 r 挂进 dad/lson/rson 树（0x436550..0x43669D 逐条）。
// ★ 副作用：`[this+0xD428]`（match_len）= 0，然后按"严格大于"更新 match_pos/match_len。
//   18 全中时走**节点替换**：`dad[r]=dad[p]` / `lson[r]=lson[p]` / `rson[r]=rson[p]`，
//   `dad[lson[p]]=r`、`dad[rson[p]]=r`（两者可能是 NIL ⇒ 写 `dad[4096]`），再把 p 从父节点上摘掉。
void lzoInsertNode(std::uint8_t* obj, std::uint32_t r) {
    std::uint32_t p = 0x1001u + lzoText(obj, r);
    lzoStore(obj, kLzoLson + 4u * r, kLzoNil);
    lzoStore(obj, kLzoRson + 4u * r, kLzoNil);
    lzoStore(obj, kLzoMatchLen, 0u);
    std::int32_t cmp = 1;  // `0x436571 mov esi,1`：第一次方向判定恒走 `cmp >= 0`（rson）支
    for (;;) {
        if (cmp >= 0) {
            const std::uint32_t next = lzoLoad(obj, kLzoRson + 4u * p);
            if (next != kLzoNil) {
                p = next;
            } else {
                lzoStore(obj, kLzoRson + 4u * p, r);
                lzoStore(obj, kLzoDad + 4u * r, p);
                return;
            }
        } else {
            const std::uint32_t next = lzoLoad(obj, kLzoLson + 4u * p);
            if (next != kLzoNil) {
                p = next;
            } else {
                lzoStore(obj, kLzoLson + 4u * p, r);
                lzoStore(obj, kLzoDad + 4u * r, p);
                return;
            }
        }
        std::uint32_t i = 1;
        for (; i < kLzoF; ++i) {
            cmp = static_cast<std::int32_t>(lzoText(obj, r + i)) -
                  static_cast<std::int32_t>(lzoText(obj, p + i));
            if (cmp != 0) break;
        }
        // ★ 平局判据**严格大于**（`0x4365EA cmp edx,[ecx+0D428h]; jle short loc_436585`）。
        if (static_cast<std::int32_t>(i) > lzoLoadI32(obj, kLzoMatchLen)) {
            lzoStore(obj, kLzoMatchPos, p);
            lzoStore(obj, kLzoMatchLen, i);
            if (i >= kLzoF) break;
        }
    }
    const std::uint32_t dad_p = lzoLoad(obj, kLzoDad + 4u * p);
    const std::uint32_t lson_p = lzoLoad(obj, kLzoLson + 4u * p);
    const std::uint32_t rson_p = lzoLoad(obj, kLzoRson + 4u * p);
    lzoStore(obj, kLzoDad + 4u * r, dad_p);
    lzoStore(obj, kLzoLson + 4u * r, lson_p);
    lzoStore(obj, kLzoRson + 4u * r, rson_p);
    lzoStore(obj, kLzoDad + 4u * lson_p, r);
    lzoStore(obj, kLzoDad + 4u * rson_p, r);
    if (lzoLoad(obj, kLzoRson + 4u * dad_p) == p) {
        lzoStore(obj, kLzoRson + 4u * dad_p, r);
    } else {
        lzoStore(obj, kLzoLson + 4u * dad_p, r);
    }
    lzoStore(obj, kLzoDad + 4u * p, kLzoNil);
}

// `sub_4366A0` DeleteNode(p)：从树上摘掉 ring 位置 p（0x4366A0..0x4367A7 逐条）。
// ★ 早退判据是 `dad[p] == NIL`（0x4366AC 的 `cmp [ecx+edx*4+1018h],edi`，edi = 0x1000）。
// ★ 三支重挂与经典 Okumura 同形：`rson[p]==NIL ⇒ q=lson[p]`；`lson[p]==NIL ⇒ q=rson[p]`；
//   否则沿 rson 下到底再重挂（`rson[dad[q]]=lson[q]` / `dad[lson[q]]=dad[q]` / `lson[q]=lson[p]` /
//   `dad[lson[p]]=q`），然后 `rson[q]=rson[p]` / `dad[rson[p]]=q`。
void lzoDeleteNode(std::uint8_t* obj, std::uint32_t p) {
    if (lzoLoad(obj, kLzoDad + 4u * p) == kLzoNil) return;
    const std::uint32_t rson_p = lzoLoad(obj, kLzoRson + 4u * p);
    const std::uint32_t lson_p = lzoLoad(obj, kLzoLson + 4u * p);
    std::uint32_t q = 0;
    if (rson_p == kLzoNil) {
        q = lson_p;
    } else if (lson_p == kLzoNil) {
        q = rson_p;
    } else {
        q = lson_p;
        if (lzoLoad(obj, kLzoRson + 4u * q) != kLzoNil) {
            do {
                q = lzoLoad(obj, kLzoRson + 4u * q);
            } while (lzoLoad(obj, kLzoRson + 4u * q) != kLzoNil);
            const std::uint32_t dad_q = lzoLoad(obj, kLzoDad + 4u * q);
            const std::uint32_t lson_q = lzoLoad(obj, kLzoLson + 4u * q);
            lzoStore(obj, kLzoRson + 4u * dad_q, lson_q);
            lzoStore(obj, kLzoDad + 4u * lson_q, dad_q);
            lzoStore(obj, kLzoLson + 4u * q, lson_p);
            lzoStore(obj, kLzoDad + 4u * lson_p, q);
        }
        lzoStore(obj, kLzoRson + 4u * q, rson_p);
        lzoStore(obj, kLzoDad + 4u * rson_p, q);
    }
    const std::uint32_t dad_p = lzoLoad(obj, kLzoDad + 4u * p);
    lzoStore(obj, kLzoDad + 4u * q, dad_p);
    if (lzoLoad(obj, kLzoRson + 4u * dad_p) == p) {
        lzoStore(obj, kLzoRson + 4u * dad_p, q);
    } else {
        lzoStore(obj, kLzoLson + 4u * dad_p, q);
    }
    lzoStore(obj, kLzoDad + 4u * p, kLzoNil);
}

}  // namespace

LzssEncodeOutcome lzssEncodeObject(std::uint8_t* obj, std::uint32_t in_ea, const std::uint8_t* in,
                                   std::uint32_t in_size, std::uint8_t* out) {
    LzssEncodeOutcome res;
    if (obj == nullptr || out == nullptr || (in == nullptr && in_size != 0u)) return res;
    // ---- InitTree（0x436520）：写的是 **NIL = 0x1000**，不是 0 ----
    for (std::uint32_t i = 0; i < kLzoN; ++i) lzoStore(obj, kLzoDad + 4u * i, kLzoNil);
    for (std::uint32_t i = 0; i < 256u; ++i) lzoStore(obj, kLzoRson + 4u * (kLzoN + 1u + i), kLzoNil);
    // ---- `memset(text_buf, 0, 0xFEE)`（0x43683D；⛔ 不是 0x1000 / 4113）----
    std::memset(obj + kLzoText, 0, 0xFEEu);
    // ---- 首灌（0x436857）：地址比较是 **32 位无符号**（`jnb`）⇒ 回绕时 0 次 ----
    const std::uint32_t in_end = in_ea + in_size;
    std::uint32_t cur = in_ea;
    std::uint32_t len = 0;
    while (len < kLzoF && cur < in_end) {
        obj[kLzoText + kLzoR0 + len] = in[cur - in_ea];
        ++cur;
        ++len;
    }
    res.consumed = len;  // `0x436879 mov [ecx+4],eax`
    if (len == 0u) return res;  // `0x43687E jz 0x436A59`：只做了 InitTree + 清零，返回 out（comp 仍 0）
    // ---- 建树（0x436884..0x4368AE）：j = 4077 → 4060（含），再 InsertNode(4078) ----
    for (std::uint32_t j = 4077u;; --j) {
        lzoInsertNode(obj, j);
        if (j == 4060u) break;
    }
    lzoInsertNode(obj, kLzoR0);

    std::uint8_t code[kLzoCodeBytes] = {};
    std::uint32_t mask = 1u;  // `[ebp-19h]` 是 **byte**；下面每轮 `<<1` 后按 0 判 flush
    std::uint32_t size = 1u;
    std::uint32_t r = kLzoR0;
    std::uint32_t s = 0u;  // edi 从 0 起
    std::uint32_t out_pos = 0u;
    bool gave_up = false;

    for (;;) {
        // 0x4368B3：`if (match_len > len) match_len = len;`
        std::int32_t mlen = lzoLoadI32(obj, kLzoMatchLen);
        if (mlen > static_cast<std::int32_t>(len)) {
            mlen = static_cast<std::int32_t>(len);
            lzoStore(obj, kLzoMatchLen, static_cast<std::uint32_t>(mlen));
        }
        if (mlen >= 3) {
            // 0x4368EE：`code[size] = match_pos & 0xFF` / `code[size+1] = ((match_pos>>4)&0xF0)|(match_len-3)`
            const std::uint32_t mp = lzoLoad(obj, kLzoMatchPos);
            code[size] = static_cast<std::uint8_t>(mp & 0xFFu);
            code[size + 1u] = static_cast<std::uint8_t>(((mp >> 4) & 0xF0u) |
                                                        (static_cast<std::uint32_t>(mlen - 3) & 0xFFu));
            size += 2u;
        } else {
            // 0x4368CD：`code[0] |= mask; match_len = 1; code[size] = text_buf[r]`（⛔ 不是从输入取）
            code[0] = static_cast<std::uint8_t>(code[0] | static_cast<std::uint8_t>(mask));
            lzoStore(obj, kLzoMatchLen, 1u);
            code[size] = obj[kLzoText + r];
            size += 1u;
        }
        mask = (mask << 1) & 0xFFu;  // `shl byte [ebp-19h],1`
        if (mask == 0u) {
            // flush（0x436923..0x43694B）：`memcpy(out_cur, code, size); out_cur += size; a3[2] += size;`
            std::memcpy(out + out_pos, code, size);
            out_pos += size;
            res.comp += size;
            if (res.comp >= in_size) {  // `0x43694B jnb loc_436A72`（**无符号**）
                gave_up = true;
                break;
            }
            code[0] = 0;
            mask = 1;
            size = 1;
        }
        // ---- 拷回（0x436965..0x4369C9）：上限 = **进入本阶段前**的 match_len 快照 ----
        const std::int32_t snapshot = lzoLoadI32(obj, kLzoMatchLen);
        std::uint32_t n = 0;
        if (snapshot > 0) {
            while (static_cast<std::int32_t>(n) < snapshot) {
                if (cur >= in_end) break;
                const std::uint8_t ch = in[cur - in_ea];
                ++cur;
                lzoDeleteNode(obj, s);
                obj[kLzoText + s] = ch;
                if (s < 17u) obj[kLzoText + kLzoN + s] = ch;  // `0x436998 cmp edi,11h`
                r = (r + 1u) & 0xFFFu;
                s = (s + 1u) & 0xFFFu;
                lzoInsertNode(obj, r);
                ++n;
            }
        }
        res.consumed += n;  // `0x4369CC add [ecx+4],eax`
        if (static_cast<std::int32_t>(n) < snapshot) {
            // 排空（0x4369D8..0x4369FC）：`--len` 为 0 时**跳过** InsertNode
            std::uint32_t rem = static_cast<std::uint32_t>(snapshot) - n;
            do {
                lzoDeleteNode(obj, s);
                s = (s + 1u) & 0xFFFu;
                r = (r + 1u) & 0xFFFu;
                --len;
                if (len != 0u) lzoInsertNode(obj, r);
                --rem;
            } while (rem != 0u);
        }
        if (len == 0u) break;  // `0x436A01 cmp [ebp-2Ch],0; jg 0x4368B3`
    }
    // ---- 收尾（0x436A0B..0x436A38）：`if (size > 1)` 才 flush；flush 后再判一次放弃 ----
    if (!gave_up && size > 1u) {
        std::memcpy(out + out_pos, code, size);
        res.comp += size;
        if (res.comp >= in_size) gave_up = true;
    }
    if (gave_up) {
        // 0x436A3B：`memcpy(out, a2, a3[0]); a3[1] = a3[2] = a3[0]`（★ 起点是 **a2**，不是 cur）
        if (in_size != 0u) std::memcpy(out, in, in_size);
        res.consumed = in_size;
        res.comp = in_size;
    }
    res.gave_up = gave_up;
    return res;
}

bool nativeLzss4367B0(X86& cpu, GuestMemory& mem, std::uint32_t* result) {
    *result = 0;
    const std::uint32_t self = cpu.ecx();
    const std::uint32_t in_ea = cpu.arg(0);
    const std::uint32_t info_ea = cpu.arg(1);
    auto decline = [&cpu](LzssWhy why) {
        if (cpu.current_override != nullptr) {
            ++cpu.current_override->declines[static_cast<std::size_t>(why)];
        }
        return false;
    };
    // ---- 准入（全部只读；任何一条不成立 ⇒ false + 零写）----
    if (info_ea == 0u || !mem.committed(info_ea, 12u)) return decline(LzssWhy::NoInfo);
    const std::uint32_t in_size = mem.rd32(info_ea);
    // ★ 判据是 `in_size + 0x1000` **不回绕**：`in_size == 0xFFFFF000` 时 guest 的 `add eax,1000h`
    //   回绕成 0（`0x4367F9..0x436801` 走 `new[](0)` ⇒ `_malloc(0)` ⇒ `HeapAlloc(1)`），那条形态
    //   既无意义又没法用"同一请求字节数"表达 ⇒ 拒绝并回退（解释器照原样跑）。
    if (in_size > kLzoInputLimit) return decline(LzssWhy::Overflow);
    if (self == 0u || self > kLzoSelfLimit || !mem.committed(self, kLzoObjectBytes)) {
        return decline(LzssWhy::BadSelf);
    }
    // ★ 输入跨度的提交检查只在 `a2 + a3[0]` **不回绕**时做（回绕 ⇒ 首灌 0 次 ⇒ 一个字节都不读）。
    const std::uint32_t in_end = in_ea + in_size;
    if (in_size != 0u && in_end > in_ea && !mem.committed(in_ea, in_size)) return decline(LzssWhy::BadInput);
    // ★ 只接管"**新对象**"（两个真实调用点都是 `new Compress` + ctor ⇒ `out == 0`）。
    //   `out != 0` 时 guest 会先 `delete[]` 再 `new[]`，与"失败可回退（零写）"不可兼得 ⇒ 拒绝并落桶。
    if (mem.rd32(self + kLzoOut) != 0u) return decline(LzssWhy::ReusedOut);
    if (cpu.heap == nullptr || !cpu.heap->ready()) return decline(LzssWhy::NoHeap);
    const std::uint32_t alloc_bytes = in_size + kLzoAllocExtra;
    // ★ 分配在**任何 guest 写之前**（与 guest 的 `0x436801 call new[]` 同一条路径：同一个 GuestHeap、
    //   同一请求字节数、同一个 zero=false）；失败 ⇒ 回退解释器，⛔ 不编造 EAX=0。
    const std::uint32_t p = cpu.heap->alloc(alloc_bytes, /*zero=*/false);
    if (p == 0u) return decline(LzssWhy::AllocFailed);

    // ---- 从这里开始才写 guest 字节；顺序照 0x4367D4..0x436809 ----
    mem.wr32(info_ea + 8u, 0u);  // a3[2] = 0
    mem.wr32(self + kLzoOut, p);  // [this+0xD42C] = p（旧值必为 0：准入第 5 条）
    const LzssEncodeOutcome r = lzssEncodeObject(mem.host(self), in_ea, mem.host(in_ea), in_size, mem.host(p));
    mem.wr32(info_ea + 4u, r.consumed);  // a3[1] = 实际消费数（**按计数**，⛔ 不写死 = a3[0]）
    mem.wr32(info_ea + 8u, r.comp);      // a3[2] = comp
    *result = p;                         // EAX = [this+0xD42C] = p
    return true;
}

}  // namespace lift
