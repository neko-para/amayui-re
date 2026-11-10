// emulator-lift · D3DX9 Sprite 簇的状态 / 批次 / 合成（M2 · 第一帧）
//
// ★ 读 `runtime/d3dx9_sprite.h` 的文件头先：vtable 取证的槽表、语义决定、纹理的现状与 loud trap 口径。
// ★ 本文件**不认 COM**：vtable / refcount / 派发在 `runtime/com.h` 与 `shims/d3dx9.cpp`。
// ★ 一切"真的做"的部分都在 compositeBatch：
//     Draw 时把变换应用到四角（与 D3DX 的"顶点当场生成"一致），End/Flush 时按像素合成：
//     白色纹理源（pTexture=0 时的 D3D9 固定功能语义）× 调制色 → 后缓冲；ALPHABLEND 走
//     SRCALPHA/INVSRCALPHA。⛔ 不是"只计数不写像素"。
#include "runtime/d3dx9_sprite.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"  // GPU 覆盖账本（sprite 也记账）
#include "runtime/heap.h"
#include "runtime/machine.h"
#include "runtime/math.h"
#include "runtime/memory.h"
#include "runtime/pixel_format.h"
#include "runtime/surface.h"
#include "runtime/log.h"
#include "runtime/x86.h"

namespace lift {
namespace d3dx9 {
namespace {
SurfaceFormat formatWhite() {
    SurfaceFormat f;
    f.bpp = 32;
    f.rmask = 0x00FF0000u;
    f.gmask = 0x0000FF00u;
    f.bmask = 0x000000FFu;
    f.amask = 0xFF000000u;
    return f;
}

// 状态块布局（**我们自己的**；⛔ 不是 Win32 结构体）
enum SpriteOff : std::uint32_t {
    kSpMagic = 0,         // 'SPR1'
    kSpDevice = 4,        // 建它时的 IDirect3DDevice9 对象 EA（⛔ 不 AddRef，见 createSpriteState）
    kSpFlags = 8,         // 最近一次 Begin 的 Flags
    kSpDrawing = 12,      // 1 = 在 Begin/End 之间
    kSpMatrix = 16,       // 16 个 float = 64 B（D3DXMATRIX，行主序，D3DX 的行向量约定）
    kSpBatch = 80,        // 批次数组（guest 堆 EA）
    kSpCount = 84,        // 已排队条目数
    kSpCapacity = 88,     // 批次数组容量（条目数）
    kSpBegins = 92,
    kSpDraws = 96,
    kSpFlushes = 100,
    kSpEnds = 104,
    kSpLost = 108,
    kSpResets = 112,
    kSpIgnoredFlags = 116,  // 最近一次 Begin 里本线不建模的位
    kSpComposedPixels = 120,
    kSpBytes = 128,
};
constexpr std::uint32_t kSpMagicValue = 0x31525053u;  // 'SPR1'

constexpr std::uint32_t kInitialBatchCapacity = 16u;
constexpr std::uint32_t kMaxBatchCapacity = 1u << 20;  // 1M 条 ≈ 64 MiB 上限（防跑飞）
constexpr std::uint32_t kRectBytes = 16u;              // RECT = 4 个 LONG
constexpr std::uint32_t kVec3Bytes = 12u;              // D3DXVECTOR3 = 3 个 float
constexpr std::uint32_t kMatrixBytes = 64u;            // D3DXMATRIX = 16 个 float

void spriteLog(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
}

// ★ **默认级别可见**的告警（默认日志级别是 `warn`；`Info` 默认看不见）。
//   用途：批次里缓存的纹理身份对不上 ⇒ 这个精灵**被跳过** —— 那正是"本会静默采错像素"的事件，
//   ⛔ 不许降成 Info。限流同 `d3d9_pipeline.cpp` 的 `pipeWarn`：前几次打全量，之后只计数
//   （`--log-level debug` 仍能看到每一次）。
constexpr std::uint32_t kSpriteWarnBudget = 4u;
void spriteWarn(const char* what, const char* fmt, ...) {
    const char* key = what ? what : "?";
    char msg[512];
    msg[0] = '\0';
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    static std::map<std::string, std::uint32_t>* counts = new std::map<std::string, std::uint32_t>();
    const std::uint32_t n = ++(*counts)[key];
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", key);
    if (n <= kSpriteWarnBudget) {
        logAt(LogLevel::Warn, LogD3dx9, prefix, "%s", msg);
        return;
    }
    if (n == kSpriteWarnBudget + 1u) {
        logAt(LogLevel::Warn, LogD3dx9, prefix,
              "同类告警已打满 %u 次 ⇒ 之后只计数（每一次仍以 debug 级别留一条）", kSpriteWarnBudget);
    }
    logAt(LogLevel::Debug, LogD3dx9, prefix, "%s", msg);
}

bool requireMachine(X86& cpu, Machine** out, std::string* why) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) {
        if (why) *why = "D3DX9：activeMachine() 不是这台 cpu 的 Machine（装配问题）";
        return false;
    }
    if (!m->surfaces().ready()) {
        if (why) *why = "D3DX9：SurfaceArena 未初始化";
        return false;
    }
    *out = m;
    return true;
}

bool committed(X86& cpu, std::uint32_t ea, std::uint32_t n, const char* what, std::string* why) {
    if (ea == 0) {
        if (why) *why = std::string("D3DX9：") + what + " 为 NULL";
        return false;
    }
    if (!cpu.mem->committed(ea, n)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "D3DX9：%s = 0x%08X 起 %u 字节未提交（guest 缓冲区非法）", what, ea, n);
        if (why) *why = buf;
        return false;
    }
    return true;
}

float rdFloat(X86& cpu, std::uint32_t ea) {
    const std::uint32_t bits = cpu.mem->rd32(ea);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
void wrFloat(X86& cpu, std::uint32_t ea, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    cpu.mem->wr32(ea, bits);
}

void writeIdentityMatrix(X86& cpu, std::uint32_t ea) {
    // 单位阵也走 math 的唯一适配点（⛔ 不再手写 i%5 的 D3DX 布局）
    float d3d[16];
    math::toD3DLayout(math::identity<float>(), d3d);
    for (int i = 0; i < 16; ++i) wrFloat(cpu, ea + static_cast<std::uint32_t>(i) * 4u, d3d[i]);
}

// 状态块（读一次拿全；内部用）
struct State {
    std::uint32_t state = 0;
    std::uint32_t device = 0;
    std::uint32_t flags = 0;
    std::uint32_t drawing = 0;
    std::uint32_t batch = 0;
    std::uint32_t count = 0;
    std::uint32_t capacity = 0;
    std::uint32_t begins = 0;
    std::uint32_t draws = 0;
    std::uint32_t flushes = 0;
    std::uint32_t ends = 0;
    std::uint32_t lost = 0;
    std::uint32_t resets = 0;
    std::uint32_t ignored_flags = 0;
    std::uint32_t composed_pixels = 0;
    float matrix[16] = {0};
};

bool readState(X86& cpu, std::uint32_t state, State* out, std::string* why) {
    if (state == 0) {
        if (why) *why = "D3DX9：Sprite 状态块 EA 为 0";
        return false;
    }
    if (!cpu.mem->committed(state, kSpBytes)) {
        if (why) *why = "D3DX9：Sprite 状态块未提交（野指针？）";
        return false;
    }
    const std::uint32_t magic = cpu.mem->rd32(state + kSpMagic);
    if (magic != kSpMagicValue) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3DX9：Sprite 状态块 0x%08X 的 magic 是 0x%08X（应为 'SPR1'）", state, magic);
        if (why) *why = buf;
        return false;
    }
    State s;
    s.state = state;
    s.device = cpu.mem->rd32(state + kSpDevice);
    s.flags = cpu.mem->rd32(state + kSpFlags);
    s.drawing = cpu.mem->rd32(state + kSpDrawing);
    for (int i = 0; i < 16; ++i) s.matrix[i] = rdFloat(cpu, state + kSpMatrix + static_cast<std::uint32_t>(i) * 4u);
    s.batch = cpu.mem->rd32(state + kSpBatch);
    s.count = cpu.mem->rd32(state + kSpCount);
    s.capacity = cpu.mem->rd32(state + kSpCapacity);
    s.begins = cpu.mem->rd32(state + kSpBegins);
    s.draws = cpu.mem->rd32(state + kSpDraws);
    s.flushes = cpu.mem->rd32(state + kSpFlushes);
    s.ends = cpu.mem->rd32(state + kSpEnds);
    s.lost = cpu.mem->rd32(state + kSpLost);
    s.resets = cpu.mem->rd32(state + kSpResets);
    s.ignored_flags = cpu.mem->rd32(state + kSpIgnoredFlags);
    s.composed_pixels = cpu.mem->rd32(state + kSpComposedPixels);
    *out = s;
    return true;
}

// 批次里由 Draw AddRef 过的纹理：销毁 / 设备丢失时对称 Release（⛔ 不留 leak）
// ★ **身份判据与合成路径同一份**（段 3 的 ④）：条目里只缓存对象 EA，而 guest 堆会复用 EA
//   ⇒ 纹理在批次还活着时被（越界）Release、EA 被**另一张**纹理拿走时，裸 `release(EA)` 减的是
//   **新占有者**。所以按 Draw 时记下的 serial（进程级单调、永不复用）校验：不符 ⇒ 只清"我们持引用"
//   的标记 + 默认级别可见的告警，⛔ 一个引用都不减。
void releaseQueuedTextures(X86& cpu, const State& s) {
    if (s.count == 0 || s.batch == 0 || !cpu.com) return;
    if (!cpu.mem || !cpu.mem->committed(s.batch, s.count * kBatchEntryBytes)) return;
    for (std::uint32_t i = 0; i < s.count; ++i) {
        const std::uint32_t ea = s.batch + i * kBatchEntryBytes;
        if ((cpu.mem->rd32(ea + kBeReserved) & 1u) == 0) continue;
        const std::uint32_t tex = cpu.mem->rd32(ea + kBeTexture);
        const std::uint32_t serial = cpu.mem->rd32(ea + kBeTextureSerial);
        cpu.mem->wr32(ea + kBeReserved, 0u);
        if (tex == 0) continue;
        // 重解析身份（与 `compositeBatch` 的判据同一条）：serial 不符 / 已不是纹理对象 ⇒ 不释放。
        d3d9::TextureInfo ti;
        if (!d3d9::textureInfo(cpu, tex, &ti, nullptr)) {
            spriteWarn("Release", "第 %u 条 Draw 的纹理 0x%08X 已不是本模块的纹理对象 ⇒ ⛔ 不释放它", i, tex);
            continue;
        }
        if (ti.serial != serial) {
            spriteWarn("Release",
                       "第 %u 条 Draw 的纹理 0x%08X 已被释放 / 被另一张纹理复用（Draw 时 serial=%u，现在=%u）"
                       "⇒ ⛔ 不释放它（新占有者不受影响）",
                       i, tex, serial, ti.serial);
            continue;
        }
        cpu.com->release(cpu, tex);
    }
}

// 批次条目里由 Draw AddRef 过的纹理：**所有出口共用这一条身份判据**。
// ★ 条目里只缓存对象 EA，而 guest 堆会复用 EA ⇒ 纹理在批次还活着时被（越界）Release、EA 被
//   **另一张**纹理拿走之后，裸 `release(EA)` 减的是**新占有者**。`compositeBatch` 有 4 个
//   **早退**出口（无 src rect / 空矩形 / 四边形退化 / 完全屏外）在合成主路径的身份校验之前 ——
//   它们以前走裸 `releaseEntry` ⇒ 同 EA 的新纹理被减到 0（`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #1）。
//   ⇒ 把校验放进 releaseEntry 本身：serial 不符 / 已不是纹理对象 ⇒ 只清"我们持引用"的标记 +
//   默认级别可见的告警，⛔ 一个引用都不减。与 `releaseQueuedTextures` 同一条判据。
bool entryTextureIsCurrent(X86& cpu, std::uint32_t tex, std::uint32_t serial, std::uint32_t index) {
    d3d9::TextureInfo ti;
    if (!d3d9::textureInfo(cpu, tex, &ti, nullptr)) {
        spriteWarn("End", "第 %u 条 Draw 的纹理 0x%08X 已不是本模块的纹理对象 ⇒ ⛔ 不释放它", index, tex);
        return false;
    }
    if (ti.serial != serial) {
        spriteWarn("End",
                   "第 %u 条 Draw 的纹理 0x%08X 已被释放 / 被另一张纹理复用（Draw 时 serial=%u，现在=%u）"
                   "⇒ ⛔ 不释放它（新占有者不受影响）",
                   index, tex, serial, ti.serial);
        return false;
    }
    return true;
}

// 批次容量增长（guest 堆 realloc；新条目由调用方整条写入，所以不清零）
bool reserveBatch(X86& cpu, const State& s, std::uint32_t need, std::string* why) {
    if (need <= s.capacity) return true;
    std::uint32_t cap = s.capacity ? s.capacity : kInitialBatchCapacity;
    while (cap < need) {
        if (cap >= kMaxBatchCapacity) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "D3DX9：Sprite 批次容量到上限（%u 条）⇒ 拒绝再排", kMaxBatchCapacity);
            if (why) *why = buf;
            return false;
        }
        cap *= 2u;
    }
    if (cap > kMaxBatchCapacity) cap = kMaxBatchCapacity;
    const std::uint64_t bytes64 = static_cast<std::uint64_t>(cap) * kBatchEntryBytes;
    if (bytes64 > 0xFFFFFFFFull) {
        if (why) *why = "D3DX9：Sprite 批次数组超过 4 GiB";
        return false;
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(bytes64);
    std::uint32_t np = 0;
    if (s.batch == 0) {
        np = cpu.heap->alloc(bytes, /*zero=*/true);
    } else {
        np = cpu.heap->realloc(s.batch, bytes, /*zero=*/false);
    }
    if (np == 0) {
        if (why) *why = std::string("D3DX9：Sprite 批次数组分配失败：") + cpu.heap->lastError();
        return false;
    }
    cpu.mem->wr32(s.state + kSpBatch, np);
    cpu.mem->wr32(s.state + kSpCapacity, cap);
    return true;
}

// 8 位通道的舍入乘法（只此一处；掩码换算 / load·store 走 runtime/pixel_format.h）
// ★ 它**不是** pixel_format 的 mask/expand 助手：这是 ALPHABLEND 的 8 位舍入乘法；流水线的
//   `blendFactors8`/`applyBlendOp` 是 D3DBLEND/D3DBLENDOP 的设备混合状态语义（另一套公式）。
//   ⛔ 不把它们并成一份 —— 并了会改像素结果（口径不同，不是"看起来重复"）。
std::uint8_t mul255(unsigned a, unsigned b) {
    return static_cast<std::uint8_t>((a * b + 127u) / 255u);
}

// 一次 Draw 的输入（读出来就定型；⛔ 不在 End 时重读 guest 指针 —— D3DX 在 Draw 时就生成顶点）
struct DrawInput {
    std::uint32_t texture = 0;
    std::uint32_t flags = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    float center[3] = {0, 0, 0};
    float position[3] = {0, 0, 0};
    std::uint32_t color = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------
// 状态块
// ---------------------------------------------------------------------------------------
std::uint32_t createSpriteState(X86& cpu, std::uint32_t device_object, std::string* why,
                                bool* out_of_memory) {
    if (out_of_memory) *out_of_memory = false;
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    (void)m;
    if (!cpu.mem) {
        if (why) *why = "D3DX9：cpu.mem 为空";
        return 0;
    }
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3DX9：guest 堆未就绪（Sprite 状态块要放在 guest 堆上）";
        return 0;
    }
    // 设备必须是**本模块建出来的** IDirect3DDevice9（D3DXCreateSprite 的第一参数）
    d3d9::DeviceInfo dev;
    if (!d3d9::deviceInfo(cpu, device_object, &dev, why)) {
        if (why) *why = std::string("D3DX9：D3DXCreateSprite 的设备不是本模块的 IDirect3DDevice9：") + *why;
        return 0;
    }
    const std::uint32_t state = cpu.heap->alloc(kSpBytes, /*zero=*/true);
    if (state == 0) {
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = std::string("D3DX9：guest 堆分配 Sprite 状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    const std::uint32_t batch = cpu.heap->alloc(kInitialBatchCapacity * kBatchEntryBytes, /*zero=*/true);
    if (batch == 0) {
        if (out_of_memory) *out_of_memory = true;
        cpu.heap->free(state);
        if (why) *why = std::string("D3DX9：guest 堆分配 Sprite 批次数组失败：") + cpu.heap->lastError();
        return 0;
    }
    cpu.mem->wr32(state + kSpMagic, kSpMagicValue);
    cpu.mem->wr32(state + kSpDevice, device_object);
    cpu.mem->wr32(state + kSpBatch, batch);
    cpu.mem->wr32(state + kSpCapacity, kInitialBatchCapacity);
    writeIdentityMatrix(cpu, state + kSpMatrix);
    // ★ 不 AddRef 设备：Sprite 由设备的持有者建/销毁（引擎 sub_4A2BA0 先 Release sprite，Scene 析构
    //   再 Release 设备）。若设备先没了，End() 会**明确失败**（deviceInfo 找不到对象），⛔ 不假装画了。
    spriteLog("CreateSprite",
              "设备对象=0x%08X（%dx%d D3DFORMAT=%u）⇒ Sprite 状态块 0x%08X / 批次 0x%08X（容量 %u 条）",
              device_object, dev.width, dev.height, dev.format, state, batch, kInitialBatchCapacity);
    return state;
}

bool spriteInfoOfState(X86& cpu, std::uint32_t state, SpriteInfo* out, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return false;
    SpriteInfo info;
    info.state = s.state;
    info.device = s.device;
    info.flags = s.flags;
    info.ignored_flags = s.ignored_flags;
    info.drawing = s.drawing != 0;
    info.begins = s.begins;
    info.draws = s.draws;
    info.flushes = s.flushes;
    info.ends = s.ends;
    info.lost = s.lost;
    info.resets = s.resets;
    info.batch_count = s.count;
    info.batch_capacity = s.capacity;
    info.composed_pixels = s.composed_pixels;
    for (int i = 0; i < 16; ++i) info.matrix[i] = s.matrix[i];
    *out = info;
    return true;
}

bool spriteInfo(X86& cpu, std::uint32_t object, SpriteInfo* out, std::string* why) {
    if (!cpu.com) {
        if (why) *why = "D3DX9：cpu.com 为空（不是 COM 对象）";
        return false;
    }
    const std::uint32_t state = cpu.com->userOf(object);
    if (state == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3DX9：0x%08X 不是本模块建的 Sprite 对象（obj+8 没有状态块）", object);
        if (why) *why = buf;
        return false;
    }
    return spriteInfoOfState(cpu, state, out, why);
}

void destroySpriteState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kSpBytes)) return;
    if (cpu.mem->rd32(state + kSpMagic) != kSpMagicValue) return;
    // 批次里还挂着的纹理 AddRef 先还掉（Sprite 在 Begin/End 之间被 Release 的情况）
    State s;
    if (readState(cpu, state, &s, nullptr)) releaseQueuedTextures(cpu, s);
    const std::uint32_t batch = cpu.mem->rd32(state + kSpBatch);
    cpu.mem->wr32(state + kSpMagic, 0);
    if (batch != 0 && cpu.heap) cpu.heap->free(batch);
    if (cpu.heap) cpu.heap->free(state);
}

std::uint32_t spriteDevice(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kSpBytes)) return 0;
    if (cpu.mem->rd32(state + kSpMagic) != kSpMagicValue) return 0;
    return cpu.mem->rd32(state + kSpDevice);
}

// ---------------------------------------------------------------------------------------
// Begin / Draw
// ---------------------------------------------------------------------------------------
std::uint32_t begin(X86& cpu, std::uint32_t state, std::uint32_t flags, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (s.drawing != 0) {
        spriteLog("Begin", "已经在 Begin 里（上一次没有 End/Flush）⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3DX9：Begin 嵌套（Begin 已在批次里）";
        return kInvalidCall;
    }
    // ★ flag 口径（二选一，写进注释与交付）：**认识位按语义处理，其余位记录并忽略**。
    //   不认识的位（含未定义位）不当作失败：D3DX 的 flag 是"提示"，真机对未知位也不返回错误；
    //   但⛔ 不静默 —— 记进 state 的 kSpIgnoredFlags + 一行日志。
    const std::uint32_t ignored =
        flags & ~(kSpriteAlphaBlend);  // 除 ALPHABLEND 外本线**不建模**（没有渲染状态 / 排序 / 3D）
    cpu.mem->wr32(state + kSpFlags, flags);
    cpu.mem->wr32(state + kSpIgnoredFlags, ignored);
    cpu.mem->wr32(state + kSpDrawing, 1u);
    cpu.mem->wr32(state + kSpCount, 0u);  // 新批次从空开始（上一次的残留不跨 Begin）
    cpu.mem->wr32(state + kSpBegins, s.begins + 1u);
    if (flags & ~kSpriteKnownFlags) {
        spriteLog("Begin", "Flags=0x%08X 含**未定义**位 0x%08X ⇒ 记录并忽略（不失败）", flags,
                  flags & ~kSpriteKnownFlags);
    }
    if (ignored != 0) {
        spriteLog("Begin",
                  "Flags=0x%08X：ALPHABLEND=%d；其余位 0x%08X **记录并忽略**"
                  "（本线没有渲染状态保存/排序/3D 空间：OBJECTSPACE/BILLBOARD/SORT_*/DONOT* 都不建模）",
                  flags, (flags & kSpriteAlphaBlend) ? 1 : 0, ignored);
    } else {
        spriteLog("Begin", "Flags=0x%08X（ALPHABLEND=1，其余 0）⇒ 批次打开", flags);
    }
    return kOk;
}

std::uint32_t draw(X86& cpu, std::uint32_t state, std::uint32_t texture, std::uint32_t rect_ea,
                   std::uint32_t center_ea, std::uint32_t position_ea, std::uint32_t color,
                   std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (s.drawing == 0) {
        spriteLog("Draw", "不在 Begin/End 之间 ⇒ D3DERR_INVALIDCALL（⛔ 不静默排进一个不存在的批次）");
        if (why) *why = "D3DX9：Draw 在 Begin 之外";
        return kInvalidCall;
    }
    DrawInput in;
    in.texture = texture;
    in.color = color;
    std::uint32_t tex_w = 0;
    std::uint32_t tex_h = 0;
    std::uint32_t tex_serial = 0;  // 纹理身份（合成时用来判"EA 还是不是那张纹理"）
    bool take_ref = false;
    if (texture != 0) {
        // 真纹理：必须是我们建的 IDirect3DTexture9，且必须与 Sprite 同一台设备（D3DX 语义）。
        d3d9::TextureInfo ti;
        if (!d3d9::textureInfo(cpu, texture, &ti, why)) {
            if (why) *why = std::string("D3DX9：Draw 的 pTexture 不是本模块的 IDirect3DTexture9：") + *why;
            return kFail;
        }
        if (ti.device != s.device) {
            if (why) *why = "D3DX9：Draw 的纹理不属于 Sprite 的设备（真机也是 D3DERR_INVALIDCALL）";
            return kInvalidCall;
        }
        tex_w = ti.width;
        tex_h = ti.height;
        // ★ **身份戳**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 #3）：批次里只存对象 EA 是不够的 ——
        //   `D3DXSPRITE_DO_NOT_ADDREF_TEXTURE` 下我们**故意**不加引用（那是微软文档写的语义：
        //   "Disables calling AddRef() on every draw, and Release() on Flush()"），所以纹理可以在
        //   批次还活着时被 guest 释放、它的对象 EA 被**另一张纹理**拿走。EA 一模一样 ⇒ 合成时
        //   只按 EA + `'TEX1'` magic 重解析会**静默采错像素**。serial 是进程级单调、永不复用的身份
        //   （`d3d9::TextureInfo::serial`）⇒ 把它随批次记下来，合成时对不上就拒绝。
        tex_serial = ti.serial;
        // ★ 与 D3DX 一致：Draw 会 **AddRef 纹理**（除非 Begin 给了 D3DXSPRITE_DO_NOT_ADDREF_TEXTURE），
        //   这样批次合成前纹理不会被提前 Release 掉。End/Flush 时对称 Release。
        if ((s.flags & kSpriteDoNotAddRefTexture) == 0) {
            if (!cpu.com || !cpu.com->addRef(cpu, texture)) {
                if (why) *why = "D3DX9：Draw 给纹理 AddRef 失败（对象表不一致）";
                return kFail;
            }
            take_ref = true;
        }
    }
    // pSrcRect：**几何尺寸的来源**。
    //  · pTexture != 0 且 pSrcRect == NULL ⇒ 用整张纹理（D3DX 语义）。
    //  · pTexture == 0 且 pSrcRect == NULL ⇒ 没有尺寸来源 ⇒ 拒绝猜（shim 层已经 loud trap）。
    if (rect_ea == 0) {
        if (texture == 0) {
            if (why) *why = "D3DX9：Draw(pTexture=NULL, pSrcRect=NULL) —— 没有尺寸来源，拒绝猜";
            return kInvalidCall;
        }
        in.flags |= kBeHasSrcRect;
        in.left = 0;
        in.top = 0;
        in.right = static_cast<std::int32_t>(tex_w);
        in.bottom = static_cast<std::int32_t>(tex_h);
    } else {
        if (!committed(cpu, rect_ea, kRectBytes, "Draw 的 pSrcRect", why)) return kInvalidCall;
        in.flags |= kBeHasSrcRect;
        in.left = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 0));
        in.top = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 4));
        in.right = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 8));
        in.bottom = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 12));
    }
    if (in.right <= in.left || in.bottom <= in.top) {
        // 真机：空 src rect ⇒ 画不出东西（S_OK）。记录，不排队（AddRef 的纹理要还回去）。
        spriteLog("Draw", "pSrcRect=(%d,%d,%d,%d) 是空矩形 ⇒ 这次 Draw 什么也不画（S_OK）", in.left, in.top,
                  in.right, in.bottom);
        if (take_ref && cpu.com) cpu.com->release(cpu, texture);
        return kOk;
    }
    if (center_ea != 0) {
        if (!committed(cpu, center_ea, kVec3Bytes, "Draw 的 pCenter", why)) {
            if (take_ref && cpu.com) cpu.com->release(cpu, texture);
            return kInvalidCall;
        }
        in.flags |= kBeHasCenter;
        for (int i = 0; i < 3; ++i) in.center[i] = rdFloat(cpu, center_ea + static_cast<std::uint32_t>(i) * 4u);
    }
    if (position_ea != 0) {
        if (!committed(cpu, position_ea, kVec3Bytes, "Draw 的 pPosition", why)) {
            if (take_ref && cpu.com) cpu.com->release(cpu, texture);
            return kInvalidCall;
        }
        in.flags |= kBeHasPosition;
        for (int i = 0; i < 3; ++i) in.position[i] = rdFloat(cpu, position_ea + static_cast<std::uint32_t>(i) * 4u);
    }
    // ★ 变换在 **Draw 时**应用到四角：D3DX 的顶点是 Draw 时生成的，之后 SetTransform 不追溯改批次。
    //   屏幕空间语义：D3DX 的行向量约定 `v' = v * M`（具体适配见 runtime/math.h，⛔ 不在这里展开公式）。
    const float* m = s.matrix;
    // 非仿射（最后一行不是 0,0,0,1）⇒ 本线不建模透视；记录并**按仿射部分**算（⛔ 不静默）
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) {
        spriteLog("Draw",
                  "变换矩阵不是仿射（m[3]=%g m[7]=%g m[11]=%g m[15]=%g）⇒ 本线不建模透视，按 2D 仿射部分合成",
                  static_cast<double>(m[3]), static_cast<double>(m[7]), static_cast<double>(m[11]),
                  static_cast<double>(m[15]));
    }
    const double ax = static_cast<double>(in.position[0]) - static_cast<double>(in.center[0]);
    const double ay = static_cast<double>(in.position[1]) - static_cast<double>(in.center[1]);
    const double az = static_cast<double>(in.position[2]) - static_cast<double>(in.center[2]);
    const double w = static_cast<double>(in.right - in.left);
    const double h = static_cast<double>(in.bottom - in.top);
    // ★ Sprite 四角变换与 D3DXMatrixMultiply / 内部 T&L 走**同一个**适配点（runtime/math.h）：
    //   四角 = 左上角点（w=1）+ 宽×u 轴（w=0）+ 高×v 轴（w=0）。
    double md[16];
    for (int i = 0; i < 16; ++i) md[i] = static_cast<double>(m[i]);
    const math::Mat4d M = math::fromD3DLayout(md);
    const math::Vec4d p0 = math::transformVec4D3D(M, math::Vec4d(ax, ay, az, 1.0));
    const math::Vec4d uaxis = math::transformVec4D3D(M, math::Vec4d(w, 0.0, 0.0, 0.0));
    const math::Vec4d vaxis = math::transformVec4D3D(M, math::Vec4d(0.0, h, 0.0, 0.0));
    const double x0 = p0.x;
    const double y0 = p0.y;
    const double x1 = x0 + uaxis.x;
    const double y1 = y0 + uaxis.y;
    const double x2 = x0 + vaxis.x;
    const double y2 = y0 + vaxis.y;
    const double x3 = x1 + vaxis.x;
    const double y3 = y1 + vaxis.y;
    // 排队
    if (!reserveBatch(cpu, s, s.count + 1u, why)) {
        if (take_ref && cpu.com) cpu.com->release(cpu, texture);
        return kFail;
    }
    const std::uint32_t ea = s.batch + s.count * kBatchEntryBytes;
    cpu.mem->wr32(ea + kBeTexture, in.texture);
    cpu.mem->wr32(ea + kBeFlags, in.flags);
    cpu.mem->wr32(ea + kBeSrc + 0, static_cast<std::uint32_t>(in.left));
    cpu.mem->wr32(ea + kBeSrc + 4, static_cast<std::uint32_t>(in.top));
    cpu.mem->wr32(ea + kBeSrc + 8, static_cast<std::uint32_t>(in.right));
    cpu.mem->wr32(ea + kBeSrc + 12, static_cast<std::uint32_t>(in.bottom));
    const double quad[8] = {x0, y0, x1, y1, x2, y2, x3, y3};
    for (int i = 0; i < 8; ++i)
        wrFloat(cpu, ea + kBeQuad + static_cast<std::uint32_t>(i) * 4u, static_cast<float>(quad[i]));
    cpu.mem->wr32(ea + kBeColor, in.color);
    cpu.mem->wr32(ea + kBeReserved, take_ref ? 1u : 0u);  // bit0 = 这条纹理是我们 AddRef 的
    cpu.mem->wr32(ea + kBeTextureSerial, tex_serial);      // 纹理身份（合成时的判据）
    cpu.mem->wr32(state + kSpCount, s.count + 1u);
    cpu.mem->wr32(state + kSpDraws, s.draws + 1u);
    return kOk;
}

// ---------------------------------------------------------------------------------------
// 合成（End / Flush 共用）
// ---------------------------------------------------------------------------------------
bool compositeBatch(X86& cpu, std::uint32_t state, std::uint32_t* entries_out,
                    std::uint64_t* pixels_written, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    State s;
    if (!readState(cpu, state, &s, why)) return false;
    if (entries_out) *entries_out = 0;
    if (pixels_written) *pixels_written = 0;
    if (s.count == 0) {
        if (pixels_written) *pixels_written = 0;
        return true;
    }
    if (s.batch == 0 || !cpu.mem->committed(s.batch, s.count * kBatchEntryBytes)) {
        if (why) *why = "D3DX9：Sprite 批次数组 EA / 范围非法";
        return false;
    }
    d3d9::DeviceInfo dev;
    if (!d3d9::deviceInfo(cpu, s.device, &dev, why)) {
        if (why) *why = std::string("D3DX9：合成时找不到 Sprite 的设备（设备已 Release？）：") + *why;
        return false;
    }
    GuestSurface* gs = m->surfaces().find(dev.arena);
    if (!gs) {
        if (why) *why = "D3DX9：设备后缓冲像素不在 arena 里（设备状态与 arena 不一致）";
        return false;
    }
    std::uint8_t* base = m->surfaces().pixels(*gs);
    const std::uint32_t bpp = gs->format.bytesPerPixel();
    if (!base || bpp == 0) {
        if (why) *why = "D3DX9：后缓冲像素指针为空";
        return false;
    }
    const SurfaceFormat fmt = gs->format;
    const bool blend = (s.flags & kSpriteAlphaBlend) != 0;
    // ★ 混合帧：如果 D3D9 的 GPU 光栅还有未读回的像素，先落回 guest；否则 CPU 精灵
    //   会合成在**过期的**后缓冲上（这是 frame 2+ 背景大面积错的根因）。
    if (GpuRaster* gpu = m->gpuRaster();
        gpu != nullptr && gpu->rasterActive() && !gpu->rasterSpritesEnabled() && gpu->rasterDirty()) {
        std::string sync_err;
        if (!gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &sync_err)) {
            if (why) *why = std::string("D3DX9：GPU 像素读回失败：") + sync_err;
            return false;
        }
    }
    std::uint64_t written = 0;
    std::uint32_t entries_done = 0;
    // ★ GPU 覆盖统计只统计"GPU 已启用、但这条 sprite 回退 CPU"的情况。
    //   `--raster cpu`（没有后端）与 `--raster-sprites 0`（显式 A/B）都**不算 fallback**。
    GpuRaster* gpu_backend = m->gpuRaster();
    const bool sprite_gpu_active =
        gpu_backend != nullptr && gpu_backend->rasterActive() && gpu_backend->rasterSpritesEnabled();
    // 条目级收尾：Draw 时 AddRef 过的纹理必须**每条路径**都还回去（含 continue 的早退分支）。
    // ★ 身份校验在这里（**所有出口之前**，不是只在合成主路径）：4 个早退分支（无 src rect /
    //   空矩形 / 退化 / 屏外）也走同一条 releaseEntry ⇒ 同 EA 的新纹理一个引用都不减。
    auto releaseEntry = [&](std::uint32_t ea, std::uint32_t index) {
        if (!cpu.com) return;
        if ((cpu.mem->rd32(ea + kBeReserved) & 1u) == 0) return;
        const std::uint32_t t = cpu.mem->rd32(ea + kBeTexture);
        const std::uint32_t serial = cpu.mem->rd32(ea + kBeTextureSerial);
        cpu.mem->wr32(ea + kBeReserved, 0u);
        if (t == 0) return;
        if (!entryTextureIsCurrent(cpu, t, serial, index)) return;
        cpu.com->release(cpu, t);
    };
    for (std::uint32_t i = 0; i < s.count; ++i) {
        const std::uint32_t ea = s.batch + i * kBatchEntryBytes;
        const std::uint32_t color = cpu.mem->rd32(ea + kBeColor);
        const std::uint32_t eflags = cpu.mem->rd32(ea + kBeFlags);
        if ((eflags & kBeHasSrcRect) == 0) {  // 排不进来的条目（防御）
            if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
            releaseEntry(ea, i);
            continue;
        }
        const std::int32_t left = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 0));
        const std::int32_t top = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 4));
        const std::int32_t right = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 8));
        const std::int32_t bottom = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 12));
        const double w = static_cast<double>(right - left);
        const double h = static_cast<double>(bottom - top);
        if (w <= 0 || h <= 0) {  // 空矩形（Draw 已经记过；这里只是防御）
            if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
            releaseEntry(ea, i);
            continue;
        }
        double q[8];
        for (int k = 0; k < 8; ++k) q[k] = static_cast<double>(rdFloat(cpu, ea + kBeQuad + static_cast<std::uint32_t>(k) * 4u));
        // 仿射逆映射：X = x0 + u*(x1-x0)/w + v*(x2-x0)/h ；Y 同理。
        const double ux = (q[2] - q[0]) / w;  // dX/du
        const double uy = (q[3] - q[1]) / w;  // dY/du
        const double vx = (q[4] - q[0]) / h;  // dX/dv
        const double vy = (q[5] - q[1]) / h;  // dY/dv
        const double det = ux * vy - vx * uy;
        if (std::fabs(det) < 1e-12) {
            spriteLog("End", "第 %u 条 Draw 的四边形退化（det≈0）⇒ 跳过", i);
            if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
            releaseEntry(ea, i);
            continue;
        }
        // 目标包围盒（四角 min/max；裁剪到后缓冲）
        double minx = q[0], maxx = q[0], miny = q[1], maxy = q[1];
        for (int k = 2; k < 8; k += 2) {
            if (q[k] < minx) minx = q[k];
            if (q[k] > maxx) maxx = q[k];
            if (q[k + 1] < miny) miny = q[k + 1];
            if (q[k + 1] > maxy) maxy = q[k + 1];
        }
        int ix0 = static_cast<int>(std::floor(minx));
        int iy0 = static_cast<int>(std::floor(miny));
        int ix1 = static_cast<int>(std::ceil(maxx));
        int iy1 = static_cast<int>(std::ceil(maxy));
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 > gs->width) ix1 = gs->width;
        if (iy1 > gs->height) iy1 = gs->height;
        if (ix1 <= ix0 || iy1 <= iy0) {
            // 完全在屏幕外：真机也不会画（不是错误，也不是回退）
            ++entries_done;
            if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
            releaseEntry(ea, i);
            continue;
        }
        // 调制色（D3D 的 MODULATE：源 × 漫反射色）。D3DCOLOR 的位序只在 pixel_format 里有一份。
        unsigned sa = 0, sr = 0, sg = 0, sb = 0;
        pixel_format::unpackColor(color, sa, sr, sg, sb);
        // ---- 源：白色（pTexture=0）或真纹理 ----
        const std::uint32_t texture = cpu.mem->rd32(ea + kBeTexture);
        const std::uint8_t* tbase = nullptr;
        SurfaceFormat tfmt;
        std::uint32_t tw = 0;
        std::uint32_t th = 0;
        std::uint32_t tpitch = 0;
        std::uint32_t tbpp = 0;
        std::uint32_t tversion = 0;  // 纹理内容版本（GPU 上传缓存用）
        std::uint32_t tserial = 0;   // 纹理身份（GPU 缓存键用）
        // ★ 方案 C 的脏区输入：最近一次写入的矩形 + 版本链（`d3d9::TextureInfo`）。
        std::uint32_t twrite_version = 0;
        std::uint32_t tprev_write_version = 0;
        std::int32_t twrite_rect[4] = {0, 0, 0, 0};
        if (texture != 0) {
            const std::uint32_t entry_serial = cpu.mem->rd32(ea + kBeTextureSerial);
            d3d9::TextureInfo ti;
            if (!d3d9::textureInfo(cpu, texture, &ti, why)) {
                if (why) *why = std::string("D3DX9：合成时纹理无效（被提前 Release？）：") + *why;
                return false;
            }
            // ★ **身份校验**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 #3）：批次里只缓存了对象 EA，
            //   而 EA 会被 guest 堆复用。`D3DXSPRITE_DO_NOT_ADDREF_TEXTURE` 的文档语义就是
            //   "Draw 不 AddRef / Flush 不 Release"（本线**有意**照做）⇒ 纹理可以在批次还活着时
            //   被释放、EA 被**另一张**纹理拿走。这时只按 EA + `'TEX1'` magic 重解析会**成功**并
            //   采到别人的像素 —— serial 不符就说明"这已经不是画那一刻那张纹理了"。
            //   ⇒ 跳过**这一个精灵**（不是整批失败：其余精灵的纹理身份各自独立）并打默认级别可见
            //     的告警。⛔ 不读一个"看起来合法"的像素。
            if (ti.serial != entry_serial) {
                spriteWarn("End",
                           "第 %u 条 Draw 的纹理 0x%08X 已被释放 / 被另一张纹理复用（Draw 时 serial=%u，"
                           "现在=%u）⇒ 跳过这个精灵（⛔ 不采别人的像素）",
                           i, texture, entry_serial, ti.serial);
                if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
                // ★ 这里**不**走 `releaseEntry`：serial 不符意味着画那一刻那张纹理已经死了（EA 才会
                //   被复用）⇒ 我们替它 AddRef 的那一份早被越界 Release 抵掉了；此时再 `release(texture)`
                //   减的是**新占有者**（同一 EA 的 T2）。清掉"我们持引用"的标记就够。
                cpu.mem->wr32(ea + kBeReserved, 0u);
                continue;
            }
            tversion = ti.version;
            tserial = ti.serial;
            twrite_version = ti.write_version;
            tprev_write_version = ti.prev_write_version;
            for (int k = 0; k < 4; ++k) twrite_rect[k] = ti.write_rect[k];
            GuestSurface* tgs = m->surfaces().find(ti.arena);
            if (!tgs) {
                if (why) *why = "D3DX9：合成时纹理像素不在 arena 里";
                return false;
            }
            tbase = m->surfaces().pixels(*tgs);
            tfmt = tgs->format;
            tw = static_cast<std::uint32_t>(tgs->width);
            th = static_cast<std::uint32_t>(tgs->height);
            tpitch = tgs->pitch;
            tbpp = tfmt.bytesPerPixel();
            if (!tbase || tbpp == 0) {
                if (why) *why = "D3DX9：合成时纹理像素指针为空";
                return false;
            }
        }
        // 源矩形按纹理边界裁剪（D3DX 的"越界源矩形"会裁掉画不出来的那一截）
        double u0 = 0.0, u1 = w, v0 = 0.0, v1 = h;
        if (texture != 0) {
            if (left < 0) u0 = static_cast<double>(-left);
            if (right > static_cast<std::int32_t>(tw)) u1 = static_cast<double>(static_cast<std::int32_t>(tw) - left);
            if (top < 0) v0 = static_cast<double>(-top);
            if (bottom > static_cast<std::int32_t>(th)) v1 = static_cast<double>(static_cast<std::int32_t>(th) - top);
            if (u1 <= u0 || v1 <= v0) {
                spriteLog("End", "第 %u 条 Draw 的源矩形完全在纹理外 ⇒ 跳过", i);
                if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
                releaseEntry(ea, i);
                ++entries_done;
                continue;
            }
        }
        // ---- GPU 光栅化：D3DXSprite 纹理四边形（sampler 用 stage 默认 NEAREST+CLAMP，⛔ 不改）----
        bool gpu_done = false;
        std::string sprite_fallback_reason;
        const std::uint64_t written_before = written;
        const bool has_texture = texture != 0;
        GpuRaster* gpu = sprite_gpu_active ? gpu_backend : nullptr;
        if (gpu != nullptr) {
            if (fmt.bpp != 32) {
                sprite_fallback_reason = "sprite: 后缓冲不是 32bpp";
                gpu = nullptr;
            } else if (has_texture && tbpp != 4) {
                sprite_fallback_reason = "sprite: 纹理不是 32bpp";
                gpu = nullptr;
            } else if (has_texture && (left < 0 || top < 0 || right > static_cast<std::int32_t>(tw) ||
                                       bottom > static_cast<std::int32_t>(th))) {
                sprite_fallback_reason = "sprite: 源矩形越界（GPU 路径不做逐像素裁剪）";
                gpu = nullptr;
            }
        }
        if (gpu != nullptr) {
            std::string gerr;
            // ★ 目标身份带 guest 后缓冲像素基址（同尺寸换 surface 必须能判出来）。
            bool ok = gpu->rasterPrepare(fmt, gs->width, gs->height, &gerr, base);
            if (ok && !gpu->rasterDirty()) {
                ok = gpu->rasterSyncFromGuest(base, gs->pitch, gs->width, gs->height, &gerr);
            }
            if (ok) {
                const float r = static_cast<float>(sr) / 255.0f;
                const float g = static_cast<float>(sg) / 255.0f;
                const float b = static_cast<float>(sb) / 255.0f;
                const float a = static_cast<float>(sa) / 255.0f;
                const float u0f = has_texture ? static_cast<float>(left) / static_cast<float>(tw) : 0.0f;
                const float u1f = has_texture ? static_cast<float>(right) / static_cast<float>(tw) : 1.0f;
                const float v0f = has_texture ? static_cast<float>(top) / static_cast<float>(th) : 0.0f;
                const float v1f = has_texture ? static_cast<float>(bottom) / static_cast<float>(th) : 1.0f;
                GpuRasterVertex v[6];
                auto set = [&](int n, double x, double y, float u, float vv) {
                    v[n].x = static_cast<float>(x);
                    v[n].y = static_cast<float>(y);
                    v[n].z = 0.0f;
                    v[n].r = r;
                    v[n].g = g;
                    v[n].b = b;
                    v[n].a = a;
                    v[n].u = u;
                    v[n].v = vv;
                };
                set(0, q[0], q[1], u0f, v0f);
                set(1, q[2], q[3], u1f, v0f);
                set(2, q[4], q[5], u0f, v1f);
                set(3, q[2], q[3], u1f, v0f);
                set(4, q[6], q[7], u1f, v1f);
                set(5, q[4], q[5], u0f, v1f);
                GpuRasterState gs_state;
                gs_state.viewport_w = gs->width;
                gs_state.viewport_h = gs->height;
                gs_state.scissor_x1 = gs->width;
                gs_state.scissor_y1 = gs->height;
                gs_state.blend = blend;
                // CPU 的 sprite 合成不看深度 ⇒ GPU 这条也要关掉，否则两边语义不等价。
                gs_state.depth_test = false;
                gs_state.depth_write = false;
                GpuRasterTexture gt;
                if (has_texture) {
                    gt.pixels = tbase;
                    gt.pitch = tpitch;
                    gt.width = static_cast<int>(tw);
                    gt.height = static_cast<int>(th);
                    gt.format = tfmt;
                    gt.key = tbase;
                    gt.generation = tversion;
                    gt.serial = tserial;
                    // ★ 方案 C：把"最近一次写的矩形 + 版本链"交给后端 —— 由它判要不要切段。
                    //   `write_version == version` 才算"写区与当前内容版本对得上"。
                    gt.content_change_known = twrite_version != 0 && twrite_version == tversion;
                    gt.content_change_rect = GpuTexelRect{twrite_rect[0], twrite_rect[1],
                                                          twrite_rect[2] - twrite_rect[0],
                                                          twrite_rect[3] - twrite_rect[1]};
                    gt.content_change_version = twrite_version;
                    gt.content_change_prev_version = tprev_write_version;
                } else {
                    // pTexture=0 的 D3DX 语义 = 白色源；GPU 用 1×1 白纹理 + 顶点色。
                    gt.pixels = nullptr;
                    gt.pitch = 0;
                    gt.width = 1;
                    gt.height = 1;
                    gt.format = formatWhite();
                    gt.key = nullptr;
                    gt.generation = 0;
                }
                // ★ 合批：这里只入队（End 不再逐条提交）；Present / Clear / 换 target 才 flush。
                //   纹理像素已在 rasterQueue 内部拷走后端 staging（⛔ 不保存 guest 指针，见下条注释）。
                ok = gpu->rasterQueue(v, 6, &gt, gs_state, &gerr);
            }
            if (ok) {
                gpu_done = true;
                written += static_cast<std::uint64_t>(ix1 - ix0) * static_cast<std::uint64_t>(iy1 - iy0);
                static bool logged_ok = false;
                if (!logged_ok) {
                    logged_ok = true;
                    spriteLog("End", "GPU sprite 成功：%ux%u → %d,%d %dx%d", tw, th, ix0, iy0,
                              ix1 - ix0, iy1 - iy0);
                }
            } else {
                // GPU 失败：把已经画出的部分落回 guest，本条回退 CPU（记账 + 首次打印）。
                spriteLog("End", "第 %u 条 Draw 的 GPU 纹理路径失败：%s ⇒ 回退 CPU", i, gerr.c_str());
                sprite_fallback_reason = "sprite: GPU 后端失败：" + gerr;
                std::string sync_err;
                gpu->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &sync_err);
            }
        }
        if (!gpu_done) {
        // ★ 合批后的新屏障：这条 sprite 要由 CPU 直接写 guest，而队列里可能还有**更早**的 sprite
        //   没提交。必须先把它们画进镜像并回读 guest，否则 Present 读回时会把这条 CPU 像素盖掉。
        // ★ 用 `gpu_backend` 而不是局部 `gpu`：上面已因本条的回退原因把局部 `gpu` 置空，但队列
        //   在 `gpu_backend` 里 —— 这条屏障不能随"本条为什么回退"一起失效。
        if (sprite_gpu_active && gpu_backend != nullptr && gpu_backend->rasterDirty()) {
            std::string sync_err;
            if (!gpu_backend->rasterSyncToGuest(base, gs->pitch, gs->width, gs->height, &sync_err)) {
                if (why) *why = std::string("D3DX9：CPU 回退前 GPU 像素读回失败：") + sync_err;
                releaseEntry(ea, i);
                return false;
            }
        }
        for (int y = iy0; y < iy1; ++y) {
            for (int x = ix0; x < ix1; ++x) {
                // 像素中心采样（D3D 的光栅化规则）
                const double px = static_cast<double>(x) + 0.5;
                const double py = static_cast<double>(y) + 0.5;
                const double dx = px - q[0];
                const double dy = py - q[1];
                const double u = (dx * vy - dy * vx) / det;
                const double v = (dy * ux - dx * uy) / det;
                if (u < u0 || u >= u1 || v < v0 || v >= v1) continue;  // 四边形外 / 纹理外（裁剪）
                unsigned tex_r = 255u;
                unsigned tex_g = 255u;
                unsigned tex_b = 255u;
                unsigned tex_a = 255u;
                if (tbase) {
                    const int srcx = left + static_cast<int>(std::floor(u));
                    const int srcy = top + static_cast<int>(std::floor(v));
                    const std::uint8_t* tp = tbase + static_cast<std::size_t>(srcy) * tpitch +
                                             static_cast<std::size_t>(srcx) * tbpp;
                    const std::uint32_t raw = pixel_format::loadPixel(tp, tfmt.bpp);
                    tex_r = pixel_format::expandChannel(raw, tfmt.rmask);
                    tex_g = pixel_format::expandChannel(raw, tfmt.gmask);
                    tex_b = pixel_format::expandChannel(raw, tfmt.bmask);
                    // mask==0（无 alpha 通道）⇒ expandChannel 返回 255（不透明），语义见 pixel_format.h
                    tex_a = pixel_format::expandChannel(raw, tfmt.amask);
                }
                const unsigned src_r = mul255(tex_r, sr);
                const unsigned src_g = mul255(tex_g, sg);
                const unsigned src_b = mul255(tex_b, sb);
                const unsigned src_a = mul255(tex_a, sa);
                std::uint8_t* p = base + static_cast<std::size_t>(y) * gs->pitch +
                                  static_cast<std::size_t>(x) * bpp;
                std::uint32_t argb = 0;
                if (!blend) {
                    // 无 ALPHABLEND：真机的默认渲染状态是"不混合" ⇒ 覆盖（含 alpha 通道）
                    argb = (src_a << 24) | (src_r << 16) | (src_g << 8) | src_b;
                } else {
                    const std::uint32_t dst_raw = pixel_format::loadPixel(p, fmt.bpp);
                    const unsigned dr = pixel_format::expandChannel(dst_raw, fmt.rmask);
                    const unsigned dg = pixel_format::expandChannel(dst_raw, fmt.gmask);
                    const unsigned db = pixel_format::expandChannel(dst_raw, fmt.bmask);
                    const unsigned da = pixel_format::expandChannel(dst_raw, fmt.amask);
                    // SRCALPHA / INVSRCALPHA（D3DXSPRITE_ALPHABLEND 的固定功能状态）
                    const unsigned ia = 255u - src_a;
                    const unsigned r = (src_r * src_a + dr * ia + 127u) / 255u;
                    const unsigned g = (src_g * src_a + dg * ia + 127u) / 255u;
                    const unsigned b = (src_b * src_a + db * ia + 127u) / 255u;
                    // alpha 通道用同一对因子（SEPARATEALPHABLEND 默认关）
                    const unsigned a = (src_a * src_a + da * ia + 127u) / 255u;
                    argb = (a << 24) | (r << 16) | (g << 8) | b;
                }
                pixel_format::storePixel(p, fmt.bpp, pixel_format::pixelFromColor(fmt, argb));
                ++written;
            }
        }
        }  // !gpu_done
        // ★ sprite 的 GPU 覆盖账：只有在 GPU 已启用（且 sprite 路径开着）时才记。
        if (sprite_gpu_active) {
            d3d9::gpuNoteSpriteResult(gpu_done, written - written_before, sprite_fallback_reason);
        }
        // Draw 时 AddRef 的纹理在这里对称 Release（与 D3DX 的批次生命周期一致）
        releaseEntry(ea, i);
        ++entries_done;
    }
    cpu.mem->wr32(state + kSpCount, 0u);
    cpu.mem->wr32(state + kSpComposedPixels, s.composed_pixels + static_cast<std::uint32_t>(written));
    if (entries_out) *entries_out = entries_done;
    if (pixels_written) *pixels_written = written;
    return true;
}

std::uint32_t flush(X86& cpu, std::uint32_t state, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (s.drawing == 0) {
        spriteLog("Flush", "不在 Begin/End 之间 ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3DX9：Flush 在 Begin 之外";
        return kInvalidCall;
    }
    std::uint32_t entries = 0;
    std::uint64_t pixels = 0;
    if (!compositeBatch(cpu, state, &entries, &pixels, why)) return kFail;
    cpu.mem->wr32(state + kSpFlushes, s.flushes + 1u);
    spriteLog("Flush", "合成 %u 条 Draw / %llu 个像素（批次保留，Begin 状态不变）", entries,
              static_cast<unsigned long long>(pixels));
    return kOk;
}

std::uint32_t end(X86& cpu, std::uint32_t state, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (s.drawing == 0 && s.count == 0) {
        // ★ 幂等：第二次 End（空批次 + 已关闭）⇒ S_OK，**不碰任何像素**。
        spriteLog("End", "空批次且不在 Begin 里 ⇒ 幂等 no-op（像素不变）");
        return kOk;
    }
    std::uint32_t entries = 0;
    std::uint64_t pixels = 0;
    if (!compositeBatch(cpu, state, &entries, &pixels, why)) return kFail;
    cpu.mem->wr32(state + kSpDrawing, 0u);
    cpu.mem->wr32(state + kSpEnds, s.ends + 1u);
    spriteLog("End", "合成 %u 条 Draw / %llu 个像素 ⇒ 批次关闭", entries,
              static_cast<unsigned long long>(pixels));
    return kOk;
}

// ---------------------------------------------------------------------------------------
// 变换 / 设备生命周期
// ---------------------------------------------------------------------------------------
std::uint32_t setTransform(X86& cpu, std::uint32_t state, std::uint32_t matrix_ea, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (matrix_ea == 0) {
        // 真机：SetTransform(NULL) ⇒ 恢复单位阵（D3DX 文档语义）
        writeIdentityMatrix(cpu, state + kSpMatrix);
        spriteLog("SetTransform", "pTransform=NULL ⇒ 恢复单位阵");
        return kOk;
    }
    if (!committed(cpu, matrix_ea, kMatrixBytes, "SetTransform 的 D3DXMATRIX", why)) return kInvalidCall;
    for (int i = 0; i < 16; ++i)
        wrFloat(cpu, state + kSpMatrix + static_cast<std::uint32_t>(i) * 4u, rdFloat(cpu, matrix_ea + static_cast<std::uint32_t>(i) * 4u));
    spriteLog("SetTransform", "矩阵 = [%g %g %g %g | %g %g %g %g | …]", static_cast<double>(rdFloat(cpu, matrix_ea + 0)),
              static_cast<double>(rdFloat(cpu, matrix_ea + 4)), static_cast<double>(rdFloat(cpu, matrix_ea + 8)),
              static_cast<double>(rdFloat(cpu, matrix_ea + 12)), static_cast<double>(rdFloat(cpu, matrix_ea + 16)),
              static_cast<double>(rdFloat(cpu, matrix_ea + 20)), static_cast<double>(rdFloat(cpu, matrix_ea + 24)),
              static_cast<double>(rdFloat(cpu, matrix_ea + 28)));
    return kOk;
}

std::uint32_t getTransform(X86& cpu, std::uint32_t state, std::uint32_t matrix_ea, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    if (!committed(cpu, matrix_ea, kMatrixBytes, "GetTransform 的 D3DXMATRIX", why)) return kInvalidCall;
    for (int i = 0; i < 16; ++i)
        wrFloat(cpu, matrix_ea + static_cast<std::uint32_t>(i) * 4u, s.matrix[i]);
    return kOk;
}

std::uint32_t onLostDevice(X86& cpu, std::uint32_t state, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    // headless：没有"设备丢失"这回事（后缓冲在 guest 内存里）⇒ 记录，丢弃未提交批次（与真机一致：
    // 设备丢失后旧批次不能再用，它 AddRef 的纹理也要还掉）。
    // ★ 但**更早的 End 已经入队**的 GPU draw 不属于这个未提交批次（CPU 路径下它们早画完了）⇒
    //   先 flush，别让"设备丢失"把已经提交过的画面一起丢掉。
    if (Machine* m = activeMachine(); m != nullptr && &m->cpu() == &cpu) {
        if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive()) {
            std::string flush_err;
            if (!gpu->rasterFlush(&flush_err)) {
                spriteLog("OnLostDevice", "GPU 队列 flush 失败：%s", flush_err.c_str());
            }
        }
    }
    releaseQueuedTextures(cpu, s);
    cpu.mem->wr32(state + kSpLost, s.lost + 1u);
    cpu.mem->wr32(state + kSpCount, 0u);
    spriteLog("OnLostDevice", "记录第 %u 次（headless 没有真设备丢失；未提交批次丢弃）", s.lost + 1u);
    return kOk;
}

std::uint32_t onResetDevice(X86& cpu, std::uint32_t state, std::string* why) {
    State s;
    if (!readState(cpu, state, &s, why)) return kInvalidCall;
    cpu.mem->wr32(state + kSpResets, s.resets + 1u);
    spriteLog("OnResetDevice", "记录第 %u 次（headless：没有要重建的设备资源）", s.resets + 1u);
    return kOk;
}

}  // namespace d3dx9
}  // namespace lift
