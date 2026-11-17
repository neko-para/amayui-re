// emulator-lift · D3DX9 effect：RT_RCDATA 读取 + `D3DXCreateEffectFromResourceA` + ID3DXEffect 最小面
//
// ★ 读 `shims/d3dx9_effect.h` 的文件头先：取法（为什么是确定的）、9 个取证槽、P1 的"只记状态"
//   边界、以及"未收录 ⇒ 响亮停"的判据。
// ★ 本文件**只依赖** guest 内存 / guest 堆 / `PeImage`（不碰 GPU、不碰 SDL、不提交 draw）。
#include "shims/d3dx9_effect.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
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
    // ★ 终止项（`id == 0`）：头文件早就把它写进契约（"4 条 + 终止项"），但表里一直**没有** ——
    //   用例 `resources-pinned` 的 `pins[4].id == 0` 因此是在**越界读**（P1 时侥幸读到 0 才绿）。
    //   P2 加了一张新的 constexpr 表之后 `.rodata` 的相邻内容变了，这条当场红 ⇒ 把契约补齐。
    {0u, 0u, "", nullptr},
};

constexpr std::uint32_t kFxMagicValue = 0x31305846u;  // 'FX01'
constexpr std::uint32_t kFxBytes = 184u;
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
    // ★ A 单（202）：`SetVector` / `SetMatrix` 两槽。⛔ 不塞进上面的 float 表（一个 `float4x4`
    //   就 16 float，会把 Fade/203 的常量表挤爆）；矩阵是状态块里的**一块独立 64 B 区**。
    kFxSetVectors = 96,
    kFxSetMatrices = 100,
    kFxVectorCount = 104,
    kFxVectorTable = 108,  // guest EA：kFxVectorCap 项 × 20 B（name_ea, 4 个 float）
    kFxVectorCap = 112,
    kFxMatrix = 116,       // **内联 64 B**：16 个 float（D3DX 行主序；没设过 ⇒ 全 0）
    kFxMatrixName = 180,   // guest EA：这块矩阵是谁设的（0 = 没设过）
};
constexpr std::uint32_t kFxEntryBytes = 8u;
constexpr std::uint32_t kFxVectorEntryBytes = 20u;  // name_ea(4) + 4 个 float(16)
constexpr std::uint32_t kFxInitialFloatCap = 8u;
constexpr std::uint32_t kFxInitialTexCap = 4u;
constexpr std::uint32_t kFxInitialVectorCap = 2u;
constexpr std::uint32_t kFxMaxFloatCap = 256u;  // 上限（防跑飞；引擎实测 7 个名字）
constexpr std::uint32_t kFxMaxTexCap = 32u;     // 引擎实测 2 张
constexpr std::uint32_t kFxMaxVectorCap = 32u;  // 引擎实测 1 个（202 的 Color）
constexpr std::uint32_t kFxShaChars = 64u;
// E_OUTOFMEMORY（winerror.h）：guest 堆不够时交回 guest 的 HRESULT（⛔ 不记成 unimplemented）
constexpr std::uint32_t kOutOfMemory = 0x8007000Eu;

// guest 内存里的 u32 自增（没有 rd32Ref 这种接口）
void bump32(X86& cpu, std::uint32_t ea) { cpu.mem->wr32(ea, cpu.mem->rd32(ea) + 1u); }

// guest 内存里的一个 float（`SetVector` / `SetMatrix` 的 `const float*` 实参）
float rdGuestF32(X86& cpu, std::uint32_t ea) {
    const std::uint32_t bits = cpu.mem->rd32(ea);
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

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
// ★ `entry_bytes` 是表项宽度（float/纹理表 8 B、向量表 20 B）—— ⛔ 不写死 `kFxEntryBytes`：
//   那样加向量表时会按 8 B 扩容、把 4 个 float 的一行切成两行（静默错位）。
bool growTable(X86& cpu, std::uint32_t state, std::uint32_t table_off, std::uint32_t cap_off,
               std::uint32_t max_cap, std::uint32_t entry_bytes, const char* what, std::string* why) {
    const std::uint32_t cap = cpu.mem->rd32(state + cap_off);
    if (cap >= max_cap) {
        if (why) *why = std::string("D3DX9 effect：") + what + "表已达上限 " + std::to_string(max_cap);
        return false;
    }
    const std::uint32_t next = (cap * 2u > max_cap) ? max_cap : cap * 2u;
    const std::uint32_t table = cpu.mem->rd32(state + table_off);
    const std::uint32_t grown = cpu.heap->realloc(table, next * entry_bytes, /*zero=*/true);
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
        if (pin.id == 0) break;  // 终止项（⛔ 不让空 sha256 命中它）
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

// =======================================================================================
// FX 源文本解析（P2）—— 4 份资源里出现过的语法子集
// =======================================================================================
// ★ 口径：**认识的就解析，不认识的就点名**。⛔ 没有"跳过这一行继续"的分支 —— 那会让
//   "文本里有个我们没建模的状态"变成静默接受（本仓最贵的一类错）。
namespace {

// 去注释后的源码 + 每个字节的**行号**（诊断只认行号：`why` 里不带字节偏移）。
struct FxText {
    std::string code;
    std::vector<std::uint32_t> line_of;
};

bool stripFxComments(const std::string& in, FxText* out, std::string* why) {
    out->code.clear();
    out->line_of.clear();
    out->code.reserve(in.size());
    out->line_of.reserve(in.size());
    std::uint32_t line = 1;
    const auto put = [&](char c) {
        out->code.push_back(c);
        out->line_of.push_back(line);
    };
    for (std::size_t i = 0; i < in.size();) {
        const char c = in[i];
        if (c == '\n') {
            put('\n');
            ++line;
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') {
                put(' ');
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            bool closed = false;
            while (i < in.size()) {
                if (in[i] == '*' && i + 1 < in.size() && in[i + 1] == '/') {
                    i += 2;
                    closed = true;
                    break;
                }
                if (in[i] == '\n') {
                    put('\n');
                    ++line;
                } else {
                    put(' ');
                }
                ++i;
            }
            if (!closed) {
                if (why) *why = "FX 源码：/* 注释没有闭合";
                return false;
            }
            continue;
        }
        put(c);
        ++i;
    }
    return true;
}

bool fxIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool fxIdentChar(char c) {
    return fxIdentStart(c) || (c >= '0' && c <= '9');
}

struct FxScan {
    const FxText& t;
    std::size_t i = 0;

    explicit FxScan(const FxText& text) : t(text) {}
    bool eof() const { return i >= t.code.size(); }
    char peek() const { return i < t.code.size() ? t.code[i] : '\0'; }
    char peekAt(std::size_t k) const { return i + k < t.code.size() ? t.code[i + k] : '\0'; }
    std::uint32_t line() const { return i < t.line_of.size() ? t.line_of[i] : (t.line_of.empty() ? 1u : t.line_of.back()); }
    void skipWs() {
        while (!eof()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                ++i;
                continue;
            }
            break;
        }
    }
    // 当前行开头的可见片段（错误消息里带一点上下文）
    std::string lineSnippet() const {
        const std::uint32_t want = line();
        std::size_t begin = i;
        while (begin > 0 && t.line_of[begin - 1] == want) --begin;
        std::size_t end = i;
        while (end < t.code.size() && t.line_of[end] == want) ++end;
        std::string s = t.code.substr(begin, end - begin);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        if (s.size() > 96) s = s.substr(0, 96) + "…";
        return s;
    }
    bool readIdent(std::string* out) {
        skipWs();
        if (eof() || !fxIdentStart(peek())) return false;
        const std::size_t begin = i;
        while (!eof() && fxIdentChar(peek())) ++i;
        if (out != nullptr) *out = t.code.substr(begin, i - begin);
        return true;
    }
    bool acceptWord(const char* w) {
        skipWs();
        const std::size_t n = std::strlen(w);
        if (t.code.compare(i, n, w) != 0) return false;
        // 词边界
        if (i + n < t.code.size() && fxIdentChar(t.code[i + n])) return false;
        if (i > 0 && fxIdentChar(t.code[i - 1])) return false;
        i += n;
        return true;
    }
    bool acceptChar(char c) {
        skipWs();
        if (peek() != c) return false;
        ++i;
        return true;
    }
    bool expectChar(char c, std::string* why, const char* what) {
        if (acceptChar(c)) return true;
        if (why) {
            *why = std::string("FX 源码第 ") + std::to_string(line()) + " 行：期待 " + std::string(1, c) +
                   "（" + what + "），实际是「" + lineSnippet() + "」";
        }
        return false;
    }
    bool expectSemi(std::string* why, const char* what) { return expectChar(';', why, what); }
};

bool fxFail(FxScan& s, FxParseStatus* st, FxParseStatus want, std::string* why, const std::string& msg) {
    if (why) *why = "FX 源码第 " + std::to_string(s.line()) + " 行：" + msg + "（「" + s.lineSnippet() + "」）";
    if (st) *st = want;
    return false;
}

// 顶层类型名：常量 / 纹理 / 函数返回类型
bool fxTypeName(const std::string& id) {
    static const char* const kTypes[] = {"float", "float2", "float3", "float4", "float4x4", "int", "bool",
                                         "void", "texture", "sampler", "half", "half4", "matrix"};
    for (const char* t : kTypes) {
        if (id == t) return true;
    }
    return false;
}

bool fxIsTextureType(const std::string& id) { return id == "texture"; }
bool fxIsSamplerType(const std::string& id) { return id == "sampler"; }

// `static const <type> <name>` 的初始化式：整段跳过（到 `;` 为止，括号/花括号配对）
bool skipInitializer(FxScan& s, FxParseStatus* st, std::string* why) {
    int depth = 0;
    while (!s.eof()) {
        const char c = s.peek();
        if (c == '(' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '}' || c == ']') --depth;
        if (c == ';' && depth <= 0) {
            ++s.i;
            return true;
        }
        ++s.i;
    }
    return fxFail(s, st, FxParseStatus::Error, why, "初始化式没有以 `;` 结束");
}

// sampler_state 里**建模**的六项（D3DSAMP_*）。别的键 ⇒ UnsupportedState（⛔ 不静默忽略）。
bool fxSamplerKey(const std::string& key) {
    return key == "Texture" || key == "AddressU" || key == "AddressV" || key == "MinFilter" ||
           key == "MagFilter" || key == "MipFilter";
}

bool parseParamList(FxScan& s, std::vector<FxParam>* out, FxParseStatus* st, std::string* why) {
    if (s.acceptChar(')')) return true;  // 空形参表
    while (true) {
        FxParam p;
        // `in` / `out` / `inout` 修饰符不是类型
        s.skipWs();
        const std::size_t save = s.i;
        std::string word;
        if (!s.readIdent(&word)) return fxFail(s, st, FxParseStatus::Error, why, "形参表里期待类型名");
        if (word == "in" || word == "out" || word == "inout") {
            if (!s.readIdent(&word)) return fxFail(s, st, FxParseStatus::Error, why, "修饰符后面期待类型名");
        } else {
            s.i = save;
            if (!s.readIdent(&word)) return fxFail(s, st, FxParseStatus::Error, why, "形参表里期待类型名");
        }
        p.type = word;
        if (!s.readIdent(&p.name)) return fxFail(s, st, FxParseStatus::Error, why, "形参名缺失");
        if (s.acceptChar(':')) {
            // ★ `TEXCOORD0` / `COLOR0` 的数字后缀**是语义的一部分** ⇒ 必须整段读进来
            //   （`readIdent` 的 `fxIdentChar` 允许数字，所以 `TEXCOORD0` 一次读完）。
            if (!s.readIdent(&p.semantic)) return fxFail(s, st, FxParseStatus::Error, why, "语义名缺失");
        }
        out->push_back(p);
        if (s.acceptChar(',')) continue;
        if (s.acceptChar(')')) return true;
        return fxFail(s, st, FxParseStatus::Error, why, "形参表里期待 `,` 或 `)`");
    }
}

// shader 函数体：配对花括号（内容原样进 `FxShader::body`，供 tex2D 的 UV 语义读）
bool parseShaderBody(FxScan& s, std::string* out) {
    s.skipWs();
    if (s.peek() != '{') return false;
    const std::size_t begin = s.i;
    int depth = 0;
    while (!s.eof()) {
        const char c = s.peek();
        if (c == '{') ++depth;
        if (c == '}') {
            --depth;
            ++s.i;
            if (depth == 0) {
                *out = s.t.code.substr(begin + 1, s.i - begin - 2);
                return true;
            }
            continue;
        }
        ++s.i;
    }
    return false;
}

bool parseTechnique(FxScan& s, FxModule* m, FxParseStatus* st, std::string* why) {
    FxTechnique tech;
    if (!s.readIdent(&tech.name)) return fxFail(s, st, FxParseStatus::Error, why, "technique 名缺失");
    if (!s.expectChar('{', why, "technique 体")) return false;
    while (!s.acceptChar('}')) {
        if (!s.acceptWord("pass")) {
            return fxFail(s, st, FxParseStatus::Error, why, "technique 体里只接受 `pass <名字> { … }`");
        }
        FxPass pass;
        if (!s.readIdent(&pass.name)) return fxFail(s, st, FxParseStatus::Error, why, "pass 名缺失");
        if (!s.expectChar('{', why, "pass 体")) return false;
        while (!s.acceptChar('}')) {
            std::string key;
            if (!s.readIdent(&key)) return fxFail(s, st, FxParseStatus::Error, why, "pass 体里期待一个状态名");
            // ★ 只有两条 pass 状态被建模（4 份文本里也只出现这两条）。其余一律点名拒绝。
            const bool is_ps = key == "PixelShader";
            const bool is_vs = key == "VertexShader";
            if (!is_ps && !is_vs) {
                return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                              "pass 里的状态 `" + key + "` 尚未建模（本模块只认识 PixelShader / VertexShader）");
            }
            if (!s.expectChar('=', why, "状态赋值")) return false;
            if (!s.acceptWord("compile")) {
                return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                              key + " 不是 `compile <profile> <entry>()` 形式 ⇒ 尚未建模");
            }
            std::string profile;
            if (!s.readIdent(&profile)) return fxFail(s, st, FxParseStatus::Error, why, "shader profile 缺失");
            std::string entry;
            if (!s.readIdent(&entry)) return fxFail(s, st, FxParseStatus::Error, why, "shader 入口名缺失");
            if (!s.expectChar('(', why, "入口调用")) return false;
            if (!s.expectChar(')', why, "入口调用（本线不接受实参）")) return false;
            if (!s.expectSemi(why, "shader 赋值")) return false;
            if (is_ps) {
                if (pass.has_pixel_shader) {
                    return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                                  "同一个 pass 里出现两次 PixelShader 赋值 ⇒ 后一条会覆盖前一条");
                }
                pass.has_pixel_shader = true;
                pass.pixel_entry = entry;
                pass.pixel_profile = profile;
            } else {
                if (pass.has_vertex_shader) {
                    return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                                  "同一个 pass 里出现两次 VertexShader 赋值 ⇒ 后一条会覆盖前一条");
                }
                pass.has_vertex_shader = true;
                pass.vertex_entry = entry;
                pass.vertex_profile = profile;
            }
        }
        tech.passes.push_back(pass);
    }
    m->techniques.push_back(tech);
    return true;
}

bool parseSamplerDecl(FxScan& s, FxModule* m, FxParseStatus* st, std::string* why) {
    FxSampler smp;
    if (!s.readIdent(&smp.name)) return fxFail(s, st, FxParseStatus::Error, why, "sampler 名缺失");
    if (!s.expectChar('=', why, "sampler 赋值")) return false;
    if (!s.acceptWord("sampler_state")) {
        return fxFail(s, st, FxParseStatus::UnsupportedState, why, "sampler 的右值不是 sampler_state");
    }
    if (!s.expectChar('{', why, "sampler_state 体")) return false;
    while (!s.acceptChar('}')) {
        std::string key;
        if (!s.readIdent(&key)) return fxFail(s, st, FxParseStatus::Error, why, "sampler_state 里期待键名");
        if (!fxSamplerKey(key)) {
            return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                          "sampler_state 的键 `" + key + "` 尚未建模（只建模 Texture / AddressU / AddressV / "
                          "MinFilter / MagFilter / MipFilter）");
        }
        if (!s.expectChar('=', why, "sampler_state 赋值")) return false;
        std::string value;
        if (s.acceptChar('<')) {
            if (!s.readIdent(&value)) return fxFail(s, st, FxParseStatus::Error, why, "`<` 后面期待参数名");
            if (!s.expectChar('>', why, "`<参数名>`")) return false;
        } else if (!s.readIdent(&value)) {
            return fxFail(s, st, FxParseStatus::Error, why, "sampler_state 的值缺失");
        }
        if (!s.expectSemi(why, "sampler_state 赋值")) return false;
        if (key == "Texture") smp.texture = value;
        else if (key == "AddressU") smp.address_u = value;
        else if (key == "AddressV") smp.address_v = value;
        else if (key == "MinFilter") smp.min_filter = value;
        else if (key == "MagFilter") smp.mag_filter = value;
        else if (key == "MipFilter") smp.mip_filter = value;
    }
    if (!s.expectSemi(why, "sampler 声明")) return false;
    if (smp.texture.empty()) {
        return fxFail(s, st, FxParseStatus::UnsupportedState, why,
                      "sampler `" + smp.name + "` 没有 `Texture=<…>`（本线不做「未绑定采样器」）");
    }
    m->samplers.push_back(smp);
    return true;
}

bool parseModuleInner(FxScan& s, FxModule* m, FxParseStatus* st, std::string* why) {
    while (true) {
        s.skipWs();
        if (s.eof()) return true;
        if (s.acceptChar(';')) continue;  // 空语句
        if (s.acceptWord("static")) {
            // `static const <type> <name> [ '[' n ']' ] = <初始化式> ;`
            if (!s.acceptWord("const")) {
                return fxFail(s, st, FxParseStatus::UnsupportedState, why, "`static` 后面只建模 `const`");
            }
            std::string type;
            std::string name;
            if (!s.readIdent(&type)) return fxFail(s, st, FxParseStatus::Error, why, "static const 的类型缺失");
            if (!s.readIdent(&name)) return fxFail(s, st, FxParseStatus::Error, why, "static const 的名字缺失");
            if (s.acceptChar('[')) {
                while (!s.eof() && s.peek() != ']') ++s.i;
                if (!s.expectChar(']', why, "数组维度")) return false;
            }
            s.skipWs();
            if (!s.expectChar('=', why, "static const 初始化")) return false;
            if (!skipInitializer(s, st, why)) return false;
            continue;
        }
        if (s.acceptWord("technique")) {
            if (!parseTechnique(s, m, st, why)) return false;
            continue;
        }
        std::string type;
        if (!s.readIdent(&type)) {
            return fxFail(s, st, FxParseStatus::Error, why, "顶层声明里期待类型名 / technique / static");
        }
        if (!fxTypeName(type)) {
            return fxFail(s, st, FxParseStatus::Error, why, "不认识的顶层类型 `" + type + "`");
        }
        if (fxIsSamplerType(type)) {
            if (!parseSamplerDecl(s, m, st, why)) return false;
            continue;
        }
        std::string name;
        if (!s.readIdent(&name)) return fxFail(s, st, FxParseStatus::Error, why, "声明名缺失");
        s.skipWs();
        if (s.peek() == '(') {
            // shader / 辅助函数
            FxShader sh;
            sh.name = name;
            sh.return_type = type;
            if (!s.expectChar('(', why, "函数形参表")) return false;
            if (!parseParamList(s, &sh.params, st, why)) return false;
            if (s.acceptChar(':')) {
                if (!s.readIdent(&sh.return_semantic)) {
                    return fxFail(s, st, FxParseStatus::Error, why, "返回值语义缺失");
                }
                while (!s.eof() && fxIdentChar(s.peek())) ++s.i;
            }
            if (!parseShaderBody(s, &sh.body)) {
                return fxFail(s, st, FxParseStatus::Error, why, "函数体没有配对的花括号");
            }
            m->shaders.push_back(sh);
            continue;
        }
        if (!s.expectSemi(why, "变量声明")) return false;
        if (!fxIsTextureType(type)) {
            FxConstant c;
            c.type = type;
            c.name = name;
            m->constants.push_back(c);
        } else {
            // texture 参数：进常量表？不进 —— 它是纹理参数，由 sampler 的 `Texture=<名字>` 引用。
            // 单独记一份"存在的纹理参数名"，好让绑定器报"SetTexture 没绑"时有名字可点。
            FxConstant t;
            t.type = type;
            t.name = name;
            m->constants.push_back(t);
        }
    }
}

}  // namespace

FxParseStatus parseEffectSource(const std::string& text, FxModule* out, std::string* why) {
    if (out == nullptr) {
        if (why) *why = "FX 源码：parseEffectSource 的 out 为空";
        return FxParseStatus::Error;
    }
    *out = FxModule{};
    FxText stripped;
    if (!stripFxComments(text, &stripped, why)) return FxParseStatus::Error;
    FxScan scan(stripped);
    FxParseStatus st = FxParseStatus::Ok;
    std::string err;
    if (!parseModuleInner(scan, out, &st, &err)) {
        if (why) *why = err;
        return st == FxParseStatus::Ok ? FxParseStatus::Error : st;
    }
    if (out->techniques.empty()) {
        if (why) *why = "FX 源码：解析完没有任何 technique（本线不画「没有 technique 的 effect」）";
        return FxParseStatus::Error;
    }
    if (why) why->clear();
    return FxParseStatus::Ok;
}

const FxPass* fxPassAt(const FxModule& module, const std::string& technique, std::uint32_t pass) {
    for (const FxTechnique& t : module.techniques) {
        if (t.name != technique) continue;
        if (pass >= t.passes.size()) return nullptr;
        return &t.passes[pass];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// 模块缓存 + 程序解析
// ---------------------------------------------------------------------------------------
namespace {

// ★ 用 `std::map`（节点稳定）：`effectModuleOf` 返回的指针会在后续解析里继续用，
//   `std::vector` 的扩容会让它失效。
std::map<std::string, FxModule>& fxModuleCache() {
    static std::map<std::string, FxModule> cache;
    return cache;
}

const FxShader* fxFindShader(const FxModule& m, const std::string& name) {
    for (const FxShader& s : m.shaders) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const FxConstant* fxFindParam(const FxModule& m, const std::string& name) {
    for (const FxConstant& c : m.constants) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

// 从 body 里读 `tex2D(<sampler>, <uv 实参>)` 的第二个实参（⛔ 不硬编码 UV 映射）。
bool fxSamplerUvArg(const FxShader& shader, const std::string& sampler, std::string* uv_param) {
    const std::string& b = shader.body;
    std::size_t pos = 0;
    while ((pos = b.find("tex2D", pos)) != std::string::npos) {
        std::size_t p = pos + 5;
        while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r')) ++p;
        if (p >= b.size() || b[p] != '(') {
            pos = p;
            continue;
        }
        ++p;
        while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r')) ++p;
        std::string name;
        while (p < b.size() && fxIdentChar(b[p])) name.push_back(b[p++]);
        while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r')) ++p;
        if (p >= b.size() || b[p] != ',') {
            pos = p;
            continue;
        }
        ++p;
        while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r')) ++p;
        std::string uv;
        while (p < b.size() && fxIdentChar(b[p])) uv.push_back(b[p++]);
        if (name == sampler) {
            if (uv_param) *uv_param = uv;
            return !uv.empty();
        }
        pos = p;
    }
    return false;
}

// `TEXCOORDn` ⇒ n（不是这个前缀 ⇒ false）。
bool fxSemanticIndex(const std::string& semantic, std::uint32_t* out) {
    const std::string prefix = "TEXCOORD";
    if (semantic.size() <= prefix.size()) return false;
    if (semantic.compare(0, prefix.size(), prefix) != 0) return false;
    std::uint32_t v = 0;
    for (std::size_t i = prefix.size(); i < semantic.size(); ++i) {
        const char c = semantic[i];
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<std::uint32_t>(c - '0');
        if (v > 7u) return false;
    }
    if (out) *out = v;
    return true;
}

// SetFloat 记录里按名字取值（没有 ⇒ false）
bool fxFloatValue(X86& cpu, std::uint32_t state, const std::string& name, float* value) {
    const std::uint32_t count = cpu.mem->rd32(state + kFxFloatCount);
    const std::uint32_t table = cpu.mem->rd32(state + kFxFloatTable);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = table + i * kFxEntryBytes;
        const std::uint32_t name_ea = cpu.mem->rd32(entry);
        if (name_ea == 0) continue;
        if (cpu.mem->readCString(name_ea) != name) continue;
        const std::uint32_t bits = cpu.mem->rd32(entry + 4);
        if (value != nullptr) std::memcpy(value, &bits, sizeof(float));
        return true;
    }
    return false;
}

std::uint32_t fxTextureValue(X86& cpu, std::uint32_t state, const std::string& name) {
    const std::uint32_t count = cpu.mem->rd32(state + kFxTexCount);
    const std::uint32_t table = cpu.mem->rd32(state + kFxTexTable);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = table + i * kFxEntryBytes;
        const std::uint32_t name_ea = cpu.mem->rd32(entry);
        if (name_ea == 0) continue;
        if (cpu.mem->readCString(name_ea) == name) return cpu.mem->rd32(entry + 4);
    }
    return 0;
}

// `SetVector` 记录里按名字取 4 个 float（没有 ⇒ false；`out` 不动）
bool fxVectorValue(X86& cpu, std::uint32_t state, const std::string& name, float out[4]) {
    const std::uint32_t count = cpu.mem->rd32(state + kFxVectorCount);
    const std::uint32_t table = cpu.mem->rd32(state + kFxVectorTable);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = table + i * kFxVectorEntryBytes;
        const std::uint32_t name_ea = cpu.mem->rd32(entry);
        if (name_ea == 0) continue;
        if (cpu.mem->readCString(name_ea) != name) continue;
        for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t bits = cpu.mem->rd32(entry + 4u + c * 4u);
            std::memcpy(&out[c], &bits, sizeof(float));
        }
        return true;
    }
    return false;
}

// 矩阵槽：按**名字**取值。`SetMatrix` 只设过一块（`kFxMaxMatrixParams == 1`）⇒
//   名字不符 = 这个 uniform **没被设过** ⇒ false（`out` 保持全 0，⛔ 不默认成单位阵）。
bool fxMatrixValue(X86& cpu, std::uint32_t state, const std::string& name, float out[16], bool* set) {
    const std::uint32_t name_ea = cpu.mem->rd32(state + kFxMatrixName);
    const bool hit = name_ea != 0 && cpu.mem->readCString(name_ea) == name;
    for (std::uint32_t c = 0; c < 16; ++c) {
        const std::uint32_t bits = cpu.mem->rd32(state + kFxMatrix + c * 4u);
        std::memcpy(&out[c], &bits, sizeof(float));
    }
    if (set != nullptr) *set = hit;
    return hit;
}

constexpr EffectProgramBinding kBindings[] = {
    // 资源 201：`technique Fade { pass P0 { PixelShader=compile ps_2_0 FadePS(); } }`
    {kEffectIdFadePs20, "Fade", "FadePS", "ps_2_0", d3d9::EffectProgram::FadePs20},
    // 资源 200：同一份文本、`compile ps_1_0`（逐通道直接 lerp）
    {kEffectIdFadePs10, "Fade", "FadePS", "ps_1_0", d3d9::EffectProgram::FadePs10},
    // 资源 202：`technique Snow` / `compile ps_1_0 SnowPS()`（`return tex2D(TexSmp, UV)*Color;`）
    //   ★ 同一 pass 还有 `VertexShader=compile vs_1_0 SnowVS()`：PS 由这张表兑现，VS 由
    //     `kVertexBindings`（CPU 顶点程序）兑现 —— 两张表各管一半，⛔ 不互相冒充。
    {kEffectIdSnowPs10, "Snow", "SnowPS", "ps_1_0", d3d9::EffectProgram::SnowPs10},
    // 资源 203：**两个** technique，各自一个 pass（P3；`ps_2_0`）
    //   · SlideBlur：21 tap（`for( float i = -10.0f; i <= 10.0f; i++ )`，每 tap `0.0476f`）
    //   · ZoomBlur ：10 tap（`for( float i = 0.0f; i < 10.0f; i++ )`，权重 = 文本的 `ratio[10]`）
    //   ★ 门牌（technique / entry / profile）全部来自 FX 文本的解析结果 ⇒ 这里只做最后一跳。
    {kEffectIdBlurPs20, "SlideBlur", "SlideBlurPS", "ps_2_0", d3d9::EffectProgram::SlideBlurPs20},
    {kEffectIdBlurPs20, "ZoomBlur", "ZoomBlurPS", "ps_2_0", d3d9::EffectProgram::ZoomBlurPs20},
    {0u, nullptr, nullptr, nullptr, d3d9::EffectProgram::None},
};

// VS 实现集：`(vertex_entry, vertex_profile)` 命中 **且只有** 这张表里的那些 ⇒ 允许。
// ★ 为什么按名字而不是"凡 vs_1_0"：本线的 CPU 顶点程序是**逐句照** `SnowVS` 的源文本写的一个
//   函数（`Pos = mul(pos, Proj); UV = uv;`，见 `d3d9_pipeline.cpp` 的 `transformVertexVs10`）。
//   只有"这条 VS 的语义就是那一个式子"时才兑现得了 ⇒ 名字 + profile 一起钉。
//   ⛔ 未命中（`vs_2_0` / `vs_3_0` / 别的 entry）⇒ 调用方**具名拒绝**，不猜"大概差不多"。
struct EffectVertexBinding {
    const char* entry;
    const char* profile;
};
constexpr EffectVertexBinding kVertexBindings[] = {
    {"SnowVS", "vs_1_0"},  // 资源 202：`void SnowVS(pos, uv, out Pos, out UV) { Pos=mul(pos,Proj); UV=uv; }`
    {nullptr, nullptr},
};

}  // namespace

bool effectVertexProgramSupported(const std::string& entry, const std::string& profile) {
    for (const EffectVertexBinding* b = kVertexBindings; b->entry != nullptr; ++b) {
        if (entry == b->entry && profile == b->profile) return true;
    }
    return false;
}

const EffectProgramBinding* effectProgramBindings() { return kBindings; }

const FxModule* effectModuleForSha(const std::string& sha256, const std::string& text, std::string* why) {
    const std::string want = toLower(sha256);
    auto it = fxModuleCache().find(want);
    if (it != fxModuleCache().end()) return &it->second;
    FxModule module;
    std::string err;
    const FxParseStatus st = parseEffectSource(text, &module, &err);
    if (st != FxParseStatus::Ok) {
        if (why) *why = err;
        return nullptr;
    }
    auto ins = fxModuleCache().emplace(want, std::move(module));
    return &ins.first->second;
}

const FxModule* effectModuleOf(X86& cpu, std::uint32_t state, std::string* why) {
    std::string state_why;
    if (!stateOk(cpu, state, &state_why)) {
        if (why) *why = state_why;
        return nullptr;
    }
    const std::uint32_t sha_ea = cpu.mem->rd32(state + kFxSha);
    if (sha_ea == 0) {
        if (why) *why = "D3DX9 effect：状态块没有 sha256（创建时没记），拿不到 FX 文本";
        return nullptr;
    }
    const std::string sha = toLower(cpu.mem->readCString(sha_ea));
    auto it = fxModuleCache().find(sha);
    if (it == fxModuleCache().end()) {
        if (why) *why = "D3DX9 effect：sha256=" + sha + " 的 FX 文本还没解析过（创建路径必须解析）";
        return nullptr;
    }
    return &it->second;
}

bool effectDrawProgram(X86& cpu, std::uint32_t state, d3d9::EffectDrawProgram* out, std::string* why) {
    if (out == nullptr) {
        if (why) *why = "D3DX9 effect：effectDrawProgram 的 out 为空";
        return false;
    }
    *out = d3d9::EffectDrawProgram{};
    const FxModule* module = effectModuleOf(cpu, state, why);
    if (module == nullptr) return false;
    const std::uint32_t resource_id = cpu.mem->rd32(state + kFxResourceId);
    const std::uint32_t tech_ea = cpu.mem->rd32(state + kFxTech);
    const std::string technique = tech_ea != 0 ? cpu.mem->readCString(tech_ea) : std::string();
    const std::uint32_t pass_index = cpu.mem->rd32(state + kFxPassIndex);
    // ★ **每一条**具名原因都带同一段上下文（资源 id / technique / pass）：
    //   "哪一批被拒"必须能从原因串读出来，⛔ 不能只写"这个 pass 带 VertexShader"而说不清是哪个资源。
    const std::string context = "资源 id=" + std::to_string(resource_id) + " technique=\"" + technique +
                                "\" pass=" + std::to_string(pass_index) + "：";
    const auto unimplemented = [out, &context](const std::string& reason) {
        out->implemented = false;
        out->program = d3d9::EffectProgram::None;
        out->has_vs = false;
        out->vs_matrix_set = false;
        out->constant_count = 0;
        out->texture_count = 0;
        out->reason = context + reason;
        return true;
    };
    const FxPass* pass = fxPassAt(*module, technique, pass_index);
    if (pass == nullptr) {
        return unimplemented("这个 pass 不在 FX 文本里（technique 表与 pass 数都由文本解析得出）");
    }
    // ★ A 单：带 VertexShader 的 pass **不再一律拒绝** —— 按 (entry, profile) 查 VS 实现集。
    //   命中 ⇒ 这一批的顶点改用 CPU 顶点程序（`transformVertexVs10`）；未命中 ⇒ 仍然具名拒绝。
    //   ⛔ 不是"凡 vs_1_0 都放行"：表在 `kVertexBindings`（唯一的 CPU 顶点程序是 SnowVS 那条）。
    if (pass->has_vertex_shader) {
        if (!effectVertexProgramSupported(pass->vertex_entry, pass->vertex_profile)) {
            return unimplemented("pass 的 VertexShader 不在 VS 实现集里（" + pass->vertex_profile + " " +
                                 pass->vertex_entry +
                                 "）⇒ 本线只有一个 CPU 顶点程序（vs_1_0 的 SnowVS：Pos=mul(pos,Proj)、"
                                 "uv 直通、无光照）；按 (entry, profile) 查表，⛔ 不是凡 vs_1_0 都放行");
        }
        out->has_vs = true;
    }
    if (!pass->has_pixel_shader) {
        return unimplemented("pass 没有 PixelShader 赋值（本模块只建模 PixelShader / VertexShader 两条 pass 状态）");
    }
    const EffectProgramBinding* binding = nullptr;
    for (const EffectProgramBinding* b = kBindings; b->resource_id != 0; ++b) {
        if (b->resource_id != resource_id) continue;
        if (technique != b->technique) continue;
        if (pass->pixel_entry != b->pixel_entry) continue;
        if (pass->pixel_profile != b->pixel_profile) continue;
        binding = b;
        break;
    }
    if (binding == nullptr) {
        return unimplemented("shader 程序不在实现集里：entry=" + pass->pixel_entry + " profile=" +
                             pass->pixel_profile +
                             "（本线的实现集 = Fade 的 ps_2_0/ps_1_0 + Snow 的 ps_1_0 + 203 的 "
                             "SlideBlur/ZoomBlur ps_2_0）");
    }
    const FxShader* shader = fxFindShader(*module, pass->pixel_entry);
    if (shader == nullptr) {
        return unimplemented("pass 引用的 " + pass->pixel_entry + "() 在 FX 文本里没有定义");
    }
    // ---- 常量表：按**声明顺序**（uniform；`texture` 参数不算常量）----
    // ★ 类型决定占几个 float（⛔ 不按"每个常量 1 个 float"硬展开）：
    //   `float` ⇒ 1（`SetFloat`）/ `float4` ⇒ 4（`SetVector` 的 4 个 float）/
    //   `float4x4` ⇒ **不进常量表**（进 `vs_matrix`，CPU 顶点程序用掉；后端 push constant 只有 32 B）/
    //   `texture` ⇒ 跳过（由 sampler 的 `Texture=<名字>` 引用）。
    std::uint32_t constant_count = 0;
    bool matrix_seen = false;
    for (const FxConstant& c : module->constants) {
        if (fxIsTextureType(c.type)) continue;
        if (c.type == "float4x4") {
            if (!pass->has_vertex_shader) {
                return unimplemented("FX 文本有 float4x4 常量 `" + c.name +
                                     "`，但这个 pass 没有 VertexShader ⇒ 本线没有它的去处"
                                     "（矩阵只喂 CPU 顶点程序，⛔ 不进 uniform）");
            }
            if (matrix_seen) {
                return unimplemented("FX 文本有多个 float4x4 常量（第二个是 `" + c.name +
                                     "）⇒ 本线的矩阵槽只有一块 64 B（kFxMaxMatrixParams=1）");
            }
            matrix_seen = true;
            // 没设过 ⇒ 全 0（D3D 的 uniform 初值；⛔ 不默认成单位阵）
            fxMatrixValue(cpu, state, c.name, out->vs_matrix, &out->vs_matrix_set);
            continue;
        }
        if (c.type == "float4") {
            float v4[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // 没 SetVector 过 ⇒ 0
            fxVectorValue(cpu, state, c.name, v4);
            if (constant_count + 4u > d3d9::kEffectMaxConstants) {
                return unimplemented("FX 文本的 uniform 常量（含 float4 展开）超过后端容量 " +
                                     std::to_string(d3d9::kEffectMaxConstants) + "（⛔ 不截断）");
            }
            for (std::uint32_t k = 0; k < 4u; ++k) out->constants[constant_count + k] = v4[k];
            constant_count += 4u;
            continue;
        }
        if (c.type != "float") {
            return unimplemented("FX 文本的 uniform 类型 `" + c.type + "` 不在本线建模的 "
                                 "float / float4 / float4x4 / texture 里（常量 `" + c.name + "`）");
        }
        if (constant_count >= d3d9::kEffectMaxConstants) {
            return unimplemented("FX 文本的 uniform 常量超过后端容量 " +
                                 std::to_string(d3d9::kEffectMaxConstants) + "（⛔ 不截断）");
        }
        float value = 0.0f;  // D3D 的 uniform 初值 = 0
        fxFloatValue(cpu, state, c.name, &value);
        out->constants[constant_count] = value;
        ++constant_count;
    }
    out->constant_count = constant_count;
    // ---- 纹理：sampler 声明顺序 → `tex2D` 的 UV 实参 → 它的 TEXCOORDn ----
    std::uint32_t texture_count = 0;
    for (const FxSampler& smp : module->samplers) {
        std::string uv_arg;
        if (!fxSamplerUvArg(*shader, smp.name, &uv_arg)) continue;  // 这个 shader 没用它
        const FxParam* uv_param = nullptr;
        for (const FxParam& p : shader->params) {
            if (p.name == uv_arg) uv_param = &p;
        }
        if (uv_param == nullptr) {
            return unimplemented("sampler `" + smp.name + "` 的 UV 实参 `" + uv_arg +
                                 "` 不是 " + pass->pixel_entry + " 的形参");
        }
        std::uint32_t uv_set = 0;
        if (!fxSemanticIndex(uv_param->semantic, &uv_set)) {
            return unimplemented("形参 `" + uv_arg + "` 的语义是 \"" + uv_param->semantic +
                                 "\"，不是 TEXCOORDn ⇒ 本线只建模纹理坐标输入");
        }
        if (texture_count >= d3d9::kEffectMaxTextures) {
            return unimplemented("shader 采样的纹理超过后端容量 " +
                                 std::to_string(d3d9::kEffectMaxTextures) + "（⛔ 不截断）");
        }
        const FxConstant* tex_param = fxFindParam(*module, smp.texture);
        if (tex_param == nullptr || !fxIsTextureType(tex_param->type)) {
            return unimplemented("sampler `" + smp.name + "` 的 `Texture=<" + smp.texture +
                                 ">` 不是 FX 文本里的 texture 参数");
        }
        const std::uint32_t ea = fxTextureValue(cpu, state, smp.texture);
        if (ea == 0) {
            return unimplemented("shader 要采样纹理参数 \"" + smp.texture +
                                 "\"，但 `SetTexture` 没有绑（⛔ 不猜「没有纹理时采到什么」）");
        }
        // 采样器状态：把 FX 文本里的名字翻成 **D3D9 枚举原值**（`d3d9_pipeline.h` 的 kTexf* / kAddress*
        // 就是这套值）。⛔ 认不出的名字 ⇒ 具名拒绝（不猜"大概等于 LINEAR"）。
        // ★ 大小写不敏感：4 份文本里 200/201 写 `LINEAR`、203 写 `Linear`（同一枚举的两种写法）。
        const auto filter_value = [](const std::string& raw, std::uint32_t* out) {
            const std::string v = toLower(raw);
            if (v == "none") { *out = 0u; return true; }
            if (v == "point") { *out = 1u; return true; }
            if (v == "linear") { *out = 2u; return true; }
            if (v == "anisotropic") { *out = 3u; return true; }
            return false;
        };
        const auto address_value = [](const std::string& raw, std::uint32_t* out) {
            const std::string v = toLower(raw);
            if (v == "wrap") { *out = 1u; return true; }
            if (v == "mirror") { *out = 2u; return true; }
            if (v == "clamp") { *out = 3u; return true; }
            return false;
        };
        std::uint32_t address_u = 1u, address_v = 1u, mag_filter = 1u, min_filter = 1u, mip_filter = 0u;
        if (!address_value(smp.address_u, &address_u) || !address_value(smp.address_v, &address_v)) {
            return unimplemented("sampler `" + smp.name + "` 的 AddressU/V（\"" + smp.address_u + "\"/\"" +
                                 smp.address_v + "\"）不在本线建模的 WRAP/MIRROR/CLAMP 里");
        }
        if (!filter_value(smp.mag_filter, &mag_filter) || !filter_value(smp.min_filter, &min_filter) ||
            !filter_value(smp.mip_filter, &mip_filter)) {
            return unimplemented("sampler `" + smp.name + "` 的过滤（Mag=" + smp.mag_filter + " Min=" +
                                 smp.min_filter + " Mip=" + smp.mip_filter +
                                 "）不在本线建模的 NONE/POINT/LINEAR/ANISOTROPIC 里");
        }
        out->textures[texture_count] = ea;
        out->uv_set[texture_count] = uv_set;
        out->address_u[texture_count] = address_u;
        out->address_v[texture_count] = address_v;
        out->mag_filter[texture_count] = mag_filter;
        out->min_filter[texture_count] = min_filter;
        out->mip_filter[texture_count] = mip_filter;
        ++texture_count;
    }
    out->texture_count = texture_count;
    out->program = binding->program;
    out->implemented = true;
    out->reason.clear();
    if (why) why->clear();
    return true;
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
    // ★ P2：创建时就**解析** FX 文本（唯一的解析时机）。解析失败 / 出现没建模的状态 ⇒ 这里就失败
    //   ⇒ 调用方响亮停（⛔ 不建一个"看起来成功"的对象、⛔ 不留到 Begin 才炸）。
    {
        const std::string text(reinterpret_cast<const char*>(src.bytes.data()), src.bytes.size());
        std::string parse_why;
        if (effectModuleForSha(src.sha256, text, &parse_why) == nullptr) {
            if (why) *why = "FX 文本解析失败：" + parse_why;
            return 0;
        }
    }
    const std::uint32_t state = cpu.heap->alloc(kFxBytes, /*zero=*/true);
    if (state == 0) {
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = std::string("D3DX9 effect：guest 堆分配状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    const std::uint32_t floats = cpu.heap->alloc(kFxInitialFloatCap * kFxEntryBytes, /*zero=*/true);
    const std::uint32_t textures = cpu.heap->alloc(kFxInitialTexCap * kFxEntryBytes, /*zero=*/true);
    const std::uint32_t vectors = cpu.heap->alloc(kFxInitialVectorCap * kFxVectorEntryBytes, /*zero=*/true);
    const std::uint32_t sha = cpu.heap->alloc(kFxShaChars + 1u, /*zero=*/true);
    if (floats == 0 || textures == 0 || vectors == 0 || sha == 0) {
        if (floats != 0) cpu.heap->free(floats);
        if (textures != 0) cpu.heap->free(textures);
        if (vectors != 0) cpu.heap->free(vectors);
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
    cpu.mem->wr32(state + kFxVectorTable, vectors);
    cpu.mem->wr32(state + kFxFloatCap, kFxInitialFloatCap);
    cpu.mem->wr32(state + kFxTexCap, kFxInitialTexCap);
    cpu.mem->wr32(state + kFxVectorCap, kFxInitialVectorCap);
    // ★ 矩阵区在状态块内联（64 B）⇒ 分配时已清零 ⇒ "没设过 = 全 0" 天然成立
    //   （⛔ 不在这里写单位阵：那会掩盖"引擎没 SetMatrix 过"）。
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
    const std::uint32_t vec_count = cpu.mem->rd32(state + kFxVectorCount);
    const std::uint32_t vec_table = cpu.mem->rd32(state + kFxVectorTable);
    for (std::uint32_t i = 0; i < vec_count; ++i) {
        const std::uint32_t name_ea = cpu.mem->rd32(vec_table + i * kFxVectorEntryBytes);
        if (name_ea != 0) cpu.heap->free(name_ea);
    }
    const std::uint32_t tech = cpu.mem->rd32(state + kFxTech);
    const std::uint32_t sha = cpu.mem->rd32(state + kFxSha);
    const std::uint32_t matrix_name = cpu.mem->rd32(state + kFxMatrixName);
    if (tech != 0) cpu.heap->free(tech);
    if (sha != 0) cpu.heap->free(sha);
    if (matrix_name != 0) cpu.heap->free(matrix_name);
    if (float_table != 0) cpu.heap->free(float_table);
    if (tex_table != 0) cpu.heap->free(tex_table);
    if (vec_table != 0) cpu.heap->free(vec_table);
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
    out->set_vectors = cpu.mem->rd32(state + kFxSetVectors);
    out->set_matrices = cpu.mem->rd32(state + kFxSetMatrices);
    out->set_textures = cpu.mem->rd32(state + kFxSetTextures);
    out->set_techniques = cpu.mem->rd32(state + kFxSetTechniques);
    out->begins = cpu.mem->rd32(state + kFxBegins);
    out->begin_passes = cpu.mem->rd32(state + kFxBeginPasses);
    out->commits = cpu.mem->rd32(state + kFxCommits);
    out->end_passes = cpu.mem->rd32(state + kFxEndPasses);
    out->ends = cpu.mem->rd32(state + kFxEnds);
    out->float_param_count = cpu.mem->rd32(state + kFxFloatCount);
    out->vector_param_count = cpu.mem->rd32(state + kFxVectorCount);
    out->texture_count = cpu.mem->rd32(state + kFxTexCount);
    out->matrix_set = cpu.mem->rd32(state + kFxMatrixName) != 0;
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

bool effectVectorParamAt(X86& cpu, std::uint32_t state, std::uint32_t index, std::string* name,
                         float value[4]) {
    std::string why;
    if (!stateOk(cpu, state, &why)) return false;
    if (index >= cpu.mem->rd32(state + kFxVectorCount)) return false;
    const std::uint32_t entry = cpu.mem->rd32(state + kFxVectorTable) + index * kFxVectorEntryBytes;
    const std::uint32_t name_ea = cpu.mem->rd32(entry);
    if (name) *name = name_ea != 0 ? cpu.mem->readCString(name_ea) : std::string();
    if (value != nullptr) {
        for (std::uint32_t c = 0; c < 4; ++c) {
            const std::uint32_t bits = cpu.mem->rd32(entry + 4u + c * 4u);
            std::memcpy(&value[c], &bits, sizeof(float));
        }
    }
    return true;
}

bool effectMatrixParamAt(X86& cpu, std::uint32_t state, std::string* name, float value[16], bool* set) {
    std::string why;
    if (!stateOk(cpu, state, &why)) return false;
    const std::uint32_t name_ea = cpu.mem->rd32(state + kFxMatrixName);
    if (name) *name = name_ea != 0 ? cpu.mem->readCString(name_ea) : std::string();
    if (value != nullptr) {
        for (std::uint32_t c = 0; c < 16; ++c) {
            const std::uint32_t bits = cpu.mem->rd32(state + kFxMatrix + c * 4u);
            std::memcpy(&value[c], &bits, sizeof(float));
        }
    }
    if (set != nullptr) *set = name_ea != 0;
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
            if (!growTable(cpu, state, kFxFloatTable, kFxFloatCap, kFxMaxFloatCap, kFxEntryBytes,
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

// `SetVector("Color", &float4)`：照 `effectSetFloat` 的形状，但一项占 **20 B**（name_ea + 4 float）。
// ★ 为什么单独一张表：`float4` 与 `float` 的**宽度不同**，混进同一张 8 B 表会把 4 个 float 切成
//   两行（表项错位），且 `effectDrawProgram` 要按 FX 文本里的类型决定读 1 个还是 4 个。
std::uint32_t effectSetVector(X86& cpu, std::uint32_t state, const std::string& name,
                              const float value[4], std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (name.empty()) {
        if (why) *why = "D3DX9 effect：SetVector 的参数名为空（D3DX 合法名必须非空）";
        return d3dx9::kInvalidArg;
    }
    const std::uint32_t count = cpu.mem->rd32(state + kFxVectorCount);
    int found = -1;
    {
        const std::uint32_t table = cpu.mem->rd32(state + kFxVectorTable);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t name_ea = cpu.mem->rd32(table + i * kFxVectorEntryBytes);
            if (name_ea != 0 && cpu.mem->readCString(name_ea) == name) {
                found = static_cast<int>(i);
                break;
            }
        }
    }
    std::uint32_t entry = 0;
    if (found >= 0) {
        entry = cpu.mem->rd32(state + kFxVectorTable) +
                static_cast<std::uint32_t>(found) * kFxVectorEntryBytes;
    } else {
        std::string grew;
        if (count >= cpu.mem->rd32(state + kFxVectorCap)) {
            if (!growTable(cpu, state, kFxVectorTable, kFxVectorCap, kFxMaxVectorCap,
                           kFxVectorEntryBytes, "向量", &grew)) {
                if (why) *why = grew;
                return kOutOfMemory;
            }
        }
        const std::uint32_t name_ea = allocName(cpu, name);
        if (name_ea == 0) {
            if (why) *why = std::string("D3DX9 effect：guest 堆分配向量参数名失败：") + cpu.heap->lastError();
            return kOutOfMemory;
        }
        entry = cpu.mem->rd32(state + kFxVectorTable) + count * kFxVectorEntryBytes;
        cpu.mem->wr32(entry, name_ea);
        cpu.mem->wr32(state + kFxVectorCount, count + 1u);
        fxLog("ID3DXEffect::SetVector", "\"%s\" = (%g, %g, %g, %g)（第 %u 个向量参数）", name.c_str(),
              static_cast<double>(value[0]), static_cast<double>(value[1]),
              static_cast<double>(value[2]), static_cast<double>(value[3]), count + 1u);
    }
    for (std::uint32_t c = 0; c < 4; ++c) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value[c], sizeof(bits));
        cpu.mem->wr32(entry + 4u + c * 4u, bits);
    }
    bump32(cpu, state + kFxSetVectors);
    return d3dx9::kOk;
}

// `SetMatrix("Proj", &float4x4)`：写进状态块的**独立 64 B 区**（⛔ 不进 constants[]）。
// ★ 同名覆盖：直接重写那 16 个 float。
// ★ 不同名：这块区只有一份（`kFxMaxMatrixParams == 1`）⇒ 覆盖并把名字换成新的，同时**警告**
//   —— 旧的那个 uniform 就此"没被设过"（D3D 语义：它的值仍是初值 0）。⛔ 不静默留两个名字。
std::uint32_t effectSetMatrix(X86& cpu, std::uint32_t state, const std::string& name,
                              const float matrix[16], std::string* why) {
    if (!stateOk(cpu, state, why)) return d3dx9::kInvalidCall;
    if (name.empty()) {
        if (why) *why = "D3DX9 effect：SetMatrix 的参数名为空（D3DX 合法名必须非空）";
        return d3dx9::kInvalidArg;
    }
    const std::uint32_t old = cpu.mem->rd32(state + kFxMatrixName);
    const bool same = old != 0 && cpu.mem->readCString(old) == name;
    if (!same) {
        const std::uint32_t name_ea = allocName(cpu, name);
        if (name_ea == 0) {
            if (why) *why = std::string("D3DX9 effect：guest 堆分配矩阵参数名失败：") + cpu.heap->lastError();
            return kOutOfMemory;
        }
        if (old != 0) {
            fxWarn("ID3DXEffect::SetMatrix",
                   "矩阵槽被换名：\"%s\" → \"%s\"（本线的矩阵槽只有一块 64 B；"
                   "kFxMaxMatrixParams=1 ⇒ 旧的那个 uniform 回到「没设过 = 全 0」）",
                   cpu.mem->readCString(old).c_str(), name.c_str());
            cpu.heap->free(old);
        }
        cpu.mem->wr32(state + kFxMatrixName, name_ea);
    }
    for (std::uint32_t c = 0; c < 16; ++c) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &matrix[c], sizeof(bits));
        cpu.mem->wr32(state + kFxMatrix + c * 4u, bits);
    }
    bump32(cpu, state + kFxSetMatrices);
    if (!same) {
        fxLog("ID3DXEffect::SetMatrix", "\"%s\" = 16 float（独立 64 B 区）", name.c_str());
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
            if (!growTable(cpu, state, kFxTexTable, kFxTexCap, kFxMaxTexCap, kFxEntryBytes, "纹理",
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
    // ★ P2：`*pPasses` = **FX 文本解析出来的** pass 数（P1 的常量 1 已退役）。
    //   technique 没设 / 名字不在文本里 ⇒ 0 个 pass（真机同样不会跑 pass 循环）⇒ 之后任何"在这段
    //   作用域里提交的 draw"都会因 `effectDrawProgram` 的具名原因被拒（bit 44）—— ⛔ 没有静默接受。
    const FxModule* module = effectModuleOf(cpu, state, nullptr);
    std::uint32_t kPassesReported = 0u;
    const std::uint32_t tech_ea = cpu.mem->rd32(state + kFxTech);
    if (module != nullptr && tech_ea != 0) {
        const std::string technique = cpu.mem->readCString(tech_ea);
        for (const FxTechnique& t : module->techniques) {
            if (t.name == technique) kPassesReported = static_cast<std::uint32_t>(t.passes.size());
        }
    }
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
    // ★ index 越界的判定不在这里：`effectDrawProgram` 按**文本解析出的** pass 表取 pass，
    //   取不到 ⇒ 该批带具名原因被拒（bit 44）。这里只推进状态。

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

// ★ P2：把"这一批该怎么画"（程序 + 常量表 + 纹理 EA）推进 `d3d9` 的 effect 作用域。
//   ⛔ 解析失败 / 不在实现集**不**在这里停：那要按**每一批**记账（bit 44 + 具名原因），
//   由 `gpuEligible` 统一做（它会读 `EffectDrawScope::program.reason`）。
void publishDrawProgram(X86& cpu, std::uint32_t state) {
    const d3d9::EffectDrawScope scope = d3d9::effectDrawScope();
    if (!scope.active || scope.state != state) return;
    d3d9::EffectDrawProgram program;
    std::string why;
    if (!d3dx9::effectDrawProgram(cpu, state, &program, &why)) {
        program = d3d9::EffectDrawProgram{};
        program.implemented = false;
        program.reason = "effect 程序解析失败（" + why + "）";
    }
    d3d9::noteEffectDrawProgram(program);
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
    if (hr == d3dx9::kOk) publishDrawProgram(cpu, state);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetFloat", hr, why);
}

// `SetVector(+0x88)`（引擎在 `0x4B55AD` 用它对 `Color` 设每粒子的 uniform float4）
// 与 `SetMatrix(+0x98)`（`0x4B5716` 设 `Proj`，**在 Begin 之前**）—— 两个槽都只**记状态**：
// 状态块里的矩阵/向量区 ⇒ `Begin`/`BeginPass` 发布时被 `effectDrawProgram` 读走（⛔ 不做全局）。
void shimEffectSetVector(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::SetVector(+0x88)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::string name = stringArg(cpu, call.u32(1), "ID3DXEffect::SetVector(+0x88)");
    const std::uint32_t ea = call.u32(2);
    if (ea == 0 || !cpu.mem->committed(ea, 16)) {
        fxTrap(cpu, "ID3DXEffect::SetVector(+0x88)", "pVector=0x%08X 为 NULL / 未提交（要 4 个 float）", ea);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (std::uint32_t c = 0; c < 4; ++c) value[c] = rdGuestF32(cpu, ea + c * 4u);
    std::string why;
    const std::uint32_t hr = d3dx9::effectSetVector(cpu, state, name, value, &why);
    if (hr == d3dx9::kOk) publishDrawProgram(cpu, state);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetVector", hr, why);
}

void shimEffectSetMatrix(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireEffect(cpu, call, "ID3DXEffect::SetMatrix(+0x98)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::string name = stringArg(cpu, call.u32(1), "ID3DXEffect::SetMatrix(+0x98)");
    const std::uint32_t ea = call.u32(2);
    if (ea == 0 || !cpu.mem->committed(ea, 64)) {
        fxTrap(cpu, "ID3DXEffect::SetMatrix(+0x98)", "pMatrix=0x%08X 为 NULL / 未提交（要 16 个 float）", ea);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    float matrix[16] = {};
    for (std::uint32_t c = 0; c < 16; ++c) matrix[c] = rdGuestF32(cpu, ea + c * 4u);
    std::string why;
    const std::uint32_t hr = d3dx9::effectSetMatrix(cpu, state, name, matrix, &why);
    if (hr == d3dx9::kOk) publishDrawProgram(cpu, state);
    call.result = hr == d3dx9::kOk ? hr : reportMethodFailure(cpu, "ID3DXEffect::SetMatrix", hr, why);
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
    if (hr == d3dx9::kOk) publishDrawProgram(cpu, state);
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
    if (hr == d3dx9::kOk) {
        d3d9::noteEffectDrawTechnique(name);
        publishDrawProgram(cpu, state);
    }
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
            publishDrawProgram(cpu, state);
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
    if (hr == d3dx9::kOk) {
        d3d9::noteEffectDrawBeginPass(call.u32(1));
        publishDrawProgram(cpu, state);
    }
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
    // ★ 引擎的次序是 BeginPass → CommitChanges → Draw（S0 §5 待核 #2）⇒ 参数/纹理在这一步再刷一次
    //   快照，draw 时读到的就是**提交后**的那一份（⛔ 不假设 CommitChanges 在 Begin 之前）。
    if (hr == d3dx9::kOk) publishDrawProgram(cpu, state);
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

// ID3DXEffect：S0 §3.2 的 9 个取证槽 + A 单的 2 槽（+0x88 / +0x98）+ IUnknown 的
// QueryInterface/AddRef（Release 是第 3 槽）。
// ★ +0x88 SetVector / +0x98 SetMatrix 的取证：202 的 `Snow::DrawOne` 用 `[eax+88h]` 设 `Color`、
//   `Snow::Render` 用 `[ecx+98h]` 设 `Proj`（EA 锚 `0x4B55AD` / `0x4B5716`）。
// ★ +0x110 GetDevice / +0x114 OnLostDevice / +0x118 OnResetDevice **没被读到** ⇒ fn=nullptr
//   ⇒ `comUnsupported` 的 loud trap（⛔ 不按"应该会调"补）。填在这里的另一个作用：
//   它们落在 vtable 内（⛔ 不越界），trap 时能打印 iface + 偏移。
const std::vector<ComMethod>& effectMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimEffectQueryInterface},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimEffectAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimEffectRelease},              // ★ 0x4AB454
        {0x78u, "SetFloat", Conv::Stdcall, 2, &shimEffectSetFloat},            // ★ 0x4B3D3E / …
        {0x88u, "SetVector", Conv::Stdcall, 2, &shimEffectSetVector},          // ★ 0x4B55AD（Color）
        {0x98u, "SetMatrix", Conv::Stdcall, 2, &shimEffectSetMatrix},          // ★ 0x4B5716（Proj）
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
            fxTrap(cpu, kApi, "建 effect 失败：%s", why.c_str());
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
