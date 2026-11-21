// emulator-lift · D3DX9 Sprite 簇的状态 / 批次 / 合成（M2 · 第一帧）
//
// ★ 读 `runtime/d3dx9_sprite.h` 的文件头先：vtable 取证的槽表、语义决定、纹理的现状与 loud trap 口径。
// ★ 本文件**不认 COM**：vtable / refcount / 派发在 `runtime/com.h` 与 `shims/d3dx9.cpp`。
// ★ 一切"真的做"的部分都在 compositeBatch：
//     Draw 时把变换应用到四角（与 D3DX 的"顶点当场生成"一致），End/Flush 时把纹理四边形交给
//     **GPU 光栅后端**：白色纹理源（pTexture=0 时的 D3D9 固定功能语义）× 调制色 → **当前绑定的
//     颜色目标**（未绑 RT = 后缓冲；绑了纹理 RT = 那张纹理的像素块）；`Begin` 的 ALPHABLEND
//     只决定"开不开"混合，**因子 / op 一律来自设备渲染状态**（`D3DRS_SRCBLEND` / `DESTBLEND` /
//     `BLENDOP` 与 separate-alpha 三项；与 fixed-function 同一份映射）。⛔ 不从 flags 推因子。
//     ★ `Begin(ALPHABLEND)` 还补上真 D3DX **自己**做的那一步：保存并把
//     `ALPHABLENDENABLE` / `SRCBLEND` / `DESTBLEND` 设成 `1` / `SRCALPHA(5)` / `INVSRCALPHA(6)`，
//     `End` 恢复（`REQ-01M7R3YPQPNPBZ327H89Q8RTEE`）。没有这一步，普通批次读到的是引擎残留的
//     `ONE/ZERO` ⇒ 透明黑 texel 被不透明写进后缓冲（走廊 / 对话框底板变黑的回归）。
// ★ **光栅只有 GPU 一条实现**（`REQ-01M7AAXNJY2PACYJHYVRV620Q5`）：原来那份"CPU 逐像素写后缓冲"
//   的兜底已删 —— GPU 不可用 / 表达不了这一条 ⇒ **记账（`skipped`）+ 跳过**，⛔ 不静默画。
//   本文件**没有**任何写 `dev.arena` / 目标像素的代码；"写了多少像素"这个数只为状态块的
//   `composed_pixels` 服务（= 交给 GPU 的那条四边形的覆盖格数，⛔ 不是 CPU 数出来的）。
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
#include "runtime/host_services.h"
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
//
// ★ `Begin` 写的 stage 采样器（MSDN `ID3DXSprite::Begin` 的 Sampler States 表）：
//   MSDN 那张表**只有 stage 0 的行**（D3DXSprite 只把纹理绑在 stage 0）⇒ `kSpTexStages = 1`；
//   常量留着是为了让"保存块 / 循环"的尺寸只有一处来源（⛔ 不要在别处再写 8 项 / 32 B）。
constexpr std::uint32_t kSpTexStages = 1u;
constexpr std::uint32_t kSpSamplerStageBytes = 32u;  // 每 stage 8 项 × 4 B
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
    // ---- `REQ-01M7R3YPQPNPBZ327H89Q8RTEE`：`Begin(ALPHABLEND)` 像真 D3DX 那样
    //      **保存并改动** `ALPHABLENDENABLE` / `SRCBLEND` / `DESTBLEND`，`End` 恢复。
    //      ★ `saved_valid` = "本次 Begin 接管了那三项"（0 = 没接管：Begin(无 ALPHABLEND) /
    //      取不到设备 / 没有流水线状态块）；⛔ 它区分的是"接管了"与"这次没接管"，
    //      不是"值是否等于 5/6"。
    kSpSavedValid = 124,             // 1 = End 要恢复下面三项
    kSpSavedAlphaBlendEnable = 128,  // Begin 之前的 D3DRS_ALPHABLENDENABLE（0/1）
    kSpSavedSrcBlend = 132,          // Begin 之前的 D3DRS_SRCBLEND
    kSpSavedDstBlend = 136,          // Begin 之前的 D3DRS_DESTBLEND
    // ---- MSDN `ID3DXSprite::Begin` 的 **Sampler States** 表（stage 0）----
    //      `Begin` 自己写、`End` 恢复（`D3DXSPRITE_DONOTSAVESTATE` 时不保存 / 不恢复）。
    //      ★ 这组状态落在**设备的采样器表**里（`GetSamplerState` 读得到），本块只是"pre-Begin 的值"；
    //      ⛔ 不是 sprite 的影子采样状态 —— `Draw` 用的始终是 device 当前 stage 0。
    kSpSavedSamplerValid = 140,  // 1 = End 要把 kSpSavedSampler 整块写回设备
    kSpSavedSampler = 144,       // kSpTexStages × kSpSamplerStageBytes（每 stage 8 项 × 4 B）
    kSpBytes = kSpSavedSampler + kSpTexStages * kSpSamplerStageBytes,
};

// =====================================================================================
// `Begin` 写的 stage 采样器（MSDN `ID3DXSprite::Begin` 的 Sampler States 表）
//
// ★ 尺寸常量（`kSpTexStages` / `kSpSamplerStageBytes`）在 `SpriteOff` 之前 —— 状态块要用它们算大小。
// ★ caps ⇒ MSDN 的分支：本线声明的 `TextureFilterCaps` 是 0x03030303（`d3d9.cpp` 的 GetDeviceCaps）
//   ⇒ 有 MAGFLINEAR / MINFLINEAR / **MIPFLINEAR**，没有 MAGFANISOTROPIC / MINFANISOTROPIC；
//   `MaxAnisotropy = kCapsMaxAnisotropyClaim = 1` ⇒ 按 MSDN：
//     MAG/MIN = ANISOTROPIC（不支持）⇒ **LINEAR**；MIPFILTER = LINEAR（支持）⇒ **LINEAR**；
//     MAXANISOTROPY = `MaxAnisotropy` = 1。
//   ★ 下面的 `static_assert` 钉住"本线没有各向异性"这个前提：caps 若哪天真的支持了各向异性，
//     这里编译期就红，而不是静默继续写 LINEAR（`MaxAnisotropy == 1` 时 ANISOTROPIC 无意义）。
// =====================================================================================
// 每个 stage 保存 / 恢复的 8 项在块内的偏移（顺序与 `kSpriteBeginSamplerTypes` **逐项对应**）。
enum SpriteSamplerSaveOff : std::uint32_t {
    kSsAddressU = 0,
    kSsAddressV = 4,
    kSsMagFilter = 8,
    kSsMinFilter = 12,
    kSsMipFilter = 16,
    kSsMaxMipLevel = 20,
    kSsMipLodBias = 24,
    kSsMaxAnisotropy = 28,
};
static_assert(d3d9::kCapsMaxAnisotropyClaim == 1u,
              "MSDN ID3DXSprite::Begin 的 MAG/MIN=ANISOTROPIC 分支以 MaxAnisotropy>1 为前提；"
              "本线声明 MaxAnisotropy=1 ⇒ 必须走 LINEAR 分支（caps 变了就重新裁这一处）");
constexpr std::uint32_t kSpriteBeginMagMin = d3d9::kTexfLinear;
constexpr std::uint32_t kSpriteBeginMip = d3d9::kTexfLinear;
constexpr std::uint32_t kSpriteBeginMaxAnisotropy = d3d9::kCapsMaxAnisotropyClaim;
// MSDN 的 8 项（type）与它们要写成的值；顺序 = `SpriteSamplerSaveOff` 的顺序。
constexpr std::uint32_t kSpriteBeginSamplerTypes[8] = {
    d3d9::kSampAddressU,     d3d9::kSampAddressV,       d3d9::kSampMagFilter,    d3d9::kSampMinFilter,
    d3d9::kSampMipFilter,    d3d9::kSampMaxMipLevel,   d3d9::kSampMipLodBias,   d3d9::kSampMaxAnisotropy};
constexpr std::uint32_t kSpriteBeginSamplerValues[8] = {
    d3d9::kAddressClamp,     d3d9::kAddressClamp,       kSpriteBeginMagMin,      kSpriteBeginMagMin,
    kSpriteBeginMip,         0u /* MAXMIPLEVEL */,     0u /* MIPMAPLODBIAS = 0.0f 的位模式 */,
    kSpriteBeginMaxAnisotropy};
constexpr std::uint32_t kSpriteSamplerSaveOffsets[8] = {
    kSsAddressU,  kSsAddressV,  kSsMagFilter, kSsMinFilter,
    kSsMipFilter, kSsMaxMipLevel, kSsMipLodBias, kSsMaxAnisotropy};
static_assert(sizeof(kSpriteBeginSamplerTypes) / sizeof(kSpriteBeginSamplerTypes[0]) ==
                  sizeof(kSpriteSamplerSaveOffsets) / sizeof(kSpriteSamplerSaveOffsets[0]),
              "采样器 type 表与保存偏移表必须逐项对应");
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

bool requireMachine(X86& cpu, HostServices** out, std::string* why) {
    HostServices* m = hostOf(cpu);
    // ★ 归属判据（`ownsCpu`）：宿主非空**且**这台 cpu 就是它装配的那一份（见 host_services.h）。
    if (m == nullptr || !m->ownsCpu(cpu)) {
        if (why) *why = "D3DX9：这台 cpu 没有可用的宿主服务（hostOf(cpu) 为空，或不是该宿主装配的那份 X86）";
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
    std::uint32_t saved_valid = 0;  // 1 = 本批 Begin 改过 27/19/20，End 要恢复
    std::uint32_t saved_alpha_blend_enable = 0;
    std::uint32_t saved_src_blend = 0;
    std::uint32_t saved_dst_blend = 0;
    // 1 = 本批 Begin 接管了stage 采样器（End 要把 `saved_sampler` 整块写回设备）。
    std::uint32_t saved_sampler_valid = 0;
    // [stage][8 项]，项序 = `kSpriteSamplerSaveOffsets`。
    std::uint32_t saved_sampler[kSpTexStages][8] = {};
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
    s.saved_valid = cpu.mem->rd32(state + kSpSavedValid);
    s.saved_alpha_blend_enable = cpu.mem->rd32(state + kSpSavedAlphaBlendEnable);
    s.saved_src_blend = cpu.mem->rd32(state + kSpSavedSrcBlend);
    s.saved_dst_blend = cpu.mem->rd32(state + kSpSavedDstBlend);
    s.saved_sampler_valid = cpu.mem->rd32(state + kSpSavedSamplerValid);
    for (std::uint32_t st = 0; st < kSpTexStages; ++st) {
        for (std::uint32_t i = 0; i < 8u; ++i) {
            s.saved_sampler[st][i] =
                cpu.mem->rd32(state + kSpSavedSampler + st * kSpSamplerStageBytes + kSpriteSamplerSaveOffsets[i]);
        }
    }
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
// ★ **必须同时更新本地 `s`**（`BUG-32`）：`GuestHeap::realloc` 在 `need > 旧块` 时**总是**
//   alloc 新块 + 拷贝 + free 旧块（`heap.cpp`），所以容量 16→32 这一趟**必然移动**。
//   只把新 base / capacity 写回 guest 状态块、却让调用方继续用旧的 `s.batch`，第 count 条就会
//   写进**旧块末尾** —— 那是紧邻块（真实 run 里是相邻纹理对象，P-8 正好是它的块头）的
//   `'LIFT'` 块头 + 头 64 B，被覆盖成一条批次条目之后，guest 在 `End` 里读到 `[vtable]=0` 的
//   fault。⇒ guest 状态块与本地 `s` 是**同一次**更新的两个视图，⛔ 不许只更新一个。
bool reserveBatch(X86& cpu, State& s, std::uint32_t need, std::string* why) {
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
    // ★ 本地视图（调用方紧接着要用它算第 count 条的地址）：不给这一句，realloc 移动之后
    //   第 count 条就写进旧块末尾 —— `BUG-32` 的根因。
    s.batch = np;
    s.capacity = cap;
    return true;
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

// ---------------------------------------------------------------------------------------
// `Begin(ALPHABLEND)` 的保存/恢复（`REQ-01M7R3YPQPNPBZ327H89Q8RTEE`；真 D3DX 的语义）
// ---------------------------------------------------------------------------------------
// ★ 真 D3DX9：`Begin(D3DXSPRITE_ALPHABLEND)` **自己**把 `ALPHABLENDENABLE=1` /
//   `SRCBLEND=SRCALPHA(5)` / `DESTBLEND=INVSRCALPHA(6)` 写进设备，并在 `End` 把**它改过的**
//   三项恢复。引擎**不会**替 sprite 设 5/6（它只在需要加性的那一条之前补 5/2）⇒ 缺了这一步，
//   普通批次读到的就是设备里残留的 `ONE/ZERO`（不透明拷贝，透明黑 texel 写进后缓冲）。
// ★ ⛔ 这**不是**"从 flags 推因子"：因子仍然只从设备状态**读**（`compositeBatch` 的
//   `readBlendState` + `gpuBlendStateFor`）——`REQ-01M7R1ABAP86QVMNBZ5S4EDKD5` 的修法不变。
// ★ 恢复**只**看 `saved_valid`：Begin 没接管（无 ALPHABLEND / 取不到设备 / 没有流水线块）⇒ 0，
//   End 一项都不碰。`Flush` **不**恢复（它只提交批次，Begin 还开着）。
bool restoreBeginSavedState(X86& cpu, std::uint32_t state, const State& s) {
    if (s.saved_valid == 0) return true;
    // 先清标记：恢复失败也**不**重试（同一台设备上重试不会有不同结果；⛔ 不每次 End 重复告警）。
    cpu.mem->wr32(state + kSpSavedValid, 0u);
    std::string why;
    d3d9::DeviceInfo dev;
    if (!d3d9::deviceInfo(cpu, s.device, &dev, &why)) {
        spriteWarn("End", "ALPHABLEND 批次收尾：取不到设备（%s）⇒ ⛔ 恢复不了 27/19/20", why.c_str());
        return false;
    }
    // 三项互不依赖，顺序随意；值来源 = Begin 时**读到的原值**。
    const std::uint32_t ids[3] = {d3d9::kRsAlphaBlendEnable, d3d9::kRsSrcBlend, d3d9::kRsDestBlend};
    const std::uint32_t values[3] = {s.saved_alpha_blend_enable, s.saved_src_blend, s.saved_dst_blend};
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
        std::string write_why;
        if (!d3d9::writeRenderState(cpu, dev.state, ids[i], values[i], &write_why)) {
            spriteWarn("End", "ALPHABLEND 批次收尾：恢复 D3DRS %u=%u 失败（%s）", ids[i], values[i],
                       write_why.c_str());
            ok = false;
        }
    }
    if (ok) {
        spriteLog("End", "ALPHABLEND 批次收尾：设备混合状态恢复成 27=%u / 19=%u / 20=%u", values[0],
                  values[1], values[2]);
    }
    return ok;
}

// ---------------------------------------------------------------------------------------
// `Begin` 的 stage-0 采样器（MSDN `ID3DXSprite::Begin` 的 Sampler States 表）
// ---------------------------------------------------------------------------------------
// ★ MSDN 原文："This method will set the following states on the device." 的 **Sampler States**
//   表（只有 stage 0 的行）：ADDRESSU / ADDRESSV = CLAMP；MAG / MIN = ANISOTROPIC（caps 不支持则
//   LINEAR）；MIPFILTER = LINEAR（不支持则 POINT）；MAXMIPLEVEL = 0；MAXANISOTROPY = MaxAnisotropy；
//   MIPMAPLODBIAS = 0。`End` 恢复 pre-Begin（MSDN `ID3DXSprite::End`："restores the device state to
//   how it was before ID3DXSprite::Begin was called"），`Flush` 不恢复（"Device states remain as they
//   were after the last call to Begin"）；`D3DXSPRITE_DONOTSAVESTATE` = "not to be saved or restored"
//   ⇒ 写仍然发生，但 End 不恢复。
// ★ 写进**设备的采样器表**（`d3d9::writeSamplerState`），⛔ 不建 sprite 自己的影子采样状态：
//   `Draw` 用的始终是 device 当前 stage 0（`compositeBatch` 每批读一次），所以这些写必须让
//   guest 的 `GetSamplerState` 读得到、guest 在 Begin 之后的覆盖也必须被 `Draw` 看到。
// ★ 失败语义沿用 Begin(ALPHABLEND) 那一段：取不到设备 / 没有流水线状态块 ⇒ **不改 Begin 的返回码**，
//   只记默认级别可见的告警 + `saved_sampler_valid=0`（End 一项都不恢复，⛔ 不假装接管了）。
bool applyBeginSamplerState(X86& cpu, const State& s, bool save_before_end) {
    d3d9::DeviceInfo dev;
    std::string why_dev;
    if (!d3d9::deviceInfo(cpu, s.device, &dev, &why_dev)) {
        spriteWarn("Begin", "采样器：取不到设备状态（%s）⇒ ⛔ 不写 D3DX 文档状态", why_dev.c_str());
        return false;
    }
    if (dev.pipeline == 0) {
        spriteWarn("Begin", "采样器：设备没有流水线状态块（'DL01'）⇒ ⛔ 不写 D3DX 文档状态");
        return false;
    }
    // ① 先读出 pre-Begin 的整块（只有 save 时才落盘）。
    d3d9::SamplerStateSnapshot before[kSpTexStages];
    bool before_ok = true;
    for (std::uint32_t st = 0; st < kSpTexStages; ++st) {
        std::string read_why;
        if (!d3d9::readSamplerState(cpu, dev.state, st, &before[st], &read_why)) {
            spriteWarn("Begin", "采样器：读不到 stage %u 的原值（%s）⇒ ⛔ End 不恢复这一块", st,
                       read_why.c_str());
            before_ok = false;
        }
    }
    // ② 保存（除非 DONOTSAVESTATE）。★ 读失败不落盘：写一个猜的值回去比"不恢复"更糟。
    if (save_before_end && before_ok) {
        for (std::uint32_t st = 0; st < kSpTexStages; ++st) {
            const std::uint32_t vals[8] = {before[st].address_u,
                                           before[st].address_v,
                                           before[st].mag_filter,
                                           before[st].min_filter,
                                           before[st].mip_filter,
                                           before[st].max_mip_level,
                                           before[st].mip_lod_bias,
                                           before[st].max_anisotropy};
            for (std::uint32_t i = 0; i < 8u; ++i) {
                cpu.mem->wr32(s.state + kSpSavedSampler + st * kSpSamplerStageBytes +
                                  kSpriteSamplerSaveOffsets[i],
                              vals[i]);
            }
        }
    }
    cpu.mem->wr32(s.state + kSpSavedSamplerValid, (save_before_end && before_ok) ? 1u : 0u);
    // ③ 写 MSDN 的文档值（caps 分支已在文件头的常量段钉死，见 `kSpriteBeginSamplerValues`）。
    bool ok = true;
    for (std::uint32_t st = 0; st < kSpTexStages; ++st) {
        for (std::uint32_t i = 0; i < 8u; ++i) {
            std::string write_why;
            if (!d3d9::writeSamplerState(cpu, dev.state, st, kSpriteBeginSamplerTypes[i],
                                         kSpriteBeginSamplerValues[i], &write_why)) {
                spriteWarn("Begin", "采样器：写 stage %u type %u 失败（%s）", st,
                           kSpriteBeginSamplerTypes[i], write_why.c_str());
                ok = false;
            }
        }
    }
    if (ok) {
        spriteLog("Begin",
                  "stage 0 采样器按 D3DX 文档写：ADDRESSU/V=CLAMP · MAG/MIN=%u · MIP=%u · MAXMIPLEVEL=0 ·"
                  " LODBIAS=0 · MAXANISO=%u（End 恢复=%d；Draw 读 device 当前值）",
                  kSpriteBeginMagMin, kSpriteBeginMip, kSpriteBeginMaxAnisotropy,
                  (save_before_end && before_ok) ? 1 : 0);
    }
    return ok;
}

// `End` 把 Begin 保存的 stage 采样器整块写回设备（`Flush` 不调用它）。
bool restoreBeginSamplerState(X86& cpu, std::uint32_t state, const State& s) {
    if (s.saved_sampler_valid == 0) return true;
    // 先清标记：恢复失败也**不**重试（同一台设备上重试不会有不同结果；⛔ 不每次 End 重复告警）。
    cpu.mem->wr32(state + kSpSavedSamplerValid, 0u);
    std::string why;
    d3d9::DeviceInfo dev;
    if (!d3d9::deviceInfo(cpu, s.device, &dev, &why)) {
        spriteWarn("End", "采样器收尾：取不到设备（%s）⇒ ⛔ 恢复不了 stage 采样器", why.c_str());
        return false;
    }
    bool ok = true;
    for (std::uint32_t st = 0; st < kSpTexStages; ++st) {
        for (std::uint32_t i = 0; i < 8u; ++i) {
            std::string write_why;
            if (!d3d9::writeSamplerState(cpu, dev.state, st, kSpriteBeginSamplerTypes[i],
                                         s.saved_sampler[st][i], &write_why)) {
                spriteWarn("End", "采样器收尾：恢复 stage %u type %u 失败（%s）", st,
                           kSpriteBeginSamplerTypes[i], write_why.c_str());
                ok = false;
            }
        }
    }
    if (ok) spriteLog("End", "stage 0 采样器恢复成 Begin 之前的值");
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// 状态块
// ---------------------------------------------------------------------------------------
std::uint32_t createSpriteState(X86& cpu, std::uint32_t device_object, std::string* why,
                                bool* out_of_memory) {
    if (out_of_memory) *out_of_memory = false;
    HostServices* m = nullptr;
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
        flags & ~(kSpriteAlphaBlend | kSpriteDoNotSaveState);  // 除这两位外本线**不建模**
    cpu.mem->wr32(state + kSpFlags, flags);
    cpu.mem->wr32(state + kSpIgnoredFlags, ignored);
    cpu.mem->wr32(state + kSpDrawing, 1u);
    cpu.mem->wr32(state + kSpCount, 0u);  // 新批次从空开始（上一次的残留不跨 Begin）
    cpu.mem->wr32(state + kSpBegins, s.begins + 1u);
    // ---- 真 D3DX 的 Begin 副作用（`REQ-01M7R3YPQPNPBZ327H89Q8RTEE`）----
    // ★ 只补 D3DX 自己会做的那一步：带 `ALPHABLEND` 时**先保存**设备里的
    //   `ALPHABLENDENABLE(27)` / `SRCBLEND(19)` / `DESTBLEND(20)`，再**写**成
    //   `1` / `SRCALPHA(5)` / `INVSRCALPHA(6)`；`End` 恢复（`Flush` 不恢复）。
    //   ⛔ 因子仍然只从设备状态**读**（`compositeBatch` 的快照）—— 本步只决定"读到什么"，
    //   不是"跳过读取、从 flags 推因子"（那会把 BUG-33 修回去）。
    // ★ 边界（本轮**不动**）：`kSpriteDoNotModifyRenderState` 仍按既有口径"记录并忽略"
    //   （MSDN 只写"device **render state** is not to be changed"；sampler state 在 D3D9 里
    //   与 render state 是两个枚举族，表格也是分开的 ⇒ 不据此跳过采样器写入）。
    //   `kSpriteDoNotSaveState` **现在建模**：只影响"保存/恢复采样器"，不影响"写文档值"
    //   （MSDN："The device state is not to be saved or restored"）。⛔ 不顺手扩到
    //   `ALPHATESTENABLE` / `ZENABLE` / `LIGHTING` 等其它状态。
    // ★ 失败语义沿用既有边界：取不到设备 / 没有流水线状态块 ⇒ **不改 Begin 的返回码**，只记一条
    //   默认级别可见的告警，并让 `saved_valid=0`（End 一项都不恢复，⛔ 不假装接管了）。
    {
        bool took = false;
        if ((flags & kSpriteAlphaBlend) != 0) {
            d3d9::DeviceInfo dev;
            d3d9::BlendStateSnapshot rs;
            std::string why_dev;
            if (!d3d9::deviceInfo(cpu, s.device, &dev, &why_dev)) {
                spriteWarn("Begin", "ALPHABLEND：取不到设备状态（%s）⇒ ⛔ 不接管 27/19/20",
                           why_dev.c_str());
            } else if (dev.pipeline == 0) {
                spriteWarn("Begin", "ALPHABLEND：设备没有流水线状态块（'DL01'）⇒ ⛔ 不接管 27/19/20");
            } else if (!d3d9::readBlendState(cpu, dev.state, &rs, &why_dev)) {
                spriteWarn("Begin", "ALPHABLEND：读不到设备混合状态（%s）⇒ ⛔ 不接管 27/19/20",
                           why_dev.c_str());
            } else {
                // 先把"原值 + 已接管"落盘，再写三项：即使某一写失败，End 仍会按这份原值恢复。
                // ★ ALPHABLENDENABLE 是真布尔状态（`setRenderState` 只收 0/1）⇒ 存 0/1 即逐字原值。
                cpu.mem->wr32(state + kSpSavedAlphaBlendEnable, rs.blend ? 1u : 0u);
                cpu.mem->wr32(state + kSpSavedSrcBlend, rs.src_color);
                cpu.mem->wr32(state + kSpSavedDstBlend, rs.dst_color);
                cpu.mem->wr32(state + kSpSavedValid, 1u);
                took = true;
                const std::uint32_t ids[3] = {d3d9::kRsAlphaBlendEnable, d3d9::kRsSrcBlend,
                                              d3d9::kRsDestBlend};
                const std::uint32_t values[3] = {1u, d3d9::kBlendSrcAlpha, d3d9::kBlendInvSrcAlpha};
                for (int i = 0; i < 3; ++i) {
                    std::string write_why;
                    if (!d3d9::writeRenderState(cpu, dev.state, ids[i], values[i], &write_why)) {
                        spriteWarn("Begin", "ALPHABLEND：写 D3DRS %u=%u 失败（%s）⇒ End 仍会恢复原值",
                                   ids[i], values[i], write_why.c_str());
                    }
                }
                spriteLog("Begin",
                          "ALPHABLEND：按 D3DX 语义设 27=1 / 19=%u / 20=%u（原值 27=%u / 19=%u / 20=%u，"
                          "End 恢复）",
                          d3d9::kBlendSrcAlpha, d3d9::kBlendInvSrcAlpha, rs.blend ? 1u : 0u, rs.src_color,
                          rs.dst_color);
            }
        }
        if (!took) cpu.mem->wr32(state + kSpSavedValid, 0u);
    }
    // ---- 真 D3DX 的 Begin 副作用之②：stage 0 采样器（MSDN 的 Sampler States 表）----
    // ★ `D3DXSPRITE_DONOTSAVESTATE` ⇒ 不保存（End 也就不恢复），但**写仍然发生** ——
    //   MSDN 的 flag 原文是"not to be saved or restored"，不是"not to be set"。
    (void)applyBeginSamplerState(cpu, s, (flags & kSpriteDoNotSaveState) == 0);
    if (flags & ~kSpriteKnownFlags) {
        spriteLog("Begin", "Flags=0x%08X 含**未定义**位 0x%08X ⇒ 记录并忽略（不失败）", flags,
                  flags & ~kSpriteKnownFlags);
    }
    if (ignored != 0) {
        spriteLog("Begin",
                  "Flags=0x%08X：ALPHABLEND=%d / DONOTSAVESTATE=%d；其余位 0x%08X **记录并忽略**"
                  "（本线没有渲染状态保存/排序/3D 空间：OBJECTSPACE/BILLBOARD/SORT_*/"
                  "DONOTMODIFY_RENDERSTATE 都不建模）",
                  flags, (flags & kSpriteAlphaBlend) ? 1 : 0,
                  (flags & kSpriteDoNotSaveState) ? 1 : 0, ignored);
    } else {
        spriteLog("Begin", "Flags=0x%08X（ALPHABLEND=%d / DONOTSAVESTATE=%d）⇒ 批次打开", flags,
                  (flags & kSpriteAlphaBlend) ? 1 : 0, (flags & kSpriteDoNotSaveState) ? 1 : 0);
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
    // ★ 防御性判据（**只在越界时 warn，不改行为**）：用 guest 堆的实际载荷大小核对
    //   "第 count 条"是否真的落在**当前**批次数组里。上一个写穿（`BUG-32`）就是"realloc 之后
    //   用了旧 base" ⇒ 把"条目越界"变成一条默认级别可见的告警，而不是等相邻对象在别处炸。
    //   两种形状都报：① base 根本不是 guest 堆里的活块 —— `GuestHeap::realloc` 移动时**先 alloc
    //   新块、再 free 旧块**，所以拿旧 base 去查 `sizeOf` 时它已经是 0xFFFFFFFF（`BUG-32` 的
    //   现场正是这一种）；② base 是活块、但第 count 条超出它的载荷（旧块已被别的对象复用）。
    //   ⛔ 不 trap / 不拒绝排队：D3DX 的语义不看我们私有的批次数组布局，越界是**我们的**簿记错误，
    //   不该改变 guest 可见行为（否则就成了"用一个内部 bug 换一个 guest 可见错误"）。
    if (cpu.heap) {
        const std::uint32_t payload = cpu.heap->sizeOf(s.batch);
        const std::uint64_t need_bytes =
            static_cast<std::uint64_t>(s.count) * kBatchEntryBytes + kBatchEntryBytes;
        const bool base_gone = payload == 0xFFFFFFFFu;
        if (base_gone || need_bytes > payload) {
            spriteWarn("Draw",
                       "%s：state=0x%08X batch=0x%08X size=%u count=%u capacity=%u ea=0x%08X"
                       "（第 %u 条；⛔ 仍按 D3DX 语义排队，不 trap）",
                       base_gone ? "批次 base 不是 guest 堆里的活块（realloc 之后没更新本地 base？）"
                                 : "批次条目越界",
                       state, s.batch, payload, s.count, s.capacity, ea, s.count);
        }
    }
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

// ★ 唯一的混合判定（`REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`）：**只转发** fixed-function 那一份映射。
//   ⛔ 不要在这里写第二套 switch、⛔ 不要"没状态就用缺省 SrcAlpha/InvSrcAlpha" —— 那正是本单的缺陷。
bool spriteBlendState(const d3d9::BlendStateSnapshot& rs, bool alpha_blend_flag, GpuRasterState* out,
                      std::string* why) {
    return d3d9::gpuBlendStateFor(rs.src_color, rs.dst_color, rs.color_op, rs.separate_alpha, rs.src_alpha,
                                  rs.dst_alpha, rs.alpha_op, alpha_blend_flag, out, why);
}

bool compositeBatch(X86& cpu, std::uint32_t state, std::uint32_t* entries_out,
                    std::uint64_t* pixels_written, std::string* why) {
    HostServices* m = nullptr;
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
    // ★ 合成目标 = **当前绑定的颜色目标**（`REQ-01M79V2TVJH98BKH0C0WQV2FET`）：
    //   绑定纹理 RT 时，sprite 的像素必须写进那张 RT 的像素块，⛔ 不是设备的隐式后缓冲。
    //   判据与 D3D9 的 `prepareContext` **同一个函数**（`d3d9::colorTargetOfState`）——
    //   `ctx.color_base` / `color_is_texture` 就是它算出来的 ⇒ 不存在第二份判断逻辑。
    //   ⛔ 不要退回 `dev.arena`：save 的 RT 窗口里 77 次 End/Flush 的合成全进后缓冲，
    //   `SAVE00.STH` payload 因此全 0（实测）。
    d3d9::ColorTarget target;
    // ★ 没有流水线状态块（例如夹具里手工造的设备）⇒ 保持"目标 = 后缓冲"的旧口径，⛔ 不静默编一个目标。
    if (dev.pipeline != 0) {
        if (!d3d9::colorTargetOfState(cpu, dev.pipeline, &target, why)) {
            if (why) *why = std::string("D3DX9：合成时取不到设备的渲染目标：") + *why;
            return false;
        }
    }
    const std::uint32_t target_arena = target.arena != 0 ? target.arena : dev.arena;
    GuestSurface* gs = m->surfaces().find(target_arena);
    if (!gs) {
        if (why) *why = target.arena != 0 ? "D3DX9：绑定纹理 RT 的像素不在 arena 里"
                                          : "D3DX9：设备后缓冲像素不在 arena 里（设备状态与 arena 不一致）";
        return false;
    }
    std::uint8_t* base = m->surfaces().pixels(*gs);
    const std::uint32_t bpp = gs->format.bytesPerPixel();
    if (!base || bpp == 0) {
        if (why) *why = target.arena != 0 ? "D3DX9：绑定纹理 RT 的像素指针为空" : "D3DX9：后缓冲像素指针为空";
        return false;
    }
    const SurfaceFormat fmt = gs->format;
    // ---- 混合：每**批**读一次设备渲染状态（⛔ 不是每条 draw 读一遍）----
    // ★ `Begin` 的 flags 只决定"开不开"；因子 / op 一律来自设备状态，判据与 fixed-function
    //   **同一份**读法（`d3d9::readBlendState`）与**同一份**映射（`gpuBlendStateFor`）。
    //   ★ 带 `ALPHABLEND` 的 `Begin` 已经把 27/19/20 设成 `1/5/6`（真 D3DX 的语义）⇒ 引擎没改过时
    //   这里读到的就是 5/6；引擎在 Begin 之后写 5/2（加性）⇒ 这里读到 5/2。两条都对。
    //   `REQ-01M7R1ABAP86QVMNBZ5S4EDKD5` 之前这里只有一句 `gs_state.blend = blend;` ⇒ 落到
    //   `gpu_raster.h` 的缺省 SrcAlpha/InvSrcAlpha，引擎的加性（SRCALPHA/ONE）被丢掉。
    const bool alpha_blend_flag = (s.flags & kSpriteAlphaBlend) != 0;
    GpuRasterState blend_state;  // 混合段（其余 viewport/scissor/depth 由每条覆盖）
    std::string blend_error;
    bool blend_ok = false;
    if (dev.pipeline == 0) {
        // 没有流水线状态块 = 一个 D3DRS 都读不到。⛔ 不许拿缺省因子冒充"读过了"。
        blend_error = "设备没有流水线状态块（'DL01'）";
    } else {
        d3d9::BlendStateSnapshot rs;
        if (!d3d9::readBlendState(cpu, dev.state, &rs, &blend_error)) {
            // why 已由 readBlendState 填好
        } else {
            blend_ok = spriteBlendState(rs, alpha_blend_flag, &blend_state, &blend_error);
        }
    }
    if (!blend_ok) {
        // ★ **一条批次级**告警（⛔ 不每条 draw 刷屏；逐条只进覆盖账本）。
        spriteWarn("End",
                   "混合状态表达不了（%s）⇒ 本批 %u 条 Draw 全部跳过"
                   "（sprite 路没有 CPU 兜底；⛔ 不退回写死的 SrcAlpha/InvSrcAlpha）",
                   blend_error.empty() ? "原因未知" : blend_error.c_str(), s.count);
    }
    // ---- 采样器：每**批**读一次设备的 stage-0（`Draw` 用的就是 device 当前值）----
    // ★ MSDN `ID3DXSprite::Draw` 只把精灵排进批次、**不设任何 state**；`Flush` 提交时的采样器
    //   就是 device 当前的 stage 0 —— `Begin` 刚按文档写过（`applyBeginSamplerState`），guest
    //   可以在 Begin 之后用 `SetSamplerState` 覆盖（引擎每个 `DrawTexture` 正是这么做的：
    //   `0x4A30FB/0x4A310C/0x4A311D` 设 MIN/MAG=LINEAR、MIP=NONE）。
    // ⇒ 本线在这里**读 device**（`d3d9::readSamplerState` + `gpuSamplerFor`，与固定功能
    //   `gpuEligible` 同一份映射），⛔ 不再硬编码 Clamp/Point/Point/mip-on（那正是修前的缺陷：
    //   Point + texel 角 UV ⇒ 半 texel 吸附；mip-on ⇒ 缩小绘制采更粗一级）。
    GpuSampler sprite_sampler;
    std::string sampler_error;
    bool sampler_ok = false;
    if (dev.pipeline == 0) {
        // 没有流水线状态块 = 一个 D3DSAMP 都读不到。⛔ 不许拿缺省值冒充"读过了"。
        sampler_error = "设备没有流水线状态块（'DL01'）";
    } else {
        d3d9::SamplerStateSnapshot ss;
        if (!d3d9::readSamplerState(cpu, dev.state, 0u, &ss, &sampler_error)) {
            // why 已由 readSamplerState 填好
        } else {
            sampler_ok = d3d9::gpuSamplerFor(ss, &sprite_sampler, &sampler_error);
        }
    }
    if (!sampler_ok) {
        // ★ **一条批次级**告警（⛔ 不每条 draw 刷屏；逐条只进覆盖账本）。
        spriteWarn("End",
                   "stage-0 采样器表达不了（%s）⇒ 带纹理的 %u 条 Draw 会跳过"
                   "（无纹理条目只用 1×1 白纹理，不受采样器影响；sprite 路没有 CPU 兜底）",
                   sampler_error.empty() ? "原因未知" : sampler_error.c_str(), s.count);
    }
    // ★ 精灵合成与 D3D9 fixed-function 共用**同一个** GPU 光栅后端（那条 A/B 已删，
    //   见 `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）：没有"先关掉 GPU 再 CPU 合成"的开关。
    std::uint64_t written = 0;
    std::uint32_t entries_done = 0;
    // ★ GPU 覆盖统计只统计"GPU 已启用、但这条 sprite 回退 CPU"的情况。
    GpuRaster* gpu_backend = m->gpuRaster();
    const bool sprite_gpu_active = gpu_backend != nullptr && gpu_backend->rasterActive();
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
        // 四边形两条边向量 ⇒ 行列式 = 面积的两倍（只为下面那条"退化"判据服务；
        // ★ 不再有逐像素逆映射 —— CPU 合成已删，光栅在 GPU）。
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
        // 目标包围盒（四角 min/max；裁剪到渲染目标）
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
        // ---- 反馈环闸（sprite 路**自己的**一条；`REQ-01M79ZRTHWWD296XRY5KYPR9J4`）----
        // ★ 为什么 sprite 需要一个自己的闸：sprite 的合成**不经** `gpuEligible`（本文件直接调
        //   `GpuRaster::rasterQueue`）⇒ draw 路那条 `recordUnsupported(kUnRtFeedbackLoop)` 管不到
        //   这里。`REQ-01M79V2TVJH98BKH0C0WQV2FET`（sprite→RT）之后，sprite 可以把像素写进绑定的
        //   纹理 RT ⇒ "同一批 sprite 采样**自己正在写的**RT"（`tbase == base`）变成**新可达**。
        // ★ 判据 = 要采样的纹理像素基址 == 本批要写的颜色目标像素基址（`tbase == base`）：
        //   GPU 采样读的是**上传缓存**（本批像素还没进缓存）、guest 像素也可能还没读回
        //   ⇒ 两个来源都可能是**旧像素**，而且画面看不出来（静默错）。
        // ★ 语义选择（写死）：**跳过这一条 sprite + 具名位 43 + 一行 warn**，⛔ 不静默读旧像素、
        //   ⛔ 不把整帧扔掉。**为什么是"这一条"而不是"整批 / 整帧"**：
        //   ① 一批里各条目采的是各自的纹理，`tbase == base` 是**逐条目**的属性 ⇒ 只有构成环的
        //      那几条不能画，其余条目本来能画对，丢掉整批等于把正确的像素一起丢掉；
        //   ② 整帧丢弃会把"一帧里一条坏 sprite"放大成"整帧黑/空"，与
        //      `REQ-01M7A74YG08R65NJBFXF7DEJDK` 的"跳过而不是回退/整帧重来"口径冲突；
        //   ③ 三害（静默旧像素 / 丢整帧 / 少画一条）相权，取"少画一条 + 账上加一行"。
        // ★ 记账**不复用** draw 路的 bit28：复用会让"draw 路命中"与"sprite 路命中"分不开
        //   （见 `d3d9_pipeline.cpp` 的 `enum UnsupportedBit` 43 那一格）。
        if (texture != 0 && tbase != nullptr && tbase == base) {
            d3d9::noteSpriteRtFeedbackLoop(cpu, dev.pipeline, texture);
            spriteWarn("End",
                       "第 %u 条 Draw 采样的纹理 0x%08X 就是本批正在写入的渲染目标（像素同址）⇒ "
                       "跳过这一条 sprite（⛔ 不静默读旧像素）",
                       i, texture);
            // ★ 账本：这一条**没画**（与其它"表达不了"分支同一个入口：`sprite_skipped` +
            //   `skipped_reasons`）。⛔ 不 fallthrough 到后面的 GPU 分支。
            d3d9::gpuNoteSpriteNotDrawn("sprite: 采样纹理是正在渲染的 RT（反馈环）");
            releaseEntry(ea, i);
            ++entries_done;
            continue;
        }
        // 源矩形**完全**落在纹理外 ⇒ 这一条什么也不画（真机语义，不是错误）。
        // ★ 与"越界裁掉一截"分开：**部分**越界由下面的 GPU 守卫按"表达不了 ⇒ 跳过"处理
        //   （GPU 路径不做逐像素裁剪；见那条注释）。⛔ 这里不写像素。
        if (texture != 0) {
            if (right <= 0 || bottom <= 0 || left >= static_cast<std::int32_t>(tw) ||
                top >= static_cast<std::int32_t>(th)) {
                spriteLog("End", "第 %u 条 Draw 的源矩形完全在纹理外 ⇒ 跳过", i);
                if (sprite_gpu_active) d3d9::gpuNoteSpriteSkipped();
                releaseEntry(ea, i);
                ++entries_done;
                continue;
            }
        }
        // ---- 混合状态表达不了 ⇒ 这一条**没画** ----
        // ★ sprite 路没有 CPU 兜底（`REQ-01M7AAXNJY2PACYJHYVRV620Q5`）：读不到 / 映射不了混合状态时
        //   ⛔ 不静默退回写死的缺省因子，而是记账 + 跳过这一条（批次级告警上面已经打过一条）。
        if (!blend_ok) {
            d3d9::gpuNoteSpriteNotDrawn("sprite: 混合状态表达不了（" +
                                        (blend_error.empty() ? std::string("原因未知") : blend_error) + "）");
            releaseEntry(ea, i);
            ++entries_done;
            continue;
        }
        // ---- GPU 光栅化：D3DXSprite 纹理四边形（采样器 = device 当前 stage 0）----
        // ★ **这条就是唯一实现**（`REQ-01M7AAXNJY2PACYJHYVRV620Q5` 删掉了 CPU 逐像素兜底）：
        //   GPU 不可用 / 表达不了这一条 ⇒ `skip_reason` 记账 + 跳过，⛔ 不写目标像素。
        bool gpu_done = false;
        std::string skip_reason;
        const bool has_texture = texture != 0;
        GpuRaster* gpu = sprite_gpu_active ? gpu_backend : nullptr;
        if (gpu == nullptr) {
            skip_reason = "sprite: 没有已激活的 GPU 光栅后端";
        } else if (fmt.bpp != 32) {
            skip_reason = "sprite: 渲染目标不是 32bpp";
            gpu = nullptr;
        } else if (has_texture && tbpp != 4) {
            skip_reason = "sprite: 纹理不是 32bpp";
            gpu = nullptr;
        } else if (has_texture && !sampler_ok) {
            // ★ 无纹理条目只用 1×1 白纹理 ⇒ 采样器表达不了也不影响像素（不跳过）。
            skip_reason = "sprite: stage-0 采样器表达不了（" +
                          (sampler_error.empty() ? std::string("原因未知") : sampler_error) + "）";
            gpu = nullptr;
        } else if (has_texture && (left < 0 || top < 0 || right > static_cast<std::int32_t>(tw) ||
                                   bottom > static_cast<std::int32_t>(th))) {
            // ★ GPU 路径不做逐像素裁剪（UV 会采到纹理边界外）⇒ 这一条整个跳过并记账。
            //   实测（TITLE/ADV/SAVE，130/200/600 帧）这个分支命中 0 次；⛔ 不为了它留 CPU 兜底。
            skip_reason = "sprite: 源矩形部分越界（GPU 路径不做逐像素裁剪）";
            gpu = nullptr;
        }
        if (gpu != nullptr) {
            std::string gerr;
            // ★ 目标身份带 guest 渲染目标像素基址（同尺寸换 surface 必须能判出来）。
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
                // ★ 混合段直接来自**本批读一次**的设备状态（见函数头那段）；⛔ 不再从 flags 推一个 bool。
                GpuRasterState gs_state = blend_state;
                gs_state.viewport_w = gs->width;
                gs_state.viewport_h = gs->height;
                gs_state.scissor_x1 = gs->width;
                gs_state.scissor_y1 = gs->height;
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
                    // ★ 采样器 = 本批读一次的 device stage 0（不是常量、也不是 sprite 影子表）：
                    //   `Begin` 写过 → guest 可覆盖 → 这里读到的就是 `Draw` 真正该用的那一组。
                    gt.sampler = sprite_sampler;
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
                // GPU 失败 ⇒ 这一条**没画**（⛔ 不 CPU 重放）。队列里更早的 sprite 不受影响：
                // 它们按提交顺序留在后端队列里，Present / Clear / 换 target 时照旧 flush + 读回。
                spriteLog("End", "第 %u 条 Draw 的 GPU 纹理路径失败：%s ⇒ 跳过这一条", i, gerr.c_str());
                skip_reason = "sprite: GPU 后端失败：" + gerr;
            }
        }
        // ★ 账本：GPU 真的入队 ⇒ `sprite_gpu_draws`；否则这一条**没画** ⇒ `sprite_skipped`
        //   （原因进 `skipped top:`）。⛔ 没有"CPU 补画"这个第三态 —— 那正是本单删掉的东西。
        if (gpu_done) {
            d3d9::gpuNoteSpriteDrawn();
        } else {
            d3d9::gpuNoteSpriteNotDrawn(skip_reason.empty() ? std::string("sprite: 未知原因") : skip_reason);
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
    spriteLog("Flush", "合成 %u 条 Draw / %llu 个覆盖格（四边形包围盒；GPU ⛔ 不回报片元数；批次保留）",
              entries, static_cast<unsigned long long>(pixels));
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
    // ★ 真 D3DX 的语义：`End` 把 `Begin(ALPHABLEND)` **改过的** 27/19/20 恢复（`Flush` 不恢复）。
    //   `saved_valid=0`（Begin 没接管）⇒ 这里一项都不碰。恢复失败只告警，**不改** End 的返回码：
    //   批次已经合成完，把一次状态恢复失败升级成"整批失败"会丢掉已经画出来的像素。
    if (s.saved_valid != 0) (void)restoreBeginSavedState(cpu, state, s);
    // ★ 同一条语义的另一半（MSDN `ID3DXSprite::End`）：stage 0 采样器恢复到 pre-Begin。
    //   `DONOTSAVESTATE` 时 `saved_sampler_valid=0` ⇒ 一项都不碰（也不告警）。
    if (s.saved_sampler_valid != 0) (void)restoreBeginSamplerState(cpu, state, s);
    cpu.mem->wr32(state + kSpDrawing, 0u);
    cpu.mem->wr32(state + kSpEnds, s.ends + 1u);
    spriteLog("End", "合成 %u 条 Draw / %llu 个覆盖格（四边形包围盒）⇒ 批次关闭", entries,
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
    // ★ 归属判据（`ownsCpu`）：`GpuRaster` 无锁，feeder 的私有 `X86` 副本不能走到这里
    //   （它继承了 `host`，但 `&Machine::cpu() != &audio_cpu_`；复审 T2a P2-1）。
    if (HostServices* m = hostOf(cpu); m != nullptr && m->ownsCpu(cpu)) {
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
