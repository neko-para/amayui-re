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
#include "runtime/native_huffman.h"
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
                           NativeBlockFn fn, std::string* err, bool feeder_ok) {
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
    e.feeder_ok = feeder_ok;
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
    //   已进 `all`（2026-11-11）：准入判据 = 实现 + 独立复核（见 `native_overrides.h` 的五条契约）。
    //   逐位判据 = tests/native_override_assets.cpp 的**函数级真字节两臂**（dst / 寄存器 / x87 状态字
    //   / 非默认控制字），回退零写 = tests/native_override.cpp 的 `affine/decline-no-write`。
    //   ★ 已知边界（登记，不是缺陷）：空循环（bound<=i0）**没有**快路径 —— `.lst` 0x4CCC71..0x4CCCDB
    //     的序言在 `0x4CCCDE jge` 之前，guest 即使不进循环也会置 `fpu.status` 的 PE/ES 并清 C0..C3 ⇒
    //     那一支一律回退解释器；另外它和其他函数级 override 一样会移动虚拟时钟相位（见头文件的准入门）。
    //     可用 `all,-sub_4CCC60` 隔离。
    {"sub_4CCC60", GuestConv::Stdcall, 28, nativeAffine4CCC60},
    // ★ 2026-11-11：退出写存档尾段的 **LZSS Encode 整函数**（`retn 8`、thiscall、EAX = `this->out`；
    //   `.lst` 0x4367B0..0x436A75）。1 次拦截覆盖尾段 92.85% 的墙钟（`incl=3328.1 ms` / `calls=1`）。
    //   契约见 `native_overrides.h` 的 `LzssEncodeOutcome` / `LzssWhy` 段。
    //   已进 `all`（2026-11-11）：准入判据 = 实现 + 独立复核（见 `native_overrides.h` 的五条契约）。
    //   真字节两臂差分见 tests/native_override_assets.cpp（解释器臂 = 同一 EA 的 oracle）；
    //   "回退之后 guest 自己走完 delete[]/new[]" 见同文件的 `lzss/decline-replay`。
    //   ★ 已知边界（登记，不是缺陷）：省掉的 guest 指令数**随数据变**（没有定值 k 可补）⇒ 存档里的
    //     虚拟时钟时间戳会比基线早一截（相位偏移）；可用 `all,-sub_4367B0` 隔离。
    {"sub_4367B0", GuestConv::Thiscall, 8, nativeLzss4367B0},
    // ★ 2026-11-11：libvorbis `mdct_backward`（cdecl 12 B = `(lookup*, float* in, float* out)`；
    //   `.lst` 0x4E0B40..0x4E0DB8；调用点 0x4E2EAA 是 `in == out` 的原地调用）。
    //   为什么是它：语音起播那一下 `sub_4E2A30 -> sub_4E0B40` = **163.3 ms / 语音**（区间 profiler），
    //   里面包含整条 FFT 链（`sub_4E08D0` 驱动 + `sub_4E0760` 蝶形 + `sub_4E09D0` 位反转）。
    //   ★ **数值容忍档**（本仓第一项）：不是逐位等价 —— guest 走 x87 80-bit 中间值，本实现用 double
    //     中间值 + float 落盘 ⇒ 每个存储值最多差 1 ulp。用户口径（2026-11-11）：音频可接受偏差。
    //     ⇒ 不满足"函数级字节等价档"；`EAX` 按 0 写（`void` 函数，调用点不读它）；不复刻 x87 的
    //       PE/UE 粘性位。契约与判据见 native_mdct.h；差分守卫见 tests/native_override_assets.cpp。
    //     可用 `all,-sub_4E0B40` 隔离。
    {"sub_4E0B40", GuestConv::Cdecl, 12, nativeMdct4E0B40},
    // ★ 2026-11-12：`__ftol2_sse` = MSVC 的 x87 → int64 截断辅助（`0x4E82C0`，28 B 入口；
    //   本体 `0x4E82F6 = __ftol2` 117 B）。它在 `sub_48D610`（Ogg 解码）底下，本机单句占 VOICE 约 10%。
    //   `arg_bytes = 0`：两条 x87 路径都是裸 `retn`（无栈参、无 `fstp st` 之外的栈动作）。
    //   ★ 名字必须是 `.lst` 里的符号本身（`__ftol2_sse`；`COLLAPSED FUNCTION` 行也进函数表，
    //     见 `lst_index.cpp`）⇒ `findFunction("__ftol2_sse")` 能命中 `0x4E82C0`。
    //   ⛔ 不要写 `__ftol2`：那是**函数内** `0x4E82E3 jz` 的目标，385 个 `call __ftol2_sse`
    //     一个都落不到它 ⇒ hits=0 却全绿（本单的反面判据）。
    //   ★ 本项是**唯一**写 `EDX` 的函数级 override（返回 `EDX:EAX`）；契约段见 native_overrides.h。
    //   判据：tests/native_override_assets.cpp 的 `ftol/*`（解释器 vs native 的边界表 + `fpu.status`
    //   逐位 + scratch 的 4 字节）；回退零写 = tests/native_override.cpp 的 `ftol/decline-no-write`。
    //   已知不覆盖（登记，不修）：`0x4B5545 jmp __ftol2_sse`（`sub_4B5500` 的 qsort 比较器尾跳）
    //   —— dispatcher 只在上一条是 `call` 时拦（`machine.cpp` 的 `call_target`）⇒ 这条既不
    //   `+hits` 也不 `+fallbacks`；另有 `0x4BC539` / `0x4BC680` 直连 `__ftol2`（不经 `0x4E82C0`）。
    //   ⇒ "hits 与 385 个 call 站点吻合"只是**下界**（实际入口 386 个）。
    //   可用 `all,-__ftol2_sse` 隔离。
    {"__ftol2_sse", GuestConv::Cdecl, 0, nativeFtol2Sse},
    // ★ 2026-11-12：libvorbis 的 **Huffman 子树两片叶子**（REQ-01M75WED0VFTEYXX43KGHNPPP0 的 S2）。
    //   为什么是这两片、为什么**不**换整块 `sub_4E3EE0`：窗口 profile 的 self 拆分
    //   （`sub_4E2A30 -> sub_4E4370 incl = 599.0 ms / 3 刀`）里，`0x4E1570` 整棵 + `0x4E1950`
    //   合计 ≈ 86%；剩下的 28 ms 才是循环 + arena 分配（`0x4D7A60` 本身 ≈ 0.3 ms/刀）。
    //   ⇒ 只换叶子就**结构性绕开** arena：两项都零分配、零 guest 回调 ⇒ `sub_4E3EE0`（仍在 guest 里跑）
    //     用同一批参数/顺序/尺寸调 `0x4D7A60`，arena 的 5 个字段与分配序列逐字节不变。
    //   ★ 两项是**成对**的（见 kMax 的注释）：`0x4E1950` 每 entry 调一次 `0x4E1570`。
    //   ★ 三处**别按"标准 Huffman"想当然**（逐字依据见 `native_huffman.h`）：
    //     ① `0x4E1666 adv(codelengths[lo])` 与 `0x4E167C adv(read)` 两条分支的 adv 字节数不同；
    //     ② `0x4E15A5 sar esi,0Fh`（算术右移，不是 `shr`）；
    //     ③ 位读状态在 guest 内存（`+0/+4/+0xC` 三个 dword）⇒ override 读写同一份。
    //   ★ `entry == 0` 必须 decline：直通支会读 `codelengths[-1]`（`0x4E15BB`），不复刻越界读。
    //   ★ 失败是**静默的**：`0x4E40A9 xor eax,eax` 让成功与失败都返回 0
    //     ⇒ 差 1 bit 引擎零信号。判据只能是"真字节两臂逐字节"
    //     （tests/native_override_assets.cpp 的 `huff/*`），⛔ 不能拿 hits/fallbacks 当判据。
    //   可用 `all,-sub_4E1570` / `all,-sub_4E1950` 各自隔离。
    {"sub_4E1570", GuestConv::Cdecl, 8, nativeHuffDecode4E1570},
    {"sub_4E1950", GuestConv::Cdecl, 16, nativeHuffVector4E1950},
};
constexpr std::size_t kKnownCount = sizeof(kKnown) / sizeof(kKnown[0]);
// ★ 表容量是**硬约束**：`--native-overrides=all` 会把每个 `debug == false` 的项都 `add()` 一遍
//   （machine.cpp 的 configureNativeOverrides），装不下 ⇒ add() 失败 ⇒ configure 返回 false ⇒
//   **模拟器直接拒绝启动**。把它变成编译期错误，而不是等到运行时"all 突然起不来"。
static_assert(kKnownCount <= NativeOverrideTable::kMax,
              "kKnown[] 的项数超过 NativeOverrideTable::kMax：请同时抬高 kMax（见头文件的注释）");

// ★ 段级项（函数中段）：entry/resume 是固定的 `.lst` EA（中段没有符号），启用前用 `.lst` 校验锚。
//   `name` 就是 `--native-overrides` 里写的名字（与函数级同名不会冲突：函数级表里没有 sub_414AC0）。
//   `entry_insn / resume_insn` = 那两条指令的**期望文本**（`.lst` 指令列；`verifyKnownBlockAnchor`
//   逐项校验）。⛔ 以前这两条是硬编码在校验函数里的 —— 加第二项时必然逐字不匹配、`all` 静默跳过。
//   `feeder_ok` 见 `NativeBlockOverride::feeder_ok`。
const KnownBlockNative kKnownBlocks[] = {
    // SYS4INI 填池循环（boot 期主线程装载器的中段）。feeder 永远走不到 ⇒ `feeder_ok=false`
    // （真走到了也只会 Declined 然后永久停用 + 一行 stderr 噪声，没有收益）。
    {"sub_414AC0", "sub_414AC0", 0x00415384u, 0x0041539Fu, nativeFillPool414AC0,
     "mov ecx, [esi-10h]", "mov eax, [ebx+5D7ECh]", /*feeder_ok=*/false},
    // `sub_48D610`（Ogg 解码）的 PCM 转换段：feeder 是它的**主消费者**（Ogg 解码全在喂料线程里）
    // ⇒ `feeder_ok=true`。段体内不含设备格式 / 分配 / guest 回调。
    {"sub_48D610", "sub_48D610", 0x0048D767u, 0x0048D89Au, nativePcm48D767,
     "mov eax, [ebp+var_118]", "mov eax, [ebp+var_114]", /*feeder_ok=*/true},
    // `sub_42F560` 的**插入排序内层扫描**段（`fellow-enter` 卡顿主因；契约见 `native_overrides.h`）。
    // ★ 执行区域 = `[0x42F5E0, 0x42F6EB)` ∪ `{0x42F783..0x42F793}`（落位块在 `0x42F782 retn` **之后**，
    //   靠 `0x42F793 jmp 0x42F6EB` 回出口）⇒ `resume_ea` 仍然唯一。
    // ★ 两个锚的期望文本逐字取自 `.lst`（`entry` = `lea eax,[ecx-1]` / `resume` = `mov edi,1`）。
    //   写成别的（例如 `loc_42F5E0`）⇒ `verifyKnownBlockAnchor` 失败 ⇒ `addBlock` 的 `required=false`
    //   分支**静默跳过** ⇒ `all` 里没有这一项而所有差分类测试照样绿（那条路径只能靠 assets 的
    //   `loader` 用例抓，本单不新增）。
    // ★ `feeder_ok=false`：段级项与音频喂料线程无关（`true` 只会白付一次每迭代的函数调用）。
    // ★ **`debug` 必须显式 `false`**：`all` 会跳过 `debug=true` 的项（`machine.cpp` 的 configure），
    //   而 A 的 A/B 就是 `all` vs `all,-sub_42F560` ⇒ 没进 `all` 的表现是"两臂一模一样、全绿"。
    {"sub_42F560", "sub_42F560", 0x0042F5E0u, 0x0042F6EBu, nativeSortStep42F560,
     "lea eax, [ecx-1]", "mov edi, 1", /*feeder_ok=*/false, /*debug=*/false},
};
constexpr std::size_t kKnownBlockCount = sizeof(kKnownBlocks) / sizeof(kKnownBlocks[0]);
// ★ 段表的容量同样是**硬约束**（A7-5）：`--native-overrides all` 会把每个 `debug == false` 的段级项都
//   `add()` 一遍，装不下 ⇒ `configureNativeOverrides` 返回 false ⇒ **模拟器直接拒绝启动**。
//   以前只有函数表有这条断言（`kKnown` 那条），段表没有 ⇒ 加第 5 个段级项时是"运行时起不来"
//   而不是编译错。判据与函数表那条同形。
static_assert(kKnownBlockCount <= NativeBlockTable::kMax,
              "kKnownBlocks[] 的项数超过 NativeBlockTable::kMax：请同时抬高 kMax（见 native_overrides.h）");
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
    // ★ 期望文本随项走（`KnownBlockNative::entry_insn / resume_insn`）。以前这里是两条硬编码字符串，
    //   只对 `sub_414AC0` 成立 —— 加第二项时 entry/resume 必然逐字不匹配，而 `all` 的分支是
    //   `required=false` ⇒ **静默跳过**（`machine.cpp` 的 addBlock），表现是"优化没生效"而不是红灯。
    if (k.entry_insn == nullptr || k.entry_insn[0] == '\0')
        return bad("已知表里没有登记 entry 指令的期望文本（entry_insn）");
    if (k.resume_insn == nullptr || k.resume_insn[0] == '\0')
        return bad("已知表里没有登记 resume 指令的期望文本（resume_insn）");
    const LstFunction* fn = lst.findFunction(k.enclosing_fn);
    if (fn == nullptr) return bad(std::string("`.lst` 里找不到函数 ") + k.enclosing_fn);
    const LstFunction* owner = lst.functionAt(k.entry_ea);
    if (owner == nullptr || owner->ea != fn->ea)
        return bad("entry EA 不在 " + std::string(k.enclosing_fn) + " 里");
    const LstEntry* entry = lst.find(k.entry_ea);
    if (entry == nullptr) return bad("entry EA 不是 `.lst` 里的一条指令");
    // 首指令必须是我们取证到的那条（换镜像 / 换 EA ⇒ 拒绝启用，⛔ 不静默换一个 EA）。
    if (normalizeAsm(entry->text) != normalizeAsm(k.entry_insn))
        return bad("entry EA 的首指令不是 `" + std::string(k.entry_insn) + "`（读到：" +
                   normalizeAsm(entry->text) + "）");
    const LstEntry* resume = lst.find(k.resume_ea);
    if (resume == nullptr) return bad("resume EA 不是 `.lst` 里的一条指令");
    if (normalizeAsm(resume->text) != normalizeAsm(k.resume_insn))
        return bad("resume EA 的首指令不是 `" + std::string(k.resume_insn) + "`（读到：" +
                   normalizeAsm(resume->text) + "）");
    // ★ 段必须**单入口单出口**且落在同一个函数里：resume 的**最近函数入口**也必须是 `enclosing_fn`
    //   （否则 fn 写的 EIP 会跳到别的函数、段级替换变成"跨函数跳转"）。这条也是防"把 entry 写成
    //   函数入口"的那一类错（`resume_ea` 不是可以放行的范围，见头文件 `KnownBlockNative` 的注释）。
    //   ⚠ `LstIndex::functionAt` 的语义是"**同段里最近的前一个函数入口**"，**不是区间检查**：
    //     resume 落在函数末尾之后、下一个函数入口之前时，它仍会返回 enclosing_fn ⇒ 这一条会通过。
    //     当前 `0x48D89A` 确实在 `sub_48D610` 内（且紧接段尾）⇒ 行为正确；要做成真正的区间检查，
    //     得拿"下一个函数入口的 EA"当右界（索引没暴露它的顺序查找接口）—— 本单不做，登记在这里。
    const LstFunction* resume_owner = lst.functionAt(k.resume_ea);
    if (resume_owner == nullptr || resume_owner->ea != fn->ea)
        return bad("resume EA 不在 " + std::string(k.enclosing_fn) + " 里");
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
    // ★ 只接管"**新对象**"（两个真实调用点的 `out` **都为 0**：一个是 `new Compress`（`.lst`
    //   `0x4375ED push 0D430h` / `0x4375F2 call ??2` / `0x43760D call sub_4364E0`），另一个是**栈局部**
    //   对象（`0x457D17 lea ecx,[ebp+var_D44C]` / `0x457D25 call sub_4364E0`）；两者都由 ctor
    //   `0x4364E8` 把 `+0xD42C` 清零）。
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

// =======================================================================================
// `sub_42F560` · 插入排序内层扫描段（**段级**；契约 / 闸门 / 闭式见 `native_overrides.h` 的对应段落）
//
// ★ 为什么实现落在本文件（而文件头说"这里不放具体热点实现"）：本单的改动面被钉成
//   "native_overrides.h / native_overrides.cpp / tests/native_override*.cpp"（与上面 `sub_4367B0`
//   完全同一情形）⇒ 纯核 + 薄包装都放这里。⛔ 不要因为它在这里就把它当成机制的一部分去复用。
// =======================================================================================
namespace {

// 五个 guest 全局：`.lst` 的**绝对 EA 是锚**（⛔ 不从 `this`/名字推）。
//   `g_j` = 0x55D598（`0x42F5E3` 写 / `0x42F6BD` / `0x42F6CA` 读）、`g_n` = 0x55D59C（`0x42F651` / `0x42F682` 读）、
//   `C` = 0x55D5A0（`0x42F62A`）、`B` = 0x55D5A4（`0x42F5F9` / `0x42F656`）、`A` = 0x55D5A8（`0x42F5F0` / `0x42F6B2`）。
constexpr std::uint32_t kSortSegGj = 0x0055D598u;
constexpr std::uint32_t kSortSegGlobalsEa = 0x0055D598u;
constexpr std::uint32_t kSortSegGlobalsBytes = 5u * 4u;  // [0x55D598, 0x55D5AC) 五个 dword（G5）
constexpr std::uint32_t kSortSegGn = 0x0055D59Cu;
constexpr std::uint32_t kSortSegC = 0x0055D5A0u;
constexpr std::uint32_t kSortSegB = 0x0055D5A4u;
constexpr std::uint32_t kSortSegA = 0x0055D5A8u;
constexpr std::uint32_t kSortSegSeedOff = 0x0005EC8Cu;   // seed = [esi + 0x5EC8C]（`0x42F601` 等 4 处）
constexpr std::uint32_t kSortSegResumeEa = 0x0042F6EBu;  // 段出口（`.lst` 73323 `loc_42F6EB`）
constexpr std::uint64_t kSortSegFallbackLimit = 4096u;   // A7-3：连败兜底的阈值

// 半开区间相交（G6 的判据；⛔ 不用"近似"或"只比基点"）。
bool sortSegSpansOverlap(std::uint64_t a0, std::uint64_t a1, std::uint64_t b0, std::uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

}  // namespace

std::uint32_t sortKeyOf(std::uint32_t seed, std::uint32_t v) {
    // 逐字：`0x42F610 rol eax,0Bh` → `0x42F613 xor eax,[ebp+var_14]` → `0x42F616 ror eax,19h`。
    // ★ 每一步都是**32 位**：`v << 11` 与 `mixed << 7` 都在 uint32 上回绕，⛔ 不许先扩到 64 位再 `|`。
    const std::uint32_t rolled = (v << 11) | (v >> 21);  // rol32(v, 11)
    const std::uint32_t mixed = rolled ^ seed;
    return (mixed >> 25) | (mixed << 7);                 // ror32(x, 25)
}

std::uint64_t sortSegReplacedInsns(std::uint32_t comparisons, bool stopped) {
    // 见头文件的逐项归属：`4 + 63*(c-1) + 56 + 落位`，落位 = 4（Stopped，区间外那 4 条）/ 3（RanOut 直落）。
    return 63ull * static_cast<std::uint64_t>(comparisons) + (stopped ? 1ull : 7ull);
}

BlockOverrideOutcome nativeSortStep42F560(X86& cpu, GuestMemory& mem, std::uint64_t* replaced_insns) {
    *replaced_insns = 0;
    // ---- A7-3：连败兜底。`fallbacks` 是**框架维护的**计数（`machine.cpp` 的段级台账），⛔ 不新增机制。
    //   作用：G1 若恒不成立，就会"每个 block 边界都重试 + 6 次 committed 查询" ⇒ 这里一次性永久停用。
    if (cpu.native_blocks != nullptr) {
        if (const NativeBlockOverride* self = cpu.native_blocks->find(cpu.eip)) {
            if (self->fallbacks > kSortSegFallbackLimit) return BlockOverrideOutcome::Declined;
        }
    }
    // ===================== 阶段 1：**只读**（一个 guest 字节都不写） =====================
    // G5 先做：safe mode 下裸 `rd32` 会 guestFault ⇒ `std::_Exit(3)`（`memory.cpp` 的 guestFault）。
    if (!mem.committed(kSortSegGlobalsEa, kSortSegGlobalsBytes)) return BlockOverrideOutcome::Failed;
    const std::uint32_t i = mem.rd32(kSortSegGn);  // g_n = 本轮插入位置（也是循环上界）
    const std::uint32_t a_base = mem.rd32(kSortSegA);
    const std::uint32_t b_base = mem.rd32(kSortSegB);
    const std::uint32_t c_base = mem.rd32(kSortSegC);
    // G1：`ECX == g_n`。★ 失败交 `Failed`（⛔ 不是 `Declined`：后者会永久停用 ⇒ "优化安静地没了"）。
    if (cpu.ecx() != i) return BlockOverrideOutcome::Failed;
    // G2：seed 的来源（段内唯一没被 G4/G5 覆盖的读）。
    const std::uint32_t esi = cpu.esi();
    if (esi == 0u) return BlockOverrideOutcome::Failed;
    const std::uint64_t seed_ea = static_cast<std::uint64_t>(esi) + kSortSegSeedOff;
    if (seed_ea + 4ull > GuestMemory::kSpace) return BlockOverrideOutcome::Failed;
    if (!mem.committed(static_cast<std::uint32_t>(seed_ea), 4u)) return BlockOverrideOutcome::Failed;
    // G3：`g_n >= 1`（`g_n == 0` 是 `0x42F5EA js` 直落那支；`0x42F5C0` 把 `g_i` 置 1 ⇒ 结构上不可达）。
    if (i < 1u) return BlockOverrideOutcome::Failed;
    // G8：预算闸门（见头文件的"单次持有时界"）。放在 G4 之前：它更便宜，且语义上"这一档不做"先于
    // "这一段可不可读"。
    if (i > kSortSegMaxStep) return BlockOverrideOutcome::Failed;
    // G4：三个跨度 `[base, base + 4*(i+1))`。★ 端点用 uint64 算（A7-4），判完再窄回。
    const std::uint64_t span = 4ull * (static_cast<std::uint64_t>(i) + 1ull);
    const std::uint64_t a_begin = a_base;
    const std::uint64_t a_end = a_begin + span;
    const std::uint64_t b_begin = b_base;
    const std::uint64_t b_end = b_begin + span;
    const std::uint64_t c_begin = c_base;
    const std::uint64_t c_end = c_begin + span;
    if (a_end > GuestMemory::kSpace || b_end > GuestMemory::kSpace || c_end > GuestMemory::kSpace)
        return BlockOverrideOutcome::Failed;
    const std::size_t span_bytes = static_cast<std::size_t>(span);
    if (!mem.committed(a_base, span_bytes) || !mem.committed(b_base, span_bytes) ||
        !mem.committed(c_base, span_bytes))
        return BlockOverrideOutcome::Failed;
    // G6：写域 W(A) 与读域（B / C / seed / 五个全局）**区间不相交**。⛔ 用近似判据就会与 guest 分叉
    //   （guest 每比较都重读 B[A[j]] / C[A[j]] / B[g_n] / C[g_n] 与 4 次 seed）。
    const std::uint64_t globals_begin = kSortSegGlobalsEa;
    const std::uint64_t globals_end = globals_begin + kSortSegGlobalsBytes;
    if (sortSegSpansOverlap(a_begin, a_end, b_begin, b_end) ||
        sortSegSpansOverlap(a_begin, a_end, c_begin, c_end) ||
        sortSegSpansOverlap(a_begin, a_end, seed_ea, seed_ea + 4ull) ||
        sortSegSpansOverlap(a_begin, a_end, globals_begin, globals_end))
        return BlockOverrideOutcome::Failed;
    const std::uint32_t seed = mem.rd32(static_cast<std::uint32_t>(seed_ea));
    auto keyAt = [&](std::uint32_t k) {
        // `K(k) = HB(k) + HC(k)`，两条 `add`（`0x42F6AC` / `0x42F6AE`）**32 位回绕**。
        return sortKeyOf(seed, mem.rd32(b_base + 4u * k)) + sortKeyOf(seed, mem.rd32(c_base + 4u * k));
    };
    const std::uint32_t key_i = keyAt(i);  // `K(g_n)`：guest 每次比较都重算（这是 G6 存在的理由之一）
    // ---- 扫描（G7 在这里顺带判；仍在阶段 1）----
    std::int64_t p = -1;
    std::uint32_t comparisons = 0;
    bool stopped = false;
    for (std::int64_t j = static_cast<std::int64_t>(i) - 1; j >= 0; --j) {
        const std::uint32_t aj = mem.rd32(a_base + 4u * static_cast<std::uint32_t>(j));
        if (aj >= i) return BlockOverrideOutcome::Failed;  // G7：越界读 B/C ⇒ 交解释器
        ++comparisons;
        if (keyAt(aj) <= key_i) {  // `0x42F6B7 jle`：**等号要停**（稳定性的唯一来源，见 `sortseg/stable-equal-keys`）
            p = j;
            stopped = true;
            break;
        }
    }
    // ===================== 阶段 2：写（此时不可能再失败） =====================
    for (std::int64_t j = static_cast<std::int64_t>(i) - 1; j > p; --j) {
        const std::uint32_t from = a_base + 4u * static_cast<std::uint32_t>(j);
        const std::uint32_t to = a_base + 4u * static_cast<std::uint32_t>(j + 1);
        mem.wr32(to, mem.rd32(from));  // `0x42F6C3` / `0x42F6C6`
    }
    mem.wr32(a_base + 4u * static_cast<std::uint32_t>(p + 1), i);  // `0x42F6E7` / `0x42F78F`
    // `g_j` 的终值 = p：guest 在 `0x42F6D0`（RanOut）与 `0x42F6CF` 之后写的就是它；Stopped 时它已经是 p。
    mem.wr32(kSortSegGj, static_cast<std::uint32_t>(p));
    cpu.eip = kSortSegResumeEa;
    // ★ `ESP` / `EBP` / `ESI` 一个字都不动（出口处还要用 ESI）；其余寄存器与 EFLAGS 是**登记的边界**，
    //   见头文件"寄存器 / ABI"那段（⛔ 不要为了"更保险"去复刻它们 —— 那会把假红引进来）。
    *replaced_insns = sortSegReplacedInsns(comparisons, stopped);
    return BlockOverrideOutcome::Replaced;
}

}  // namespace lift
