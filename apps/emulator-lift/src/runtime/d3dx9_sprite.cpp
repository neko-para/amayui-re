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
#include <string>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/heap.h"
#include "runtime/machine.h"
#include "runtime/math.h"
#include "runtime/memory.h"
#include "runtime/pixel_format.h"
#include "runtime/surface.h"
#include "runtime/x86.h"

namespace lift {
namespace d3dx9 {
namespace {

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
    std::fprintf(stderr, "[d3dx9] %s: ", what);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
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
void releaseQueuedTextures(X86& cpu, const State& s) {
    if (s.count == 0 || s.batch == 0 || !cpu.com) return;
    if (!cpu.mem || !cpu.mem->committed(s.batch, s.count * kBatchEntryBytes)) return;
    for (std::uint32_t i = 0; i < s.count; ++i) {
        const std::uint32_t ea = s.batch + i * kBatchEntryBytes;
        if ((cpu.mem->rd32(ea + kBeReserved) & 1u) == 0) continue;
        const std::uint32_t tex = cpu.mem->rd32(ea + kBeTexture);
        cpu.mem->wr32(ea + kBeReserved, 0u);
        if (tex != 0) cpu.com->release(cpu, tex);
    }
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
std::uint32_t createSpriteState(X86& cpu, std::uint32_t device_object, std::string* why) {
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
        if (why) *why = std::string("D3DX9：guest 堆分配 Sprite 状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    const std::uint32_t batch = cpu.heap->alloc(kInitialBatchCapacity * kBatchEntryBytes, /*zero=*/true);
    if (batch == 0) {
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
    std::uint64_t written = 0;
    std::uint32_t entries_done = 0;
    // 条目级收尾：Draw 时 AddRef 过的纹理必须**每条路径**都还回去（含 continue 的早退分支）
    auto releaseEntry = [&](std::uint32_t ea) {
        if (!cpu.com) return;
        if ((cpu.mem->rd32(ea + kBeReserved) & 1u) == 0) return;
        const std::uint32_t t = cpu.mem->rd32(ea + kBeTexture);
        cpu.mem->wr32(ea + kBeReserved, 0u);
        if (t != 0) cpu.com->release(cpu, t);
    };
    for (std::uint32_t i = 0; i < s.count; ++i) {
        const std::uint32_t ea = s.batch + i * kBatchEntryBytes;
        const std::uint32_t color = cpu.mem->rd32(ea + kBeColor);
        const std::uint32_t eflags = cpu.mem->rd32(ea + kBeFlags);
        if ((eflags & kBeHasSrcRect) == 0) {  // 排不进来的条目（防御）
            releaseEntry(ea);
            continue;
        }
        const std::int32_t left = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 0));
        const std::int32_t top = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 4));
        const std::int32_t right = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 8));
        const std::int32_t bottom = static_cast<std::int32_t>(cpu.mem->rd32(ea + kBeSrc + 12));
        const double w = static_cast<double>(right - left);
        const double h = static_cast<double>(bottom - top);
        if (w <= 0 || h <= 0) {  // 空矩形（Draw 已经记过；这里只是防御）
            releaseEntry(ea);
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
            releaseEntry(ea);
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
            // 完全在屏幕外：真机也不会画（不是错误）
            ++entries_done;
            releaseEntry(ea);
            continue;
        }
        // 调制色（D3D 的 MODULATE：源 × 漫反射色）
        const unsigned sa = (color >> 24) & 0xFFu;
        const unsigned sr = (color >> 16) & 0xFFu;
        const unsigned sg = (color >> 8) & 0xFFu;
        const unsigned sb = color & 0xFFu;
        // ---- 源：白色（pTexture=0）或真纹理 ----
        const std::uint32_t texture = cpu.mem->rd32(ea + kBeTexture);
        const std::uint8_t* tbase = nullptr;
        SurfaceFormat tfmt;
        std::uint32_t tw = 0;
        std::uint32_t th = 0;
        std::uint32_t tpitch = 0;
        std::uint32_t tbpp = 0;
        if (texture != 0) {
            d3d9::TextureInfo ti;
            if (!d3d9::textureInfo(cpu, texture, &ti, why)) {
                if (why) *why = std::string("D3DX9：合成时纹理无效（被提前 Release？）：") + *why;
                return false;
            }
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
                releaseEntry(ea);
                ++entries_done;
                continue;
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
                    tex_a = tfmt.amask != 0 ? pixel_format::expandChannel(raw, tfmt.amask) : 255u;
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
        // Draw 时 AddRef 的纹理在这里对称 Release（与 D3DX 的批次生命周期一致）
        releaseEntry(ea);
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
