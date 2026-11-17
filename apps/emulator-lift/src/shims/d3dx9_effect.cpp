// emulator-lift · D3DX9 effect：RT_RCDATA 读取 + `D3DXCreateEffectFromResourceA` + ID3DXEffect 最小面
//
// ★ 读 `shims/d3dx9_effect.h` 的文件头先：取法（为什么是确定的）、9 个取证槽、P1 的"只记状态"
//   边界、以及"未收录 ⇒ 响亮停"的判据。
// ★ 本文件**只依赖** guest 内存 / guest 堆 / `PeImage`（不碰 GPU、不碰 SDL、不提交 draw）。
#include "shims/d3dx9_effect.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/d3dx9_sprite.h"  // HRESULT 常量（kOk / kInvalidCall / …）与同一套 D3DX 口径
#include "runtime/exec.h"
#include "runtime/heap.h"
#include "runtime/image.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/pe_resource.h"
#include "runtime/sha256.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace d3dx9 {
namespace {

// 入表（S0 §2.3 的读数；大小 + 完整 sha256 都来自**实际字节**，⛔ 不是估计值）。
// ★ 这张表是 P1 "未收录 ⇒ 响亮停" 的**唯一**收录集；P2 会把"sha256 → 后端管线"挂到它上面。
constexpr EffectResourcePin kPins[] = {
    {kEffectIdFadePs10, 892, "678dd662f4d7bc98eb4042261dd20571f11ea70045e66f38c8548d17197f8518",
     "Fade（ps_1_0）"},
    {kEffectIdFadePs20, 1077, "fb76f089c287e69a0928ef640ed15e1044f87ff19e3e739be2192ee2a46d419b",
     "Fade（ps_2_0，alpha 归一化）"},
    {kEffectIdSnowPs10, 899, "a948d29bc5aab3e0fa62ec551903a0f102ad56e3cd5e5943d0a8c0c4bd0473ac",
     "Snow（vs_1_0 + ps_1_0）"},
    {kEffectIdBlurPs20, 1776, "0ec494f035f7a6678b0b8aea1ec25a3ed8b4967b6084b8e4469a3e94ec5847bd",
     "SlideBlur / ZoomBlur（ps_2_0）"},
};

constexpr std::uint32_t kFxMagicValue = 0x31305846u;  // 'FX01'
constexpr std::uint32_t kFxBytes = 96u;
enum FxOff : std::uint32_t {
    kFxMagic = 0,
    kFxDevice = 4,
    kFxResourceId = 8,
    kFxSourceSize = 12,
    kFxSha = 16,            // guest EA：64 个十六进制字符 + NUL（诊断 / 响亮停用）
    kFxTech = 20,           // guest EA：当前 technique 名（0 = 没有）
    kFxPassesReported = 24, // 最近一次 Begin 写给 *pPasses 的值
    kFxPassIndex = 28,
    kFxActive = 32,
    kFxInPass = 36,
    kFxSetFloats = 40,
    kFxSetTextures = 44,
    kFxSetTechniques = 48,
    kFxBegins = 52,
    kFxBeginPasses = 56,
    kFxCommits = 60,
    kFxEndPasses = 64,
    kFxEnds = 68,
    kFxFloatCount = 72,
    kFxTexCount = 76,
    kFxFloatTable = 80,  // guest EA：kFxFloatCap 项 × 8 B（name_ea, value bits）
    kFxTexTable = 84,    // guest EA：kFxTexCap 项 × 8 B（name_ea, texture EA）
    kFxFloatCap = 88,
    kFxTexCap = 92,
};
constexpr std::uint32_t kFxEntryBytes = 8u;
constexpr std::uint32_t kFxInitialFloatCap = 8u;
constexpr std::uint32_t kFxInitialTexCap = 4u;
constexpr std::uint32_t kFxMaxFloatCap = 256u;  // 上限（防跑飞；引擎实测 7 个名字）
constexpr std::uint32_t kFxMaxTexCap = 32u;     // 引擎实测 2 张
constexpr std::uint32_t kFxShaChars = 64u;
// E_OUTOFMEMORY（winerror.h）：guest 堆不够时交回 guest 的 HRESULT（⛔ 不记成 unimplemented）
constexpr std::uint32_t kOutOfMemory = 0x8007000Eu;

// guest 内存里的 u32 自增（没有 rd32Ref 这种接口）
void bump32(X86& cpu, std::uint32_t ea) { cpu.mem->wr32(ea, cpu.mem->rd32(ea) + 1u); }

// 日志前缀与 d3dx9.cpp 同形（`[d3dx9] <what>: `）
void fxLog(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
}

void fxWarn(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Warn, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
}

// loud trap（与 `d3dxTrap` 同形，标签独立：**effect 资源 / 收录集**的缺口与"导出没实现"分得开）。
// ★ 停机种类 = `Trap` ⇒ 非正常结局；⛔ 不静默返回一个"看起来成功"的句柄。
void fxTrap(X86& cpu, const char* what, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== unimplemented D3DX effect ===\n  %s\n  reason: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr,
                 "\n  （⛔ 不返回看起来成功的句柄；未收录的 effect 由 ADR "
                 "REQ-01M7MNYPDXM2NEQR77DXYZM3CJ 要求响亮停）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented D3DX effect";
    cpu.stop_kind = HostStopKind::Trap;
    ++cpu.shim.unimplemented_calls;
}

// 资源耗尽（**不是** unimplemented）：guest 堆不够。口径同 `d3dx9.cpp` 的 `d3dxResourceStop`。
void fxResourceStop(X86& cpu, const char* what, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== D3DX effect 资源耗尽（不是 unimplemented）===\n  %s\n  reason: ",
                 what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n  （真机语义 = E_OUTOFMEMORY；本线在此硬停，见 d3dx9.cpp 的同名处置）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "D3DX effect 资源耗尽";
    cpu.stop_kind = HostStopKind::ResourceExhausted;
    // ⛔ 不 ++cpu.shim.unimplemented_calls：这不是缺口。
}

bool stateOk(X86& cpu, std::uint32_t state, std::string* why) {
    if (cpu.mem == nullptr) {
        if (why) *why = "D3DX9 effect：cpu.mem 为空";
        return false;
    }
    if (state == 0 || !cpu.mem->committed(state, kFxBytes)) {
        if (why) *why = "D3DX9 effect：状态块为 NULL / 未提交";
        return false;
    }
    if (cpu.mem->rd32(state + kFxMagic) != kFxMagicValue) {
        if (why) *why = "D3DX9 effect：状态块 magic 不是 'FX01'（重复释放 / 野指针？）";
        return false;
    }
    return true;
}

// 在 guest 堆上拷一份名字串（调用方负责释放）。0 = 堆不够。
std::uint32_t allocName(X86& cpu, const std::string& name) {
    const std::uint32_t n = static_cast<std::uint32_t>(name.size()) + 1u;
    const std::uint32_t ea = cpu.heap->alloc(n, /*zero=*/true);
    if (ea == 0) return 0;
    cpu.mem->writeBytes(ea, name.data(), name.size());
    return ea;
}

// 名字表里找同名项；-1 = 没有。`table` = 表 EA，`count` = 已用项数。
int findName(X86& cpu, std::uint32_t table, std::uint32_t count, const std::string& name) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t name_ea = cpu.mem->rd32(table + i * kFxEntryBytes);
        if (name_ea != 0 && cpu.mem->readCString(name_ea) == name) return static_cast<int>(i);
    }
    return -1;
}

// 表满 ⇒ 扩一倍（上限见常量）。返回 false = 堆不够 / 超过上限（why 点名）。
bool growTable(X86& cpu, std::uint32_t state, std::uint32_t table_off, std::uint32_t cap_off,
               std::uint32_t max_cap, const char* what, std::string* why) {
    const std::uint32_t cap = cpu.mem->rd32(state + cap_off);
    if (cap >= max_cap) {
        if (why) *why = std::string("D3DX9 effect：") + what + "表已达上限 " + std::to_string(max_cap);
        return false;
    }
    const std::uint32_t next = (cap * 2u > max_cap) ? max_cap : cap * 2u;
    const std::uint32_t table = cpu.mem->rd32(state + table_off);
    const std::uint32_t grown = cpu.heap->realloc(table, next * kFxEntryBytes, /*zero=*/true);
    if (grown == 0) {
        if (why) *why = std::string("D3DX9 effect：guest 堆扩充 ") + what + "表失败：" + cpu.heap->lastError();
        return false;
    }
    cpu.mem->wr32(state + table_off, grown);
    cpu.mem->wr32(state + cap_off, next);
    return true;
}

}  // namespace

const EffectResourcePin* effectResourcePins() {
    return kPins;
}

const EffectResourcePin* effectResourcePinBySha(const std::string& sha256) {
    const std::string want = toLower(sha256);
    for (const EffectResourcePin& pin : kPins) {
        if (want == pin.sha256) return &pin;
    }
    return nullptr;
}

std::string effectSourceNote(const EffectSource& src) {
    std::string note = "id=" + std::to_string(src.id) + " " + std::to_string(src.size) + " B sha256=" +
                       src.sha256 + " 前 " + std::to_string(src.first32_len) + " 字节:";
    char buf[8];
    for (std::uint32_t i = 0; i < src.first32_len; ++i) {
        std::snprintf(buf, sizeof(buf), " %02x", src.first32[i]);
        note += buf;
    }
    return note;
}

namespace {

EffectSourceStatus readFromEntry(const PeResourceEntry& entry, const std::string& entry_name,
                                 EffectSource* out, std::string* why, PeResources* res) {
    std::string err;
    std::vector<std::uint8_t> bytes;
    if (!res->readData(entry, &bytes, &err)) {
        if (why) *why = "D3DX9 effect：读取 RT_RCDATA " + entry_name + " 的字节失败：" + err;
        return EffectSourceStatus::Missing;
    }
    out->id = entry.has_name ? 0u : entry.id;
    out->size = static_cast<std::uint32_t>(bytes.size());
    out->bytes = std::move(bytes);
    out->sha256 = toHex(sha256(out->bytes));
    out->first32_len = out->size < 32u ? out->size : 32u;
    if (out->first32_len != 0) std::memcpy(out->first32, out->bytes.data(), out->first32_len);
    out->pin = effectResourcePinBySha(out->sha256);
    if (out->pin == nullptr) {
        if (why) *why = "D3DX9 effect：资源 " + entry_name + " 的字节**不在收录集**里（" +
                        effectSourceNote(*out) + "）⇒ 重跑收录（ADR：未收录必须响亮停）";
        return EffectSourceStatus::Unrecorded;
    }
    if (out->pin->id != 0) out->id = out->pin->id;  // 按名字取时用收录表里的 id
    if (why) why->clear();
    return EffectSourceStatus::Ok;
}

}  // namespace

EffectSourceStatus readEffectSource(const PeImage& img, std::uint32_t id, EffectSource* out,
                                    std::string* why) {
    if (out == nullptr) {
        if (why) *why = "D3DX9 effect：readEffectSource 的 out 为空";
        return EffectSourceStatus::Missing;
    }
    *out = EffectSource{};
    if (img.bytes.empty()) {
        if (why) *why = "D3DX9 effect：镜像字节为空（cpu.image 没装配？）";
        return EffectSourceStatus::Missing;
    }
    PeResources res;
    std::string err;
    if (!res.init(img, &err)) {
        if (why) *why = "D3DX9 effect：资源目录解析失败：" + err;
        return EffectSourceStatus::Missing;
    }
    if (!res.hasResources()) {
        if (why) *why = "D3DX9 effect：这份镜像**没有**资源目录（RT_RCDATA 无从取）";
        return EffectSourceStatus::Missing;
    }
    PeResourceEntry entry;
    if (!res.findEntry(kRtRcData, id, &entry, &err)) {
        if (why) *why = "D3DX9 effect：RT_RCDATA 里没有 id=" + std::to_string(id) + "（" + err + "）";
        return EffectSourceStatus::Missing;
    }
    return readFromEntry(entry, "id=" + std::to_string(id), out, why, &res);
}

EffectSourceStatus readEffectSourceByName(const PeImage& img, const std::string& name,
                                          EffectSource* out, std::string* why) {
    if (out == nullptr) {
        if (why) *why = "D3DX9 effect：readEffectSourceByName 的 out 为空";
        return EffectSourceStatus::Missing;
    }
    *out = EffectSource{};
    if (img.bytes.empty()) {
        if (why) *why = "D3DX9 effect：镜像字节为空（cpu.image 没装配？）";
        return EffectSourceStatus::Missing;
    }
    PeResources res;
    std::string err;
    if (!res.init(img, &err)) {
        if (why) *why = "D3DX9 effect：资源目录解析失败：" + err;
        return EffectSourceStatus::Missing;
    }
    if (!res.hasResources()) {
        if (why) *why = "D3DX9 effect：这份镜像**没有**资源目录（RT_RCDATA 无从取）";
        return EffectSourceStatus::Missing;
    }
    PeResourceEntry entry;
    if (!res.findEntryByName(kRtRcData, name, &entry, &err)) {
        if (why) *why = "D3DX9 effect：RT_RCDATA 里没有名字 \"" + name + "\"（" + err + "）";
        return EffectSourceStatus::Missing;
    }
    return readFromEntry(entry, "name=\"" + name + "\"", out, why, &res);
}

// ---------------------------------------------------------------------------------------
// 状态块
// ---------------------------------------------------------------------------------------
std::uint32_t createEffectState(X86& cpu, std::uint32_t device_object, const EffectSource& src,
                                std::string* why, bool* out_of_memory) {
    if (out_of_memory) *out_of_memory = false;
    if (cpu.mem == nullptr || cpu.heap == nullptr || !cpu.heap->ready()) {
        if (why) *why = "D3DX9 effect：需要 guest 内存 + 就绪的 guest 堆（状态块放在 guest 堆上）";
        return 0;
    }
    const std::uint32_t state = cpu.heap->alloc(kFxBytes, /*zero=*/true);
    if (state == 0) {
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = std::string("D3DX9 effect：guest 堆分配状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    const std::uint32_t floats = cpu.heap->alloc(kFxInitialFloatCap * kFxEntryBytes, /*zero=*/true);
    const std::uint32_t textures = cpu.heap->alloc(kFxInitialTexCap * kFxEntryBytes, /*zero=*/true);
    const std::uint32_t sha = cpu.heap->alloc(kFxShaChars + 1u, /*zero=*/true);
    if (floats == 0 || textures == 0 || sha == 0) {
        if (floats != 0) cpu.heap->free(floats);
        if (textures != 0) cpu.heap->free(textures);
        if (sha != 0) cpu.heap->free(sha);
        cpu.heap->free(state);
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = std::string("D3DX9 effect：guest 堆分配参数表 / 名字失败：") + cpu.heap->lastError();
        return 0;
    }
    cpu.mem->writeBytes(sha, src.sha256.c_str(), src.sha256.size());
    cpu.mem->wr32(state + kFxMagic, kFxMagicValue);
    cpu.mem->wr32(state + kFxDevice, device_object);
    cpu.mem->wr32(state + kFxResourceId, src.id);
    cpu.mem->wr32(state + kFxSourceSize, src.size);
    cpu.mem->wr32(state + kFxSha, sha);
    cpu.mem->wr32(state + kFxFloatTable, floats);
    cpu.mem->wr32(state + kFxTexTable, textures);
    cpu.mem->wr32(state + kFxFloatCap, kFxInitialFloatCap);
    cpu.mem->wr32(state + kFxTexCap, kFxInitialTexCap);
    return state;
}

void destroyEffectState(X86& cpu, std::uint32_t state) {
    if (cpu.mem == nullptr || cpu.heap == nullptr || !cpu.heap->ready()) return;
    std::string why;
    if (!stateOk(cpu, state, &why)) return;  // 幂等：重复调用 / 野指针 ⇒ 什么都不做
    // ★ effect draw 作用域：这个 effect 被销毁 ⇒ 它开的 Begin..End 作用域跟着结束（⛔ 不留下
    //   一个"永远生效"的拒绝条件）。见 `d3d9.h` 的 `EffectDrawScope`。
    d3d9::noteEffectDrawDestroyed(state);
    // 先清 magic ⇒ 之后任何重入都不会二次释放
    cpu.mem->wr32(state + kFxMagic, 0u);
    const std::uint32_t float_count = cpu.mem->rd32(state + kFxFloatCount);
    const std::uint32_t float_table = cpu.mem->rd32(state + kFxFloatTable);
    for (std::uint32_t i = 0; i < float_count; ++i) {
        const std::uint32_t name_ea = cpu.mem->rd32(float_table + i * kFxEntryBytes);
        if (name_ea != 0) cpu.heap->free(name_ea);
    }
    const std::uint32_t tex_count = cpu.mem->rd32(state + kFxTexCount);
    const std::uint32_t tex_table = cpu.mem->rd32(state + kFxTexTable);
    for (std::uint32_t i = 0; i < tex_count; ++i) {
        const std::uint32_t name_ea = cpu.mem->rd32(tex_table + i * kFxEntryBytes);
        if (name_ea != 0) cpu.heap->free(name_ea);
    }
    const std::uint32_t tech = cpu.mem->rd32(state + kFxTech);
    const std::uint32_t sha = cpu.mem->rd32(state + kFxSha);
    if (tech != 0) cpu.heap->free(tech);
    if (sha != 0) cpu.heap->free(sha);
    if (float_table != 0) cpu.heap->free(float_table);
    if (tex_table != 0) cpu.heap->free(tex_table);
    cpu.heap->free(state);
}

bool effectInfoOfState(X86& cpu, std::uint32_t state, EffectInfo* out, std::string* why) {
    if (out == nullptr) {
        if (why) *why = "D3DX9 effect：effectInfoOfState 的 out 为空";
        return false;
    }
    if (!stateOk(cpu, state, why)) return false;
    *out = EffectInfo{};
    out->state = state;
    out->device = cpu.mem->rd32(state + kFxDevice);
    out->resource_id = cpu.mem->rd32(state + kFxResourceId);
    out->source_size = cpu.mem->rd32(state + kFxSourceSize);
    const std::uint32_t sha = cpu.mem->rd32(state + kFxSha);
    if (sha != 0) out->source_sha256 = cpu.mem->readCString(sha);
    out->active = cpu.mem->rd32(state + kFxActive) != 0;
    out->in_pass = cpu.mem->rd32(state + kFxInPass) != 0;
    out->pass_index = cpu.mem->rd32(state + kFxPassIndex);
    out->passes_reported = cpu.mem->rd32(state + kFxPassesReported);
    const std::uint32_t tech = cpu.mem->rd32(state + kFxTech);
    if (tech != 0) out->technique = cpu.mem->readCString(tech);
    out->set_floats = cpu.mem->rd32(state + kFxSetFloats);
    out->set_textures = cpu.mem->rd32(state + kFxSetTextures);
    out->set_techniques = cpu.mem->rd32(state + kFxSetTechniques);
    out->begins = cpu.mem->rd32(state + kFxBegins);
    out->begin_passes = cpu.mem->rd32(state + kFxBeginPasses);
    out->commits = cpu.mem->rd32(state + kFxCommits);
    out->end_passes = cpu.mem->rd32(state + kFxEndPasses);
    out->ends = cpu.mem->rd32(state + kFxEnds);
    out->float_param_count = cpu.mem->rd32(state + kFxFloatCount);
    out->texture_count = cpu.mem->rd32(state + kFxTexCount);
    return true;
}

bool effectInfo(X86& cpu, std::uint32_t object, EffectInfo* out, std::string* why) {
    if (cpu.com == nullptr || !cpu.com->find(object)) {
        if (why) *why = "D3DX9 effect：对象不在 ComRegistry 里（野指针 / 已释放）";
        return false;
    }
    const std::uint32_t state = cpu.com->userOf(object);
    if (state == 0) {
        if (why) *why = "D3DX9 effect：对象的 obj+8 没有状态块";
        return false;
    }
    return effectInfoOfState(cpu, state, out, why);
}

bool effectFloatParamAt(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* name,
                        float* value) {
    std::string why;
    if (!stateOk(cpu, state, &why)) return false;
    if (index >= cpu.mem->rd32(state + kFxFloatCount)) return false;
    const std::uint32_t entry = cpu.mem->rd32(state + kFxFloatTable) + index * kFxEntryBytes;
    const std::uint32_t name_ea = cpu.mem->rd32(entry);
    const std::uint32_t bits = cpu.mem->rd32(entry + 4);
    if (name) *name = name_ea != 0 ? cpu.mem->readCString(name_ea) : std::string();
    if (value) std::memcpy(value, &bits, sizeof(float));
    return true;
}

bool effectTextureAt(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* name,
                     std::uint32_t* texture) {
    std::string why;
    if (!stateOk(cpu, state, &why)) return false;
    if (index >= cpu.mem->rd32(state + kFxTexCount)) return false;
    const std::uint32_t entry = cpu.mem->rd32(state + kFxTexTable) + index * kFxEntryBytes;
    const std::uint32_t name_ea = cpu.mem->rd32(entry);
    if (name) *name = name_ea != 0 ? cpu.mem->readCString(name_ea) : std::string();
    if (texture) *texture = cpu.mem->rd32(entry + 4);
    return true;
}

// ---------------------------------------------------------------------------------------
// ID3DXEffect 方法（只记状态）
// ---------------------------------------------------------------------------------------
std::uint32_t effectSetFloat(X86& cpu, std::uint32_t state, const std::string& name, float value,
                       std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (name.empty()) {
        if (why) *why = "D3DX9 effect：SetFloat 的参数名为空（D3DX 合法名必须非空）";
        return d3dx9::kInvalidArg;
    }
    const std::uint32_t count = cpu.mem->rd32(state + kFxFloatCount);
    const std::uint32_t table_off = kFxFloatTable;
    const int found = findName(cpu, cpu.mem->rd32(state + table_off), count, name);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (found >= 0) {
        const std::uint32_t entry = cpu.mem->rd32(state + table_off) + static_cast<std::uint32_t>(found) * kFxEntryBytes;
        cpu.mem->wr32(entry + 4, bits);
        bump32(cpu, state + kFxSetFloats);
    } else {
        std::string grew;
        if (count >= cpu.mem->rd32(state + kFxFloatCap)) {
            if (!growTable(cpu, state, kFxFloatTable, kFxFloatCap, kFxMaxFloatCap,
                           "float", &grew)) {
                if (why) *why = grew;
                return kOutOfMemory;
            }
        }
        const std::uint32_t name_ea = allocName(cpu, name);
        if (name_ea == 0) {
            if (why) *why = std::string("D3DX9 effect：guest 堆分配参数名失败：") + cpu.heap->lastError();
            return kOutOfMemory;
        }
        const std::uint32_t entry = cpu.mem->rd32(state + table_off) + count * kFxEntryBytes;
        cpu.mem->wr32(entry, name_ea);
        cpu.mem->wr32(entry + 4, bits);
        cpu.mem->wr32(state + kFxFloatCount, count + 1u);
        bump32(cpu, state + kFxSetFloats);
        fxLog("ID3DXEffect::SetFloat", "\"%s\" = %g（第 %u 个 float 参数）", name.c_str(),
              static_cast<double>(value), count + 1u);
    }
    return d3dx9::kOk;
}

std::uint32_t effectSetTexture(X86& cpu, std::uint32_t state, const std::string& name, std::uint32_t texture,
                         std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (name.empty()) {
        if (why) *why = "D3DX9 effect：SetTexture 的参数名为空（D3DX 合法名必须非空）";
        return d3dx9::kInvalidArg;
    }
    const std::uint32_t count = cpu.mem->rd32(state + kFxTexCount);
    const int found = findName(cpu, cpu.mem->rd32(state + kFxTexTable), count, name);
    if (found >= 0) {
        const std::uint32_t entry = cpu.mem->rd32(state + kFxTexTable) + static_cast<std::uint32_t>(found) * kFxEntryBytes;
        cpu.mem->wr32(entry + 4, texture);
        bump32(cpu, state + kFxSetTextures);
    } else {
        std::string grew;
        if (count >= cpu.mem->rd32(state + kFxTexCap)) {
            if (!growTable(cpu, state, kFxTexTable, kFxTexCap, kFxMaxTexCap, "纹理",
                           &grew)) {
                if (why) *why = grew;
                return kOutOfMemory;
            }
        }
        const std::uint32_t name_ea = allocName(cpu, name);
        if (name_ea == 0) {
            if (why) *why = std::string("D3DX9 effect：guest 堆分配纹理参数名失败：") + cpu.heap->lastError();
            return kOutOfMemory;
        }
        const std::uint32_t entry = cpu.mem->rd32(state + kFxTexTable) + count * kFxEntryBytes;
        cpu.mem->wr32(entry, name_ea);
        cpu.mem->wr32(entry + 4, texture);
        cpu.mem->wr32(state + kFxTexCount, count + 1u);
        bump32(cpu, state + kFxSetTextures);
        fxLog("ID3DXEffect::SetTexture", "\"%s\" = 0x%08X（第 %u 个纹理参数）", name.c_str(), texture,
              count + 1u);
    }
    return d3dx9::kOk;
}

std::uint32_t effectSetTechnique(X86& cpu, std::uint32_t state, const std::string& name, std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    const std::uint32_t old = cpu.mem->rd32(state + kFxTech);
    if (name.empty()) {
        // SetTechnique(NULL)：D3DX 语义 = 清掉当前 technique（合法）
        if (old != 0) cpu.heap->free(old);
        cpu.mem->wr32(state + kFxTech, 0u);
        bump32(cpu, state + kFxSetTechniques);
        return d3dx9::kOk;
    }
    if (old != 0 && cpu.mem->readCString(old) == name) {
        bump32(cpu, state + kFxSetTechniques);
        return d3dx9::kOk;  // 同名 ⇒ 不重新分配
    }
    const std::uint32_t ea = allocName(cpu, name);
    if (ea == 0) {
        if (why) *why = std::string("D3DX9 effect：guest 堆分配 technique 名失败：") + cpu.heap->lastError();
        return kOutOfMemory;
    }
    if (old != 0) cpu.heap->free(old);
    cpu.mem->wr32(state + kFxTech, ea);
    bump32(cpu, state + kFxSetTechniques);
    fxLog("ID3DXEffect::SetTechnique", "\"%s\"", name.c_str());
    return d3dx9::kOk;
}

std::uint32_t effectBegin(X86& cpu, std::uint32_t state, std::uint32_t passes_ea, std::uint32_t flags,
                    std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (cpu.mem->rd32(state + kFxActive) != 0) {
        fxWarn("ID3DXEffect::Begin", "上一次 Begin 还没 End ⇒ 仍然推进（只记状态）");
    }
    // ★ 本段不解析 FX 文本：按 S0 已读到的 4 份资源文本（每个 technique 恰好一个 `pass P0`）
    //   报 1 个 pass。⛔ 不宣称这是解析结果；P2 才真的解析。理由：引擎的 pass 循环必须至少跑一次，
    //   否则 BeginPass/CommitChanges/EndPass 与那条 draw 根本不会发生（P1 的观测面就空了）。
    constexpr std::uint32_t kPassesReported = 1u;
    cpu.mem->wr32(state + kFxPassesReported, kPassesReported);
    if (passes_ea != 0) {
        if (!cpu.mem->committed(passes_ea, 4)) {
            if (why) *why = "D3DX9 effect：Begin 的 pPasses 未提交";
            return d3dx9::kInvalidArg;
        }
        cpu.mem->wr32(passes_ea, kPassesReported);
    }
    cpu.mem->wr32(state + kFxActive, 1u);
    cpu.mem->wr32(state + kFxInPass, 0u);
    bump32(cpu, state + kFxBegins);
    fxLog("ID3DXEffect::Begin", "passes=%u Flags=0x%08X（只记状态，⛔ 不提交 draw）", kPassesReported,
          flags);
    return d3dx9::kOk;
}

std::uint32_t effectBeginPass(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (cpu.mem->rd32(state + kFxActive) == 0) {
        fxWarn("ID3DXEffect::BeginPass", "不在 Begin..End 之间（仍推进状态）");
    }
    if (index != 0) {
        // 本段只报 1 个 pass（见 begin 的注释）⇒ 其它索引是**没被记录的语义**，别静默接受。
        fxWarn("ID3DXEffect::BeginPass", "index=%u 超出本段记录的唯一 pass（0）⇒ 记账但不改像素",
               index);
    }
    cpu.mem->wr32(state + kFxInPass, 1u);
    cpu.mem->wr32(state + kFxPassIndex, index);
    bump32(cpu, state + kFxBeginPasses);
    return d3dx9::kOk;
}

std::uint32_t effectCommitChanges(X86& cpu, std::uint32_t state, std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    bump32(cpu, state + kFxCommits);
    return d3dx9::kOk;
}

std::uint32_t effectEndPass(X86& cpu, std::uint32_t state, std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (cpu.mem->rd32(state + kFxInPass) == 0) {
        fxWarn("ID3DXEffect::EndPass", "没有配对的 BeginPass（仍推进状态）");
    }
    cpu.mem->wr32(state + kFxInPass, 0u);
    bump32(cpu, state + kFxEndPasses);
    return d3dx9::kOk;
}

std::uint32_t effectEnd(X86& cpu, std::uint32_t state, std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (cpu.mem->rd32(state + kFxActive) == 0) {
        fxWarn("ID3DXEffect::End", "没有配对的 Begin（仍推进状态）");
    }
    cpu.mem->wr32(state + kFxActive, 0u);
    cpu.mem->wr32(state + kFxInPass, 0u);
    bump32(cpu, state + kFxEnds);
    return d3dx9::kOk;
}

namespace {

// 从 `this` 取 effect 状态块；不是我们的对象 ⇒ loud trap（返回 0）
std::uint32_t requireEffect(X86& cpu, ShimCall& call, const char* method) {
    const std::uint32_t self = call.u32(0);
    if (cpu.com == nullptr || !cpu.com->find(self)) {
        fxTrap(cpu, method, "this=0x%08X 不是本模块建的 ID3DXEffect 对象", self);
        return 0;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    if (state == 0) {
        fxTrap(cpu, method, "ID3DXEffect 对象 0x%08X 的 obj+8 没有状态块", self);
        return 0;
    }
    return state;
}

// 方法失败的统一出口：`kOutOfMemory` ⇒ 资源耗尽硬停；其余 ⇒ warn 后把 HRESULT 交回 guest。
std::uint32_t reportMethodFailure(X86& cpu, const char* method, std::uint32_t hr,
                                  const std::string& why) {
    if (hr == kOutOfMemory) {
        fxResourceStop(cpu, method, "%s", why.c_str());
        return hr;
    }
    fxWarn(method, "%s", why.c_str());
    return hr;
}

std::string stringArg(X86& cpu, std::uint32_t ea, const char* method) {
    if (ea == 0) return std::string();
    if (!cpu.mem->committed(ea, 1)) {
        fxTrap(cpu, method, "参数名字符串 0x%08X 未提交", ea);
        return std::string();
    }
    return cpu.mem->readCString(ea);
}

// ---------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

bool sameGuid(X86& cpu, std::uint32_t ea, const std::uint8_t (&want)[16]) {
    if (ea == 0 || !cpu.mem->committed(ea, 16)) return false;
    std::uint8_t got[16];
    cpu.mem->readBytes(ea, got, sizeof(got));
    return std::memcmp(got, want, sizeof(got)) == 0;
}

void shimEffectQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        fxWarn("ID3DXEffect::QueryInterface", "ppvObj 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3dx9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        fxWarn("ID3DXEffect::QueryInterface", "riid 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3dx9::kInvalidArg;
        return;
    }
    // ★ 只认 IUnknown。IID_ID3DXEffect 的 GUID 本段**没有可校验的来源**（S0 只钉了槽号；
    //   引擎在 0x4B079D 之后直接用返回的指针、不做 QI）⇒ 别的 IID 一律 E_NOINTERFACE，
    //   ⛔ 不凭记忆写一个 GUID（那会让"QI 得到别的东西"变成静默错误）。
    if (!sameGuid(cpu, riid, kIidUnknown)) {
        fxWarn("ID3DXEffect::QueryInterface",
               "riid 不是 IID_IUnknown ⇒ E_NOINTERFACE（本段没有 IID_ID3DXEffect 的可校验来源）");
        call.result = d3dx9::kNoInterface;
        return;
    }
    if (cpu.com == nullptr || !cpu.com->addRef(cpu, self)) {
        fxWarn("ID3DXEffect::QueryInterface", "this=0x%08X 不在 ComRegistry 里", self);
        call.result = d3dx9::kNoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = d3dx9::kOk;
}

void shimEffectAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (cpu.com == nullptr || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

// Release：对象到 0 由 `ComRegistry` 走 **on_zero 钩子**回收状态块（⛔ 不在这里再回收一次）
void shimEffectRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (cpu.com == nullptr || !cpu.com->find(self)) {
        fxLog("ID3DXEffect::Release", "this=0x%08X 不是本 ComRegistry 的对象 ⇒ 返回 0", self);
        call.result = 0;
        return;
    }
    const std::uint32_t rc = cpu.com->refCount(self);
    const bool last = rc <= 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// 9 个取证槽的其余 7 个
// ---------------------------------------------------------------------------------------
void shimEffectSetFloat(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::SetFloat(+0x78)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::string name = stringArg(cpu, call.u32(1), "ID3DXEffect::SetFloat(+0x78)");
    const std::uint32_t bits = call.u32(2);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    std::string why;
    const std::uint32_t hr = d3dx9::effectSetFloat(cpu, state, name, value, &why);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetFloat", hr, why);
}

void shimEffectSetTexture(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::SetTexture(+0xD0)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::string name = stringArg(cpu, call.u32(1), "ID3DXEffect::SetTexture(+0xD0)");
    const std::uint32_t texture = call.u32(2);
    std::string why;
    const std::uint32_t hr = d3dx9::effectSetTexture(cpu, state, name, texture, &why);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetTexture", hr, why);
}

void shimEffectSetTechnique(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::SetTechnique(+0xE8)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::uint32_t name_ea = call.u32(1);
    // SetTechnique(NULL) 合法（清掉当前 technique）；非 NULL 才读串。
    const std::string name = name_ea == 0 ? std::string()
                                          : stringArg(cpu, name_ea, "ID3DXEffect::SetTechnique(+0xE8)");
    std::string why;
    const std::uint32_t hr = d3dx9::effectSetTechnique(cpu, state, name, &why);
    if (hr == d3dx9::kOk) d3d9::noteEffectDrawTechnique(name);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetTechnique", hr, why);
}

void shimEffectBegin(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::Begin(+0xFC)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::effectBegin(cpu, state, call.u32(1), call.u32(2), &why);
    if (hr == d3dx9::kOk) {
        // ★ 上报 "这次 Begin 开了哪个 effect"（资源 id / technique 从状态块读；见 d3d9.h 的
        //   `EffectDrawScope`）。⛔ 不在这里判 draw —— 判据在 `gpuEligible`（D3D9 层）。
        EffectInfo info;
        std::string info_why;
        if (effectInfoOfState(cpu, state, &info, &info_why)) {
            d3d9::noteEffectDrawBegin(state, call.u32(0), info.resource_id, info.technique);
        }
    }
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::Begin", hr, why);
}

void shimEffectBeginPass(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::BeginPass(+0x100)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::effectBeginPass(cpu, state, call.u32(1), &why);
    if (hr == d3dx9::kOk) d3d9::noteEffectDrawBeginPass(call.u32(1));
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::BeginPass", hr, why);
}

void shimEffectCommitChanges(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::CommitChanges(+0x104)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::effectCommitChanges(cpu, state, &why);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::CommitChanges", hr, why);
}

void shimEffectEndPass(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::EndPass(+0x108)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::effectEndPass(cpu, state, &why);
    if (hr == d3dx9::kOk) d3d9::noteEffectDrawEndPass();
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::EndPass", hr, why);
}

void shimEffectEnd(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::End(+0x10C)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::effectEnd(cpu, state, &why);
    if (hr == d3dx9::kOk) d3d9::noteEffectDrawEnd();
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::End", hr, why);
}

// ID3DXEffect：S0 §3.2 的 9 个取证槽 + IUnknown 的 QueryInterface/AddRef（Release 是第 3 槽）。
// ★ +0x110 GetDevice / +0x114 OnLostDevice / +0x118 OnResetDevice **没被读到** ⇒ fn=nullptr
//   ⇒ `comUnsupported` 的 loud trap（⛔ 不按"应该会调"补）。填在这里的另一个作用：
//   它们落在 vtable 内（⛔ 不越界），trap 时能打印 iface + 偏移。
const std::vector<ComMethod>& effectMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimEffectQueryInterface},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimEffectAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimEffectRelease},              // ★ 0x4AB454
        {0x78u, "SetFloat", Conv::Stdcall, 2, &shimEffectSetFloat},            // ★ 0x4B3D3E / …
        {0xD0u, "SetTexture", Conv::Stdcall, 2, &shimEffectSetTexture},        // ★ 0x4B3D68 / …
        {0xE8u, "SetTechnique", Conv::Stdcall, 1, &shimEffectSetTechnique},    // ★ 0x4B3D21 / …
        {0xFCu, "Begin", Conv::Stdcall, 2, &shimEffectBegin},                  // ★ 0x4B3DAC / 0x4B3667
        {0x100u, "BeginPass", Conv::Stdcall, 1, &shimEffectBeginPass},         // ★ 0x4B3DD0 / 0x4B3690
        {0x104u, "CommitChanges", Conv::Stdcall, 0, &shimEffectCommitChanges}, // ★ 0x4B3DE1 / 0x4B36A1
        {0x108u, "EndPass", Conv::Stdcall, 0, &shimEffectEndPass},             // ★ 0x4B3E0D / 0x4B36CE
        {0x10Cu, "End", Conv::Stdcall, 0, &shimEffectEnd},                     // ★ 0x4B3E28 / 0x4B36E8
        {0x110u, "GetDevice", Conv::Stdcall, 1, nullptr},        // 未取证 ⇒ comUnsupported
        {0x114u, "OnLostDevice", Conv::Stdcall, 0, nullptr},     // 未取证 ⇒ comUnsupported
        {0x118u, "OnResetDevice", Conv::Stdcall, 0, nullptr},    // 未取证 ⇒ comUnsupported
    };
    return methods;
}

// ---------------------------------------------------------------------------------------
// D3DXCreateEffectFromResourceA（9 参 stdcall）
//   HRESULT D3DXCreateEffectFromResourceA(LPDIRECT3DDEVICE9 pDevice, HMODULE hSrcModule,
//       LPCSTR pSrcResource, const D3DXMACRO* pDefines, LPD3DXINCLUDE pInclude, DWORD Flags,
//       LPD3DXEFFECTPOOL pPool, LPD3DXEFFECT* ppEffect, LPD3DXBUFFER* ppCompilationErrors)
//   实参形状（S0 §2.1，callsite 0x004B0798）：pDevice / hSrcModule=NULL / pSrcResource=201 /
//   pDefines=NULL / pInclude=NULL / Flags=0 / pPool=NULL / ppEffect=&[this+0xB590] /
//   ppCompilationErrors=NULL。
// ---------------------------------------------------------------------------------------
void shimD3DXCreateEffectFromResourceA(X86& cpu, ShimCall& call) {
    const char* kApi = "D3DXCreateEffectFromResourceA";
    const std::uint32_t device = call.u32(0);
    const std::uint32_t src_module = call.u32(1);
    const std::uint32_t src_resource = call.u32(2);
    const std::uint32_t defines = call.u32(3);
    const std::uint32_t include = call.u32(4);
    const std::uint32_t flags = call.u32(5);
    const std::uint32_t pool = call.u32(6);
    const std::uint32_t pp_effect = call.u32(7);
    const std::uint32_t pp_errors = call.u32(8);

    if (pp_effect == 0 || !cpu.mem->committed(pp_effect, 4)) {
        // 没有出参 ⇒ 拿不到句柄；这是调用方的契约错，返回失败（⛔ 不假装成功）。
        fxWarn(kApi, "ppEffect=0x%08X 为 NULL / 未提交 ⇒ D3DERR_INVALIDCALL", pp_effect);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(pp_effect, 0u);  // 失败时出参保持 0（⛔ 不留悬空旧值）
    if (device == 0) {
        fxWarn(kApi, "pDevice=NULL ⇒ D3DERR_INVALIDCALL");
        call.result = d3dx9::kInvalidCall;
        return;
    }
    // hSrcModule：NULL = 当前模块；主模块句柄也接受（同一份执行源）。其它 ⇒ 响亮失败
    // （本线只有一份 PE 的 `.rsrc`，别处的模块没有可读的资源树）。
    if (src_module != 0 && (cpu.shim.module_handle == 0 || src_module != cpu.shim.module_handle)) {
        fxTrap(cpu, kApi,
               "hSrcModule=0x%08X 不是主模块句柄 0x%08X ⇒ 本线只从执行源的 .rsrc 取资源"
               "（⛔ 不静默换成当前模块）",
               src_module, cpu.shim.module_handle);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    if (defines != 0 || include != 0) {
        // pDefines / pInclude 会**改变源码语义**（宏替换 / include 展开）；本段不做，⛔ 不忽略它们
        // 假装源码没变（ADR：解析不了 ⇒ 响亮停）。S0 的 4 个调用点都传 NULL，所以这条只挡新形态。
        fxTrap(cpu, kApi, "pDefines=0x%08X pInclude=0x%08X 非 NULL ⇒ 本段不展开宏 / include",
               defines, include);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    if (pool != 0) {
        fxTrap(cpu, kApi, "pPool=0x%08X 非 NULL ⇒ 本线没有 D3DXCreateEffectPool 的对象", pool);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    if (cpu.image == nullptr || cpu.image->bytes.empty()) {
        fxTrap(cpu, kApi, "cpu.image 为空 / 没有字节 ⇒ 取不到 RT_RCDATA（装配问题？）");
        call.result = d3dx9::kFail;
        return;
    }

    const bool by_name = (src_resource >> 16) != 0;
    EffectSource src;
    std::string why;
    EffectSourceStatus status;
    if (by_name) {
        if (!cpu.mem->committed(src_resource, 1)) {
            fxWarn(kApi, "pSrcResource=0x%08X 是字符串指针但未提交 ⇒ D3DERR_INVALIDCALL", src_resource);
            call.result = d3dx9::kInvalidCall;
            return;
        }
        const std::string name = cpu.mem->readCString(src_resource);
        status = readEffectSourceByName(*cpu.image, name, &src, &why);
        if (status == EffectSourceStatus::Missing) {
            fxTrap(cpu, kApi, "RT_RCDATA 里没有名字 \"%s\"：%s", name.c_str(), why.c_str());
            call.result = d3dx9::kFail;
            return;
        }
    } else {
        const std::uint32_t id = src_resource & 0xFFFFu;
        if (id == 0) {
            fxWarn(kApi, "pSrcResource=0（MAKEINTRESOURCE(0)）不是合法资源 id ⇒ D3DERR_INVALIDCALL");
            call.result = d3dx9::kInvalidCall;
            return;
        }
        status = readEffectSource(*cpu.image, id, &src, &why);
        if (status == EffectSourceStatus::Missing) {
            fxTrap(cpu, kApi, "取不到 effect 资源：%s", why.c_str());
            call.result = d3dx9::kFail;
            return;
        }
    }
    if (status != EffectSourceStatus::Ok) {
        // Unrecorded：字节读到了但不在收录集里 ⇒ 打印 id + sha256 + 前 32 字节并停。
        fxTrap(cpu, kApi, "%s", why.c_str());
        call.result = d3dx9::kFail;
        return;
    }
    if (cpu.com == nullptr) {
        fxTrap(cpu, kApi, "cpu.com 为空（装配问题）⇒ 拿不到 guest 可见的 COM 对象");
        call.result = d3dx9::kFail;
        return;
    }
    bool out_of_memory = false;
    const std::uint32_t state = createEffectState(cpu, device, src, &why, &out_of_memory);
    if (state == 0) {
        if (out_of_memory) {
            fxResourceStop(cpu, kApi, "%s", why.c_str());
        } else {
            fxTrap(cpu, kApi, "建 effect 状态块失败：%s", why.c_str());
        }
        call.result = d3dx9::kFail;
        return;
    }
    const std::uint32_t obj =
        cpu.com->createObject(cpu, "ID3DXEffect", effectMethods(), state, &destroyEffectState);
    if (obj == 0) {
        destroyEffectState(cpu, state);  // 半成品不留在 guest 堆里
        fxTrap(cpu, kApi, "ComRegistry::createObject(ID3DXEffect) 失败（槽位 / 堆？）");
        call.result = d3dx9::kFail;
        return;
    }
    // ppCompilationErrors：真机在**成功**时写 NULL。本段没有编译器 ⇒ 只写 0（S0 §5.3：⛔ 不伪造 buffer）。
    if (pp_errors != 0 && cpu.mem->committed(pp_errors, 4)) cpu.mem->wr32(pp_errors, 0u);
    cpu.mem->wr32(pp_effect, obj);
    // ★ 默认级别可见（**每次创建一条，最多 4 条** —— 不是周期性输出）：这是"真的走到了 effect 路"
    //   的**唯一**默认可见证据（缺省 caps 报 0 时引擎不会走到这里 ⇒ 缺省档输出不受影响）。
    //   诊断价值：把"停因从 unimplemented 变成正常结局"这件事与"effect 收录命中"对得上。
    fxWarn(kApi, "%s ⇒ S_OK：ID3DXEffect 对象 0x%08X（状态块 0x%08X，pDevice=0x%08X Flags=0x%08X，"
                 "frame=%u）",
           effectSourceNote(src).c_str(), obj, state, device, flags, d3d9::frameSeq());
    call.result = d3dx9::kOk;
}

}  // namespace

void registerD3DX9EffectShims(ShimRegistry& shims) {
    // 9 参 stdcall（S0 §2.1 的压栈形状逐字对上：pDevice 在第 0 个参数位）。
    shims.add("d3dx9_43", "D3DXCreateEffectFromResourceA", Conv::Stdcall, 9,
              &shimD3DXCreateEffectFromResourceA);
}

}  // namespace d3dx9
}  // namespace lift
