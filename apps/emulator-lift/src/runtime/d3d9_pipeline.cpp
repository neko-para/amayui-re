// emulator-lift · D3D9 fixed-function 流水线（状态机 + 顶点处理 + 软件光栅）
//
// ★ 先读 `runtime/d3d9_pipeline.h` 的文件头：枚举值来源、vtable 偏移的取证口径、判据口径。
// ★ 本文件**不认 COM**：对象 / vtable / refcount 在 `runtime/com.h`，方法 shim 在 `shims/d3d9.cpp`。
//   ⛔ 但 Draw* 会去 `cpu.com` 核对绑定的是不是**本模块建的**纹理 / 顶点缓冲对象（不是就响亮失败）。
//
// ★ 状态放哪儿：'D9PL' 块在 **guest 堆**，EA 挂在设备状态块的 +56。为什么（与 `d3d9.cpp` 同一套理由）：
//   生命周期跟着设备 refcount 走、⛔ 不需要宿主侧静态表（Machine 反复建在同一栈地址时会串台）。
//   深度缓冲放在 `SurfaceArena`（guest 地址空间里的 32bpp 表面，像素里存 float 位模式）——
//   与后缓冲同一块 arena ⇒ 一起被 `destroyForDevice` 回收，不出现"设备没了深度还在"。
//
// ★ 哪些是**近似**（⛔ 不冒充真机；每处都在代码里点明）：
//   · 光照：Gouraud per-vertex；支持 DIRECTIONAL / POINT（SPOT 按 POINT+falloff 近似）；
//     法线只做 world/view 线性变换后归一化（真机用逆转置 ⇒ 非等比缩放下会偏）。
//   · 雾：**不实现**（FOGENABLE=1 时记录 + 一行日志，颜色不参与雾化）。
//   · 纹理过滤：MIN/MAG **都参与**决策（任一要求 LINEAR ⇒ 双线性；各向异性按 LINEAR 近似）。
//     本线**有**派生 mip 链（`REQ-01M73G2724479XAVS176WBZ65Q`）：按屏幕空间 UV 导数算 LOD，
//     取**最近级**（不做三线性）—— 但"1:1 / 放大 ⇒ level 0"这条必须成立（判据在守卫里）。
//     决策见 `samplerUsesLinear` + `mipLevelForLod`（⛔ 不是"参数传了却不用"）。
//   · 采样器 MIPFILTER：`NONE` ⇒ 只 level 0；非 NONE ⇒ 允许选 mip 级（LINEAR 三线性按**最近级**近似）。
//   · 采样器 MAXMIPLEVEL / LODBIAS：仍记录、不影响像素（本线不做 LOD 偏置/上限，⛔ 不静默改像素）。
//   · 多重采样 / 抗锯齿 / 线框 / patch / 顶点混合：不支持 ⇒ 记录 + 一行日志。
//   · 顶点声明（D3DVERTEXELEMENT9）：`DrawPrimitive` / `DrawIndexedPrimitive` 支持**一份**受支持形状
//     —— POSITION(FLOAT3) + TEXCOORD0(FLOAT2) [+ COLOR0(D3DCOLOR)]，元素可以落在**两条流**上
//     （引擎的 2D 图层就是 stream0=POSITION、stream1=TEXCOORD）。做法 = 按声明把两条流**去交错**
//     成一份临时缓冲（XYZ+UV[+diffuse]）再走既有 FVF 路；⛔ 其它元素（法线 / 混合权 / 第二套 UV /
//     非 32 位分量）与 `*UP` 变体仍**响亮失败**（`declLayoutOf` 给原因）。
//   · 纹理级运算：支持 DISABLE/SELECTARG1/2/MODULATE(2X/4X)/ADD/ADDSIGNED(2X)/SUBTRACT/
//     ADDSMOOTH/BLEND*ALPHA/MULTIPLYADD/LERP；其余 ⇒ 记录一次 + 退回 SELECTARG1。
//   · 光栅：像素中心采样 + 左上填充规则（顶点顺序 = 屏幕空间顺时针 = D3D 正面）。
//   · 点 / 线：**只做无纹理**（顶点色 + z 按段参数 t 插值），走与三角形**同一份**像素收尾
//     （`writeShadedPixel`：深度 / alpha test / 混合 / 写掩码）；线宽恒 1px（diamond-exit 主轴步进）。
//     带纹理或 COLOROP/ALPHAOP 需要采样的点 / 线**响亮拒绝**（见 `admitLinePointDraw`）。
#include "runtime/d3d9_pipeline.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/gpu_raster.h"
#include <deque>
#include <map>
#include <new>  // std::bad_alloc（状态块快照的分配失败面）
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/heap.h"
#include "runtime/machine.h"
#include "runtime/math.h"
#include "runtime/memory.h"
#include "runtime/pixel_format.h"
#include "runtime/surface.h"
#include "runtime/log.h"
#include "runtime/x86.h"

namespace lift {
// =====================================================================================
// GPU 覆盖账本的**批号表**（`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`；"为什么"见 `gpu_raster.h`）
// =====================================================================================
// ★ 放在 `lift` 而不是 `lift::d3d9` 里：回报入口是 `lift::gpuLedgerNoteRecordedDraw`
//   （声明在 `gpu_raster.h`，后端 `d3d9_gpu.cpp` 调），而写账的 `GpuCoverage` 在 `lift::d3d9` ——
//   两边都要看得见，所以表放外层、计数留原处。
// ★ 每个 tag 两个布尔（为什么不是"一个 recorded"）：`LIFT_GPU_VERIFY` 的旧接口会在
//   `++empty` **之前**就 flush 并回报 ⇒ 回报时还不知道这笔"该不该从 empty 里减"。
//   · `recorded`      = 后端回报过"这条 draw 真的进了 render pass"；
//   · `counted_empty` = 这批已经被记进 `GpuCoverage::empty`（⇒ 回报时要把它挪出去）。
namespace {
struct GpuBatchLedgerEntry {
    bool recorded = false;
    bool counted_empty = false;
};
std::map<std::uint64_t, GpuBatchLedgerEntry> g_gpu_batch_ledger;
// ★ 进程内只增、跨轮不复用：`gpuCoverageReset` 清表，但序号继续往前 ⇒ 上一轮迟到的回报
//   撞不上新一轮的 slot（否则"上一轮的批"会被记成"这一轮已 recorded"）。
std::uint64_t g_gpu_batch_seq = 0;
// `[draw]` 的**延后打印**（只在 `LIFT_SLOW_DRAW=1` 时非空）：入队那一刻还不可能知道是否 recorded，
// 所以先把行挂进**提交顺序**队列，由后端回报点按序放出 `rec=1`（见 `drainPendingDrawLines`）。
// ★ `tag == 0` = 这一批**没入队**（CPU 路 / 回退 / 顶点全被裁掉）⇒ 永远不会有回报，当场定为 rec=0。
struct GpuPendingDrawLine {
    std::uint64_t tag = 0;
    std::string text;
};
std::deque<GpuPendingDrawLine> g_pending_draw_lines;

// 把**队首**已经能定论的行按提交顺序打出去：
//   · `tag == 0` ⇒ rec=0（没入队，永远不会有记录）；
//   · 队首那批已经 recorded ⇒ rec=1；
//   · 队首那批**已入队但还没回报** ⇒ 停（否则日志顺序会与提交顺序不一致）。
// ★ `force = true` 只在一轮收口时用：剩下的永远不会再被回报 ⇒ 一律 rec=0 打出来。
void drainPendingDrawLines(bool force) {
    bool printed = false;
    while (!g_pending_draw_lines.empty()) {
        const GpuPendingDrawLine& front = g_pending_draw_lines.front();
        const auto it = front.tag != 0 ? g_gpu_batch_ledger.find(front.tag) : g_gpu_batch_ledger.end();
        const bool recorded = it != g_gpu_batch_ledger.end() && it->second.recorded;
        if (!recorded && front.tag != 0 && !force) break;
        std::fprintf(stderr, "[draw] rec=%d %s\n", recorded ? 1 : 0, front.text.c_str());
        g_pending_draw_lines.pop_front();
        printed = true;
    }
    if (printed) std::fflush(stderr);
}
}  // namespace

namespace d3d9 {
std::uint64_t g_gpu_draws = 0;
std::uint64_t g_gpu_triangles = 0;
namespace {
// GPU 覆盖账本（定义放最前：submitPrimitives / d3dx9_sprite 都写它；dumpRunSummary 读它）。
GpuCoverage g_gpu_coverage;

// ★ 本单（`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`）：一批被记进 `empty` 的**唯一入口**。
//   两个调用点（"入队成功但还没回报" / "连队都没入"）共用 ⇒ 要验"empty 恒 0 会被抓住"只需改这一行。
void noteCoverageEmpty() { ++g_gpu_coverage.empty; }

void noteFallbackReason(const std::string& reason) {
    std::uint64_t& n = g_gpu_coverage.fallback_reasons[reason];
    ++n;
    if (n == 1) {
        // ★ 默认（不设环境变量）也打印：第一次出现每种回退原因就明说，⛔ 不"悄悄 fallback"。
        std::fprintf(stderr, "[gpu] fallback: %s（该原因只打印一次）\n", reason.c_str());
        std::fflush(stderr);
    }
}

// =====================================================================================
// 状态块布局（**我们自己的**；⛔ 不是 Win32 结构体）
// =====================================================================================
constexpr std::uint32_t kPlMagic = 0;
constexpr std::uint32_t kPlDevice = 4;    // 设备状态块 EA
constexpr std::uint32_t kPlInScene = 8;
constexpr std::uint32_t kPlFvf = 12;
constexpr std::uint32_t kPlVertexDecl = 16;
constexpr std::uint32_t kPlViewport = 20;   // D3DVIEWPORT9：X/Y/W/H/MinZ/MaxZ（6 dword）
constexpr std::uint32_t kPlScissor = 44;    // RECT：left/top/right/bottom（4 dword）
constexpr std::uint32_t kPlTransforms = 64;  // 260 个 D3DMATRIX（64 B 一个）
constexpr std::uint32_t kPlTransformCount = 260;
constexpr std::uint32_t kPlRenderStates = kPlTransforms + kPlTransformCount * 64u;  // 256 个 DWORD
constexpr std::uint32_t kPlRenderStateCount = 256;
// D3DRENDERSTATETYPE 的**点尺寸族**（只在点光栅里读；状态数组本身覆盖 0..255，⛔ 不新增槽位）：
//   POINTSIZE=154 / POINTSPRITEENABLE=156 / POINTSCALEENABLE=157（值来自 SDK d3d9types.h）。
// ★ 本线**不实现**点尺寸：只读它们判"这一批的点不是 1px ⇒ 记账"，⛔ 不猜缩放语义。
constexpr std::uint32_t kRsPointSize = 154;
constexpr std::uint32_t kRsPointSpriteEnable = 156;
constexpr std::uint32_t kRsPointScaleEnable = 157;
constexpr std::uint32_t kPlSamplers = kPlRenderStates + kPlRenderStateCount * 4u;   // 16 * 16
constexpr std::uint32_t kPlSamplerCount = kMaxSamplers * 16u;
constexpr std::uint32_t kPlTexStages = kPlSamplers + kPlSamplerCount * 4u;          // 8 * 40
constexpr std::uint32_t kPlTexStageTypes = 40;
constexpr std::uint32_t kPlTextures = kPlTexStages + kMaxTextureStages * kPlTexStageTypes * 4u;  // 16 槽
// ★ 流槽 = **(顶点缓冲对象 EA, offset, stride, 绑定时代际)**，⛔ 不是状态块 EA。
//   为什么必须存对象 EA + 代际（缺陷单 `REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 #4）：guest 堆会
//   **复用 EA** —— 同长度的下一个顶点缓冲会拿到同一个对象 EA **与**同一个 `'VB09'` 状态块 EA
//   ⇒ 只验状态块 magic 的旧口径在那一格**通过**，Draw 静默改用别人的顶点。
constexpr std::uint32_t kPlStreams = kPlTextures + kMaxSamplers * 4u;  // 16 流 × (vb, offset, stride, gen)
constexpr std::uint32_t kPlIndices = kPlStreams + kMaxStreams * 16u;
constexpr std::uint32_t kPlIndicesGen = kPlIndices + 4u;  // 索引缓冲绑定时的代际（配对 `kPlIndices`）
constexpr std::uint32_t kPlMaterial = kPlIndicesGen + 4u;    // D3DMATERIAL9 = 68 B
constexpr std::uint32_t kPlLights = kPlMaterial + 68u;    // 8 × D3DLIGHT9 = 104 B
constexpr std::uint32_t kPlLightEnabled = kPlLights + kMaxLights * 104u;
constexpr std::uint32_t kPlDraws = kPlLightEnabled + kMaxLights * 4u;
constexpr std::uint32_t kPlDrawCalls = kPlDraws + 4u;
constexpr std::uint32_t kPlTriangles = kPlDrawCalls + 4u;
constexpr std::uint32_t kPlRasterized = kPlTriangles + 4u;
constexpr std::uint32_t kPlPixels = kPlRasterized + 8u;   // uint64
constexpr std::uint32_t kPlBegins = kPlPixels + 8u;
constexpr std::uint32_t kPlEnds = kPlBegins + 4u;
constexpr std::uint32_t kPlDepthArena = kPlEnds + 4u;
constexpr std::uint32_t kPlColorArena = kPlDepthArena + 4u;    // 0 = 设备后缓冲
constexpr std::uint32_t kPlColorSurface = kPlColorArena + 4u;  // IDirect3DSurface9 对象 EA（0 = 后缓冲）
constexpr std::uint32_t kPlStageSnapshots = kPlColorSurface + 4u;  // 纹理级/采样器状态快照次数（诊断）
constexpr std::uint32_t kPlUnsupported = kPlStageSnapshots + 4u;  // 未支持状态的位图（诊断）
// ★ 顶点声明绑定时的代际（配对 `kPlVertexDecl`；追加在末尾 ⇒ 不挪任何既有偏移）。
//   用途：`SetVertexDeclaration` / `SetFVF` 换绑 / 设备销毁时对**旧声明**做 `releaseIf(EA, 代际)`，
//   代际不符（EA 已被复用）⇒ 只出声、⛔ 不减到当前占有者身上。
constexpr std::uint32_t kPlVertexDeclGen = kPlUnsupported + 32u;
// ★ 纹理绑定时的代际（与 `kPlTextures` 的 16 个槽**一一对应**；追加在末尾 ⇒ 不挪任何既有偏移）。
//   与 `kPlVertexDeclGen` 同口径：`SetTexture` 把对象代际记下来，`GetTexture` / 设备销毁 /
//   换绑时用它判"槽里这个 EA 还是不是那一刻那张纹理"。为什么必须有：guest 堆复用 EA ⇒
//   "同 EA 的另一张纹理"只按 iface 判是**通过**的，会把**新纹理** +1 交回 guest。
constexpr std::uint32_t kPlTextureGens = kPlVertexDeclGen + 4u;  // 16 槽
constexpr std::uint32_t kPlBytes = kPlTextureGens + kMaxSamplers * 4u;
constexpr std::uint32_t kPlMagicValue = 0x31304C44u;  // 'DL01'（写成可读：'D','L','0','1'）

// 顶点缓冲 / 索引缓冲状态块（都在 guest 堆；数据紧跟头部）
constexpr std::uint32_t kVbMagic = 0;
constexpr std::uint32_t kVbDevice = 4;
constexpr std::uint32_t kVbLength = 8;
constexpr std::uint32_t kVbUsage = 12;
constexpr std::uint32_t kVbFvf = 16;
constexpr std::uint32_t kVbPool = 20;
constexpr std::uint32_t kVbLocked = 24;
constexpr std::uint32_t kVbHeader = 28;
constexpr std::uint32_t kVbMagicValue = 0x30394256u;  // 'VB09'
constexpr std::uint32_t kIbMagic = 0;
constexpr std::uint32_t kIbDevice = 4;
constexpr std::uint32_t kIbLength = 8;
constexpr std::uint32_t kIbUsage = 12;
constexpr std::uint32_t kIbFormat = 16;
constexpr std::uint32_t kIbPool = 20;
constexpr std::uint32_t kIbLocked = 24;
constexpr std::uint32_t kIbHeader = 28;
constexpr std::uint32_t kIbMagicValue = 0x30394249u;  // 'IB09'
constexpr std::uint32_t kMaxBufferBytes = 64u * 1024u * 1024u;

void pipeLog(const char* what, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3d9] %s: ", what ? what : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3d9, prefix, fmt, ap);
    va_end(ap);
}

// ★ **默认级别可见**的告警（`warn` 是默认日志级别；`LogLevel::Info` 默认看不见 ⇒ ⛔ 不能拿
//   `pipeLog` 报"身份不符"这类"本会静默采错数据"的事件）。
// ★ 限流：同一 **(调用点, 类别)** 在游戏路径里可能**每帧**都命中（陈旧绑定 + 每帧 Draw）⇒
//   前 `kPipeWarnBudget` 次打全量（Warn），之后只打一行汇总 + 每次留 Debug（`--log-level debug`
//   仍能看见每一次）。⛔ 不是"静默"：默认级别下第一次就一定看得见，且次数在汇总行里给出。
// ★ 为什么 key 不能只有调用点名（`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #2）：同一个 `DrawPrimitive`
//   会同时报"顶点流陈旧"和"纹理陈旧" —— 共用一个桶时，前者每帧刷屏会把后者的配额吃光，
//   于是纹理那条**默认级别看不见**。类别见 `PipeWarnCategory`（顶点流 / 索引 / 纹理 / 其它）。
constexpr std::uint32_t kPipeWarnBudget = 4u;

const char* pipeWarnCategoryKey(PipeWarnCategory cat) {
    switch (cat) {
        case PipeWarnCategory::VertexStream: return "vertex-stream";
        case PipeWarnCategory::Index: return "index";
        case PipeWarnCategory::Texture: return "texture";
        case PipeWarnCategory::Other: return "other";
    }
    return "other";
}

// 每类拒绝次数（与限流桶同一份：进程级；`dumpRunSummary` 的 `pipe-warn` 行读它）。
PipeWarnCounts g_pipe_warn_counts;

void pipeWarn(PipeWarnCategory cat, const char* what, const char* fmt, ...) {
    const char* key_call = what ? what : "?";
    char msg[512];
    msg[0] = '\0';
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // ★ 配额 key = (调用点, 类别)：类别是桶的一部分，⛔ 不共用。
    const std::string key = std::string(key_call) + "#" + pipeWarnCategoryKey(cat);
    static std::map<std::string, std::uint32_t>* counts = new std::map<std::string, std::uint32_t>();
    const std::uint32_t n = ++(*counts)[key];
    ++g_pipe_warn_counts.by_category[static_cast<unsigned>(cat)];
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3d9] %s: ", key_call);
    if (n <= kPipeWarnBudget) {
        logAt(LogLevel::Warn, LogD3d9, prefix, "%s", msg);
        return;
    }
    if (n == kPipeWarnBudget + 1u) {
        logAt(LogLevel::Warn, LogD3d9, prefix,
              "同类告警已打满 %u 次 ⇒ 之后只计数（每一次仍以 debug 级别留一条）", kPipeWarnBudget);
    }
    logAt(LogLevel::Debug, LogD3d9, prefix, "%s", msg);
}

bool requireMachine(X86& cpu, Machine** out, std::string* why) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) {
        if (why) *why = "D3D9 流水线：activeMachine() 不是这台 cpu 的 Machine（装配问题）";
        return false;
    }
    if (!m->surfaces().ready()) {
        if (why) *why = "D3D9 流水线：SurfaceArena 未初始化";
        return false;
    }
    *out = m;
    return true;
}

bool committed(X86& cpu, std::uint32_t ea, std::size_t n, const char* what, std::string* why) {
    if (ea == 0) {
        if (why) *why = std::string("D3D9 流水线：") + what + " 为 NULL";
        return false;
    }
    if (!cpu.mem->committed(ea, n)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "D3D9 流水线：%s = 0x%08X 起 %u 字节未提交", what, ea,
                      static_cast<unsigned>(n));
        if (why) *why = buf;
        return false;
    }
    return true;
}

void wrU32(X86& cpu, std::uint32_t ea, std::uint32_t v) { cpu.mem->wr32(ea, v); }

float rdF32(X86& cpu, std::uint32_t ea) {
    const std::uint32_t bits = cpu.mem->rd32(ea);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
void wrF32(X86& cpu, std::uint32_t ea, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    cpu.mem->wr32(ea, bits);
}

bool finite(double v) { return v == v && v > -1.0e30 && v < 1.0e30; }

// 单位阵：走 runtime/math 的唯一适配点（与 d3dx9_sprite 的 writeIdentityMatrix 同一份；
// ⛔ 这里不再手写 `i % 5` 的 D3DX 布局）。
void writeIdentity(X86& cpu, std::uint32_t ea) {
    float d3d[16];
    math::toD3DLayout(math::identity<float>(), d3d);
    for (int i = 0; i < 16; ++i) wrF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u, d3d[i]);
}

// ---- 状态块读取（内部；magic 校验后直接用）----
std::uint32_t pipeEaOf(X86& cpu, std::uint32_t dev_state) {
    return cpu.mem->rd32(dev_state + 56u);  // 与 d3d9.cpp 的 kDvPipeline 同一格
}

std::uint32_t plGet(X86& cpu, std::uint32_t pe, std::uint32_t off) { return cpu.mem->rd32(pe + off); }
void plSet(X86& cpu, std::uint32_t pe, std::uint32_t off, std::uint32_t v) { cpu.mem->wr32(pe + off, v); }

// 换绑 / 解绑时把**旧绑定**的那份引用还回去；带代际 ⇒ EA 已被复用就只出声、⛔ 不减别人。
// （定义放前面：`setFVF` / `setVertexDeclaration` 早于流绑定那一节用到它。）
// `cat` 决定配额桶的**类别**（同一个调用点上的"顶点流 / 索引 / 纹理"不共用可见配额）。
void releaseBound(X86& cpu, std::uint32_t obj, std::uint32_t gen, const char* what, PipeWarnCategory cat) {
    if (obj == 0 || !cpu.com) return;
    if (!cpu.com->releaseIf(cpu, obj, gen)) {
        pipeWarn(cat, "绑定释放",
                 "%s：旧绑定 0x%08X（绑定时代际 %u）已不是那个对象 ⇒ ⛔ 不释放它（新占有者不受影响）", what, obj,
                 gen);
    }
}

bool pipelineOf(X86& cpu, std::uint32_t dev_state, std::uint32_t* pe, std::string* why) {
    if (dev_state == 0) {
        if (why) *why = "D3D9 流水线：设备状态块 EA 为 0";
        return false;
    }
    const std::uint32_t pe_ea = pipeEaOf(cpu, dev_state);
    if (pe_ea == 0 || !cpu.mem->committed(pe_ea, kPlBytes)) {
        if (why) *why = "D3D9 流水线：设备没有流水线状态块（createForDevice 没跑？）";
        return false;
    }
    if (cpu.mem->rd32(pe_ea + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：流水线状态块的 magic 不对（块被写坏 / 野指针）";
        return false;
    }
    *pe = pe_ea;
    return true;
}

std::uint32_t rsOf(X86& cpu, std::uint32_t pe, std::uint32_t state) {
    return plGet(cpu, pe, kPlRenderStates + state * 4u);
}

void setRs(X86& cpu, std::uint32_t pe, std::uint32_t state, std::uint32_t value) {
    plSet(cpu, pe, kPlRenderStates + state * 4u, value);
}

std::uint32_t samplerOf(X86& cpu, std::uint32_t pe, std::uint32_t stage, std::uint32_t type) {
    return plGet(cpu, pe, kPlSamplers + (stage * 16u + type) * 4u);
}

std::uint32_t texStageOf(X86& cpu, std::uint32_t pe, std::uint32_t stage, std::uint32_t type) {
    return plGet(cpu, pe, kPlTexStages + (stage * kPlTexStageTypes + type) * 4u);
}

bool validTransformState(std::uint32_t state) {
    if (state == kTsView || state == kTsProjection) return true;
    if (state >= kTsTexture0 && state < kTsTexture0 + 8u) return true;
    if (state >= kTsWorld && state < kTsWorld + 4u) return true;
    return false;
}

// 未支持状态：记录位 + 只打一次日志（⛔ 不静默；也⛔ 不反复刷屏）
void recordUnsupported(X86& cpu, std::uint32_t pe, std::uint32_t bit, const char* what, std::uint32_t value) {
    const std::uint32_t word = bit / 32u;
    const std::uint32_t mask = 1u << (bit % 32u);
    const std::uint32_t off = kPlUnsupported + word * 4u;
    const std::uint32_t cur = plGet(cpu, pe, off);
    if ((cur & mask) != 0) return;
    plSet(cpu, pe, off, cur | mask);
    pipeLog("unsupported", "%s（值 0x%X）——本线不实现，已记录（⛔ 不冒充真机）", what, value);
}

// =====================================================================================
// 状态默认值（SDK 的默认状态；每条都在这里写死，⛔ 不留未初始化）
// =====================================================================================
void initDefaults(X86& cpu, std::uint32_t pe, int width, int height) {
    for (std::uint32_t i = 0; i < kPlBytes; ++i) cpu.mem->wr8(pe + i, 0);
    wrU32(cpu, pe + kPlMagic, kPlMagicValue);
    // 变换：单位阵（VIEW / PROJECTION / TEXTURE0..7 / WORLD..WORLD3）
    for (std::uint32_t i = 0; i < kPlTransformCount; ++i)
        writeIdentity(cpu, pe + kPlTransforms + i * 64u);
    // 渲染状态默认值（d3d9.h 的默认状态机）
    setRs(cpu, pe, kRsZEnable, 1u);
    setRs(cpu, pe, kRsFillMode, 3u);  // D3DFILL_SOLID
    setRs(cpu, pe, kRsShadeMode, kShadeGouraud);
    setRs(cpu, pe, kRsZWriteEnable, 1u);
    setRs(cpu, pe, kRsAlphaTestEnable, 0u);
    setRs(cpu, pe, kRsLastPixel, 1u);
    setRs(cpu, pe, kRsSrcBlend, kBlendOne);
    setRs(cpu, pe, kRsDestBlend, kBlendZero);
    setRs(cpu, pe, kRsCullMode, kCullCcw);
    setRs(cpu, pe, kRsZFunc, kCmpLessEqual);
    setRs(cpu, pe, kRsAlphaRef, 0u);
    setRs(cpu, pe, kRsAlphaFunc, kCmpAlways);
    setRs(cpu, pe, kRsDitherEnable, 0u);
    setRs(cpu, pe, kRsAlphaBlendEnable, 0u);
    setRs(cpu, pe, kRsFogEnable, 0u);
    setRs(cpu, pe, kRsSpecularEnable, 0u);
    setRs(cpu, pe, kRsFogColor, 0u);
    setRs(cpu, pe, kRsStencilEnable, 0u);
    setRs(cpu, pe, kRsTextureFactor, 0xFFFFFFFFu);
    setRs(cpu, pe, kRsClipping, 1u);
    setRs(cpu, pe, kRsLighting, 1u);
    setRs(cpu, pe, kRsAmbient, 0u);
    setRs(cpu, pe, kRsColorVertex, 1u);
    setRs(cpu, pe, kRsLocalViewer, 0u);
    setRs(cpu, pe, kRsNormalizeNormals, 0u);
    setRs(cpu, pe, kRsDiffuseMaterialSource, 0u);   // D3DMCS_MATERIAL
    setRs(cpu, pe, kRsSpecularMaterialSource, 0u);
    setRs(cpu, pe, kRsAmbientMaterialSource, 0u);
    setRs(cpu, pe, kRsEmissiveMaterialSource, 0u);
    setRs(cpu, pe, kRsVertexBlend, 0u);
    setRs(cpu, pe, kRsClipPlaneEnable, 0u);
    setRs(cpu, pe, kRsMultiSampleAntiAlias, 0u);
    setRs(cpu, pe, kRsMultiSampleMask, 0xFFFFFFFFu);
    setRs(cpu, pe, kRsIndexedVertexBlendEnable, 0u);
    setRs(cpu, pe, kRsColorWriteEnable, 0xFu);
    setRs(cpu, pe, kRsTweenFactor, 0u);
    setRs(cpu, pe, kRsBlendOp, kBlendOpAdd);
    setRs(cpu, pe, kRsScissorTestEnable, 0u);
    setRs(cpu, pe, kRsSeparateAlphaBlendEnable, 0u);
    setRs(cpu, pe, kRsSrcBlendAlpha, kBlendOne);
    setRs(cpu, pe, kRsDestBlendAlpha, kBlendZero);
    setRs(cpu, pe, kRsBlendOpAlpha, kBlendOpAdd);
    // 采样器默认值：WRAP + POINT + MIPFILTER NONE
    for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressU) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressV) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampAddressW) * 4u, kAddressWrap);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMagFilter) * 4u, kTexfPoint);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMinFilter) * 4u, kTexfPoint);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMipFilter) * 4u, kTexfNone);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMaxMipLevel) * 4u, 0u);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampMaxAnisotropy) * 4u, 1u);
        plSet(cpu, pe, kPlSamplers + (s * 16u + kSampBorderColor) * 4u, 0u);
    }
    // 纹理级默认：级 0 = MODULATE(TEXTURE, CURRENT=DIFFUSE)；其余级 DISABLE
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorOp) * 4u, kTopModulate);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorArg1) * 4u, kTaTexture);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssColorArg2) * 4u, kTaCurrent);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaOp) * 4u, kTopModulate);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaArg1) * 4u, kTaTexture);
    plSet(cpu, pe, kPlTexStages + (0u * kPlTexStageTypes + kTssAlphaArg2) * 4u, kTaCurrent);
    for (std::uint32_t s = 1; s < kMaxTextureStages; ++s) {
        plSet(cpu, pe, kPlTexStages + (s * kPlTexStageTypes + kTssColorOp) * 4u, kTopDisable);
        plSet(cpu, pe, kPlTexStages + (s * kPlTexStageTypes + kTssAlphaOp) * 4u, kTopDisable);
    }
    // 材质默认：diffuse/ambient = 1，specular/emissive = 0，power = 0（自洽默认，见文件头）
    for (std::uint32_t c = 0; c < 4; ++c) {
        wrF32(cpu, pe + kPlMaterial + c * 4u, 1.0f);          // Diffuse
        wrF32(cpu, pe + kPlMaterial + 16u + c * 4u, 1.0f);    // Ambient
        wrF32(cpu, pe + kPlMaterial + 32u + c * 4u, 0.0f);    // Specular
        wrF32(cpu, pe + kPlMaterial + 48u + c * 4u, 0.0f);    // Emissive
    }
    wrF32(cpu, pe + kPlMaterial + 64u, 0.0f);  // Power
    // 视口 = 整个后缓冲；裁剪矩形 = 整个后缓冲
    plSet(cpu, pe, kPlViewport + 0u, 0u);
    plSet(cpu, pe, kPlViewport + 4u, 0u);
    plSet(cpu, pe, kPlViewport + 8u, static_cast<std::uint32_t>(width));
    plSet(cpu, pe, kPlViewport + 12u, static_cast<std::uint32_t>(height));
    wrF32(cpu, pe + kPlViewport + 16u, 0.0f);
    wrF32(cpu, pe + kPlViewport + 20u, 1.0f);
    plSet(cpu, pe, kPlScissor + 0u, 0u);
    plSet(cpu, pe, kPlScissor + 4u, 0u);
    plSet(cpu, pe, kPlScissor + 8u, static_cast<std::uint32_t>(width));
    plSet(cpu, pe, kPlScissor + 12u, static_cast<std::uint32_t>(height));
}

// =====================================================================================
// 纹理 / 顶点缓冲的解析（COM 对象 EA → 状态块）
// =====================================================================================
struct TextureView {
    bool present = false;
    const std::uint8_t* pixels = nullptr;
    SurfaceFormat format;
    int width = 0;
    int height = 0;
    std::uint32_t pitch = 0;
    std::uint32_t version = 0;  // 内容版本（TextureInfo::version）；GPU 上传缓存用
    std::uint32_t serial = 0;   // 纹理身份（TextureInfo::serial）；GPU 缓存键用
    // ---- 派生 mip 链（`REQ-01M73G2724479XAVS176WBZ65Q`）----
    // `state` = `'TEX1'` 状态块 EA（按需重取内容版本 / 现算链）；`mips` = 本批用的链
    // （宿主旁路表里的条目，生命周期到下一次重建 / 纹理销毁 ⇒ ⛔ 不要跨 Draw 存）。
    std::uint32_t state = 0;
    const TextureMipChain* mips = nullptr;
    // ★ 方案 C 的脏区输入（TextureInfo 的写入矩形 + 版本链）
    std::uint32_t write_version = 0;
    std::uint32_t prev_write_version = 0;
    std::int32_t write_rect[4] = {0, 0, 0, 0};
};

bool resolveTexture(X86& cpu, Machine* m, std::uint32_t object_ea, TextureView* out, std::string* why) {
    out->present = false;
    if (object_ea == 0) return true;
    TextureInfo ti;
    if (!textureInfo(cpu, object_ea, &ti, why)) return false;
    GuestSurface* gs = m->surfaces().find(ti.arena);
    if (!gs) {
        if (why) *why = "D3D9 流水线：纹理像素不在 arena 里";
        return false;
    }
    out->pixels = m->surfaces().pixels(*gs);
    out->format = gs->format;
    out->width = gs->width;
    out->height = gs->height;
    out->pitch = gs->pitch;
    out->version = ti.version;
    out->serial = ti.serial;
    out->state = ti.state;
    out->write_version = ti.write_version;
    out->prev_write_version = ti.prev_write_version;
    for (int k = 0; k < 4; ++k) out->write_rect[k] = ti.write_rect[k];
    out->present = out->pixels != nullptr && out->format.bytesPerPixel() != 0;
    return true;
}

// ---- 颜色换算：实现只在 runtime/pixel_format.h（唯一一份）----

// =====================================================================================
// FVF 解析
// =====================================================================================
struct FvfLayout {
    bool valid = false;
    std::uint32_t fvf = 0;
    bool xyz = false;    // D3DFVF_XYZ
    bool xyz_rhw = false;
    bool xyzw = false;
    bool has_normal = false;
    bool has_psize = false;
    bool has_diffuse = false;
    bool has_specular = false;
    std::uint32_t tex_count = 0;
    std::uint32_t tex_size[8] = {0};  // 每个坐标的 float 数（2 默认）
    std::uint32_t pos_offset = 0;
    std::uint32_t normal_offset = 0;
    std::uint32_t psize_offset = 0;
    std::uint32_t diffuse_offset = 0;
    std::uint32_t specular_offset = 0;
    std::uint32_t tex_offset[8] = {0};
    std::uint32_t stride = 0;
};

FvfLayout parseFvf(std::uint32_t fvf, std::string* why) {
    FvfLayout l;
    l.fvf = fvf;
    const std::uint32_t pos = fvf & kFvfPositionMask;
    if (pos == kFvfXyz) {
        l.xyz = true;
    } else if (pos == kFvfXyzRhw) {
        l.xyz_rhw = true;
    } else if (pos == kFvfXyzw) {
        l.xyzw = true;
    } else {
        if (why) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "D3D9 流水线：FVF=0x%08X 的位置类型 0x%X 不在本线支持集（XYZ/XYZRHW/XYZW）", fvf, pos);
            *why = buf;
        }
        return l;
    }
    std::uint32_t off = 0;
    if (l.xyzw) {
        off += 16u;
    } else if (l.xyz_rhw) {
        off += 16u;  // x,y,z,rhw
    } else {
        off += 12u;  // x,y,z
        // XYZB1..B5：混权（本线不做顶点混合，但**步长要算对**）
        switch (pos) {
            case 0x0006u: off += 4u; break;
            case 0x0008u: off += 8u; break;
            case 0x000Au: off += 12u; break;
            case 0x000Cu: off += 16u; break;
            case 0x000Eu: off += 20u; break;
            default: break;
        }
    }
    l.has_normal = (fvf & kFvfNormal) != 0;
    if (l.has_normal) {
        l.normal_offset = off;
        off += 12u;
    }
    l.has_psize = (fvf & kFvfPSize) != 0;
    if (l.has_psize) {
        l.psize_offset = off;
        off += 4u;
    }
    l.has_diffuse = (fvf & kFvfDiffuse) != 0;
    if (l.has_diffuse) {
        l.diffuse_offset = off;
        off += 4u;
    }
    l.has_specular = (fvf & kFvfSpecular) != 0;
    if (l.has_specular) {
        l.specular_offset = off;
        off += 4u;
    }
    l.tex_count = (fvf & kFvfTexCountMask) >> kFvfTexCountShift;
    if (l.tex_count > 8u) {
        if (why) *why = "D3D9 流水线：FVF 的纹理坐标数 > 8（本线上限 8）";
        return l;
    }
    for (std::uint32_t i = 0; i < l.tex_count; ++i) {
        // FVF 位 16+2i 起的 2 位：0=2 floats / 1=3 / 2=4 / 3=1
        const std::uint32_t code = (fvf >> (16u + i * 2u)) & 0x3u;
        std::uint32_t n = 2u;
        if (code == 1u) n = 3u;
        else if (code == 2u) n = 4u;
        else if (code == 3u) n = 1u;
        l.tex_size[i] = n;
        l.tex_offset[i] = off;
        off += n * 4u;
    }
    l.stride = off;
    l.valid = true;
    return l;
}

// =====================================================================================
// 顶点：读 + 变换 + 光照
// =====================================================================================
constexpr std::uint32_t kMaxClipVerts = 16u;   // 三角形过 6 个裁剪面后最多 3+6=9 个（留余量）
constexpr std::uint32_t kMaxTexCoords = 8u;

struct CVertex {
    double x = 0, y = 0, z = 0, w = 1;  // 裁剪空间（XYZRHW 时 x,y 已是屏幕坐标）
    double u[kMaxTexCoords] = {0};
    double v[kMaxTexCoords] = {0};
    double r = 255, g = 255, b = 255, a = 255;   // diffuse（0..255）
    double sr = 0, sg = 0, sb = 0, sa = 0;       // specular
    bool pretransformed = false;                 // XYZRHW
};

struct SVertex {
    double x = 0, y = 0;  // 屏幕（像素）
    double z = 0;         // [0,1]
    double invw = 1;
    double u[kMaxTexCoords] = {0};  // 已乘 invw
    double v[kMaxTexCoords] = {0};
    double r = 255, g = 255, b = 255, a = 255;  // 已乘 invw
    double sr = 0, sg = 0, sb = 0, sa = 0;
};

// ---- 变换：统一走 runtime/math 的 D3DX↔GLM 适配（⛔ 这里不再有行/列主序的手写展开）----
math::Mat4d readMatrix(X86& cpu, std::uint32_t ea) {
    double d3d[16];
    for (int i = 0; i < 16; ++i) d3d[i] = static_cast<double>(rdF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u));
    return math::fromD3DLayout(d3d);
}

void writeMatrix(X86& cpu, std::uint32_t ea, const math::Mat4d& m) {
    double d3d[16];
    math::toD3DLayout(m, d3d);
    for (int i = 0; i < 16; ++i) wrF32(cpu, ea + static_cast<std::uint32_t>(i) * 4u, static_cast<float>(d3d[i]));
}

struct LightState {
    std::uint32_t type = 0;
    double diffuse[4] = {0, 0, 0, 0};
    double specular[4] = {0, 0, 0, 0};
    double ambient[4] = {0, 0, 0, 0};
    double position[3] = {0, 0, 0};
    double direction[3] = {0, 0, 0};
    double range = 0;
    double falloff = 0;
    double atten0 = 0, atten1 = 0, atten2 = 0;
};

struct MaterialState {
    double diffuse[4] = {1, 1, 1, 1};
    double ambient[4] = {1, 1, 1, 1};
    double specular[4] = {0, 0, 0, 0};
    double emissive[4] = {0, 0, 0, 0};
    double power = 0;
};

void readMaterial(X86& cpu, std::uint32_t pe, MaterialState* out) {
    for (int i = 0; i < 4; ++i) {
        out->diffuse[i] = static_cast<double>(rdF32(cpu, pe + kPlMaterial + static_cast<std::uint32_t>(i) * 4u));
        out->ambient[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 16u + static_cast<std::uint32_t>(i) * 4u));
        out->specular[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 32u + static_cast<std::uint32_t>(i) * 4u));
        out->emissive[i] =
            static_cast<double>(rdF32(cpu, pe + kPlMaterial + 48u + static_cast<std::uint32_t>(i) * 4u));
    }
    out->power = static_cast<double>(rdF32(cpu, pe + kPlMaterial + 64u));
}

void readLight(X86& cpu, std::uint32_t pe, std::uint32_t index, LightState* out) {
    const std::uint32_t base = pe + kPlLights + index * 104u;
    out->type = cpu.mem->rd32(base);
    for (int i = 0; i < 4; ++i) {
        out->diffuse[i] = static_cast<double>(rdF32(cpu, base + 4u + static_cast<std::uint32_t>(i) * 4u));
        out->specular[i] = static_cast<double>(rdF32(cpu, base + 20u + static_cast<std::uint32_t>(i) * 4u));
        out->ambient[i] = static_cast<double>(rdF32(cpu, base + 36u + static_cast<std::uint32_t>(i) * 4u));
    }
    for (int i = 0; i < 3; ++i) out->position[i] = static_cast<double>(rdF32(cpu, base + 52u + static_cast<std::uint32_t>(i) * 4u));
    for (int i = 0; i < 3; ++i) out->direction[i] = static_cast<double>(rdF32(cpu, base + 64u + static_cast<std::uint32_t>(i) * 4u));
    out->range = static_cast<double>(rdF32(cpu, base + 76u));
    out->falloff = static_cast<double>(rdF32(cpu, base + 80u));
    out->atten0 = static_cast<double>(rdF32(cpu, base + 84u));
    out->atten1 = static_cast<double>(rdF32(cpu, base + 88u));
    out->atten2 = static_cast<double>(rdF32(cpu, base + 92u));
}

// 顶点颜色（D3DCOLOR 0xAARRGGBB → 0..255）：位序走 pixel_format 的唯一一份 `unpackColor`
void splitColor(std::uint32_t color, double* a, double* r, double* g, double* b) {
    unsigned ua = 0, ur = 0, ug = 0, ub = 0;
    pixel_format::unpackColor(color, ua, ur, ug, ub);
    *a = static_cast<double>(ua);
    *r = static_cast<double>(ur);
    *g = static_cast<double>(ug);
    *b = static_cast<double>(ub);
}

// 固定功能光照（Gouraud；只在 LIGHTING=1 且顶点有法线时做）
void applyLighting(X86& cpu, std::uint32_t pe, const math::Vec3d& world_pos,
                   const math::Vec3d& world_normal, const math::Mat4d& view, double lit[4]) {
    MaterialState mat;
    readMaterial(cpu, pe, &mat);
    // 材质来源（diffuse/ambient）——COLORVERTEX 为真有顶点色时用顶点色（在 Draw 里覆盖）
    const math::Vec3d cam_pos = math::transformPoint3D3D(view, world_pos);
    const math::Vec3d n =
        math::normalized3D3D(math::transformVector3D3D(view, math::normalized3D3D(world_normal)));
    const std::uint32_t ambient_argb = rsOf(cpu, pe, kRsAmbient);
    double ga = 0, gr = 0, gg = 0, gb = 0;
    splitColor(ambient_argb, &ga, &gr, &gg, &gb);
    double r = mat.emissive[0] + mat.ambient[0] * (gr / 255.0);
    double g = mat.emissive[1] + mat.ambient[1] * (gg / 255.0);
    double b = mat.emissive[2] + mat.ambient[2] * (gb / 255.0);
    const double alpha = mat.diffuse[3];
    for (std::uint32_t i = 0; i < kMaxLights; ++i) {
        if (plGet(cpu, pe, kPlLightEnabled + i * 4u) == 0) continue;
        LightState ls;
        readLight(cpu, pe, i, &ls);
        math::Vec3d ldir(0.0);  // 从顶点指向光源
        double atten = 1.0;
        switch (ls.type) {
            case kLightDirectional:
                ldir = math::normalized3D3D(math::Vec3d(-ls.direction[0], -ls.direction[1], -ls.direction[2]));
                break;
            case kLightPoint:
            case kLightSpot: {
                const math::Vec3d pos =
                    math::transformPoint3D3D(view, math::Vec3d(ls.position[0], ls.position[1], ls.position[2]));
                ldir = pos - cam_pos;
                const double dist = glm::length(ldir);
                if (ls.range > 0.0 && dist > ls.range) continue;
                const double denom = ls.atten0 + ls.atten1 * dist + ls.atten2 * dist * dist;
                atten = denom > 1e-12 ? 1.0 / denom : 1.0;
                if (atten > 1.0) atten = 1.0;
                ldir = math::normalized3D3D(ldir);
                if (ls.type == kLightSpot) {
                    // 本线把 SPOT 按 POINT + falloff 近似（⛔ 没做锥角）
                    atten *= 1.0;
                }
                break;
            }
            default:
                continue;
        }
        double ndl = glm::dot(n, ldir);
        if (ndl < 0.0) ndl = 0.0;
        if (ndl > 0.0) {
            r += ls.diffuse[0] * mat.diffuse[0] * ndl * atten;
            g += ls.diffuse[1] * mat.diffuse[1] * ndl * atten;
            b += ls.diffuse[2] * mat.diffuse[2] * ndl * atten;
        }
        // 镜面：LOCALVIEWER=FALSE 时 D3D 用 (0,0,-1)；这里统一用"顶点 → 眼"（更接近 LOCALVIEWER=TRUE）
        if (mat.power > 0.0 && rsOf(cpu, pe, kRsSpecularEnable) != 0) {
            const math::Vec3d vdir = math::normalized3D3D(-cam_pos);
            const math::Vec3d h = math::normalized3D3D(ldir + vdir);
            double ndh = glm::dot(n, h);
            if (ndh < 0.0) ndh = 0.0;
            const double spec = std::pow(ndh, mat.power) * atten;
            r += ls.specular[0] * mat.specular[0] * spec;
            g += ls.specular[1] * mat.specular[1] * spec;
            b += ls.specular[2] * mat.specular[2] * spec;
        }
    }
    lit[0] = r > 1.0 ? 1.0 : (r < 0.0 ? 0.0 : r);
    lit[1] = g > 1.0 ? 1.0 : (g < 0.0 ? 0.0 : g);
    lit[2] = b > 1.0 ? 1.0 : (b < 0.0 ? 0.0 : b);
    lit[3] = alpha > 1.0 ? 1.0 : (alpha < 0.0 ? 0.0 : alpha);
}

// =====================================================================================
// 光栅化上下文（含**每次 Draw 的状态快照**）
// ★ 消除偏差 #4：像素循环不再逐像素读 guest —— guest 在 Draw 期间不会执行（本线不做嵌套回调），
//   所以纹理级 / 采样器状态在 Draw 入口读一遍就定型，像素循环只读 DrawContext 里的副本
//   （⛔ 不再调 texStageOf/samplerOf）。
// =====================================================================================
struct TextureStageSnapshot {
    std::uint32_t color_op = kTopDisable;
    std::uint32_t alpha_op = kTopDisable;
    std::uint32_t color_arg0 = 0;
    std::uint32_t color_arg1 = 0;
    std::uint32_t color_arg2 = 0;
    std::uint32_t alpha_arg1 = 0;
    std::uint32_t alpha_arg2 = 0;
    std::uint32_t constant = 0;         // D3DCOLOR（TFactor 之外的另一份常量）
    std::uint32_t tex_coord_index = 0;  // D3DTSS_TEXCOORDINDEX
};

struct SamplerSnapshot {
    std::uint32_t address_u = kAddressWrap;
    std::uint32_t address_v = kAddressWrap;
    std::uint32_t mag_filter = kTexfPoint;
    std::uint32_t min_filter = kTexfPoint;
    // ★ `D3DTEXF_NONE` = **不做 mipmap**（D3D9 语义）⇒ 只采 level 0。缺省与 `initDefaults` 一致。
    std::uint32_t mip_filter = kTexfNone;
};

struct DrawContext {
    X86* cpu = nullptr;
    Machine* machine = nullptr;
    GpuRaster* gpu = nullptr;  // --raster gpu 且 sink 提供时非空
    std::uint32_t pe = 0;
    std::uint32_t fvf = 0;  // 当前 SetFVF（诊断用）
    std::uint8_t* color_base = nullptr;
    SurfaceFormat color_fmt;
    int color_width = 0;
    int color_height = 0;
    std::uint32_t color_pitch = 0;
    std::uint32_t color_surface = 0;  // 0 = 设备后缓冲
    float* depth_base = nullptr;  // 后缓冲尺寸的 float 深度
    std::uint32_t depth_pitch_floats = 0;
    // ★ 深度缓冲的**矩形**（来自 `createForDevice` 的分配尺寸 = 后缓冲尺寸）。
    //   像素循环的上界是**渲染目标**尺寸 ⇒ RT 比后缓冲大时 py/px 会越出深度缓冲，
    //   越界写会踩进 SurfaceArena 的相邻表面（静默损坏）⇒ z 的读/写只在矩形内生效。
    int depth_width = 0;
    int depth_height = 0;
    int viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
    int scissor_x0 = 0, scissor_y0 = 0, scissor_x1 = 0, scissor_y1 = 0;
    // 状态快照
    bool z_enable = true;
    bool z_write = true;
    std::uint32_t z_func = kCmpLessEqual;
    std::uint32_t cull = kCullCcw;
    std::uint32_t shade = kShadeGouraud;
    bool alpha_test = false;
    std::uint32_t alpha_func = kCmpAlways;
    std::uint32_t alpha_ref = 0;
    bool blend = false;
    std::uint32_t src_blend = kBlendOne;
    std::uint32_t dst_blend = kBlendZero;
    std::uint32_t blend_op = kBlendOpAdd;
    bool separate_alpha_blend = false;
    std::uint32_t src_blend_a = kBlendOne;
    std::uint32_t dst_blend_a = kBlendZero;
    std::uint32_t blend_op_a = kBlendOpAdd;
    std::uint32_t color_write = 0xF;
    std::uint32_t texture_factor = 0xFFFFFFFFu;
    TextureView textures[kMaxTextureStages];
    // ★ 每次 Draw 的快照（prepareContext 里一次读完；像素循环只读这里）
    TextureStageSnapshot stages[kMaxTextureStages];
    SamplerSnapshot samplers[kMaxTextureStages];
    std::uint64_t pixels_written = 0;
    int serial = 0;
    // ---- 派生 mip 的诊断 / 记账（`REQ-01M73G2724479XAVS176WBZ65Q`）----
    // ★ `mip_attempted[s]`：本批对第 s 级**已经尝试过**取链（内容版本未知的纹理 —— RT 降级 ——
    //   每次取链都要全量重算，⛔ 不能每个三角形都重算一遍）。
    // ★ `mip_bytes_built`：本批因失效而重建的链字节数（`[draw] miprb=`）。
    bool mip_attempted[kMaxTextureStages] = {false, false, false, false, false, false, false, false};
    bool mip_level0_fallback = false;  // 本批有"要 mip 但链不可用 ⇒ 退回 level 0"（记账一次）
    std::uint64_t mip_bytes_built = 0;
    std::uint32_t mip_level_min = 0xFFFFFFFFu;  // 本批采到的级号（min..max；0xFFFFFFFF = 没选过）
    std::uint32_t mip_level_max = 0;
    // ★ 诊断（`[draw] bbox=`）：本批真的写进屏幕的像素范围（右/下开区间；0x7FFFFFFF = 没写过）。
    int bbox_x0 = 0x7FFFFFFF, bbox_y0 = 0x7FFFFFFF, bbox_x1 = 0, bbox_y1 = 0;
};

// 8 位通道的混合因子
// ★ 这一族（blendFactors8 / applyBlendOp）是**设备混合状态**（D3DBLEND / D3DBLENDOP）的语义，
//   ⛔ 与 pixel_format 的 mask/expand 或 Sprite 的 ALPHABLEND `mul255` 不是同一件事
//   （公式不同；合并会改像素结果）—— 审计结论：只保留各自一份，不强行收敛。
void blendFactors8(std::uint32_t factor, unsigned sr, unsigned sg, unsigned sb, unsigned sa, unsigned dr,
                   unsigned dg, unsigned db, unsigned da, unsigned* fr, unsigned* fg, unsigned* fb,
                   unsigned* fa) {
    switch (factor) {
        case kBlendZero: *fr = *fg = *fb = *fa = 0u; break;
        case kBlendOne: *fr = *fg = *fb = *fa = 255u; break;
        case kBlendSrcColor: *fr = sr; *fg = sg; *fb = sb; *fa = sa; break;
        case kBlendInvSrcColor: *fr = 255u - sr; *fg = 255u - sg; *fb = 255u - sb; *fa = 255u - sa; break;
        case kBlendSrcAlpha: *fr = *fg = *fb = *fa = sa; break;
        case kBlendInvSrcAlpha: *fr = *fg = *fb = *fa = 255u - sa; break;
        case kBlendDestAlpha: *fr = *fg = *fb = *fa = da; break;
        case kBlendInvDestAlpha: *fr = *fg = *fb = *fa = 255u - da; break;
        case kBlendDestColor: *fr = dr; *fg = dg; *fb = db; *fa = da; break;
        case kBlendInvDestColor: *fr = 255u - dr; *fg = 255u - dg; *fb = 255u - db; *fa = 255u - da; break;
        case kBlendSrcAlphaSat: {
            const unsigned f = sa < (255u - da) ? sa : (255u - da);
            *fr = *fg = *fb = f;
            *fa = 255u;
            break;
        }
        case kBlendBothSrcAlpha: *fr = sr; *fg = sg; *fb = sb; *fa = sa; break;
        case kBlendBothInvSrcAlpha: *fr = 255u - sr; *fg = 255u - sg; *fb = 255u - sb; *fa = 255u - sa; break;
        default: *fr = *fg = *fb = *fa = 0u; break;
    }
}

unsigned applyBlendOp(std::uint32_t op, unsigned s, unsigned d) {
    switch (op) {
        case kBlendOpAdd: return (s + d + 127u) / 255u > 255u ? 255u : (s + d + 127u) / 255u;
        case kBlendOpSubtract: return s >= d ? (s - d + 127u) / 255u : 0u;
        case kBlendOpRevSubtract: return d >= s ? (d - s + 127u) / 255u : 0u;
        case kBlendOpMin: return s < d ? s : d;
        case kBlendOpMax: return s > d ? s : d;
        default: return (s + d + 127u) / 255u > 255u ? 255u : (s + d + 127u) / 255u;
    }
}

unsigned clamp255(double v) {
    if (v <= 0.0) return 0u;
    if (v >= 255.0) return 255u;
    return static_cast<unsigned>(v + 0.5);
}

// ★ 诊断开关（默认 = 开）：`LIFT_NO_MIP=1` 关掉 mip 选级 ⇒ 恒 level 0。
//   唯一用途 = e2e 守卫的**对照臂**（同一份 repro、同一份二进制，只差这一步）——
//   ⛔ 不是"画质开关"，也不改变任何其它语义（TEXF_*/寻址/混合都不动）。
bool mipSamplingEnabled() {
    static const bool enabled = std::getenv("LIFT_NO_MIP") == nullptr;
    return enabled;
}

// 采样**一个 mip 级**：寻址 + 过滤的唯一实现（level 0 与派生各级都走这里，
// 只是 `pixels/pitch/w/h` 换成该级的）。⛔ 不要再写第二份双线性。
void sampleTextureLevel(const std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                        const SurfaceFormat& fmt, const std::uint32_t address_u, const std::uint32_t address_v,
                        const std::uint32_t mag_filter, const std::uint32_t min_filter, double u, double v,
                        unsigned out[4]) {
    out[0] = 255u;
    out[1] = 255u;
    out[2] = 255u;
    out[3] = 255u;
    if (pixels == nullptr || width <= 0 || height <= 0) return;
    const std::uint32_t bpp = fmt.bytesPerPixel();
    if (bpp == 0) return;
    auto applyAddress = [](std::uint32_t mode, double coord, int size) -> int {
        double c = coord;
        switch (mode) {
            case kAddressWrap:
                c = c - std::floor(c);
                break;
            case kAddressMirror: {
                // 镜像：周期 2（0..1 正序、1..2 逆序）
                double m = c - std::floor(c / 2.0) * 2.0;
                c = m > 1.0 ? 2.0 - m : m;
                break;
            }
            case kAddressClamp:
            case kAddressMirrorOnce:
                if (c < 0.0) c = 0.0;
                if (c > 1.0) c = 1.0;
                break;
            case kAddressBorder:
                if (c < 0.0 || c > 1.0) return -1;
                break;
            default:
                c = c - std::floor(c);
                break;
        }
        int i = static_cast<int>(std::floor(c * static_cast<double>(size)));
        if (i < 0) i = 0;
        if (i >= size) i = size - 1;
        return i;
    };
    auto load = [&](int x, int y, unsigned rgba[4]) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0u;
            return;
        }
        const std::uint8_t* p = pixels + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * bpp;
        const std::uint32_t raw = pixel_format::loadPixel(p, fmt.bpp);
        rgba[0] = pixel_format::expandChannel(raw, fmt.rmask);
        rgba[1] = pixel_format::expandChannel(raw, fmt.gmask);
        rgba[2] = pixel_format::expandChannel(raw, fmt.bmask);
        // ★ mask==0（无 alpha 通道）⇒ expandChannel 返回 255（= 不透明）；这份语义只在 pixel_format 里
        rgba[3] = pixel_format::expandChannel(raw, fmt.amask);
    };
    // ★ MIN 与 MAG 都参与（MIN 不再被忽略）；mip 选级在调用方（按 LOD），这里只管单级过滤。
    const bool linear = samplerUsesLinear(mag_filter, min_filter);
    if (mag_filter == kTexfNone && min_filter == kTexfNone) {
        // 不可见组合（D3D 不会这样用）：不猜像素 ⇒ 透明黑；SetSamplerState 已 recordUnsupported。
        out[0] = out[1] = out[2] = out[3] = 0u;
        return;
    }
    if (!linear) {
        const int x = applyAddress(address_u, u, width);
        const int y = applyAddress(address_v, v, height);
        if (x < 0 || y < 0) {
            out[0] = out[1] = out[2] = out[3] = 0u;
            return;
        }
        unsigned rgba[4];
        load(x, y, rgba);
        out[0] = rgba[0];
        out[1] = rgba[1];
        out[2] = rgba[2];
        out[3] = rgba[3];
        return;
    }
    // LINEAR：双线性（texel 中心对齐；D3D 的 -0.5 偏移）
    const double fx = u * static_cast<double>(width) - 0.5;
    const double fy = v * static_cast<double>(height) - 0.5;
    const double x0f = std::floor(fx);
    const double y0f = std::floor(fy);
    const double tx = fx - x0f;
    const double ty = fy - y0f;
    const int x0 = applyAddress(address_u, (x0f + 0.5) / static_cast<double>(width), width);
    const int x1 = applyAddress(address_u, (x0f + 1.5) / static_cast<double>(width), width);
    const int y0 = applyAddress(address_v, (y0f + 0.5) / static_cast<double>(height), height);
    const int y1 = applyAddress(address_v, (y0f + 1.5) / static_cast<double>(height), height);
    unsigned c00[4], c10[4], c01[4], c11[4];
    load(x0, y0, c00);
    load(x1, y0, c10);
    load(x0, y1, c01);
    load(x1, y1, c11);
    for (int i = 0; i < 4; ++i) {
        const double a = static_cast<double>(c00[i]) * (1.0 - tx) + static_cast<double>(c10[i]) * tx;
        const double b = static_cast<double>(c01[i]) * (1.0 - tx) + static_cast<double>(c11[i]) * tx;
        out[i] = clamp255(a * (1.0 - ty) + b * ty);
    }
}

// 按**级号**采样：`level == 0` ⇒ arena 里的 level 0；`level > 0` ⇒ 派生链里的第 level 级。
// ★ 链不可用（没算出来 / 级号越界）⇒ **退回 level 0**（调用方必须已经记过账，⛔ 不在这里静默）。
void sampleTexture(const TextureView& t, const std::uint32_t address_u, const std::uint32_t address_v,
                   const std::uint32_t mag_filter, const std::uint32_t min_filter, std::uint32_t level, double u,
                   double v, unsigned out[4]) {
    if (!t.present || t.width <= 0 || t.height <= 0) {
        out[0] = out[1] = out[2] = out[3] = 255u;
        return;
    }
    if (level > 0 && (t.mips == nullptr || level >= t.mips->levels || t.mips->bytes.empty())) level = 0;
    if (level == 0) {
        sampleTextureLevel(t.pixels, t.pitch, t.width, t.height, t.format, address_u, address_v, mag_filter,
                           min_filter, u, v, out);
        return;
    }
    const std::uint32_t w0 = static_cast<std::uint32_t>(t.width);
    const std::uint32_t h0 = static_cast<std::uint32_t>(t.height);
    const std::uint32_t offset = mipLevelOffset(w0, h0, t.format.bpp, level);
    const std::uint32_t pitch = mipLevelPitch(w0, t.format.bpp, level);
    const std::uint8_t* pixels = t.mips->bytes.data() + offset;
    sampleTextureLevel(pixels, pitch, static_cast<int>(mipLevelWidth(w0, level)),
                       static_cast<int>(mipLevelHeight(h0, level)), t.format, address_u, address_v, mag_filter,
                       min_filter, u, v, out);
}

struct StageColor {
    double c[4] = {1.0, 1.0, 1.0, 1.0};  // 0..255
};

// 取一个 stage 的源操作数（D3DTA_*）
void stageArg(X86& cpu, const DrawContext& ctx, std::uint32_t arg, const StageColor& current,
              const StageColor& diffuse, const StageColor& specular, const StageColor& texture,
              const StageColor& temp, const StageColor& constant, double out[4]) {
    (void)cpu;
    const std::uint32_t sel = arg & kTaSelectMask;
    const StageColor* src = &diffuse;
    switch (sel) {
        case kTaDiffuse: src = &diffuse; break;
        case kTaCurrent: src = &current; break;
        case kTaTexture: src = &texture; break;
        case kTaTFactor: {
            double a = 1.0, r = 1.0, g = 1.0, b = 1.0;
            splitColor(ctx.texture_factor, &a, &r, &g, &b);
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
            if ((arg & kTaAlphaReplicate) != 0) out[0] = out[1] = out[2] = out[3];
            if ((arg & kTaComplement) != 0)
                for (int i = 0; i < 4; ++i) out[i] = 255.0 - out[i];
            return;
        }
        case kTaSpecular: src = &specular; break;
        case kTaTemp: src = &temp; break;
        case kTaConstant: src = &constant; break;
        default: src = &diffuse; break;
    }
    for (int i = 0; i < 4; ++i) out[i] = src->c[i];
    if ((arg & kTaAlphaReplicate) != 0) out[0] = out[1] = out[2] = out[3];
    if ((arg & kTaComplement) != 0)
        for (int i = 0; i < 4; ++i) out[i] = 255.0 - out[i];
}

void applyTextureOp(X86& cpu, const DrawContext& ctx, std::uint32_t stage, const StageColor& diffuse,
                    const StageColor& specular, const StageColor& texture, const StageColor& current_in,
                    StageColor* color_out, StageColor* alpha_out) {
    // ★ 状态全部来自 Draw 入口的快照（⛔ 不在像素循环里 texStageOf）
    //   唯一还会碰 guest 的是"不支持的纹理级 OP"的 `recordUnsupported`（幂等、只在真出现时）；
    //   本线支持的 OP 集合内，像素循环零 guest 读。
    const TextureStageSnapshot& ts = ctx.stages[stage];
    const StageColor temp;  // 本线没有 TEMP 寄存器：全 0（记录见下）
    StageColor constant;
    {
        double a = 1.0, r = 1.0, g = 1.0, b = 1.0;
        splitColor(ts.constant, &a, &r, &g, &b);
        constant.c[0] = r;
        constant.c[1] = g;
        constant.c[2] = b;
        constant.c[3] = a;
    }
    const std::uint32_t color_op = ts.color_op;
    const std::uint32_t alpha_op = ts.alpha_op;
    const std::uint32_t arg0 = ts.color_arg0;
    double a1[4], a2[4], a0[4];
    stageArg(cpu, ctx, ts.color_arg1, current_in, diffuse, specular, texture, temp, constant, a1);
    stageArg(cpu, ctx, ts.color_arg2, current_in, diffuse, specular, texture, temp, constant, a2);
    stageArg(cpu, ctx, arg0, current_in, diffuse, specular, texture, temp, constant, a0);
    double aa1[4], aa2[4];
    stageArg(cpu, ctx, ts.alpha_arg1, current_in, diffuse, specular, texture, temp, constant, aa1);
    stageArg(cpu, ctx, ts.alpha_arg2, current_in, diffuse, specular, texture, temp, constant, aa2);
    // 颜色通道的 op（alpha 通道单独走下面那段：D3D 的 alpha op 只作用在 alpha 分量上）
    auto run = [&](std::uint32_t code, const double x[4], const double y[4], const double z[4], double out[3]) {
        std::uint32_t unsupported = 0;
        switch (code) {
            case kTopSelectArg1: out[0] = x[0]; out[1] = x[1]; out[2] = x[2]; break;
            case kTopSelectArg2: out[0] = y[0]; out[1] = y[1]; out[2] = y[2]; break;
            case kTopModulate: out[0] = x[0] * y[0] / 255.0; out[1] = x[1] * y[1] / 255.0; out[2] = x[2] * y[2] / 255.0; break;
            case kTopModulate2x:
                out[0] = x[0] * y[0] / 255.0 * 2.0;
                out[1] = x[1] * y[1] / 255.0 * 2.0;
                out[2] = x[2] * y[2] / 255.0 * 2.0;
                break;
            case kTopModulate4x:
                out[0] = x[0] * y[0] / 255.0 * 4.0;
                out[1] = x[1] * y[1] / 255.0 * 4.0;
                out[2] = x[2] * y[2] / 255.0 * 4.0;
                break;
            case kTopAdd: out[0] = x[0] + y[0]; out[1] = x[1] + y[1]; out[2] = x[2] + y[2]; break;
            case kTopAddSigned:
                out[0] = x[0] + y[0] - 127.5;
                out[1] = x[1] + y[1] - 127.5;
                out[2] = x[2] + y[2] - 127.5;
                break;
            case kTopAddSigned2x:
                out[0] = (x[0] + y[0] - 127.5) * 2.0;
                out[1] = (x[1] + y[1] - 127.5) * 2.0;
                out[2] = (x[2] + y[2] - 127.5) * 2.0;
                break;
            case kTopSubtract: out[0] = x[0] - y[0]; out[1] = x[1] - y[1]; out[2] = x[2] - y[2]; break;
            case kTopAddSmooth:
                out[0] = x[0] + y[0] - x[0] * y[0] / 255.0;
                out[1] = x[1] + y[1] - x[1] * y[1] / 255.0;
                out[2] = x[2] + y[2] - x[2] * y[2] / 255.0;
                break;
            case kTopBlendDiffuseAlpha:
            case kTopBlendTextureAlpha:
            case kTopBlendFactorAlpha:
            case kTopBlendCurrentAlpha:
            case kTopBlendTextureAlphaPM: {
                double t = 0.0;
                if (code == kTopBlendDiffuseAlpha) t = x[3];
                else if (code == kTopBlendTextureAlpha) t = y[3];
                else if (code == kTopBlendFactorAlpha) t = 255.0;
                else if (code == kTopBlendCurrentAlpha) t = x[3];
                else t = y[3];
                t /= 255.0;
                out[0] = x[0] * t + y[0] * (1.0 - t);
                out[1] = x[1] * t + y[1] * (1.0 - t);
                out[2] = x[2] * t + y[2] * (1.0 - t);
                break;
            }
            case kTopMultiplyAdd:
                out[0] = z[0] + x[0] * y[0] / 255.0;
                out[1] = z[1] + x[1] * y[1] / 255.0;
                out[2] = z[2] + x[2] * y[2] / 255.0;
                break;
            case kTopLerp: {
                const double t = z[0] / 255.0;
                out[0] = x[0] * t + y[0] * (1.0 - t);
                out[1] = x[1] * t + y[1] * (1.0 - t);
                out[2] = x[2] * t + y[2] * (1.0 - t);
                break;
            }
            case kTopModulateAlphaAddColor:
                out[0] = x[0] + y[0] * x[3] / 255.0;
                out[1] = x[1] + y[1] * x[3] / 255.0;
                out[2] = x[2] + y[2] * x[3] / 255.0;
                break;
            case kTopModulateColorAddAlpha:
                out[0] = x[3] + y[0] * x[0] / 255.0;
                out[1] = x[3] + y[1] * x[1] / 255.0;
                out[2] = x[3] + y[2] * x[2] / 255.0;
                break;
            default:
                unsupported = code;
                out[0] = x[0];
                out[1] = x[1];
                out[2] = x[2];
                break;
        }
        if (unsupported != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "纹理级 OP 0x%X（级 %u）", unsupported, stage);
            recordUnsupported(cpu, ctx.pe, 24u, buf, unsupported);
        }
        for (int i = 0; i < 3; ++i) {
            if (out[i] < 0.0) out[i] = 0.0;
            if (out[i] > 255.0) out[i] = 255.0;
        }
    };
    double rgb[3];
    run(color_op, a1, a2, a0, rgb);
    color_out->c[0] = rgb[0];
    color_out->c[1] = rgb[1];
    color_out->c[2] = rgb[2];
    color_out->c[3] = current_in.c[3];
    // alpha 级（D3D 的 alpha op 只作用在 alpha 分量上）
    alpha_out->c[0] = current_in.c[0];
    alpha_out->c[1] = current_in.c[1];
    alpha_out->c[2] = current_in.c[2];
    double alpha_val = 0.0;
    {
        std::uint32_t unsupported = 0;
        switch (alpha_op) {
            case kTopSelectArg1: alpha_val = aa1[3]; break;
            case kTopSelectArg2: alpha_val = aa2[3]; break;
            case kTopModulate: alpha_val = aa1[3] * aa2[3] / 255.0; break;
            case kTopModulate2x: alpha_val = aa1[3] * aa2[3] / 255.0 * 2.0; break;
            case kTopModulate4x: alpha_val = aa1[3] * aa2[3] / 255.0 * 4.0; break;
            case kTopAdd: alpha_val = aa1[3] + aa2[3]; break;
            case kTopAddSigned: alpha_val = aa1[3] + aa2[3] - 127.5; break;
            case kTopAddSigned2x: alpha_val = (aa1[3] + aa2[3] - 127.5) * 2.0; break;
            case kTopSubtract: alpha_val = aa1[3] - aa2[3]; break;
            case kTopAddSmooth: alpha_val = aa1[3] + aa2[3] - aa1[3] * aa2[3] / 255.0; break;
            case kTopBlendDiffuseAlpha:
            case kTopBlendTextureAlpha:
            case kTopBlendFactorAlpha:
            case kTopBlendCurrentAlpha:
            case kTopBlendTextureAlphaPM: {
                double t = 0.0;
                if (alpha_op == kTopBlendDiffuseAlpha) t = aa1[3];
                else if (alpha_op == kTopBlendTextureAlpha) t = aa2[3];
                else if (alpha_op == kTopBlendFactorAlpha) t = 255.0;
                else if (alpha_op == kTopBlendCurrentAlpha) t = aa1[3];
                else t = aa2[3];
                t /= 255.0;
                alpha_val = aa1[3] * t + aa2[3] * (1.0 - t);
                break;
            }
            default:
                unsupported = alpha_op;
                alpha_val = aa1[3];
                break;
        }
        if (unsupported != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "纹理级 ALPHAOP 0x%X（级 %u）", unsupported, stage);
            recordUnsupported(cpu, ctx.pe, 25u, buf, unsupported);
        }
        if (alpha_val < 0.0) alpha_val = 0.0;
        if (alpha_val > 255.0) alpha_val = 255.0;
        alpha_out->c[3] = alpha_val;
    }
}

// ---------------------------------------------------------------------------------------
// 光栅化一个屏幕空间三角形
// ---------------------------------------------------------------------------------------
// ★ 诊断聚合：这一批 CPU 光栅**真正用来混合**的 α（均值/最小/最大）。只读、不影响像素。
struct AlphaAgg {
    unsigned long long n = 0;
    unsigned long long sum = 0;
    unsigned mn = 255;
    unsigned mx = 0;
};
static AlphaAgg g_alpha_agg;

// ---------------------------------------------------------------------------------------
// 一个三角形的**派生 mip 级号**（每三角形常量；`REQ-01M73G2724479XAVS176WBZ65Q`）
//   · LOD = log2(纹理足迹)，足迹 = 屏幕空间 UV 导数 × **该 stage 自己**的纹理尺寸；
//   · UV 导数用**透视校正后的 u（U/W）** —— 与采样器同一个插值式子；
//   · 退化三角形 / 尺寸非法 / 只有一级 ⇒ 0。
// ★ 这一个函数同时服务三条路：CPU 选级（`rasterTriangle`）、GPU 的 MIPFILTER 分叉判据
//   （`submitPrimitives`：`MIPFILTER=NONE` 但这一批有缩小采样 ⇒ GPU 的 sampler 表达不了
//   "关掉 mip"，必须回退 CPU），以及"1:1 / 放大必须仍是 level 0"这条判据。
// ---------------------------------------------------------------------------------------
std::uint32_t triangleMipLevel(const SVertex& a, const SVertex& b, const SVertex& c, std::uint32_t coord,
                               int tex_width, int tex_height) {
    if (tex_width <= 0 || tex_height <= 0 || coord >= kMaxTexCoords) return 0u;
    const std::uint32_t levels =
        mipLevelCount(static_cast<std::uint32_t>(tex_width), static_cast<std::uint32_t>(tex_height));
    if (levels <= 1u) return 0u;
    const double vx[3] = {a.x, b.x, c.x};
    const double vy[3] = {a.y, b.y, c.y};
    const double invw[3] = {a.invw, b.invw, c.invw};
    const double uw[3] = {a.u[coord], b.u[coord], c.u[coord]};
    const double vw[3] = {a.v[coord], b.v[coord], c.v[coord]};
    double dux = 0.0, dvx = 0.0, duy = 0.0, dvy = 0.0;
    if (!screenUvDerivatives(vx, vy, invw, uw, vw, &dux, &dvx, &duy, &dvy)) return 0u;
    const double lod = lodFromUvDerivatives(dux, dvx, duy, dvy, static_cast<double>(tex_width),
                                            static_cast<double>(tex_height));
    return mipLevelForLod(lod, levels);
}

// =====================================================================================
// 像素收尾（**唯一一份**）：深度测试 → 着色 → alpha test → 混合 → 颜色写掩码 → 深度写。
//
// ★ 为什么抽出来（`REQ-01M7372JNFAC2RWX2FXVV6YM5C`）：点 / 线光栅必须走与三角形**同一套**
//   深度测试 / alpha test / 混合 / 写掩码语义 —— 再抄一份公式，就会在"线的 α 怎么混"这类
//   地方与三角形分叉。
// ★ `shade` 在深度测试**之后**才调用：顺序照原像素循环（被深度挡住的像素既不采样、也不算
//   纹理级 / 插值）。它返回最终像素色（0..255，已 clamp）。
// ★ 返回 false = 这个像素没写（越界 / 深度测试 / alpha test 没过）；调用方据此跳过。
// ★ **越界检查在这里是唯一的兜底**：三角形外面还有 bbox∩scissor，而点 / 线的像素是 floor
//   出来的 —— 不查就会写穿 arena（静默损坏）。
// =====================================================================================
struct PixelRGBA {
    unsigned r = 0, g = 0, b = 0, a = 0;
};

template <typename Shade>
bool writeShadedPixel(DrawContext& ctx, int px, int py, double z, Shade&& shade) {
    if (px < ctx.scissor_x0 || px >= ctx.scissor_x1 || py < ctx.scissor_y0 || py >= ctx.scissor_y1)
        return false;
    std::size_t depth_off = static_cast<std::size_t>(py) * ctx.depth_pitch_floats +
                            static_cast<std::size_t>(px);
    // ★ 深度缓冲只有**后缓冲尺寸**（`createForDevice` 分配），而像素循环的上界是**渲染目标**
    //   尺寸（scissor 夹到 color 表面）⇒ RT > 后缓冲时 py/px 会越出深度缓冲的行/末尾，越界写
    //   会踩进 SurfaceArena 的相邻表面（静默损坏）。⇒ z 的读/写只在深度缓冲矩形内生效
    //   （**颜色照写**）。本线"共用一张设备深度缓冲"是明写的偏离（`setRenderTarget` 的
    //   pNewZStencil 分支）；RT 与深度尺寸不符时真机也没有可依赖的语义。
    const bool depth_in_range =
        ctx.z_enable && px >= 0 && py >= 0 && px < ctx.depth_width && py < ctx.depth_height;
    if (depth_in_range) {
        const float old = ctx.depth_base[depth_off];
        bool pass = false;
        switch (ctx.z_func) {
            case kCmpNever: pass = false; break;
            case kCmpLess: pass = z < static_cast<double>(old); break;
            case kCmpEqual: pass = z == static_cast<double>(old); break;
            case kCmpLessEqual: pass = z <= static_cast<double>(old); break;
            case kCmpGreater: pass = z > static_cast<double>(old); break;
            case kCmpNotEqual: pass = z != static_cast<double>(old); break;
            case kCmpGreaterEqual: pass = z >= static_cast<double>(old); break;
            case kCmpAlways: pass = true; break;
            default: pass = false; break;
        }
        if (!pass) return false;
    }
    const PixelRGBA c = shade();
    // ★ 诊断聚合（`[draw] fa=`）：这一批**真正参与混合**的 α 的均值/最小/最大。
    //   用途 = 分辨"α 是 0 / 是 TFACTOR 的 α / 是顶点 α"。只在 `LIFT_SLOW_DRAW=1` 时累积
    //   （静态缓存 ⇒ 每像素只是一次可预测分支），⛔ 不影响像素。
    static const bool alpha_agg_on = std::getenv("LIFT_SLOW_DRAW") != nullptr;
    if (alpha_agg_on) {
        ++g_alpha_agg.n;
        g_alpha_agg.sum += c.a;
        if (c.a < g_alpha_agg.mn) g_alpha_agg.mn = c.a;
        if (c.a > g_alpha_agg.mx) g_alpha_agg.mx = c.a;
    }
    // alpha test
    if (ctx.alpha_test) {
        const std::uint32_t ref = ctx.alpha_ref & 0xFFu;
        bool pass = false;
        switch (ctx.alpha_func) {
            case kCmpNever: pass = false; break;
            case kCmpLess: pass = c.a < ref; break;
            case kCmpEqual: pass = c.a == ref; break;
            case kCmpLessEqual: pass = c.a <= ref; break;
            case kCmpGreater: pass = c.a > ref; break;
            case kCmpNotEqual: pass = c.a != ref; break;
            case kCmpGreaterEqual: pass = c.a >= ref; break;
            case kCmpAlways: pass = true; break;
            default: pass = false; break;
        }
        if (!pass) return false;
    }
    std::uint8_t* dst = ctx.color_base + static_cast<std::size_t>(py) * ctx.color_pitch +
                        static_cast<std::size_t>(px) * ctx.color_fmt.bytesPerPixel();
    std::uint32_t out_r = c.r, out_g = c.g, out_b = c.b, out_a = c.a;
    if (ctx.blend) {
        const std::uint32_t dst_raw = pixel_format::loadPixel(dst, ctx.color_fmt.bpp);
        const unsigned dr = pixel_format::expandChannel(dst_raw, ctx.color_fmt.rmask);
        const unsigned dg = pixel_format::expandChannel(dst_raw, ctx.color_fmt.gmask);
        const unsigned db = pixel_format::expandChannel(dst_raw, ctx.color_fmt.bmask);
        const unsigned da = pixel_format::expandChannel(dst_raw, ctx.color_fmt.amask);
        unsigned sr_f = 0, sg_f = 0, sb_f = 0, sa_f = 0;
        blendFactors8(ctx.src_blend, c.r, c.g, c.b, c.a, dr, dg, db, da, &sr_f, &sg_f, &sb_f, &sa_f);
        unsigned dr_f = 0, dg_f = 0, db_f = 0, da_f = 0;
        blendFactors8(ctx.dst_blend, c.r, c.g, c.b, c.a, dr, dg, db, da, &dr_f, &dg_f, &db_f, &da_f);
        out_r = applyBlendOp(ctx.blend_op, c.r * sr_f, dr * dr_f);
        out_g = applyBlendOp(ctx.blend_op, c.g * sg_f, dg * dg_f);
        out_b = applyBlendOp(ctx.blend_op, c.b * sb_f, db * db_f);
        if (ctx.separate_alpha_blend) {
            unsigned sa2 = 0, da2 = 0, dummy1 = 0, dummy2 = 0;
            blendFactors8(ctx.src_blend_a, c.r, c.g, c.b, c.a, dr, dg, db, da, &sa2, &dummy1, &dummy2,
                          &dummy1);
            unsigned ds2 = 0;
            blendFactors8(ctx.dst_blend_a, c.r, c.g, c.b, c.a, dr, dg, db, da, &ds2, &dummy1, &dummy2,
                          &dummy1);
            out_a = applyBlendOp(ctx.blend_op_a, c.a * sa2, da * ds2);
        } else {
            out_a = applyBlendOp(ctx.blend_op, c.a * sa_f, da * da_f);
        }
    }
    const std::uint32_t mask = ctx.color_write & 0xFu;
    if ((mask & 0x1u) == 0) out_r = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.rmask);
    if ((mask & 0x2u) == 0) out_g = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.gmask);
    if ((mask & 0x4u) == 0) out_b = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp), ctx.color_fmt.bmask);
    if ((mask & 0x8u) == 0)
        out_a = pixel_format::expandChannel(pixel_format::loadPixel(dst, ctx.color_fmt.bpp),
                                            ctx.color_fmt.amask);
    if (mask != 0u) {
        pixel_format::storePixel(dst, ctx.color_fmt.bpp,
                                 pixel_format::packColor(ctx.color_fmt, out_r, out_g, out_b, out_a));
        // ★ 诊断（`[draw] bbox=`）：这一批**真的写进屏幕**的像素范围 —— 用来分辨
        //   "哪笔贡献了屏幕上的哪一段"（此前 `[draw]` 没有 bbox，A5-2 指出的诊断缺口）。
        //   只在 `LIFT_SLOW_DRAW=1` 时统计（与 `[draw] fa=` 同一个开关 ⇒ 热路径上只多一个
        //   可预测分支），⛔ 不改变任何像素。
        static const bool bbox_on = std::getenv("LIFT_SLOW_DRAW") != nullptr;
        if (bbox_on) {
            if (px < ctx.bbox_x0) ctx.bbox_x0 = px;
            if (py < ctx.bbox_y0) ctx.bbox_y0 = py;
            if (px + 1 > ctx.bbox_x1) ctx.bbox_x1 = px + 1;
            if (py + 1 > ctx.bbox_y1) ctx.bbox_y1 = py + 1;
        }
    }
    if (depth_in_range && ctx.z_write) ctx.depth_base[depth_off] = static_cast<float>(z);
    ++ctx.pixels_written;
    return true;
}

void rasterTriangle(DrawContext& ctx, const SVertex& va, const SVertex& vb, const SVertex& vc) {
    X86& cpu = *ctx.cpu;
    const double area = (vb.x - va.x) * (vc.y - va.y) - (vb.y - va.y) * (vc.x - va.x);
    if (!finite(area) || std::fabs(area) < 1e-12) return;
    // 正面 = 屏幕空间顺时针（y 向下时 area > 0）；CULLMODE 决定丢哪一侧
    if (area > 0.0) {
        if (ctx.cull == kCullCw) return;  // 剔除正面
    } else {
        if (ctx.cull == kCullCcw) return;  // 剔除背面
    }
    // 统一成 area > 0 的绕序（只影响插值符号，不影响可见性）
    const SVertex* p0 = &va;
    const SVertex* p1 = &vb;
    const SVertex* p2 = &vc;
    double a2 = area;
    if (a2 < 0.0) {
        p1 = &vc;
        p2 = &vb;
        a2 = -a2;
    }
    // 包围盒 ∩ 视口 ∩ 裁剪矩形
    double minx = p0->x < p1->x ? p0->x : p1->x;
    if (p2->x < minx) minx = p2->x;
    double maxx = p0->x > p1->x ? p0->x : p1->x;
    if (p2->x > maxx) maxx = p2->x;
    double miny = p0->y < p1->y ? p0->y : p1->y;
    if (p2->y < miny) miny = p2->y;
    double maxy = p0->y > p1->y ? p0->y : p1->y;
    if (p2->y > maxy) maxy = p2->y;
    int x0 = static_cast<int>(std::floor(minx));
    int x1 = static_cast<int>(std::ceil(maxx));
    int y0 = static_cast<int>(std::floor(miny));
    int y1 = static_cast<int>(std::ceil(maxy));
    if (x0 < ctx.scissor_x0) x0 = ctx.scissor_x0;
    if (y0 < ctx.scissor_y0) y0 = ctx.scissor_y0;
    if (x1 > ctx.scissor_x1) x1 = ctx.scissor_x1;
    if (y1 > ctx.scissor_y1) y1 = ctx.scissor_y1;
    if (x1 <= x0 || y1 <= y0) return;
    // 边函数（p0→p1→p2，area>0 ⇒ 内部为正）
    auto edge = [](const SVertex& a, const SVertex& b, double px, double py) {
        return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
    };
    // 左上规则：top = 水平边且 dx > 0；left = dy < 0（y 向下、area>0 的顺时针绕序）
    auto topLeft = [](const SVertex& a, const SVertex& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        if (dy < 0.0) return true;                 // left edge
        if (dy == 0.0 && dx > 0.0) return true;    // top edge
        return false;
    };
    const bool tl0 = topLeft(*p0, *p1);
    const bool tl1 = topLeft(*p1, *p2);
    const bool tl2 = topLeft(*p2, *p0);
    // =====================================================================================
    // ★ 派生 mip 的级号（每三角形常量；`REQ-01M73G2724479XAVS176WBZ65Q`）
    //   · LOD = log2(纹理足迹)，足迹 = 屏幕空间 UV 导数 × 该 **stage 自己**的纹理尺寸；
    //   · UV 导数用**透视校正后的 u（U/W）** —— 与像素循环里的插值同一个式子；
    //   · `MIPFILTER = NONE` ⇒ 只 level 0（D3D9 语义：mipmap 关掉）；`LIFT_NO_MIP=1` 同理（诊断对照臂）；
    //   · 链失效 / 算不出来 ⇒ 退回 level 0 并**记账**（`ctx.mip_level0_fallback`），⛔ 不静默。
    // =====================================================================================
    std::uint32_t stage_level[kMaxTextureStages] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    const bool mip_ok = mipSamplingEnabled();
    for (std::uint32_t s = 0; mip_ok && s < kMaxTextureStages; ++s) {
        const TextureStageSnapshot& ts = ctx.stages[s];
        if (ts.color_op == kTopDisable && ts.alpha_op == kTopDisable) break;
        TextureView& tv = ctx.textures[s];
        if (!tv.present || tv.width <= 0 || tv.height <= 0) continue;
        if (ctx.samplers[s].mip_filter == kTexfNone) continue;  // 关掉 mipmap ⇒ 只 level 0
        const std::uint32_t coord = (ts.tex_coord_index & 0xFFFFu) < kMaxTexCoords ? (ts.tex_coord_index & 0xFFFFu) : 0u;
        // ★ 级号只由"这一三角形的屏幕足迹 × 该 stage 的纹理尺寸"决定（`triangleMipLevel` 一处实现）。
        std::uint32_t level = triangleMipLevel(*p0, *p1, *p2, coord, tv.width, tv.height);
        if (level == 0u) continue;
        // 需要用 mip：本批第一次（内容版本未知时每次取链都要全量重算 ⇒ 每级每批只试一次）
        if (!ctx.mip_attempted[s]) {
            ctx.mip_attempted[s] = true;
            TextureInfo ti;
            std::string mip_why;
            if (!textureInfoOfState(cpu, tv.state, &ti, &mip_why)) {
                pipeWarn(PipeWarnCategory::Texture, "mip",
                         "纹理级 %u 的状态块读不出来（%s）⇒ 本批按 level 0 采样并记账", s, mip_why.c_str());
                ctx.mip_level0_fallback = true;
                continue;
            }
            bool rebuilt = false;
            const TextureMipChain* chain = ensureTextureMipChain(cpu, ti, &rebuilt, &mip_why);
            if (chain == nullptr) {
                pipeWarn(PipeWarnCategory::Texture, "mip",
                         "纹理级 %u（serial=%u，%dx%d）的派生 mip 链不可用：%s ⇒ 本批按 level 0 采样并记账", s,
                         tv.serial, tv.width, tv.height, mip_why.c_str());
                ctx.mip_level0_fallback = true;
                continue;
            }
            tv.mips = chain;
            if (rebuilt) ctx.mip_bytes_built += chain->bytes.size();
        }
        if (tv.mips == nullptr || level >= tv.mips->levels || tv.mips->bytes.empty()) {
            ctx.mip_level0_fallback = true;
            continue;
        }
        stage_level[s] = level;
        if (level < ctx.mip_level_min) ctx.mip_level_min = level;
        if (level > ctx.mip_level_max) ctx.mip_level_max = level;
    }
    for (int py = y0; py < y1; ++py) {
        const double cy = static_cast<double>(py) + 0.5;
        for (int px = x0; px < x1; ++px) {
            const double cx = static_cast<double>(px) + 0.5;
            double e0 = edge(*p0, *p1, cx, cy);
            double e1 = edge(*p1, *p2, cx, cy);
            double e2 = edge(*p2, *p0, cx, cy);
            // 填充规则
            if (!(e0 > 0.0 || (e0 == 0.0 && tl0))) continue;
            if (!(e1 > 0.0 || (e1 == 0.0 && tl1))) continue;
            if (!(e2 > 0.0 || (e2 == 0.0 && tl2))) continue;
            // ★ 边函数 E_i 是"边 i→i+1 对第三个顶点"的面积 ⇒ 权重是**轮换**的：
            //   w0 = e1/a2（v0 对面是边 v1→v2）、w1 = e2/a2、w2 = e0/a2。
            const double l0 = e1 / a2;
            const double l1 = e2 / a2;
            const double l2 = e0 / a2;
            const double wsum = l0 * p0->invw + l1 * p1->invw + l2 * p2->invw;
            const double invw_sum = wsum;
            if (invw_sum == 0.0) continue;
            const double z = l0 * p0->z + l1 * p1->z + l2 * p2->z;
            // ★ 深度测试 / alpha test / 混合 / 写掩码只在 `writeShadedPixel` 一处（点 / 线与
            //   三角形共用）；顶点属性 + 纹理级链放在 `shade` 里 ⇒ 被深度挡住的像素不采样
            //   （与抽取前的顺序一致：深度测试在插值 / 采样之前）。
            writeShadedPixel(ctx, px, py, z, [&]() -> PixelRGBA {
                // 顶点属性（透视校正）
                double u[kMaxTexCoords];
                double v[kMaxTexCoords];
                for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
                    u[t] = (l0 * p0->u[t] + l1 * p1->u[t] + l2 * p2->u[t]) / invw_sum;
                    v[t] = (l0 * p0->v[t] + l1 * p1->v[t] + l2 * p2->v[t]) / invw_sum;
                }
                StageColor diffuse;
                diffuse.c[0] = (l0 * p0->r + l1 * p1->r + l2 * p2->r) / invw_sum;
                diffuse.c[1] = (l0 * p0->g + l1 * p1->g + l2 * p2->g) / invw_sum;
                diffuse.c[2] = (l0 * p0->b + l1 * p1->b + l2 * p2->b) / invw_sum;
                diffuse.c[3] = (l0 * p0->a + l1 * p1->a + l2 * p2->a) / invw_sum;
                StageColor specular;
                specular.c[0] = (l0 * p0->sr + l1 * p1->sr + l2 * p2->sr) / invw_sum;
                specular.c[1] = (l0 * p0->sg + l1 * p1->sg + l2 * p2->sg) / invw_sum;
                specular.c[2] = (l0 * p0->sb + l1 * p1->sb + l2 * p2->sb) / invw_sum;
                specular.c[3] = (l0 * p0->sa + l1 * p1->sa + l2 * p2->sa) / invw_sum;
                if (ctx.shade == kShadeFlat) {
                    // FLAT：用 p0（D3D 的 provoking vertex 是第 0 个顶点；本线按此实现）
                    diffuse.c[0] = p0->r;
                    diffuse.c[1] = p0->g;
                    diffuse.c[2] = p0->b;
                    diffuse.c[3] = p0->a;
                    specular.c[0] = p0->sr;
                    specular.c[1] = p0->sg;
                    specular.c[2] = p0->sb;
                    specular.c[3] = p0->sa;
                    for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
                        u[t] = p0->u[t] / (p0->invw != 0.0 ? p0->invw : 1.0);
                        v[t] = p0->v[t] / (p0->invw != 0.0 ? p0->invw : 1.0);
                    }
                }
                // 纹理级链（★ 状态全来自 Draw 快照；⛔ 不再每像素读 guest）
                StageColor current = diffuse;
                for (std::uint32_t stage = 0; stage < kMaxTextureStages; ++stage) {
                    const TextureStageSnapshot& ts = ctx.stages[stage];
                    if (ts.color_op == kTopDisable && ts.alpha_op == kTopDisable) break;
                    StageColor texture;
                    texture.c[0] = texture.c[1] = texture.c[2] = texture.c[3] = 255.0;
                    std::uint32_t coord = ts.tex_coord_index & 0xFFFFu;
                    if (coord >= kMaxTexCoords) coord = 0;
                    const TextureView& tv = ctx.textures[stage];
                    if (tv.present) {
                        const SamplerSnapshot& ss = ctx.samplers[stage];
                        unsigned rgba[4];
                        sampleTexture(tv, ss.address_u, ss.address_v, ss.mag_filter, ss.min_filter,
                                      stage_level[stage], u[coord], v[coord], rgba);
                        texture.c[0] = static_cast<double>(rgba[0]);
                        texture.c[1] = static_cast<double>(rgba[1]);
                        texture.c[2] = static_cast<double>(rgba[2]);
                        texture.c[3] = static_cast<double>(rgba[3]);
                    }
                    StageColor c_out;
                    StageColor a_out;
                    applyTextureOp(cpu, ctx, stage, diffuse, specular, texture, current, &c_out, &a_out);
                    current.c[0] = c_out.c[0];
                    current.c[1] = c_out.c[1];
                    current.c[2] = c_out.c[2];
                    current.c[3] = a_out.c[3];
                }
                return PixelRGBA{clamp255(current.c[0]), clamp255(current.c[1]), clamp255(current.c[2]),
                                 clamp255(current.c[3])};
            });
        }
    }
}

// =====================================================================================
// 顶点读取 / 装配
// =====================================================================================
struct VertexSource {
    const std::uint8_t* data = nullptr;
    std::uint32_t stride = 0;
    std::uint32_t base_offset = 0;  // stream offset
    std::uint32_t count = 0;        // 可用顶点数（越界检查用）
};

bool readVertex(X86& cpu, const FvfLayout& fvf, const std::uint8_t* base, std::uint32_t ea, CVertex* out,
                std::string* why) {
    (void)cpu;
    auto f32 = [&](std::uint32_t off) {
        float f = 0.0f;
        std::memcpy(&f, base + off, sizeof(f));
        return static_cast<double>(f);
    };
    out->pretransformed = fvf.xyz_rhw;
    if (fvf.xyz_rhw) {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        const double rhw = f32(fvf.pos_offset + 12u);
        out->w = rhw != 0.0 ? 1.0 / rhw : 1.0;
    } else if (fvf.xyzw) {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        out->w = f32(fvf.pos_offset + 12u);
    } else {
        out->x = f32(fvf.pos_offset + 0u);
        out->y = f32(fvf.pos_offset + 4u);
        out->z = f32(fvf.pos_offset + 8u);
        out->w = 1.0;
    }
    for (std::uint32_t t = 0; t < fvf.tex_count; ++t) {
        out->u[t] = f32(fvf.tex_offset[t] + 0u);
        out->v[t] = fvf.tex_size[t] >= 2u ? f32(fvf.tex_offset[t] + 4u) : 0.0;
    }
    if (fvf.has_diffuse) {
        const std::uint8_t* p = base + fvf.diffuse_offset;
        const std::uint32_t c = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) |
                                (static_cast<std::uint32_t>(p[3]) << 24);
        splitColor(c, &out->a, &out->r, &out->g, &out->b);
    }
    if (fvf.has_specular) {
        const std::uint8_t* p = base + fvf.specular_offset;
        const std::uint32_t c = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                (static_cast<std::uint32_t>(p[2]) << 16) |
                                (static_cast<std::uint32_t>(p[3]) << 24);
        splitColor(c, &out->sa, &out->sr, &out->sg, &out->sb);
    }
    (void)ea;
    (void)why;
    return true;
}

// =====================================================================================
// 裁剪（偏差 #5：⛔ 不再只有近平面）
//
// D3D 的**齐次裁剪空间**约定：0 <= z <= w、-w <= x <= w、-w <= y <= w。
//   · XYZ / XYZW 顶点：对 6 个半空间做 Sutherland–Hodgman 裁剪（保持绕序）：
//       近 z>=0 · 远 w-z>=0 · 左 x+w>=0 · 右 w-x>=0 · 下 y+w>=0 · 上 w-y>=0。
//     `w<0`（相机背后）的点必然落在远/左/右…之外 ⇒ 整体被裁掉（旧实现只裁近平面时
//     会拿负 w 做投影，落到错误位置）。
//   · XYZRHW 顶点：x/y **已经是屏幕坐标**，齐次面在数学上不适用（`w=1/rhw` 与屏幕 x/y 无关）；
//     真机对预变换顶点是在**屏幕空间裁到视口矩形**，这里照做；z 仍是 [0,1] 深度
//     （`toScreen` 不对预变换顶点缩放 MinZ/MaxZ，所以远平面就是 z<=1）。
//
// ⚠ 明确未做（⛔ 不静默）：用户裁剪面（D3DRS_CLIPPLANEENABLE）仍由 `recordUnsupported` 记录；
//   guard band 没有单独实现（精确裁剪已经让屏幕外几何不进光栅，效果等价）。
// =====================================================================================
CVertex lerpVertex(const CVertex& a, const CVertex& b, double t) {
    CVertex m;
    m.pretransformed = a.pretransformed;  // 同一批顶点同为预变换（FVF 是每次 Draw 一份）
    m.x = a.x + (b.x - a.x) * t;
    m.y = a.y + (b.y - a.y) * t;
    m.z = a.z + (b.z - a.z) * t;
    m.w = a.w + (b.w - a.w) * t;
    for (std::uint32_t k = 0; k < kMaxTexCoords; ++k) {
        m.u[k] = a.u[k] + (b.u[k] - a.u[k]) * t;
        m.v[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
    }
    m.r = a.r + (b.r - a.r) * t;
    m.g = a.g + (b.g - a.g) * t;
    m.b = a.b + (b.b - a.b) * t;
    m.a = a.a + (b.a - a.a) * t;
    m.sr = a.sr + (b.sr - a.sr) * t;
    m.sg = a.sg + (b.sg - a.sg) * t;
    m.sb = a.sb + (b.sb - a.sb) * t;
    m.sa = a.sa + (b.sa - a.sa) * t;
    return m;
}

// 单个半空间：dist(v) >= 0 为内侧。`in` / `out` 不得别名。
template <typename Dist>
std::uint32_t clipHalfSpace(const CVertex* in, std::uint32_t n, Dist dist, CVertex* out) {
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        const CVertex& a = in[i];
        const CVertex& b = in[(i + 1u) % n];
        const double da = dist(a);
        const double db = dist(b);
        if (da >= 0.0) {
            if (count < kMaxClipVerts) out[count++] = a;
        }
        if ((da >= 0.0) != (db >= 0.0)) {
            const double t = da / (da - db);
            if (count < kMaxClipVerts) out[count++] = lerpVertex(a, b, t);
        }
    }
    return count;
}

// 凸多边形过一串平面。三角形最多变成 3+6=9 个顶点（< kMaxClipVerts=16）。
template <typename... Dist>
std::uint32_t clipAgainst(const CVertex* in, std::uint32_t n, CVertex* out, Dist... dists) {
    CVertex buf_a[kMaxClipVerts];
    CVertex buf_b[kMaxClipVerts];
    CVertex* cur = buf_a;
    CVertex* next = buf_b;
    const std::uint32_t start = n < kMaxClipVerts ? n : kMaxClipVerts;
    for (std::uint32_t i = 0; i < start; ++i) cur[i] = in[i];
    n = start;
    auto pass = [&](auto dist) {
        n = clipHalfSpace(cur, n, dist, next);
        CVertex* tmp = cur;
        cur = next;
        next = tmp;
    };
    (pass(dists), ...);
    for (std::uint32_t i = 0; i < n; ++i) out[i] = cur[i];
    return n;
}

// XYZ / XYZW：齐次裁剪空间 6 个面
std::uint32_t clipHomogeneous(const CVertex* in, std::uint32_t n, CVertex* out) {
    return clipAgainst(in, n, out, [](const CVertex& v) { return v.z; },        // 近
                       [](const CVertex& v) { return v.w - v.z; },              // 远
                       [](const CVertex& v) { return v.x + v.w; },              // 左
                       [](const CVertex& v) { return v.w - v.x; },              // 右
                       [](const CVertex& v) { return v.y + v.w; },              // 下
                       [](const CVertex& v) { return v.w - v.y; });             // 上
}

// XYZRHW：屏幕空间裁到**视口矩形**（x/y）+ 深度 [0,1]（z）
std::uint32_t clipScreen(const CVertex* in, std::uint32_t n, const DrawContext& ctx, CVertex* out) {
    if (ctx.viewport_w <= 0 || ctx.viewport_h <= 0) return 0;
    const double x0 = static_cast<double>(ctx.viewport_x);
    const double y0 = static_cast<double>(ctx.viewport_y);
    const double x1 = x0 + static_cast<double>(ctx.viewport_w);
    const double y1 = y0 + static_cast<double>(ctx.viewport_h);
    return clipAgainst(in, n, out, [x0](const CVertex& v) { return v.x - x0; },
                       [x1](const CVertex& v) { return x1 - v.x; },
                       [y0](const CVertex& v) { return v.y - y0; },
                       [y1](const CVertex& v) { return y1 - v.y; },
                       [](const CVertex& v) { return v.z; },        // 近
                       [](const CVertex& v) { return 1.0 - v.z; }); // 远
}

bool toScreen(const CVertex& v, const DrawContext& ctx, SVertex* out) {
    if (v.pretransformed) {
        out->x = v.x;
        out->y = v.y;
        out->z = v.z;
        const double invw = v.w != 0.0 ? 1.0 / v.w : 1.0;  // w = 1/rhw ⇒ invw = rhw
        out->invw = invw;
        for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
            out->u[t] = v.u[t] * invw;
            out->v[t] = v.v[t] * invw;
        }
        out->r = v.r * invw;
        out->g = v.g * invw;
        out->b = v.b * invw;
        out->a = v.a * invw;
        out->sr = v.sr * invw;
        out->sg = v.sg * invw;
        out->sb = v.sb * invw;
        out->sa = v.sa * invw;
        return true;
    }
    if (!finite(v.w) || std::fabs(v.w) < 1e-12) return false;
    const double invw = 1.0 / v.w;
    out->x = (v.x * invw * 0.5 + 0.5) * static_cast<double>(ctx.viewport_w) + static_cast<double>(ctx.viewport_x);
    out->y = (-v.y * invw * 0.5 + 0.5) * static_cast<double>(ctx.viewport_h) + static_cast<double>(ctx.viewport_y);
    out->z = v.z * invw;
    out->invw = invw;
    for (std::uint32_t t = 0; t < kMaxTexCoords; ++t) {
        out->u[t] = v.u[t] * invw;
        out->v[t] = v.v[t] * invw;
    }
    out->r = v.r * invw;
    out->g = v.g * invw;
    out->b = v.b * invw;
    out->a = v.a * invw;
    out->sr = v.sr * invw;
    out->sg = v.sg * invw;
    out->sb = v.sb * invw;
    out->sa = v.sa * invw;
    return true;
}

// =====================================================================================
// 点 / 线图元（`REQ-01M7372JNFAC2RWX2FXVV6YM5C`）
//
// ★ 线段像素规则 = **diamond-exit 的主轴步进实现**（D3D9 文档规则）：
//   · 主轴 = |dx| >= |dy| ? x : y；主轴像素索引从 floor(起点) 到 floor(终点)**含两端**，
//     每个主轴列落**一个**像素；步进按符号（LINELIST 的顶点顺序可能右→左 / 下→上）。
//   · 次轴 = 该列像素中心处线段值所在的像素（`floor`，= 最近像素中心）；段参数
//     `t = (主轴像素中心 − 主轴起点) / 主轴跨度`（clamp 到 [0,1]）。
//   · 颜色 / z 在 t 处插值：z 用屏幕空间线性（`SVertex.z` 已是 z/w）；颜色用与三角形
//     **同一个式子** `(Σ t·c) / (Σ t·invw)`（`SVertex` 的颜色已乘过 invw）。
//   · 为什么选它：① 真机 D3D9 的文档规则就是 diamond-exit，|slope|≤1 时"每列取该列中心处
//     的行"与 exit-point 记账等价；② 线宽**恒 1px**（D3D9 固定功能没有线宽 state）——
//     "垂直距离 ≤ 0.5px"那种规则会在整数屏幕坐标处同时覆盖相邻两行（变 2px）；
//     ③ 每段像素数 = 主轴覆盖列数 ⇒ 无缺口、可预测。残差：精确 45°/端点所属像素是实现
//     定义（微软文档允许）；LASTPIXEL=0 仍画最后一像素（记账，见 `submitPrimitives`）。
// ★ 裁剪：线段用参数区间（Liang-Barsky 式；`dist` 对 t 是线性的），面与 `clipHomogeneous` /
//   `clipScreen` **同一组**（XYZ 六齐次面；XYZRHW 视口矩形 + z∈[0,1]）。⛔ 不用 `clipAgainst`：
//   它把输入当**闭合多边形**，2 个顶点会退化成 a→b→a。
// ★ 点也裁剪（w<0 的点不裁会被 `toScreen` 镜像到错误位置）；落 1px（`floor(x), floor(y)`）。
// ⚠ 未做（⛔ 不静默）：线宽 / 抗锯齿线；点尺寸（POINTSIZE / POINTSCALE / POINTSPRITE /
//   FVF PSIZE）—— 按 1px 画并在 `submitPrimitives` 里 `recordUnsupported`。
// =====================================================================================
bool clipSegmentEndpoints(const DrawContext& ctx, const CVertex& a, const CVertex& b, CVertex* out_a,
                          CVertex* out_b) {
    double t0 = 0.0;
    double t1 = 1.0;
    auto pass = [&](auto dist) {
        const double da = dist(a);
        const double db = dist(b);
        if (da >= 0.0 && db >= 0.0) return true;
        if (da < 0.0 && db < 0.0) return false;
        const double t = da / (da - db);  // 穿点（与 clipHalfSpace 同一个式子）
        if (da >= 0.0) {
            if (t < t1) t1 = t;
        } else if (t > t0) {
            t0 = t;
        }
        return t0 <= t1;
    };
    bool ok = true;
    if (a.pretransformed) {
        if (ctx.viewport_w <= 0 || ctx.viewport_h <= 0) return false;
        const double x0 = static_cast<double>(ctx.viewport_x);
        const double y0 = static_cast<double>(ctx.viewport_y);
        const double x1 = x0 + static_cast<double>(ctx.viewport_w);
        const double y1 = y0 + static_cast<double>(ctx.viewport_h);
        ok = pass([x0](const CVertex& v) { return v.x - x0; }) &&
             pass([x1](const CVertex& v) { return x1 - v.x; }) &&
             pass([y0](const CVertex& v) { return v.y - y0; }) &&
             pass([y1](const CVertex& v) { return y1 - v.y; }) &&
             pass([](const CVertex& v) { return v.z; }) &&
             pass([](const CVertex& v) { return 1.0 - v.z; });
    } else {
        ok = pass([](const CVertex& v) { return v.z; }) &&
             pass([](const CVertex& v) { return v.w - v.z; }) &&
             pass([](const CVertex& v) { return v.x + v.w; }) &&
             pass([](const CVertex& v) { return v.w - v.x; }) &&
             pass([](const CVertex& v) { return v.y + v.w; }) &&
             pass([](const CVertex& v) { return v.w - v.y; });
    }
    if (!ok || t0 > t1) return false;
    *out_a = lerpVertex(a, b, t0);
    *out_b = lerpVertex(a, b, t1);
    return true;
}

bool clipPointVertex(const DrawContext& ctx, const CVertex& v) {
    if (v.pretransformed) {
        if (ctx.viewport_w <= 0 || ctx.viewport_h <= 0) return false;
        const double x0 = static_cast<double>(ctx.viewport_x);
        const double y0 = static_cast<double>(ctx.viewport_y);
        const double x1 = x0 + static_cast<double>(ctx.viewport_w);
        const double y1 = y0 + static_cast<double>(ctx.viewport_h);
        return v.x >= x0 && v.x <= x1 && v.y >= y0 && v.y <= y1 && v.z >= 0.0 && v.z <= 1.0;
    }
    return v.z >= 0.0 && v.w - v.z >= 0.0 && v.x + v.w >= 0.0 && v.w - v.x >= 0.0 &&
           v.y + v.w >= 0.0 && v.w - v.y >= 0.0;
}

// 点：1px（顶点色 + 屏幕 z），像素收尾走与三角形同一份助手。
void rasterizePoint(DrawContext& ctx, const CVertex& v) {
    if (!clipPointVertex(ctx, v)) return;
    SVertex sv;
    if (!toScreen(v, ctx, &sv)) return;
    const PixelRGBA c{clamp255(v.r), clamp255(v.g), clamp255(v.b), clamp255(v.a)};
    const int px = static_cast<int>(std::floor(sv.x));
    const int py = static_cast<int>(std::floor(sv.y));
    writeShadedPixel(ctx, px, py, sv.z, [c]() { return c; });
}

void rasterizeLineSegment(DrawContext& ctx, const CVertex& ca, const CVertex& cb) {
    CVertex a;
    CVertex b;
    if (!clipSegmentEndpoints(ctx, ca, cb, &a, &b)) return;
    SVertex sa;
    SVertex sb;
    // w≈0 与三角形同口径：整段丢（⛔ 不拿坏坐标继续）
    if (!toScreen(a, ctx, &sa) || !toScreen(b, ctx, &sb)) return;
    const double dx = sb.x - sa.x;
    const double dy = sb.y - sa.y;
    if (std::fabs(dx) < 1e-12 && std::fabs(dy) < 1e-12) {
        // 退化线段 ⇒ 单像素（与点同口径：取裁剪后起点的顶点色）
        const PixelRGBA c{clamp255(a.r), clamp255(a.g), clamp255(a.b), clamp255(a.a)};
        writeShadedPixel(ctx, static_cast<int>(std::floor(sa.x)), static_cast<int>(std::floor(sa.y)), sa.z,
                         [c]() { return c; });
        return;
    }
    const bool major_x = std::fabs(dx) >= std::fabs(dy);
    const double m0 = major_x ? sa.x : sa.y;
    const double m1 = major_x ? sb.x : sb.y;
    const int first = static_cast<int>(std::floor(m0));
    const int last = static_cast<int>(std::floor(m1));
    // ★ 方向：顶点顺序可能右→左 / 下→上 ⇒ 步进按符号；否则 `p <= last` 一个像素都不画。
    const int dir = (last >= first) ? 1 : -1;
    for (int p = first;; p += dir) {
        const double major_center = static_cast<double>(p) + 0.5;
        double t = (m1 != m0) ? (major_center - m0) / (m1 - m0) : 0.0;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const double minor_center =
            (major_x ? sa.y : sa.x) + t * (major_x ? (sb.y - sa.y) : (sb.x - sa.x));
        const int q = static_cast<int>(std::floor(minor_center));  // 最近像素中心
        const int px = major_x ? p : q;
        const int py = major_x ? q : p;
        const double wsum = (1.0 - t) * sa.invw + t * sb.invw;
        if (finite(wsum) && wsum != 0.0) {
            // z：屏幕空间线性（真机对线段的深度就是屏幕线性）；颜色：与三角形同一个透视校正式。
            const double z = (1.0 - t) * sa.z + t * sb.z;
            const PixelRGBA c{
                clamp255(((1.0 - t) * sa.r + t * sb.r) / wsum),
                clamp255(((1.0 - t) * sa.g + t * sb.g) / wsum),
                clamp255(((1.0 - t) * sa.b + t * sb.b) / wsum),
                clamp255(((1.0 - t) * sa.a + t * sb.a) / wsum),
            };
            writeShadedPixel(ctx, px, py, z, [c]() { return c; });
        }
        if (p == last) break;
    }
}

}  // namespace

// =====================================================================================
// 状态块生命周期
// =====================================================================================
std::uint32_t createForDevice(X86& cpu, std::uint32_t device_state, int width, int height, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪（流水线状态块要放在 guest 堆上）";
        return 0;
    }
    if (width <= 0 || height <= 0) {
        if (why) *why = "D3D9 流水线：后缓冲尺寸非法";
        return 0;
    }
    const std::uint32_t pe = cpu.heap->alloc(kPlBytes, /*zero=*/true);
    if (pe == 0) {
        if (why) *why = std::string("D3D9 流水线：guest 堆分配状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    initDefaults(cpu, pe, width, height);
    // 深度缓冲：SurfaceArena 里的 32bpp 表面（像素存 float 位模式）。清成 1.0f（远平面）。
    // ★ 格式走 pixel_format 的唯一一份（⛔ 不再在这里手抄 BGRA8888 的四个掩码）
    const SurfaceFormat depth_fmt = pixel_format::formatForBpp(32);
    const std::uint32_t depth = m->surfaces().create(width, height, depth_fmt);
    if (depth == 0) {
        cpu.heap->free(pe);
        if (why) *why = std::string("D3D9 流水线：深度缓冲分配失败：") + m->surfaces().lastError();
        return 0;
    }
    GuestSurface* dgs = m->surfaces().find(depth);
    if (!dgs) {
        m->surfaces().destroy(depth);
        cpu.heap->free(pe);
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return 0;
    }
    std::uint8_t* dp = m->surfaces().pixels(*dgs);
    if (!dp) {
        m->surfaces().destroy(depth);
        cpu.heap->free(pe);
        if (why) *why = "D3D9 流水线：深度缓冲像素指针为空";
        return 0;
    }
    const float one = 1.0f;
    std::uint32_t one_bits = 0;
    std::memcpy(&one_bits, &one, sizeof(one_bits));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            std::uint8_t* p = dp + static_cast<std::size_t>(y) * dgs->pitch + static_cast<std::size_t>(x) * 4u;
            std::memcpy(p, &one_bits, 4u);
        }
    }
    plSet(cpu, pe, kPlDepthArena, depth);
    plSet(cpu, pe, kPlDevice, device_state);
    wrU32(cpu, device_state + 56u, pe);
    pipeLog("Init",
            "流水线状态块 0x%08X（'DL01'，%u B）· 深度缓冲 0x%08X（%dx%d，float）· 视口 0..%d/0..%d",
            pe, kPlBytes, depth, width, height, width, height);
    return pe;
}

namespace {
// 释放流水线**当前绑定的渲染目标**，并把 `kPlColorSurface` / `kPlColorArena` 一起清成"后缓冲"。
//
// ★ 成对的两份引用（REQ-01M6X4N7C0PAF6XT7EAC172V85）：
//   ① `surface` 对象 —— 绑定时 `addRef` 过；
//   ② **像素块 owner**（= 父纹理对象，`ComRegistry::ownerOf(surface)`）—— 也 `addRef` 过。
//   ⛔ 只 addRef ① 不够：surface 没有析构钩子（`com.h` 文件头），像素块 + `'TEX1'` 状态块的唯一
//   回收入口是**纹理对象** refcount 到 0 时的 `d3d9::destroyTextureState` ⇒ 纹理在"仍被绑定为 RT"
//   时被 guest Release 会把像素块回收掉，流水线继续往已释放 / 已被别人复用的 arena 写。
// ★ 顺序：**先 owner 再 surface**（同一张纹理的两个表面互切时，新 owner 已在调用点先 addRef，
//   先放旧 owner 也不会把它降到 0）；owner 先到 0 会回收像素块，那时 surface 的 `user` 已经是
//   悬空 EA，但 release(surface) 不读 `user`，所以顺序安全。
// ★ **owner 为什么要走 `releaseIf`（带代际）**（REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）：
//   `Object::owner` 是"我代表谁"的 EA 缓存。04C 之后 surface **自己也替父纹理持一份引用**
//   （`owns_owner`，见 `com.h`：`GetSurfaceLevel` 建 surface 时就 addRef）⇒ **合法时序**里
//   owner 死不了、EA 也不会被复用。但 guest **越界 Release**（把 surface 与设备那几份引用也放掉）
//   之后 owner 会死，它的 EA 被**下一个对象**拿走（guest 堆首次适配，确定性）。绑定期那份 addRef
//   让它平时减不到别人身上；但引用记账只是"数值上钉住"，不是"身份校验" ⇒ 一旦谁少放一次，
//   按 EA 的 `release` 就会减到**当前占有者**身上。代际在**建 surface 时**固化，与 EA 一起
//   过 release→reuse 都还有效 ⇒ `releaseIf` 不符时只出声、不减引用。
//   ⚠ 边界：`kPlColorSurface` 自己（下面那一行 `release(surface)`）**不带代际** —— 它由绑定期的
//   addRef 钉住（对象在表里 ⇒ EA 不会被复用），只有 guest 越界 Release 才可能陈旧；那一类
//   对所有"已钉住"的缓存一样（见报告里的审计表），且流水线块在 guest 内存里、没有宿主侧
//   per-surface 槽位可放代际 ⇒ 本单不为它新造一张宿主映射表。
//   ★ `owner_generation != 0` 是**绑定的前提**（`setRenderTarget` 的判据拒绝 0）⇒ 进到这里时它一定非 0，
//     所以不存在"没有代际 ⇒ 退化成不可校验的 release"这条路。
// ★ ⛔ 只在"换 target / 解绑到后缓冲 / 设备销毁"三处调用。**不在纹理 Release 处**调用 ——
//   那会把仍在绑定中的 RT 静默换成后缓冲（改语义）。
void releaseRenderTarget(X86& cpu, std::uint32_t pe) {
    const std::uint32_t surface = plGet(cpu, pe, kPlColorSurface);
    const std::uint32_t owner = (cpu.com != nullptr && surface != 0) ? cpu.com->ownerOf(surface) : 0u;
    const std::uint32_t owner_generation =
        (cpu.com != nullptr && surface != 0) ? cpu.com->ownerGenerationOf(surface) : 0u;
    plSet(cpu, pe, kPlColorSurface, 0u);
    plSet(cpu, pe, kPlColorArena, 0u);
    if (cpu.com == nullptr) return;
    if (owner != 0 && !cpu.com->releaseIf(cpu, owner, owner_generation)) {
        // 代际不符（EA 已被新对象拿走）或 owner 已不在表里 ⇒ ⛔ 不减任何人的引用。
        pipeLog("releaseRenderTarget",
                "owner 0x%08X（建 surface 时的代际 %u）已不是那个对象 ⇒ ⛔ 不释放它（新占有者不受影响）",
                owner, owner_generation);
    }
    if (surface != 0) cpu.com->release(cpu, surface);
}
}  // namespace

void destroyForDevice(X86& cpu, std::uint32_t device_state) {
    if (device_state == 0 || !cpu.mem || !cpu.mem->committed(device_state, 64u)) return;
    const std::uint32_t pe = pipeEaOf(cpu, device_state);
    if (pe == 0 || !cpu.mem->committed(pe, kPlBytes)) return;
    if (cpu.mem->rd32(pe + kPlMagic) != kPlMagicValue) return;
    // ★ 先取 owner，再清 magic（REQ-01M6X4MSCMMFEXYXCSK6V61QR2）：清完 magic 就不会有第二次
    //   回收机会；owner 来自装配（`machineOf`），不是"最近 init 的那台"。
    Machine* m = machineOf(cpu);
    // 绑定的纹理在 SetTexture 时 AddRef 过 ⇒ 这里对称释放；渲染目标同理（surface 对象 + 像素块 owner）。
    // ★ 顺序（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 2）：**先**声明 / 流 / 索引，**再**纹理，**最后** RT。
    //   前一组是"设备替 guest 持的绑定引用"（段 2 新增），与后两组不相交；全部走 `releaseBound`
    //   （= `releaseIf` 带代际）⇒ 期间 guest 越界 Release 造成的 EA 复用只会出声、⛔ 不减到新占有者。
    //   ★ 此时设备对象**已从对象表删掉**（`com.cpp` 先 erase 再跑 on_zero）⇒ 这条路径不许再 `find(device)`。
    if (cpu.com) {
        releaseBound(cpu, plGet(cpu, pe, kPlVertexDecl), plGet(cpu, pe, kPlVertexDeclGen),
                     "设备销毁/顶点声明", PipeWarnCategory::Other);
        plSet(cpu, pe, kPlVertexDecl, 0u);
        plSet(cpu, pe, kPlVertexDeclGen, 0u);
        for (std::uint32_t s = 0; s < kMaxStreams; ++s) {
            const std::uint32_t slot = kPlStreams + s * 16u;
            releaseBound(cpu, plGet(cpu, pe, slot + 0u), plGet(cpu, pe, slot + 12u), "设备销毁/流绑定",
                         PipeWarnCategory::VertexStream);
            plSet(cpu, pe, slot + 0u, 0u);
            plSet(cpu, pe, slot + 12u, 0u);
        }
        releaseBound(cpu, plGet(cpu, pe, kPlIndices), plGet(cpu, pe, kPlIndicesGen), "设备销毁/索引绑定",
                     PipeWarnCategory::Index);
        plSet(cpu, pe, kPlIndices, 0u);
        plSet(cpu, pe, kPlIndicesGen, 0u);
        for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
            // ★ 纹理也走 `releaseBound`（= `releaseIf` 带代际）：绑定期 guest 越界 Release 之后
            //   槽里的 EA 可能已经被**另一张**纹理拿走 ⇒ 裸 `release` 会减到新占有者身上。
            releaseBound(cpu, plGet(cpu, pe, kPlTextures + s * 4u), plGet(cpu, pe, kPlTextureGens + s * 4u),
                         "设备销毁/纹理绑定", PipeWarnCategory::Texture);
            plSet(cpu, pe, kPlTextures + s * 4u, 0u);
            plSet(cpu, pe, kPlTextureGens + s * 4u, 0u);
        }
        releaseRenderTarget(cpu, pe);
    }
    const std::uint32_t depth = plGet(cpu, pe, kPlDepthArena);
    cpu.mem->wr32(pe + kPlMagic, 0u);
    if (m != nullptr && depth != 0) m->surfaces().destroy(depth);
    if (cpu.heap) cpu.heap->free(pe);
    cpu.mem->wr32(device_state + 56u, 0u);
}

std::uint32_t pipelineOfDevice(X86& cpu, std::uint32_t device_state) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, device_state, &pe, nullptr)) return 0;
    return pe;
}

bool pipelineInfoOfState(X86& cpu, std::uint32_t state, PipelineInfo* out, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kPlBytes) || cpu.mem->rd32(state + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块的流水线状态块";
        return false;
    }
    PipelineInfo i;
    i.state = state;
    i.device = plGet(cpu, state, kPlDevice);
    i.in_scene = plGet(cpu, state, kPlInScene) != 0;
    i.fvf = plGet(cpu, state, kPlFvf);
    i.vertex_decl = plGet(cpu, state, kPlVertexDecl);
    i.depth_arena = plGet(cpu, state, kPlDepthArena);
    i.color_arena = plGet(cpu, state, kPlColorArena);
    i.color_surface = plGet(cpu, state, kPlColorSurface);
    i.begins = plGet(cpu, state, kPlBegins);
    i.ends = plGet(cpu, state, kPlEnds);
    i.draws = plGet(cpu, state, kPlDraws);
    i.draw_calls = plGet(cpu, state, kPlDrawCalls);
    i.stage_snapshots = plGet(cpu, state, kPlStageSnapshots);
    i.triangles = plGet(cpu, state, kPlTriangles);
    i.rasterized = plGet(cpu, state, kPlRasterized);
    i.pixels = (static_cast<std::uint64_t>(plGet(cpu, state, kPlPixels + 4u)) << 32) |
               plGet(cpu, state, kPlPixels);
    i.viewport_x = plGet(cpu, state, kPlViewport + 0u);
    i.viewport_y = plGet(cpu, state, kPlViewport + 4u);
    i.viewport_w = plGet(cpu, state, kPlViewport + 8u);
    i.viewport_h = plGet(cpu, state, kPlViewport + 12u);
    *out = i;
    return true;
}

bool pipelineInfoOfDevice(X86& cpu, std::uint32_t device_state, PipelineInfo* out, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, device_state, &pe, why)) return false;
    return pipelineInfoOfState(cpu, pe, out, why);
}

// =====================================================================================
// 渲染目标（SetRenderTarget）
// =====================================================================================
std::uint32_t renderTargetArena(X86& cpu, std::uint32_t pipeline_state) {
    if (pipeline_state == 0 || !cpu.mem->committed(pipeline_state, kPlBytes) ||
        cpu.mem->rd32(pipeline_state + kPlMagic) != kPlMagicValue) {
        return 0;
    }
    return plGet(cpu, pipeline_state, kPlColorArena);
}

namespace {
// D3D9：**绑定新 render target 会把 viewport 设成新目标的全幅**
// （IDirect3DDevice9::SetRenderTarget 的 Remarks）。本引擎只在 Reset 之后设一次全屏 viewport
// （`0x4988D1-0x498910`），之后每次渲染到纹理（`0x4A778B` / `0x4A515E` / `0x4A521B`）都靠这条
// ⇒ 不回写就按**旧**视口做 NDC→屏幕映射（`toScreen`）与 XYZRHW 的视口裁剪，渲染到纹理的那一段
// 整块错位/被裁（REQ-01M7372FBT0WBXAS2Z1NESBRFG）。
// ★ 只写 X/Y/W/H；`MinZ/MaxZ`（+16/+20）**不动**（本线只把它们原样存取，从不消费）。
// ★ w/h == 0 不写：`SetViewport` 把 0 拒成 D3DERR_INVALIDCALL（见该函数），写出 guest 造不出来的
//   状态会让视口裁剪把一切都裁掉。尺寸的来源与调用点同源（纹理表面 = `TextureInfo`，
//   后缓冲 = `DeviceInfo`），⛔ 不在这里另读一份表面状态块。
void writeViewportFullTarget(X86& cpu, std::uint32_t pe, std::uint32_t w, std::uint32_t h) {
    if (w == 0 || h == 0) return;
    plSet(cpu, pe, kPlViewport + 0u, 0u);
    plSet(cpu, pe, kPlViewport + 4u, 0u);
    plSet(cpu, pe, kPlViewport + 8u, w);
    plSet(cpu, pe, kPlViewport + 12u, h);
}
}  // namespace

std::uint32_t setRenderTarget(X86& cpu, std::uint32_t dev, std::uint32_t target, std::uint32_t zstencil,
                              std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    const std::uint32_t old_surface = plGet(cpu, pe, kPlColorSurface);
    // ★ 屏障（屏障清单「换 target」）：切换前先把已入队的 draw 画进**旧**目标并回读旧目标的
    //   guest 像素。理由：D3D9 允许换成另一张同尺寸 surface，后端只看尺寸/格式认不出来；
    //   而且切完之后即使一条 draw 都没有，Present 也不能把旧镜像的内容读进新后缓冲。
    // ★ 只回读"镜像真的对应该旧目标"的情况：`rasterSyncToGuest` 会按基址校验，若镜像属于另一块
    //   surface（例如纹理 RT 走了 CPU 回退、镜像仍是后缓冲），这里**不能**整帧往回读。
    //   `target == old_surface`（重复设同一个目标）也不是换 target，⛔ 不做整帧读回。
    if (target != old_surface) {
        if (Machine* m = activeMachine(); m != nullptr && &m->cpu() == &cpu) {
            if (GpuRaster* gpu = m->gpuRaster(); gpu != nullptr && gpu->rasterActive() && gpu->rasterDirty()) {
                DeviceInfo di;
                if (deviceInfoOfState(cpu, dev, &di, nullptr)) {
                    // 旧目标 = 旧 RT surface（arena）或 0 ⇒ 设备后缓冲
                    const std::uint32_t old_arena = old_surface != 0 ? plGet(cpu, pe, kPlColorArena) : 0u;
                    GuestSurface* ogs = m->surfaces().find(old_arena != 0 ? old_arena : di.arena);
                    if (ogs != nullptr) {
                        std::uint8_t* old_base = m->surfaces().pixels(*ogs);
                        const bool is_mirror_target =
                            gpu->rasterTargetBase() == nullptr || gpu->rasterTargetBase() == old_base;
                        if (is_mirror_target) {
                            std::string sync_err;
                            if (!gpu->rasterSyncToGuest(old_base, ogs->pitch, ogs->width, ogs->height, &sync_err)) {
                                pipeLog("SetRenderTarget", "切换前 GPU 像素读回失败：%s", sync_err.c_str());
                            }
                        } else {
                            // 镜像属于别的 surface：只把队列落进它自己的 target（不回读、不静默写错）。
                            std::string flush_err;
                            if (!gpu->rasterFlush(&flush_err)) {
                                pipeLog("SetRenderTarget", "切换前 GPU 队列 flush 失败：%s", flush_err.c_str());
                            }
                        }
                    }
                }
            }
        }
    }
    if (target == 0) {
        if (cpu.com) releaseRenderTarget(cpu, pe);
        // ★ viewport 回写成**新目标的全幅**（D3D9 语义）：`target == 0` 在本线代表后缓冲
        //   ⇒ 后缓冲尺寸。尺寸与 `surfaceInfoOfState` 的后缓冲分支同源（`DeviceInfo`）。
        DeviceInfo di;
        if (deviceInfoOfState(cpu, dev, &di, nullptr)) {
            writeViewportFullTarget(cpu, pe, static_cast<std::uint32_t>(di.width),
                                    static_cast<std::uint32_t>(di.height));
        } else {
            pipeLog("SetRenderTarget", "⛔ 取不到后缓冲尺寸 ⇒ viewport 未回写（保留旧视口）");
        }
        pipeLog("SetRenderTarget", "目标 = 后缓冲（0）");
        return kOk;
    }
    const char* iface = cpu.com ? cpu.com->ifaceOf(target) : nullptr;
    if (iface == nullptr || std::strcmp(iface, "IDirect3DSurface9") != 0) {
        if (why) *why = "D3D9 流水线：SetRenderTarget 的 pRenderTarget 不是本模块的 IDirect3DSurface9";
        return kInvalidCall;
    }
    const std::uint32_t tex_state = cpu.com->userOf(target);
    TextureInfo ti;
    if (!surfaceInfoOfState(cpu, tex_state, &ti, why)) return kInvalidCall;
    // 这块表面是不是**纹理**表面（像素块归纹理对象）；后缓冲表面的状态是设备状态块（'D9D1'）。
    TextureInfo tex_probe;
    const bool is_texture_surface = textureInfoOfState(cpu, tex_state, &tex_probe, nullptr);
    if (zstencil != 0) {
        // 本线只有一个设备深度缓冲；纹理与后缓冲同尺寸时共用它。不同尺寸会错，但不静默。
        pipeLog("SetRenderTarget", "pNewZStencil=0x%08X **被忽略**（本线共用设备深度缓冲；⛔ 不静默）",
                zstencil);
    }
    if (old_surface != target) {
        if (!cpu.com->addRef(cpu, target)) {
            if (why) *why = "D3D9 流水线：SetRenderTarget AddRef 失败";
            return kInvalidCall;
        }
        // ★ 语义 addRef 的第二份：**像素块的 owner**（父纹理对象，建 surface 时固化；
        //   `d3dx9.cpp` 的 GetSurfaceLevel 传的是 `self`）。★ 04C 之后 surface 自己**也**替父纹理
        //   持一份（`owns_owner`，见 `com.h`）—— 两份都要：surface 那份管"surface 活着 ⇒ 纹理活着"，
        //   设备这份管"绑定期间像素块不被回收"（见 `releaseRenderTarget` 的注释 /
        //   REQ-01M6X4N7C0PAF6XT7EAC172V85）。
        // ★ guest 可见差异（有意）：绑定期间对这张纹理 `Release` 会返回 +1（设备的 RT 引用算在纹理
        //   的公开 refcount 上），对象活到解绑 / 换绑；叠上 surface 自己那份 ⇒ 绑定期公开 refcount
        //   是 guest + surface + 设备**三份**（`d3d9-pipeline` 的 ⑪② 把这三个数钉住）。
        const std::uint32_t owner = cpu.com->ownerOf(target);
        const std::uint32_t owner_generation = cpu.com->ownerGenerationOf(target);
        // ★ 陈旧 owner 的判据 = **代际**，⛔ 不是"owner 的状态块 EA 对不对得上"
        //   （REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）。旧判据是 `userOf(owner) != tex_state`，它认对象用的是
        //   "EA + guest 派生状态"，而 EA 复用往往**同时**复用那个状态块（`kTxBytes` 是常量 ⇒
        //   同尺寸的下一张纹理拿到同一个 `'TEX1'` EA）⇒ 判据通过 ⇒ 静默把 RT 绑到**别人的像素**上，
        //   还会对那张纹理 `markTextureContentUnknown`。代际在宿主表里、建 surface 时固化 ⇒ 骗不过。
        //   ⚠ `owner == 0`（后缓冲表面 / 建 surface 时没传 owner）不走这条判据：后缓冲表面是
        //   合法目标（`shims/d3d9.cpp` 的 GetBackBuffer 表面 owner 就是 0）。
        if (owner != 0) {
            const std::uint32_t owner_now = cpu.com->generationOf(owner);
            // 两种都拒绝：① `owner_generation == 0`（建 surface 时 owner 就不在表里）；
            // ② 该 EA 已经被**另一个**对象拿走（代际变了）。绑下去会保住错的对象，而像素块照样被回收。
            if (owner_generation == 0 || owner_now != owner_generation) {
                cpu.com->release(cpu, target);
                pipeLog("SetRenderTarget",
                        "surface 0x%08X 的 owner 0x%08X 已不是建 surface 时的那个对象"
                        "（缓存代际 %u / 当前 %u，状态块 0x%08X）⇒ D3DERR_INVALIDCALL",
                        target, owner, owner_generation, owner_now, tex_state);
                if (why) *why = "D3D9 流水线：SetRenderTarget 的 surface owner 已被对象表复用（代际不符）";
                return kInvalidCall;
            }
        }
        if (owner != 0 && !cpu.com->addRef(cpu, owner)) {
            cpu.com->release(cpu, target);
            if (why) *why = "D3D9 流水线：SetRenderTarget 的像素块 owner AddRef 失败";
            return kInvalidCall;
        }
        if (owner == 0 && is_texture_surface) {
            // 纹理表面却没有 owner ⇒ 绑定后像素块没有任何引用保着它（本单的缺陷会回来）。
            // ⛔ 不静默放过：这是"建 surface 时漏传 owner"的实现错误。
            cpu.com->release(cpu, target);
            pipeLog("SetRenderTarget", "纹理表面 0x%08X 没有像素块 owner ⇒ D3DERR_INVALIDCALL", target);
            if (why) *why = "D3D9 流水线：SetRenderTarget 的纹理表面没有 owner（建 surface 时漏传？）";
            return kInvalidCall;
        }
        // 旧目标的两份引用成对释放（先新 owner 的 addRef、再旧 owner 的 release ⇒ 同纹理互切安全）
        releaseRenderTarget(cpu, pe);
        plSet(cpu, pe, kPlColorSurface, target);
        plSet(cpu, pe, kPlColorArena, ti.arena);
    }
    // ★ viewport = **新目标的全幅**（D3D9 SetRenderTarget 的 Remarks）。放在所有校验/引用记账
    //   **之后** ⇒ 失败返回不留半个状态；重复设同一 target（`old_surface == target`，不走上面那块）
    //   也写 —— 这条副作用不依赖"目标换没换"。
    //   ★ 尺寸取 `ti`（`surfaceInfoOfState`：纹理表面读 `'TEX1'`，后缓冲表面读 `DeviceInfo`）
    //     ⇒ 引擎"切回后缓冲"传的是 `GetBackBuffer` 的 surface 对象（`0x4A521B`），不是 NULL，
    //     这里同样得到后缓冲尺寸；⛔ 不另写一份"从表面状态块读宽高"的旁路。
    writeViewportFullTarget(cpu, pe, ti.width, ti.height);
    pipeLog("SetRenderTarget", "目标 = 纹理表面 0x%08X（%ux%u，像素块 0x%08X）", target, ti.width,
            ti.height, ti.arena);
    // ★ 纹理当 render target ⇒ 内容版本降级为"未知"（0）：之后 Clear(TARGET) / CPU 光栅回退 /
    //   GPU 读回都会往这块 guest 像素写而不经过 `unlockRect`，版本号不能再声称"已知"。
    //   ⛔ 判据在 `markTextureContentUnknown` 里是 `'TEX1'` magic —— 不要在这里改成
    //   `surfaceInfoOfState` 的返回值（它对后缓冲的 `'D9D1'` 也返回 true）。
    d3d9::markTextureContentUnknown(cpu, tex_state);
    return kOk;
}

// =====================================================================================
// 场景 / 变换 / 渲染状态
// =====================================================================================
std::uint32_t beginScene(X86& cpu, std::uint32_t dev, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (plGet(cpu, pe, kPlInScene) != 0) {
        pipeLog("BeginScene", "已经在场景里 ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：BeginScene 嵌套";
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlInScene, 1u);
    plSet(cpu, pe, kPlBegins, plGet(cpu, pe, kPlBegins) + 1u);
    return kOk;
}

std::uint32_t endScene(X86& cpu, std::uint32_t dev, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (plGet(cpu, pe, kPlInScene) == 0) {
        pipeLog("EndScene", "不在场景里 ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：EndScene 没有配对的 BeginScene";
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlInScene, 0u);
    plSet(cpu, pe, kPlEnds, plGet(cpu, pe, kPlEnds) + 1u);
    const std::uint64_t pixels = (static_cast<std::uint64_t>(plGet(cpu, pe, kPlPixels + 4u)) << 32) |
                                 plGet(cpu, pe, kPlPixels);
    pipeLog("EndScene", "场景结束（累计 Draw %u / 三角形 %u / 像素 %llu）", plGet(cpu, pe, kPlDraws),
            plGet(cpu, pe, kPlTriangles), static_cast<unsigned long long>(pixels));
    return kOk;
}

std::uint32_t setTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "SetTransform 的 State=%u 不是合法的 D3DTRANSFORMSTATETYPE", state);
        pipeLog("SetTransform", "%s ⇒ D3DERR_INVALIDCALL", buf);
        if (why) *why = buf;
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "SetTransform 的 pMatrix", why)) return kInvalidCall;
    for (unsigned i = 0; i < 16; ++i)
        wrU32(cpu, pe + kPlTransforms + state * 64u + i * 4u, cpu.mem->rd32(matrix_ea + i * 4u));
    return kOk;
}

std::uint32_t getTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                           std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        if (why) *why = "D3D9 流水线：GetTransform 的 State 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "GetTransform 的 pMatrix", why)) return kInvalidCall;
    for (unsigned i = 0; i < 16; ++i)
        wrU32(cpu, matrix_ea + i * 4u, cpu.mem->rd32(pe + kPlTransforms + state * 64u + i * 4u));
    return kOk;
}

std::uint32_t multiplyTransform(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t matrix_ea,
                                std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!validTransformState(state)) {
        if (why) *why = "D3D9 流水线：MultiplyTransform 的 State 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, matrix_ea, 64u, "MultiplyTransform 的 pMatrix", why)) return kInvalidCall;
    // D3DX 语义：state = state * pMatrix（行向量 v' = v*state*pMatrix ⇒ 先 state 后 pMatrix）
    const math::Mat4d cur = readMatrix(cpu, pe + kPlTransforms + state * 64u);
    const math::Mat4d mul = readMatrix(cpu, matrix_ea);
    writeMatrix(cpu, pe + kPlTransforms + state * 64u, math::multiplyD3D(cur, mul));
    return kOk;
}

std::uint32_t setRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t value,
                             std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    // ★ 上界必须在**任何写**之前（`REQ-01M7372P560TMY51NJ3ARG4ENG`）：`setRs` 直接把 state 当索引算
    //   `kPlRenderStates + state*4`，越界的 state 会写穿状态块（state=0xFFFF ⇒ 写出 ≈256 KB）踩到
    //   相邻 guest 堆。`GetRenderState` 早就对 256+ 返回 `D3DERR_INVALIDCALL` ⇒ Set/Get 必须对称。
    if (state >= kPlRenderStateCount) {
        if (why) *why = "D3D9 流水线：SetRenderState 的 State 越界（" + std::to_string(state) + " ≥ " +
                        std::to_string(kPlRenderStateCount) + "）⇒ D3DERR_INVALIDCALL（⛔ 不写状态块）";
        return kInvalidCall;
    }
    switch (state) {
        case kRsZEnable:
            if (value > 2u) {
                pipeLog("SetRenderState", "ZENABLE=%u 非法（D3DZB_FALSE/TRUE/USEW）⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value == 2u) recordUnsupported(cpu, pe, 0u, "D3DRS_ZENABLE=D3DZB_USEW（w 缓冲）", value);
            break;
        case kRsFillMode:
            if (value != 1u && value != 2u && value != 3u) {
                pipeLog("SetRenderState", "FILLMODE=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value != 3u) recordUnsupported(cpu, pe, 1u, "D3DFILL_POINT/LINE（只做 SOLID）", value);
            break;
        case kRsShadeMode:
            if (value != 1u && value != 2u) {
                pipeLog("SetRenderState", "SHADEMODE=%u 非法（只认 FLAT/GOURAUD）⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsCullMode:
            if (value < 1u || value > 3u) {
                pipeLog("SetRenderState", "CULLMODE=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsZFunc:
        case kRsAlphaFunc:
            if (value < 1u || value > 8u) {
                pipeLog("SetRenderState", "比较函数=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsAlphaBlendEnable:
        case kRsAlphaTestEnable:
            if (value > 1u) {
                pipeLog("SetRenderState", "布尔状态=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsSrcBlend:
        case kRsDestBlend:
        case kRsSrcBlendAlpha:
        case kRsDestBlendAlpha:
            if (value < 1u || value > 13u) {
                pipeLog("SetRenderState", "D3DBLEND=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsBlendOp:
        case kRsBlendOpAlpha:
            if (value < 1u || value > 5u) {
                pipeLog("SetRenderState", "D3DBLENDOP=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kRsFogEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 2u, "D3DRS_FOGENABLE（本线不做雾）", value);
            break;
        case kRsStencilEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 3u, "D3DRS_STENCILENABLE（本线没有模板缓冲）", value);
            break;
        case kRsMultiSampleAntiAlias:
            if (value != 0u) recordUnsupported(cpu, pe, 4u, "D3DRS_MULTISAMPLEANTIALIAS", value);
            break;
        case kRsVertexBlend:
            if (value != 0u) recordUnsupported(cpu, pe, 5u, "D3DRS_VERTEXBLEND（本线不做顶点混合）", value);
            break;
        case kRsIndexedVertexBlendEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 6u, "D3DRS_INDEXEDVERTEXBLENDENABLE", value);
            break;
        case kRsClipPlaneEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 7u, "D3DRS_CLIPPLANEENABLE（本线不做用户裁剪面）", value);
            break;
        case kRsClipping:
            // 本线**总是**做裁剪（齐次 6 面 / 预变换视口矩形）。真机 CLIPPING=0 会关掉落屏外的裁剪，
            // ⛔ 不静默：记录，行为仍是"总是裁剪"（与既有近平面裁剪的口径一致）。
            if (value == 0u) recordUnsupported(cpu, pe, 18u, "D3DRS_CLIPPING=0（本线总是裁剪）", value);
            break;
        case kRsTweenFactor:
            if (value != 0u) recordUnsupported(cpu, pe, 8u, "D3DRS_TWEENFACTOR", value);
            break;
        case kRsDitherEnable:
            if (value != 0u) recordUnsupported(cpu, pe, 9u, "D3DRS_DITHERENABLE", value);
            break;
        default:
            break;
    }
    setRs(cpu, pe, state, value);
    return kOk;
}

std::uint32_t getRenderState(X86& cpu, std::uint32_t dev, std::uint32_t state, std::uint32_t out_ea,
                             std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (state >= kPlRenderStateCount) {
        if (why) *why = "D3D9 流水线：GetRenderState 的 State 越界";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetRenderState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, rsOf(cpu, pe, state));
    return kOk;
}

std::uint32_t setSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t value, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) {
        pipeLog("SetSamplerState", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxSamplers);
        return kInvalidCall;
    }
    if (type == 0u || type > 13u) {
        pipeLog("SetSamplerState", "Type=%u 不是合法的 D3DSAMPLERSTATETYPE ⇒ D3DERR_INVALIDCALL", type);
        return kInvalidCall;
    }
    switch (type) {
        case kSampAddressU:
        case kSampAddressV:
        case kSampAddressW:
            if (value < 1u || value > 5u) {
                pipeLog("SetSamplerState", "D3DTADDRESS=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            break;
        case kSampMagFilter:
        case kSampMinFilter:
            if (value > 3u) {
                pipeLog("SetSamplerState", "过滤器=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
                return kInvalidCall;
            }
            if (value == kTexfAnisotropic) recordUnsupported(cpu, pe, 10u, "各向异性过滤（按 LINEAR 近似）", value);
            if (value == kTexfNone)
                recordUnsupported(cpu, pe, 17u, "D3DTEXF_NONE（MIN/MAG 无过滤；两侧都 NONE 时按透明黑）", value);
            break;
        case kSampMipFilter:
            // ★ 语义（`REQ-01M73G2724479XAVS176WBZ65Q`）：NONE(0) = 关掉 mipmap ⇒ 只采 level 0；
            //   POINT(1) / LINEAR(2) 都允许按 LOD 选级。本线取**最近级** ⇒ LINEAR（三线性）按最近级近似，
            //   这一条要记账（⛔ 不要再说"本线没有 mip 链" —— 那是修前的状态）。
            if (value == kTexfLinear) {
                recordUnsupported(cpu, pe, 11u, "MIPFILTER=LINEAR（三线性）按最近级近似", value);
            } else if (value > kTexfLinear) {
                recordUnsupported(cpu, pe, 11u, "MIPFILTER（不认识的值）", value);
            }
            break;
        case kSampMaxMipLevel:
        case kSampMaxAnisotropy:
            break;
        default:
            recordUnsupported(cpu, pe, 12u, "其它 sampler state", type);
            break;
    }
    plSet(cpu, pe, kPlSamplers + (stage * 16u + type) * 4u, value);
    return kOk;
}

std::uint32_t getSamplerState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                              std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers || type == 0u || type > 13u) {
        if (why) *why = "D3D9 流水线：GetSamplerState 的 Stage/Type 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetSamplerState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, samplerOf(cpu, pe, stage, type));
    return kOk;
}

std::uint32_t setTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t value, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxTextureStages) {
        pipeLog("SetTextureStageState", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxTextureStages);
        return kInvalidCall;
    }
    if (type == 0u || type > kPlTexStageTypes - 1u) {
        pipeLog("SetTextureStageState", "Type=%u 越界 ⇒ D3DERR_INVALIDCALL", type);
        return kInvalidCall;
    }
    // ★ 诊断在 shim 层（`shims/d3d9.cpp` 的 shimSetTextureStageState）：那里才有**返回地址**。
    if (type == kTssColorOp || type == kTssAlphaOp) {
        if (value == 0u || value > 26u) {
            pipeLog("SetTextureStageState", "D3DTOP=%u 非法 ⇒ D3DERR_INVALIDCALL", value);
            return kInvalidCall;
        }
        if (value > kTopMultiplyAdd && value != kTopLerp && value != kTopModulateAlphaAddColor &&
            value != kTopModulateColorAddAlpha) {
            recordUnsupported(cpu, pe, 13u, "D3DTOP", value);
        }
    }
    if (type == kTssTextureTransformFlags && value != 0u) {
        recordUnsupported(cpu, pe, 14u, "D3DTSS_TEXTURETRANSFORMFLAGS（本线不做纹理坐标变换）", value);
    }
    if (type == kTssTexCoordIndex && (value & 0x10000u) != 0u) {
        recordUnsupported(cpu, pe, 15u, "D3DTSS_TEXCOORDINDEX 的相机空间生成标志", value);
    }
    plSet(cpu, pe, kPlTexStages + (stage * kPlTexStageTypes + type) * 4u, value);
    return kOk;
}

std::uint32_t getTextureStageState(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t type,
                                   std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxTextureStages || type == 0u || type >= kPlTexStageTypes) {
        if (why) *why = "D3D9 流水线：GetTextureStageState 的 Stage/Type 非法";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "GetTextureStageState 的 pValue", why)) return kInvalidCall;
    wrU32(cpu, out_ea, texStageOf(cpu, pe, stage, type));
    return kOk;
}

std::uint32_t setTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t texture,
                         std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) {
        pipeLog("SetTexture", "Stage=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stage, kMaxSamplers);
        return kInvalidCall;
    }
    std::uint32_t tex_gen = 0;
    if (texture != 0) {
        TextureInfo ti;
        if (!textureInfo(cpu, texture, &ti, why)) {
            pipeLog("SetTexture", "0x%08X 不是本模块建的 IDirect3DTexture9 ⇒ D3DERR_INVALIDCALL", texture);
            if (why) *why = "D3D9 流水线：SetTexture 的纹理不是本模块建的";
            return kInvalidCall;
        }
        // ★ 身份的一半：绑定时记下对象代际（与顶点声明 / 流 / 索引同口径）。没有代际 ⇒ 绑定
        //   就不可校验（`GetTexture` / 换绑 / 设备销毁都会退化成"按 EA 减引用"）⇒ 响亮拒绝。
        tex_gen = cpu.com ? cpu.com->generationOf(texture) : 0u;
        if (tex_gen == 0) {
            pipeWarn(PipeWarnCategory::Texture, "SetTexture",
                     "纹理 0x%08X 没有代际（对象表不一致）⇒ D3DERR_INVALIDCALL", texture);
            if (why) *why = "D3D9 流水线：SetTexture 的对象没有代际";
            return kInvalidCall;
        }
    }
    const std::uint32_t old = plGet(cpu, pe, kPlTextures + stage * 4u);
    const std::uint32_t old_gen = plGet(cpu, pe, kPlTextureGens + stage * 4u);
    // ★ "同一份绑定" = **(EA, 代际) 都相同**。同 EA 不同代际 = guest 堆复用出来的**另一张纹理**
    //   ⇒ 必须按新对象 addRef、旧绑定按 (EA, 旧代际) 判还（`releaseBound` 会响亮拒绝、⛔ 不减到
    //   新占有者身上）。重复 Set 同一张则两边都不动 ⇒ 引用计数不漂。
    const bool same_binding = (texture == old && tex_gen == old_gen);
    if (texture != 0 && !same_binding && (!cpu.com || !cpu.com->addRef(cpu, texture))) {
        pipeLog("SetTexture", "纹理 0x%08X 的 AddRef 失败（COM 状态坏了）", texture);
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlTextures + stage * 4u, texture);
    plSet(cpu, pe, kPlTextureGens + stage * 4u, tex_gen);
    if (old != 0 && !same_binding) releaseBound(cpu, old, old_gen, "SetTexture", PipeWarnCategory::Texture);
    if ((stage == 0 || texture != 0) && old != texture) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Stage=%u ⇒ 纹理 0x%08X（原 0x%08X）", stage, texture, old);
        pipeLog("SetTexture", "%s", buf);
    }
    return kOk;
}

std::uint32_t getTexture(X86& cpu, std::uint32_t dev, std::uint32_t stage, std::uint32_t out_ea,
                         std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stage >= kMaxSamplers) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetTexture 的 ppTexture", why)) return kInvalidCall;
    const std::uint32_t texture = plGet(cpu, pe, kPlTextures + stage * 4u);
    // ★ `IDirect3DDevice9::GetTexture` 文档明确："Calling this method will increase the internal
    //   reference count on the IDirect3DTexture9 interface. Failure to call IUnknown::Release ...
    //   results in a memory leak." ⇒ **借出的是新的一份引用**（与同族的 Resource9::GetDevice /
    //   GetStreamSource / GetIndices / GetRenderTarget 一致）。
    //   · guest **不** Release ⇒ 每次 GetTexture 多留一份引用，纹理永远到不了 0（heap / arena 都不回收）；
    //   · guest **多** Release 一次 ⇒ 减掉的是 `setTexture` 替设备持的那份绑定引用，之后换绑 /
    //     设备销毁会对一个「已经不属于自己的 EA」release（`ComRegistry` 会亮声拒绝，不是静默）。
    //   ★ 绑定期间流水线自己持一份 ⇒ 这里的 AddRef **不是**在补"绑定"，而是"再借一份给出参"。
    //   ★ **身份校验**（段 3 的 ②）：槽里只缓存 EA，guest 堆会复用 ⇒ 必须同时证明
    //     ① iface 还是 `IDirect3DTexture9` ② 代际与绑定时一致。否则把**新纹理** +1 交回 guest，
    //     而 guest Release 时减的是自己的那份 / 换绑时又去 releaseIf 一个不存在的身份。
    //     ⛔ 不降级成"只验 iface"：同 EA 的另一张纹理 iface 也是对的（这正是缺陷形态）。
    if (texture != 0) {
        const char* iface = cpu.com ? cpu.com->ifaceOf(texture) : nullptr;
        const std::uint32_t gen = plGet(cpu, pe, kPlTextureGens + stage * 4u);
        const bool same_object =
            cpu.com != nullptr && iface != nullptr && std::strcmp(iface, "IDirect3DTexture9") == 0 &&
            gen != 0 && cpu.com->generationOf(texture) == gen;
        if (!same_object) {
            pipeWarn(PipeWarnCategory::Texture, "GetTexture",
                     "Stage=%u 的槽里 0x%08X 已不是绑定时那张纹理（%s；绑定时代际 %u，当前 %u）⇒ "
                     "D3DERR_INVALIDCALL（⛔ 不把新占有者交回 guest）",
                     stage, texture, iface ? iface : "不在对象表里", gen,
                     cpu.com ? cpu.com->generationOf(texture) : 0u);
            if (why) *why = "D3D9 流水线：GetTexture 的槽已不指向绑定时那张纹理";
            return kInvalidCall;
        }
        if (!cpu.com->addRef(cpu, texture)) {
            pipeWarn(PipeWarnCategory::Texture, "GetTexture",
                     "Stage=%u 的纹理 0x%08X 借不出引用（COM 状态坏了）", stage, texture);
            if (why) *why = "D3D9 流水线：GetTexture 借出的纹理引用不成立";
            return kInvalidCall;
        }
    }
    wrU32(cpu, out_ea, texture);
    return kOk;
}

std::uint32_t setFVF(X86& cpu, std::uint32_t dev, std::uint32_t fvf, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    // FVF 与顶点声明是**二选一**（D3D 语义）⇒ 两个清掉声明的分支都必须把绑定引用还回去。
    //   ★ 漏掉任一支 = 设备永远替一句已经用不上的声明持引用 ⇒ 声明到不了 0、`'VDCL'` 留在堆上。
    //   ★ 守卫不是 `d3d9/vertex-declaration` 的堆基线（那条只走 `SetVertexDeclaration(0)` 的路径）：
    //     两条分支各由 `d3d9/setfvf-releases-decl` 的对应子块盯（声明 refcount 归 0 + 对象消失 +
    //     堆回基线）—— 复审实测过"把 fvf==0 那一支改成只清槽不还引用"时，旧用例全绿。
    auto dropDeclaration = [&]() {
        const std::uint32_t decl = plGet(cpu, pe, kPlVertexDecl);
        const std::uint32_t gen = plGet(cpu, pe, kPlVertexDeclGen);
        plSet(cpu, pe, kPlVertexDecl, 0u);
        plSet(cpu, pe, kPlVertexDeclGen, 0u);
        releaseBound(cpu, decl, gen, "SetFVF", PipeWarnCategory::Other);
    };
    if (fvf == 0u) {
        // 真机：SetFVF(0) 合法（= 当前没有 FVF）；之后 Draw 会因缺少顶点格式失败。
        plSet(cpu, pe, kPlFvf, 0u);
        dropDeclaration();
        pipeLog("SetFVF", "FVF=0 ⇒ 当前没有 FVF（合法的清除；之后 Draw 会 INVALIDCALL）");
        return kOk;
    }
    std::string parse_why;
    const FvfLayout l = parseFvf(fvf, &parse_why);
    if (!l.valid) {
        pipeLog("SetFVF", "%s ⇒ D3DERR_INVALIDCALL", parse_why.c_str());
        if (why) *why = parse_why;
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlFvf, fvf);
    dropDeclaration();  // 用 FVF 就清掉顶点声明（D3D 也是二选一）
    return kOk;
}

std::uint32_t getFVF(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetFVF 的 pFVF", why)) return kInvalidCall;
    wrU32(cpu, out_ea, plGet(cpu, pe, kPlFvf));
    return kOk;
}

std::uint32_t setVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t decl, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    const std::uint32_t old = plGet(cpu, pe, kPlVertexDecl);
    const std::uint32_t old_gen = plGet(cpu, pe, kPlVertexDeclGen);
    if (decl != 0) {
        // 只接受**本模块建的** IDirect3DVertexDeclaration9。用声明代替 FVF 是合法路径；本线的
        // Draw 仍只认 FVF（`drawPrecheck` 会 D3DERR_INVALIDCALL），但 Set 本身不该拦。
        const char* iface = cpu.com ? cpu.com->ifaceOf(decl) : nullptr;
        if (!cpu.com || !cpu.com->find(decl) || !iface ||
            std::strcmp(iface, "IDirect3DVertexDeclaration9") != 0) {
            pipeLog("SetVertexDeclaration", "pDecl=0x%08X 不是本模块的 IDirect3DVertexDeclaration9 ⇒ D3DERR_INVALIDCALL",
                    decl);
            if (why) *why = "D3D9 流水线：SetVertexDeclaration 收到非顶点声明对象";
            return kInvalidCall;
        }
        const std::uint32_t gen = cpu.com->generationOf(decl);
        if (gen == 0) {
            pipeWarn(PipeWarnCategory::Other, "SetVertexDeclaration",
                     "pDecl=0x%08X 没有代际（对象表不一致）⇒ D3DERR_INVALIDCALL", decl);
            if (why) *why = "D3D9 流水线：SetVertexDeclaration 的对象没有代际";
            return kInvalidCall;
        }
        // ★ 设备替 guest 持一份**绑定引用**（与 `SetTexture` 的既有口径一致）：这样
        //   "`SetVertexDeclaration` 之后 guest 放掉自己那份、再 `GetVertexDeclaration` / Draw"
        //   这条**合法**时序不会把声明提前释放掉（绑定引用在换绑 / 解绑 / 设备销毁时对称还回）。
        //   顺序 = 先 addRef 新的、再写槽、最后放旧的 ⇒ 同一对象重复 Set 不会瞬时归零。
        // ★ 判"同一个"必须比 **(EA, 代际)**：guest 堆会复用 EA ⇒ 同 EA 不同代际是**新对象**，
        //   漏掉 addRef 会让"绑定期只有 guest 自己那份"，guest 一放就归 0（对象 + `'VDCL'` 当场没）。
        const bool same_binding = (decl == old && gen == old_gen);
        if (!same_binding && !cpu.com->addRef(cpu, decl)) {
            pipeWarn(PipeWarnCategory::Other, "SetVertexDeclaration",
                     "pDecl=0x%08X 的 AddRef 失败（COM 状态坏了）⇒ D3DERR_INVALIDCALL", decl);
            if (why) *why = "D3D9 流水线：SetVertexDeclaration 持有绑定引用失败";
            return kInvalidCall;
        }
        plSet(cpu, pe, kPlVertexDecl, decl);
        plSet(cpu, pe, kPlVertexDeclGen, gen);
        plSet(cpu, pe, kPlFvf, 0u);  // 用声明就清掉 FVF（D3D 二选一）
        // 旧绑定的 (EA, 代际) 不符 ⇒ `releaseBound` 只出声、⛔ 不减到新占有者身上。
        if (old != 0 && !same_binding)
            releaseBound(cpu, old, old_gen, "SetVertexDeclaration", PipeWarnCategory::Other);
        return kOk;
    }
    plSet(cpu, pe, kPlVertexDecl, 0u);
    plSet(cpu, pe, kPlVertexDeclGen, 0u);
    releaseBound(cpu, old, old_gen, "SetVertexDeclaration(0)", PipeWarnCategory::Other);
    return kOk;
}

std::uint32_t getVertexDeclaration(X86& cpu, std::uint32_t dev, std::uint32_t out_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, out_ea, 4u, "GetVertexDeclaration 的 ppDecl", why)) return kInvalidCall;
    const std::uint32_t decl = plGet(cpu, pe, kPlVertexDecl);
    // ★ 借出的是**新的一份引用**（裁决：按 D3D9 `Get*` 家族一致性办 —— `GetTexture` /
    //   `GetStreamSource` / `GetIndices` / `GetRenderTarget` / `Resource9::GetDevice` 的文档都写
    //   AddRef + 调用方必须 Release；`GetVertexDeclaration` 的页面**没写**，本仓按家族一致办）。
    //   · guest **不** Release ⇒ 声明永远到不了 0：`'VDCL'` 状态块 + 对象头都留在 guest 堆；
    //   · guest **多** Release 一次 ⇒ 减到当前占用该 EA 的**别人**身上（本对象已归 0 时表里没它，
    //     `release` 只会亮声返回 false；但"多一次"的那份记账错会一路传到下一次复用）。
    //   ★ `setVertexDeclaration` **也替设备持一份绑定引用**（段 2 的口径，与 `SetTexture` /
    //     `SetStreamSource` / `SetIndices` 同形）⇒ 绑定期有两份（设备那份 + guest 自己那份）；
    //     换绑 / 解绑 / 设备销毁时设备那份由 `releaseBound` 还回。
    //   ★ 但**点名身份**仍然必须做：槽里存的只是 EA，guest 堆会复用 ⇒ 同 EA 的新对象 iface
    //     对得上、绑定期那份引用也早被 guest 越界 Release 抵掉时，按 EA 借出就会把**别人的**对象
    //     交给 guest。所以借出前先证明"槽里这个 EA 现在还是那个代际的顶点声明对象"。
    if (decl != 0) {
        const char* iface = cpu.com ? cpu.com->ifaceOf(decl) : nullptr;
        const std::uint32_t gen = plGet(cpu, pe, kPlVertexDeclGen);
        const bool same_object = cpu.com != nullptr && iface != nullptr &&
                                 std::strcmp(iface, "IDirect3DVertexDeclaration9") == 0 && gen != 0 &&
                                 cpu.com->generationOf(decl) == gen;
        if (!same_object || !cpu.com->addRef(cpu, decl)) {
            // ★ 默认级别可见：这条是"本会静默交出别人的对象"的事件（Info 默认看不见）。
            pipeWarn(PipeWarnCategory::Other, "GetVertexDeclaration",
                     "槽里的 0x%08X 已不是绑定时那个顶点声明对象（%s；绑定时代际 %u，当前 %u）⇒ "
                     "D3DERR_INVALIDCALL（⛔ 不把新占有者交回 guest）",
                     decl, iface ? iface : "不在对象表里", gen,
                     cpu.com ? cpu.com->generationOf(decl) : 0u);
            if (why) *why = "D3D9 流水线：GetVertexDeclaration 的槽已不指向绑定时那个顶点声明对象";
            return kInvalidCall;
        }
    }
    wrU32(cpu, out_ea, decl);
    return kOk;
}

std::uint32_t setViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, viewport_ea, 24u, "SetViewport 的 pViewport", why)) return kInvalidCall;
    const std::uint32_t w = cpu.mem->rd32(viewport_ea + 8u);
    const std::uint32_t h = cpu.mem->rd32(viewport_ea + 12u);
    if (w == 0 || h == 0) {
        pipeLog("SetViewport", "Width/Height 为 0 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    for (unsigned i = 0; i < 4; ++i)
        plSet(cpu, pe, kPlViewport + i * 4u, cpu.mem->rd32(viewport_ea + i * 4u));
    for (unsigned i = 4; i < 6; ++i)
        wrU32(cpu, pe + kPlViewport + i * 4u, cpu.mem->rd32(viewport_ea + i * 4u));
    return kOk;
}

std::uint32_t getViewport(X86& cpu, std::uint32_t dev, std::uint32_t viewport_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, viewport_ea, 24u, "GetViewport 的 pViewport", why)) return kInvalidCall;
    for (unsigned i = 0; i < 6; ++i)
        wrU32(cpu, viewport_ea + i * 4u, cpu.mem->rd32(pe + kPlViewport + i * 4u));
    return kOk;
}

std::uint32_t setScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, rect_ea, 16u, "SetScissorRect 的 pRect", why)) return kInvalidCall;
    const std::int32_t left = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 0u));
    const std::int32_t top = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 4u));
    const std::int32_t right = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 8u));
    const std::int32_t bottom = static_cast<std::int32_t>(cpu.mem->rd32(rect_ea + 12u));
    if (left < 0 || top < 0 || right < left || bottom < top) {
        pipeLog("SetScissorRect", "矩形 (%d,%d,%d,%d) 非法 ⇒ D3DERR_INVALIDCALL", left, top, right, bottom);
        return kInvalidCall;
    }
    for (unsigned i = 0; i < 4; ++i) plSet(cpu, pe, kPlScissor + i * 4u, cpu.mem->rd32(rect_ea + i * 4u));
    return kOk;
}

std::uint32_t getScissorRect(X86& cpu, std::uint32_t dev, std::uint32_t rect_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, rect_ea, 16u, "GetScissorRect 的 pRect", why)) return kInvalidCall;
    for (unsigned i = 0; i < 4; ++i) wrU32(cpu, rect_ea + i * 4u, cpu.mem->rd32(pe + kPlScissor + i * 4u));
    return kOk;
}

std::uint32_t setMaterial(X86& cpu, std::uint32_t dev, std::uint32_t material_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, material_ea, 68u, "SetMaterial 的 pMaterial", why)) return kInvalidCall;
    for (unsigned i = 0; i < 68u; ++i) cpu.mem->wr8(pe + kPlMaterial + i, cpu.mem->rd8(material_ea + i));
    return kOk;
}

std::uint32_t setLight(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t light_ea,
                       std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (index >= kMaxLights) {
        pipeLog("SetLight", "Index=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", index, kMaxLights);
        return kInvalidCall;
    }
    if (!committed(cpu, light_ea, 104u, "SetLight 的 pLight", why)) return kInvalidCall;
    for (unsigned i = 0; i < 104u; ++i) cpu.mem->wr8(pe + kPlLights + index * 104u + i, cpu.mem->rd8(light_ea + i));
    return kOk;
}

std::uint32_t lightEnable(X86& cpu, std::uint32_t dev, std::uint32_t index, std::uint32_t enable,
                          std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (index >= kMaxLights) {
        pipeLog("LightEnable", "Index=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", index, kMaxLights);
        return kInvalidCall;
    }
    plSet(cpu, pe, kPlLightEnabled + index * 4u, enable != 0 ? 1u : 0u);
    return kOk;
}

std::uint32_t validateDevice(X86& cpu, std::uint32_t dev, std::uint32_t passes_ea, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (!committed(cpu, passes_ea, 4u, "ValidateDevice 的 pNumPasses", why)) return kInvalidCall;
    wrU32(cpu, passes_ea, 1u);  // 本线的状态组合一律单 pass
    return kOk;
}

// =====================================================================================
// 顶点缓冲 / 索引缓冲
// =====================================================================================
std::uint32_t createVertexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                      std::uint32_t usage, std::uint32_t fvf, std::uint32_t pool,
                                      std::string* why) {
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪（顶点缓冲要放在 guest 堆上）";
        return 0;
    }
    if (length == 0u || length > kMaxBufferBytes) {
        if (why) *why = "D3D9 流水线：创建顶点缓冲的长度非法";
        return 0;
    }
    const std::uint32_t total = kVbHeader + length;
    const std::uint32_t state = cpu.heap->alloc(total, /*zero=*/true);
    if (state == 0) {
        if (why) *why = std::string("D3D9 流水线：分配顶点缓冲失败：") + cpu.heap->lastError();
        return 0;
    }
    wrU32(cpu, state + kVbMagic, kVbMagicValue);
    wrU32(cpu, state + kVbDevice, device_object);
    wrU32(cpu, state + kVbLength, length);
    wrU32(cpu, state + kVbUsage, usage);
    wrU32(cpu, state + kVbFvf, fvf);
    wrU32(cpu, state + kVbPool, pool);
    pipeLog("CreateVertexBuffer", "%u B（Usage=0x%X FVF=0x%X Pool=%u）⇒ 状态块 0x%08X", length, usage, fvf, pool,
            state);
    return state;
}

bool vertexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                             std::uint32_t* fvf, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kVbHeader)) {
        if (why) *why = "D3D9 流水线：顶点缓冲状态块未提交";
        return false;
    }
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块建的顶点缓冲";
        return false;
    }
    if (length) *length = cpu.mem->rd32(state + kVbLength);
    if (data_ea) *data_ea = state + kVbHeader;
    if (fvf) *fvf = cpu.mem->rd32(state + kVbFvf);
    return true;
}

std::uint32_t vertexBufferDevice(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem->committed(state, kVbHeader)) return 0;
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) return 0;
    return cpu.mem->rd32(state + kVbDevice);
}

void destroyVertexBufferState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kVbHeader)) return;
    if (cpu.mem->rd32(state + kVbMagic) != kVbMagicValue) return;
    cpu.mem->wr32(state + kVbMagic, 0u);
    if (cpu.heap) cpu.heap->free(state);
}

std::uint32_t vertexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                               std::uint32_t out_ea, std::uint32_t flags, std::string* why) {
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    if (!vertexBufferInfoOfState(cpu, state, &length, &data, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kVbLocked) != 0) {
        if (why) *why = "D3D9 流水线：顶点缓冲已锁（真机也是 D3DERR_INVALIDCALL）";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "顶点缓冲 Lock 的 ppbData", why)) return kInvalidCall;
    if (offset > length) {
        if (why) *why = "D3D9 流水线：顶点缓冲 Lock 的 OffsetToLock 越界（" + std::to_string(offset) + " > " +
                        std::to_string(length) + "）";
        return kInvalidCall;
    }
    // ★ `offset + size > length` ⇒ `D3DERR_INVALIDCALL`（`REQ-01M7372P560TMY51NJ3ARG4ENG`）：
    //   旧口径只是把 size 夹到剩余长度、照样返回 `kOk` ⇒ guest 以为"锁了 16 字节"，实际只锁到尾部，
    //   之后的写入跨过缓冲末尾、踩到相邻 guest 堆，而返回值装作成功。
    //   ★ `size == 0` 仍然合法 = "锁到尾部"（D3D9 文档口径），不是越界。
    //   ★ 用 `length - offset` 判（⛔ 不用 `offset + size`：那是 uint32 溢出面，size=0xFFFFFFFF 会绕过）。
    const std::uint32_t avail = length - offset;
    if (size > avail) {
        if (why) *why = "D3D9 流水线：顶点缓冲 Lock 的 OffsetToLock + SizeToLock 越界（" + std::to_string(offset) +
                        " + " + std::to_string(size) + " > " + std::to_string(length) +
                        "）⇒ D3DERR_INVALIDCALL（⛔ 不夹到尾部、⛔ 不置锁）";
        return kInvalidCall;
    }
    wrU32(cpu, out_ea, data + offset);
    wrU32(cpu, state + kVbLocked, 1u);
    if (flags != 0u) {
        // DISCARD/NOOVERWRITE 对 guest 内存里的顶点缓冲没有可观察差异 ⇒ 只记录（不静默）
        pipeLog("VertexBuffer::Lock", "Flags=0x%X 被记录（guest 内存没有 DISCARD/NOOVERWRITE 语义）", flags);
    }
    return kOk;
}

std::uint32_t vertexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why) {
    std::uint32_t length = 0;
    if (!vertexBufferInfoOfState(cpu, state, &length, nullptr, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kVbLocked) == 0) {
        if (why) *why = "D3D9 流水线：Unlock 未锁的顶点缓冲（真机也是 D3DERR_INVALIDCALL）";
        return kInvalidCall;
    }
    wrU32(cpu, state + kVbLocked, 0u);
    return kOk;
}

std::uint32_t createIndexBufferState(X86& cpu, std::uint32_t device_object, std::uint32_t length,
                                     std::uint32_t usage, std::uint32_t format, std::uint32_t pool,
                                     std::string* why) {
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "D3D9 流水线：guest 堆未就绪";
        return 0;
    }
    if (length == 0u || length > kMaxBufferBytes) {
        if (why) *why = "D3D9 流水线：创建索引缓冲的长度非法";
        return 0;
    }
    if (format != 101u && format != 102u) {
        if (why) *why = "D3D9 流水线：索引缓冲格式只支持 D3DFMT_INDEX16(101) / INDEX32(102)";
        return 0;
    }
    const std::uint32_t total = kIbHeader + length;
    const std::uint32_t state = cpu.heap->alloc(total, /*zero=*/true);
    if (state == 0) {
        if (why) *why = std::string("D3D9 流水线：分配索引缓冲失败：") + cpu.heap->lastError();
        return 0;
    }
    wrU32(cpu, state + kIbMagic, kIbMagicValue);
    wrU32(cpu, state + kIbDevice, device_object);
    wrU32(cpu, state + kIbLength, length);
    wrU32(cpu, state + kIbUsage, usage);
    wrU32(cpu, state + kIbFormat, format);
    wrU32(cpu, state + kIbPool, pool);
    pipeLog("CreateIndexBuffer", "%u B（Usage=0x%X Format=%u Pool=%u）⇒ 状态块 0x%08X", length, usage, format,
            pool, state);
    return state;
}

bool indexBufferInfoOfState(X86& cpu, std::uint32_t state, std::uint32_t* length, std::uint32_t* data_ea,
                            std::uint32_t* format, std::string* why) {
    if (state == 0 || !cpu.mem->committed(state, kIbHeader)) {
        if (why) *why = "D3D9 流水线：索引缓冲状态块未提交";
        return false;
    }
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) {
        if (why) *why = "D3D9 流水线：不是本模块建的索引缓冲";
        return false;
    }
    if (length) *length = cpu.mem->rd32(state + kIbLength);
    if (data_ea) *data_ea = state + kIbHeader;
    if (format) *format = cpu.mem->rd32(state + kIbFormat);
    return true;
}

std::uint32_t indexBufferDevice(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem->committed(state, kIbHeader)) return 0;
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) return 0;
    return cpu.mem->rd32(state + kIbDevice);
}

void destroyIndexBufferState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kIbHeader)) return;
    if (cpu.mem->rd32(state + kIbMagic) != kIbMagicValue) return;
    cpu.mem->wr32(state + kIbMagic, 0u);
    if (cpu.heap) cpu.heap->free(state);
}

std::uint32_t indexBufferLock(X86& cpu, std::uint32_t state, std::uint32_t offset, std::uint32_t size,
                              std::uint32_t out_ea, std::uint32_t flags, std::string* why) {
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    if (!indexBufferInfoOfState(cpu, state, &length, &data, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kIbLocked) != 0) {
        if (why) *why = "D3D9 流水线：索引缓冲已锁";
        return kInvalidCall;
    }
    if (!committed(cpu, out_ea, 4u, "索引缓冲 Lock 的 ppbData", why)) return kInvalidCall;
    if (offset > length) {
        if (why) *why = "D3D9 流水线：索引缓冲 Lock 的 OffsetToLock 越界（" + std::to_string(offset) + " > " +
                        std::to_string(length) + "）";
        return kInvalidCall;
    }
    // ★ 与顶点缓冲同口径（`REQ-01M7372P560TMY51NJ3ARG4ENG` 的第 3 处）：`offset + size > length`
    //   ⇒ `D3DERR_INVALIDCALL`；旧口径**完全不看 size**，越界照样 `kOk`。`size == 0` = 锁到尾部。
    const std::uint32_t avail = length - offset;
    if (size > avail) {
        if (why) *why = "D3D9 流水线：索引缓冲 Lock 的 OffsetToLock + SizeToLock 越界（" + std::to_string(offset) +
                        " + " + std::to_string(size) + " > " + std::to_string(length) +
                        "）⇒ D3DERR_INVALIDCALL（⛔ 不置锁）";
        return kInvalidCall;
    }
    wrU32(cpu, out_ea, data + offset);
    wrU32(cpu, state + kIbLocked, 1u);
    (void)flags;
    return kOk;
}

std::uint32_t indexBufferUnlock(X86& cpu, std::uint32_t state, std::string* why) {
    std::uint32_t length = 0;
    if (!indexBufferInfoOfState(cpu, state, &length, nullptr, nullptr, why)) return kInvalidCall;
    if (cpu.mem->rd32(state + kIbLocked) == 0) {
        if (why) *why = "D3D9 流水线：Unlock 未锁的索引缓冲";
        return kInvalidCall;
    }
    wrU32(cpu, state + kIbLocked, 0u);
    return kOk;
}

// =====================================================================================
// 绑定槽的**身份校验**（对象 EA + 绑定时代际 → 状态块）
//
// ★ 为什么不能只验状态块 magic（缺陷单 `REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 #4）：
//   guest 堆是首次适配 ⇒ 旧缓冲释放后，**同长度**的新缓冲会拿到同一个对象 EA **与**同一个
//   `'VB09'` / `'IB09'` 状态块 EA。magic 是常量，所以旧句柄在这一格**看起来完全合法** ——
//   于是 Draw 静默改用别人的顶点数据。身份（代际）**只在宿主表里**（`ComRegistry::generationOf`，
//   ⛔ 不写进对象 / Win32 结构体），代际单调自增、永不复用 ⇒ 复用一定判得出来。
// ★ 不符就**响亮失败**：⛔ 不读"看起来合法"的值，也 ⛔ 不静默跳过 —— 用 `pipeWarn`（默认级别可见）。
// =====================================================================================
bool boundVertexBufferOfState(X86& cpu, std::uint32_t obj, std::uint32_t gen, const char* what,
                              std::uint32_t* state, std::uint32_t* length, std::uint32_t* data_ea,
                              std::string* why) {
    const char* slot = "顶点缓冲";
    if (obj == 0 || !cpu.com || !cpu.com->find(obj)) {
        pipeWarn(PipeWarnCategory::VertexStream, what,
                 "绑定身份不符：槽里的%s 0x%08X 已不在对象表里（绑定时代际 %u）⇒ D3DERR_INVALIDCALL",
                 slot, obj, gen);
        if (why) *why = "D3D9 流水线：绑定的顶点缓冲已不是本模块的对象";
        return false;
    }
    if (cpu.com->generationOf(obj) != gen || gen == 0) {
        pipeWarn(PipeWarnCategory::VertexStream, what,
                 "绑定身份不符：槽里的%s 0x%08X 已被**另一个对象**复用（绑定时代际 %u，当前 %u）⇒ D3DERR_INVALIDCALL"
                 "（⛔ 不改用新占有者的顶点数据）",
                 slot, obj, gen, cpu.com->generationOf(obj));
        if (why) *why = "D3D9 流水线：绑定的顶点缓冲已被复用（身份不符）";
        return false;
    }
    const char* iface = cpu.com->ifaceOf(obj);
    if (iface == nullptr || std::strcmp(iface, "IDirect3DVertexBuffer9") != 0) {
        pipeWarn(PipeWarnCategory::VertexStream, what,
                 "绑定身份不符：槽里的 0x%08X 现在不是顶点缓冲（%s）⇒ D3DERR_INVALIDCALL", obj,
                 iface ? iface : "?");
        if (why) *why = "D3D9 流水线：绑定的顶点缓冲已被别的类型复用";
        return false;
    }
    const std::uint32_t st = cpu.com->userOf(obj);
    if (!vertexBufferInfoOfState(cpu, st, length, data_ea, nullptr, why)) {
        pipeWarn(PipeWarnCategory::VertexStream, what,
                 "绑定身份不符：顶点缓冲 0x%08X 的状态块 0x%08X 不是 'VB09' ⇒ D3DERR_INVALIDCALL", obj, st);
        return false;
    }
    if (state) *state = st;
    return true;
}

bool boundIndexBufferOfState(X86& cpu, std::uint32_t obj, std::uint32_t gen, const char* what,
                             std::uint32_t* state, std::uint32_t* length, std::uint32_t* data_ea,
                             std::uint32_t* format, std::string* why) {
    const char* slot = "索引缓冲";
    if (obj == 0 || !cpu.com || !cpu.com->find(obj)) {
        pipeWarn(PipeWarnCategory::Index, what,
                 "绑定身份不符：槽里的%s 0x%08X 已不在对象表里（绑定时代际 %u）⇒ D3DERR_INVALIDCALL",
                 slot, obj, gen);
        if (why) *why = "D3D9 流水线：绑定的索引缓冲已不是本模块的对象";
        return false;
    }
    if (cpu.com->generationOf(obj) != gen || gen == 0) {
        pipeWarn(PipeWarnCategory::Index, what,
                 "绑定身份不符：槽里的%s 0x%08X 已被**另一个对象**复用（绑定时代际 %u，当前 %u）⇒ D3DERR_INVALIDCALL"
                 "（⛔ 不改用新占有者的索引数据）",
                 slot, obj, gen, cpu.com->generationOf(obj));
        if (why) *why = "D3D9 流水线：绑定的索引缓冲已被复用（身份不符）";
        return false;
    }
    const char* iface = cpu.com->ifaceOf(obj);
    if (iface == nullptr || std::strcmp(iface, "IDirect3DIndexBuffer9") != 0) {
        pipeWarn(PipeWarnCategory::Index, what,
                 "绑定身份不符：槽里的 0x%08X 现在不是索引缓冲（%s）⇒ D3DERR_INVALIDCALL", obj,
                 iface ? iface : "?");
        if (why) *why = "D3D9 流水线：绑定的索引缓冲已被别的类型复用";
        return false;
    }
    const std::uint32_t st = cpu.com->userOf(obj);
    if (!indexBufferInfoOfState(cpu, st, length, data_ea, format, why)) {
        pipeWarn(PipeWarnCategory::Index, what,
                 "绑定身份不符：索引缓冲 0x%08X 的状态块 0x%08X 不是 'IB09' ⇒ D3DERR_INVALIDCALL", obj, st);
        return false;
    }
    if (state) *state = st;
    return true;
}

std::uint32_t setStreamSource(X86& cpu, std::uint32_t dev, std::uint32_t stream, std::uint32_t vb,
                              std::uint32_t offset, std::uint32_t stride, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    if (stream >= kMaxStreams) {
        pipeLog("SetStreamSource", "Stream=%u 越界（上限 %u）⇒ D3DERR_INVALIDCALL", stream, kMaxStreams);
        return kInvalidCall;
    }
    const std::uint32_t slot = kPlStreams + stream * 16u;
    const std::uint32_t old_obj = plGet(cpu, pe, slot + 0u);
    const std::uint32_t old_gen = plGet(cpu, pe, slot + 12u);
    std::uint32_t obj = 0;
    std::uint32_t obj_gen = 0;
    // 槽里的 (EA, 代际) 与这次要绑的是不是**同一份绑定**（决定 addRef / releaseIf 要不要动）。
    // 解绑（vb == 0）⇒ obj = obj_gen = 0：只有"本来就是空的"才算同一份。
    bool same_binding = (old_obj == 0 && old_gen == 0);
    if (vb != 0) {
        // ★ 入参是 guest 可见的 **IDirect3DVertexBuffer9 对象**；槽里存对象 EA + **绑定时代际**
        //   （状态块在 Draw 时按 `userOf` 再算 —— 见 `boundVertexBufferOfState`）。
        //   旧实现把**状态块** EA 存进槽里、Draw 只验 magic ⇒ 同 EA 新缓冲会"假匹配"。
        if (!cpu.com) {
            if (why) *why = "D3D9 流水线：SetStreamSource 需要 cpu.com 来解析顶点缓冲对象";
            return kInvalidCall;
        }
        const std::uint32_t vb_state = cpu.com->userOf(vb);
        std::uint32_t length = 0;
        if (vb_state == 0 ||
            !vertexBufferInfoOfState(cpu, vb_state, &length, nullptr, nullptr, why)) {
            pipeLog("SetStreamSource", "0x%08X 不是本模块建的顶点缓冲 ⇒ D3DERR_INVALIDCALL", vb);
            return kInvalidCall;
        }
        if (offset > length) {
            pipeLog("SetStreamSource", "OffsetInBytes=%u 超过缓冲长度 %u ⇒ D3DERR_INVALIDCALL", offset, length);
            return kInvalidCall;
        }
        obj = vb;
        obj_gen = cpu.com->generationOf(vb);
        if (obj_gen == 0) {
            pipeWarn(PipeWarnCategory::VertexStream, "SetStreamSource",
                     "顶点缓冲 0x%08X 没有代际（对象表不一致）⇒ D3DERR_INVALIDCALL", vb);
            if (why) *why = "D3D9 流水线：SetStreamSource 的对象没有代际";
            return kInvalidCall;
        }
        // ★ 设备替 guest 持一份**绑定引用**（与 `SetTexture` 的既有口径一致）：这样
        //   "SetStreamSource 之后 guest 放掉自己那份、再 Draw" 这条**合法**时序不会把对象释放掉。
        //   顺序 = 先 addRef 新的、再写槽、最后放旧的 ⇒ 同一对象重复 Set 不会瞬时归零。
        // ★ 判"同一个"必须比 **(EA, 代际)**：同 EA 不同代际 = 复用 EA 的**新缓冲**（另一个对象）。
        if (obj != old_obj || obj_gen != old_gen) {
            if (!cpu.com->addRef(cpu, obj)) {
                pipeWarn(PipeWarnCategory::VertexStream, "SetStreamSource",
                         "顶点缓冲 0x%08X 的 AddRef 失败（COM 状态坏了）⇒ D3DERR_INVALIDCALL", vb);
                if (why) *why = "D3D9 流水线：SetStreamSource 持有绑定引用失败";
                return kInvalidCall;
            }
        }
        same_binding = (obj == old_obj && obj_gen == old_gen);
    }
    plSet(cpu, pe, slot + 0u, obj);
    plSet(cpu, pe, slot + 4u, offset);
    plSet(cpu, pe, slot + 8u, stride);
    plSet(cpu, pe, slot + 12u, obj_gen);
    if (old_obj != 0 && !same_binding)
        releaseBound(cpu, old_obj, old_gen, "SetStreamSource", PipeWarnCategory::VertexStream);
    return kOk;
}

std::uint32_t setIndices(X86& cpu, std::uint32_t dev, std::uint32_t ib, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return kInvalidCall;
    const std::uint32_t old_obj = plGet(cpu, pe, kPlIndices);
    const std::uint32_t old_gen = plGet(cpu, pe, kPlIndicesGen);
    std::uint32_t obj = 0;
    std::uint32_t obj_gen = 0;
    // 同 SetStreamSource：(EA, 代际) 都相同才算"同一份绑定"。
    bool same_binding = (old_obj == 0 && old_gen == 0);
    if (ib != 0) {
        // 同 SetStreamSource：入参是对象 EA，槽里存**对象 EA + 绑定时代际**。
        if (!cpu.com) {
            if (why) *why = "D3D9 流水线：SetIndices 需要 cpu.com 来解析索引缓冲对象";
            return kInvalidCall;
        }
        const std::uint32_t ib_state = cpu.com->userOf(ib);
        std::uint32_t length = 0;
        if (ib_state == 0 ||
            !indexBufferInfoOfState(cpu, ib_state, &length, nullptr, nullptr, why)) {
            pipeLog("SetIndices", "0x%08X 不是本模块建的索引缓冲 ⇒ D3DERR_INVALIDCALL", ib);
            return kInvalidCall;
        }
        obj = ib;
        obj_gen = cpu.com->generationOf(ib);
        if (obj_gen == 0) {
            pipeWarn(PipeWarnCategory::Index, "SetIndices",
                     "索引缓冲 0x%08X 没有代际（对象表不一致）⇒ D3DERR_INVALIDCALL", ib);
            if (why) *why = "D3D9 流水线：SetIndices 的对象没有代际";
            return kInvalidCall;
        }
        // ★ 同 EA 不同代际 = 复用 EA 的**新索引缓冲**（另一个对象）⇒ 必须 addRef 它、
        //   ⛔ 不能因为 EA 相同就当成"重复 Set 同一个"（那样绑定没有引用，guest 一放就归 0）。
        if (obj != old_obj || obj_gen != old_gen) {
            if (!cpu.com->addRef(cpu, obj)) {
                pipeWarn(PipeWarnCategory::Index, "SetIndices",
                         "索引缓冲 0x%08X 的 AddRef 失败（COM 状态坏了）⇒ D3DERR_INVALIDCALL", ib);
                if (why) *why = "D3D9 流水线：SetIndices 持有绑定引用失败";
                return kInvalidCall;
            }
        }
        same_binding = (obj == old_obj && obj_gen == old_gen);
    }
    plSet(cpu, pe, kPlIndices, obj);
    plSet(cpu, pe, kPlIndicesGen, obj_gen);
    if (old_obj != 0 && !same_binding)
        releaseBound(cpu, old_obj, old_gen, "SetIndices", PipeWarnCategory::Index);
    return kOk;
}

// =====================================================================================
// IDirect3DStateBlock9：宿主私有状态快照（`REQ-01M7372BPVGBD247KAGHB53YK7`）
//
// ★ 为什么要有它：引擎每帧 Live2D 都走 `CreateStateBlock(D3DSBT_ALL=1) → draw → Apply → Release`
//   （`.lst` 0x47843A–0x47847B）。修前 Capture / Apply 直接 S_OK ⇒ draw 里改的 FVF / 纹理绑定 /
//   渲染状态 / 变换**不还原**，泄漏到后续 2D 绘制。
// ★ 快照收什么：**本线 `kPl*` 区里真的有的字段**（⛔ 不按 SDK 全字段猜），按 D3DSBT 掩码选：
//   · ALL(1)    = 全部。渲染状态取**整块 256 项** —— 本线没有它们的"像素 / 顶点"归类证据，
//                 所以 ALL 不挑（也让 `GetRenderState` 的任意一项都能回滚）；
//   · PIXEL(2)  = 采样器 + 纹理级 + 纹理绑定 + **光栅阶段真的读**的渲染状态
//                 （`kSbPixelRenderStates`，读点全在 `prepareContext` 的一次性快照里）；
//   · VERTEX(3) = 变换 + FVF / 顶点声明 + 流 + 索引 + 视口 / 裁剪矩形 + 材质 / 灯
//                 + **顶点 / 光照阶段真的读**的渲染状态（`kSbVertexRenderStates`，读点在
//                 `applyLighting` / `transformVertex`）。
//   ⇒ ALL ⊇ PIXEL ∪ VERTEX；两个半块各有对方没有的字段 ⇒ 掩码**有意义**（不是三档同一个东西）。
//   ★ 如实登记的缺项（⛔ 不按记忆补）：
//     ① 渲染目标（`kPlColorArena` / `kPlColorSurface`）不在任何掩码里 —— D3D9 的状态块也不恢复 RT；
//     ② `kPlUnsupported` / `kPlStageSnapshots` / Draw 计数器是**诊断**，不是设备状态；
//     ③ 渲染状态的"像素 / 顶点"归类**只按本线的读点**给出，⛔ 不声称与 SDK 的 D3DSBT 分类一致
//        （那需要一份能再校验的分类表，本单没有）。
// ★ 引用：快照**替 guest 持一份**四类绑定（纹理 / 流 / 索引 / 顶点声明）的引用 —— 与 `SetTexture`
//   等既有口径同形（`addRef` + 带代际的 `releaseIf`）。于是"建块之后 guest 放掉自己那份"这条
//   合法的 D3D9 时序里对象死不了，Apply 还能把它写回；块销毁时成对还回去。
// =====================================================================================
namespace {

// ---- 渲染状态的掩码表（⛔ 不是 SDK 分类表：每一条都能在本文件找到读点）----
// 光栅（像素）阶段：`prepareContext` 的一次性状态快照（`rsOf(cpu, pe, …)` 的批量段）。
const std::uint32_t kSbPixelRenderStates[] = {
    kRsScissorTestEnable,     kRsZEnable,       kRsZWriteEnable,        kRsZFunc,
    kRsCullMode,              kRsShadeMode,     kRsAlphaTestEnable,     kRsAlphaFunc,
    kRsAlphaRef,              kRsAlphaBlendEnable, kRsSrcBlend,        kRsDestBlend,
    kRsBlendOp,               kRsSeparateAlphaBlendEnable, kRsSrcBlendAlpha,
    kRsDestBlendAlpha,        kRsBlendOpAlpha,  kRsColorWriteEnable,    kRsTextureFactor,
};
// 顶点 / 光照阶段：`applyLighting`（环境光 / 镜面开关）与 `transformVertex`（光照开关 / 材质来源）。
const std::uint32_t kSbVertexRenderStates[] = {
    kRsLighting,
    kRsAmbient,
    kRsSpecularEnable,
    kRsDiffuseMaterialSource,
};
constexpr std::uint32_t kSbPixelRenderStateCount =
    static_cast<std::uint32_t>(sizeof(kSbPixelRenderStates) / sizeof(kSbPixelRenderStates[0]));
constexpr std::uint32_t kSbVertexRenderStateCount =
    static_cast<std::uint32_t>(sizeof(kSbVertexRenderStates) / sizeof(kSbVertexRenderStates[0]));

struct SbMask {
    bool render_all = false;     // 渲染状态整块（只有 ALL 走这一档）
    bool render_pixel = false;   // 只取 `kSbPixelRenderStates`
    bool render_vertex = false;  // 只取 `kSbVertexRenderStates`
    bool transforms = false;     // `kPlTransforms`（260 个 D3DMATRIX）
    bool sampler_stages = false;  // `kPlSamplers` + `kPlTexStages`
    bool textures = false;       // `kPlTextures` + `kPlTextureGens`
    bool vertex_format = false;  // `kPlFvf` + `kPlVertexDecl`(+gen)
    bool streams = false;        // `kPlStreams`
    bool indices = false;        // `kPlIndices`(+gen)
    bool vertex_geometry = false;  // `kPlViewport` + `kPlScissor`
    bool material_lights = false;  // `kPlMaterial` + 8 个灯 + `kPlLightEnabled`
    bool any() const {
        return render_all || render_pixel || render_vertex || transforms || sampler_stages ||
               textures || vertex_format || streams || indices || vertex_geometry ||
               material_lights;
    }
};

SbMask sbMaskOf(std::uint32_t type) {
    SbMask m;
    switch (type) {
        case 1u:  // D3DSBT_ALL
            m.render_all = true;
            m.transforms = m.sampler_stages = m.textures = m.vertex_format = true;
            m.streams = m.indices = m.vertex_geometry = m.material_lights = true;
            break;
        case 2u:  // D3DSBT_PIXELSTATE
            m.render_pixel = m.sampler_stages = m.textures = true;
            break;
        case 3u:  // D3DSBT_VERTEXSTATE
            m.render_vertex = m.transforms = m.vertex_format = true;
            m.streams = m.indices = m.vertex_geometry = m.material_lights = true;
            break;
        default:
            break;
    }
    return m;
}

// 一条绑定：**(对象 EA, 捕获时代际)** —— 与 `kPl*Gens` 的既有约定同一口径。
struct SbBinding {
    std::uint32_t obj = 0;
    std::uint32_t gen = 0;
    // ★ 捕获时身份就不符 ⇒ `usable = false`：Apply **跳过**这一槽。⛔ 不把它记成 0 ——
    //   记 0 会让 Apply 主动**解绑**，把"我们认不出这个 EA 是谁"变成"设备状态被改了"。
    bool usable = true;
    bool held = false;  // 本快照替 guest 持了这份引用（AddRef 成功）
};

struct SbStreamState {
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    SbBinding binding;
};

struct StateBlockSnapshot {
    std::uint32_t type = 0;
    std::uint32_t dev_object = 0;
    std::uint32_t dev_state = 0;
    std::vector<std::uint32_t> render_states;   // 256 项（按掩码填；未选中的保持 0 且不写回）
    std::vector<std::uint8_t> transforms;       // [kPlTransforms, kPlRenderStates)
    std::vector<std::uint8_t> samplers;         // [kPlSamplers, kPlTexStages)
    std::vector<std::uint8_t> tex_stages;       // [kPlTexStages, kPlTextures)
    std::vector<std::uint8_t> vertex_geometry;  // [kPlViewport, kPlTransforms)（视口 + 裁剪矩形）
    std::vector<std::uint8_t> material_lights;  // [kPlMaterial, kPlDraws)（材质 + 灯 + 开关）
    std::uint32_t fvf = 0;
    SbBinding vertex_decl;
    SbBinding textures[kMaxSamplers];
    SbStreamState streams[kMaxStreams];
    SbBinding indices;
};

// 宿主表：serial → 快照。
// ★ 键是**进程内只增不复用**的 serial（`kSbSerial`，由 `shims/d3d9.cpp` 发），⛔ 不是
//   `(ComRegistry*, 代际)` —— 后者会跨 Machine 串台（见本文件头第 22 行的禁令：`ComRegistry` 的
//   代际每个 registry 各自从 1 起，而夹具的 Machine 反复建在同一栈地址）。
// ★ 故意 `new` 出来不释放（与 `pipeWarn` 的计数表同形：进程退出时跑静态析构没有意义）；
//   表项由 `on_zero` 的 `destroyStateBlockSnapshot` 摘除。
std::map<std::uint32_t, StateBlockSnapshot>& sbSnapshots() {
    static std::map<std::uint32_t, StateBlockSnapshot>* table =
        new std::map<std::uint32_t, StateBlockSnapshot>();
    return *table;
}

void sbCaptureRange(X86& cpu, std::uint32_t pe, std::uint32_t off, std::uint32_t bytes,
                    std::vector<std::uint8_t>* out) {
    out->resize(bytes);
    cpu.mem->readBytes(pe + off, out->data(), bytes);
}

// ★ 只用于**纯标量**区段：⛔ 绝不给绑定槽用（绑定槽必须走 setter，见 `applyStateBlock`）。
void sbApplyRange(X86& cpu, std::uint32_t pe, std::uint32_t off, const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return;
    cpu.mem->writeBytes(pe + off, bytes.data(), bytes.size());
}

SbBinding sbMakeBinding(X86& cpu, std::uint32_t obj, std::uint32_t gen, const char* iface,
                        const char* what) {
    SbBinding b;
    if (obj == 0) return b;  // 捕获那一刻就是空槽 ⇒ Apply 写回 0（解绑），这是正确语义
    b.obj = obj;
    b.gen = gen;
    const char* got = cpu.com ? cpu.com->ifaceOf(obj) : nullptr;
    const bool same = cpu.com != nullptr && got != nullptr && std::strcmp(got, iface) == 0 &&
                      gen != 0 && cpu.com->generationOf(obj) == gen;
    if (!same) {
        b.usable = false;
        pipeWarn(PipeWarnCategory::Other, "状态块快照",
                 "%s：槽里的 0x%08X 已不是捕获那一刻的对象（%s；捕获时代际 %u，当前 %u）"
                 "⇒ 这一槽不进快照（⛔ 不按 EA 抓别人的对象）",
                 what, obj, got ? got : "不在对象表里", gen,
                 cpu.com ? cpu.com->generationOf(obj) : 0u);
    }
    return b;
}

// 把快照持有的引用**成对还回去**（带代际 ⇒ EA 已被复用就只出声、⛔ 不减到新占有者身上）。
void sbReleaseRefs(X86& cpu, StateBlockSnapshot& snap) {
    auto drop = [&cpu](SbBinding& b, const char* what) {
        if (!b.held) {
            b.obj = 0;
            b.gen = 0;
            b.usable = true;
            return;
        }
        b.held = false;
        const std::uint32_t obj = b.obj;
        const std::uint32_t gen = b.gen;
        b.obj = 0;
        b.gen = 0;
        b.usable = true;
        if (obj != 0) releaseBound(cpu, obj, gen, what, PipeWarnCategory::Other);
    };
    for (std::uint32_t s = 0; s < kMaxSamplers; ++s) drop(snap.textures[s], "状态块快照/纹理");
    drop(snap.vertex_decl, "状态块快照/顶点声明");
    drop(snap.indices, "状态块快照/索引");
    for (std::uint32_t s = 0; s < kMaxStreams; ++s) drop(snap.streams[s].binding, "状态块快照/流");
    snap.render_states.clear();
    snap.transforms.clear();
    snap.samplers.clear();
    snap.tex_stages.clear();
    snap.vertex_geometry.clear();
    snap.material_lights.clear();
    snap.fvf = 0;
}

// 替 guest 持引用（`addRef`）。失败 ⇒ 一条 warn + 该槽标成不可用（返回值 = 有没有失败）。
bool sbHoldRefs(X86& cpu, StateBlockSnapshot* snap) {
    bool ok = true;
    auto hold = [&](SbBinding& b, const char* what) {
        if (!b.usable || b.obj == 0 || b.held) return;
        if (!cpu.com || !cpu.com->addRef(cpu, b.obj)) {
            pipeWarn(PipeWarnCategory::Other, "状态块快照",
                     "%s 0x%08X 的 AddRef 失败（COM 状态坏了）⇒ 快照不持有它", what, b.obj);
            b.usable = false;
            ok = false;
            return;
        }
        b.held = true;
    };
    for (std::uint32_t s = 0; s < kMaxSamplers; ++s) hold(snap->textures[s], "纹理");
    hold(snap->vertex_decl, "顶点声明");
    hold(snap->indices, "索引缓冲");
    for (std::uint32_t s = 0; s < kMaxStreams; ++s) hold(snap->streams[s].binding, "顶点缓冲");
    return ok;
}

// 采集：**只填纯数据**（不 addRef ⇒ 分配失败时没有任何引用要回滚；引用由 `sbHoldRefs` 统一加）。
void sbCapture(X86& cpu, std::uint32_t pe, const SbMask& mask, std::uint32_t type,
               const DeviceInfo& dev, StateBlockSnapshot* out) {
    out->type = type;
    out->dev_object = dev.object;
    out->dev_state = dev.state;
    if (mask.render_all || mask.render_pixel || mask.render_vertex) {
        out->render_states.assign(kPlRenderStateCount, 0u);
        if (mask.render_all) {
            for (std::uint32_t id = 0; id < kPlRenderStateCount; ++id)
                out->render_states[id] = rsOf(cpu, pe, id);
        } else if (mask.render_pixel) {
            for (std::uint32_t i = 0; i < kSbPixelRenderStateCount; ++i) {
                const std::uint32_t id = kSbPixelRenderStates[i];
                if (id < kPlRenderStateCount) out->render_states[id] = rsOf(cpu, pe, id);
            }
        } else {
            for (std::uint32_t i = 0; i < kSbVertexRenderStateCount; ++i) {
                const std::uint32_t id = kSbVertexRenderStates[i];
                if (id < kPlRenderStateCount) out->render_states[id] = rsOf(cpu, pe, id);
            }
        }
    }
    if (mask.transforms)
        sbCaptureRange(cpu, pe, kPlTransforms, kPlRenderStates - kPlTransforms, &out->transforms);
    if (mask.sampler_stages) {
        sbCaptureRange(cpu, pe, kPlSamplers, kPlTexStages - kPlSamplers, &out->samplers);
        sbCaptureRange(cpu, pe, kPlTexStages, kPlTextures - kPlTexStages, &out->tex_stages);
    }
    if (mask.vertex_geometry)
        sbCaptureRange(cpu, pe, kPlViewport, kPlTransforms - kPlViewport, &out->vertex_geometry);
    if (mask.material_lights)
        sbCaptureRange(cpu, pe, kPlMaterial, kPlDraws - kPlMaterial, &out->material_lights);
    if (mask.vertex_format) {
        out->fvf = plGet(cpu, pe, kPlFvf);
        out->vertex_decl = sbMakeBinding(cpu, plGet(cpu, pe, kPlVertexDecl),
                                         plGet(cpu, pe, kPlVertexDeclGen), "IDirect3DVertexDeclaration9",
                                         "捕获/顶点声明");
    }
    if (mask.textures) {
        for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
            out->textures[s] = sbMakeBinding(cpu, plGet(cpu, pe, kPlTextures + s * 4u),
                                             plGet(cpu, pe, kPlTextureGens + s * 4u),
                                             "IDirect3DTexture9", "捕获/纹理绑定");
        }
    }
    if (mask.streams) {
        for (std::uint32_t s = 0; s < kMaxStreams; ++s) {
            const std::uint32_t slot = kPlStreams + s * 16u;
            out->streams[s].offset = plGet(cpu, pe, slot + 4u);
            out->streams[s].stride = plGet(cpu, pe, slot + 8u);
            out->streams[s].binding =
                sbMakeBinding(cpu, plGet(cpu, pe, slot + 0u), plGet(cpu, pe, slot + 12u),
                              "IDirect3DVertexBuffer9", "捕获/流绑定");
        }
    }
    if (mask.indices) {
        out->indices = sbMakeBinding(cpu, plGet(cpu, pe, kPlIndices), plGet(cpu, pe, kPlIndicesGen),
                                     "IDirect3DIndexBuffer9", "捕获/索引绑定");
    }
}

}  // namespace

std::uint32_t captureStateBlock(X86& cpu, std::uint32_t handle, std::string* why,
                                bool* out_of_memory) {
    if (out_of_memory) *out_of_memory = false;
    if (handle == 0 || !cpu.mem || !cpu.mem->committed(handle, kSbBytes) ||
        cpu.mem->rd32(handle + kSbMagic) != kSbMagicValue) {
        if (why) *why = "D3D9 状态块：句柄块不是 'SB09'（对象状态崩坏）";
        return kInvalidCall;
    }
    const std::uint32_t serial = cpu.mem->rd32(handle + kSbSerial);
    const std::uint32_t type = cpu.mem->rd32(handle + kSbType);
    const std::uint32_t dev_object = cpu.mem->rd32(handle + kSbDevice);
    if (serial == 0) {
        if (why) *why = "D3D9 状态块：句柄块里的 serial 为 0（没走 nextStateBlockSerial？）";
        return kInvalidCall;
    }
    if (!cpu.com || !cpu.com->find(dev_object)) {
        if (why) *why = "D3D9 状态块：句柄里的设备对象已不在对象表里";
        return kInvalidCall;
    }
    DeviceInfo dev;
    if (!deviceInfo(cpu, dev_object, &dev, why)) return kInvalidCall;
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev.state, &pe, why)) return kInvalidCall;
    const SbMask mask = sbMaskOf(type);
    if (!mask.any()) {
        if (why) *why = "D3D9 状态块：Type 不是 D3DSBT_ALL / PIXELSTATE / VERTEXSTATE";
        return kInvalidCall;
    }
    StateBlockSnapshot fresh;
    try {
        sbCapture(cpu, pe, mask, type, dev, &fresh);
        if (!sbHoldRefs(cpu, &fresh)) {
            sbReleaseRefs(cpu, fresh);
            if (why) *why = "D3D9 状态块：快照替绑定对象持引用失败（COM 状态坏了）";
            return kFail;
        }
        StateBlockSnapshot& slot = sbSnapshots()[serial];
        // ★ 顺序 = **先加新的、再放旧的**（与 `SetTexture` 的换绑同口径）：同一对象重捕时不会
        //   在两步之间瞬时针到 0。
        sbReleaseRefs(cpu, slot);
        slot = std::move(fresh);  // `std::vector` 的 move 赋值 ⇒ 不抛
    } catch (const std::bad_alloc&) {
        sbReleaseRefs(cpu, fresh);
        if (out_of_memory) *out_of_memory = true;
        if (why) *why = "D3D9 状态块：宿主堆分配快照失败 ⇒ E_OUTOFMEMORY（不 trap、不留半个快照）";
        return kOutOfMemory;
    }
    return kOk;
}

std::uint32_t applyStateBlock(X86& cpu, std::uint32_t handle, std::string* why) {
    if (handle == 0 || !cpu.mem || !cpu.mem->committed(handle, kSbBytes) ||
        cpu.mem->rd32(handle + kSbMagic) != kSbMagicValue) {
        if (why) *why = "D3D9 状态块：句柄块不是 'SB09'（对象状态崩坏）";
        return kInvalidCall;
    }
    const std::uint32_t serial = cpu.mem->rd32(handle + kSbSerial);
    const auto it = sbSnapshots().find(serial);
    if (it == sbSnapshots().end()) {
        if (why) *why = "D3D9 状态块：宿主快照表里没有这个 serial（块没建过 / 已销毁）";
        return kInvalidCall;
    }
    const StateBlockSnapshot& snap = it->second;
    // ★ 设备按**句柄里的设备对象**重新解析（⛔ 不缓存设备状态块 EA：guest 堆会复用 EA）。
    const std::uint32_t dev_object = cpu.mem->rd32(handle + kSbDevice);
    if (!cpu.com || !cpu.com->find(dev_object)) {
        if (why) *why = "D3D9 状态块：句柄里的设备对象已不在对象表里";
        return kInvalidCall;
    }
    DeviceInfo dev;
    if (!deviceInfo(cpu, dev_object, &dev, why)) return kInvalidCall;
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev.state, &pe, why)) return kInvalidCall;
    const SbMask mask = sbMaskOf(snap.type);
    bool failed = false;
    // ① 绑定：走既有 setter ⇒ 身份校验 / addRef / `releaseIf` / 日志全部是**同一份**既有约定。
    //    ⛔ 不先写绑定槽再调 setter：setter 按槽里的 (EA, 代际) 判"同一份绑定"，先写就会让
    //    旧绑定的设备引用永远还不回去（设备销毁时正是从这些槽里回收的）。
    if (mask.textures) {
        for (std::uint32_t s = 0; s < kMaxSamplers; ++s) {
            const SbBinding& b = snap.textures[s];
            if (!b.usable) {
                failed = true;
                continue;
            }
            // ★ 空槽也写回 0：捕获那一刻没绑 ⇒ Apply 之后也没绑（⛔ 不是"跳过"）。
            if (setTexture(cpu, dev.state, s, b.obj, nullptr) != kOk) failed = true;
        }
    }
    if (mask.streams) {
        for (std::uint32_t s = 0; s < kMaxStreams; ++s) {
            const SbStreamState& st = snap.streams[s];
            if (!st.binding.usable) {
                failed = true;
                continue;
            }
            if (setStreamSource(cpu, dev.state, s, st.binding.obj, st.offset, st.stride, nullptr) != kOk)
                failed = true;
        }
    }
    if (mask.indices) {
        if (!snap.indices.usable) {
            failed = true;
        } else if (setIndices(cpu, dev.state, snap.indices.obj, nullptr) != kOk) {
            failed = true;
        }
    }
    if (mask.vertex_format) {
        if (!snap.vertex_decl.usable) {
            failed = true;  // 声明身份不符 ⇒ ⛔ 不动顶点格式（跳过，不是解绑）
        } else if (snap.vertex_decl.obj != 0) {
            if (setVertexDeclaration(cpu, dev.state, snap.vertex_decl.obj, nullptr) != kOk) failed = true;
        } else if (setFVF(cpu, dev.state, snap.fvf, nullptr) != kOk) {
            // 捕获那一刻没有顶点声明 ⇒ FVF 路（本线的二选一：`setFVF` 会清声明）。
            failed = true;
        }
    }
    // ② 纯标量：⛔ 不含任何绑定槽（`kPlTextures` / `kPlStreams` / `kPlIndices` /
    //    `kPlVertexDecl`(+gen) / `kPlFvf`）—— 那些必须由上面的 setter 改。
    if (!snap.render_states.empty()) {
        if (mask.render_all) {
            for (std::uint32_t id = 0; id < kPlRenderStateCount; ++id)
                plSet(cpu, pe, kPlRenderStates + id * 4u, snap.render_states[id]);
        } else if (mask.render_pixel) {
            for (std::uint32_t i = 0; i < kSbPixelRenderStateCount; ++i) {
                const std::uint32_t id = kSbPixelRenderStates[i];
                if (id < kPlRenderStateCount)
                    plSet(cpu, pe, kPlRenderStates + id * 4u, snap.render_states[id]);
            }
        } else if (mask.render_vertex) {
            for (std::uint32_t i = 0; i < kSbVertexRenderStateCount; ++i) {
                const std::uint32_t id = kSbVertexRenderStates[i];
                if (id < kPlRenderStateCount)
                    plSet(cpu, pe, kPlRenderStates + id * 4u, snap.render_states[id]);
            }
        }
    }
    sbApplyRange(cpu, pe, kPlTransforms, snap.transforms);
    sbApplyRange(cpu, pe, kPlSamplers, snap.samplers);
    sbApplyRange(cpu, pe, kPlTexStages, snap.tex_stages);
    sbApplyRange(cpu, pe, kPlViewport, snap.vertex_geometry);
    sbApplyRange(cpu, pe, kPlMaterial, snap.material_lights);
    if (failed) {
        // ★ 默认级别可见（`pipeWarn`），⛔ 不谎报 S_OK：引擎虽然丢弃了 Apply 的返回值
        //   （`.lst` 0x478470 之后直接取 var_4），但"没恢复"这件事必须留痕。
        pipeWarn(PipeWarnCategory::Other, "StateBlock::Apply",
                 "0x%08X：有绑定没能恢复（槽里的对象已不是捕获时那个 / COM 状态坏了）"
                 "⇒ D3DERR_INVALIDCALL（已写回的标量不回滚）",
                 handle);
        if (why) *why = "D3D9 状态块：Apply 有绑定未能恢复";
        return kInvalidCall;
    }
    return kOk;
}

void destroyStateBlockSnapshot(X86& cpu, std::uint32_t handle) {
    if (handle == 0 || !cpu.mem || !cpu.heap) return;
    if (!cpu.mem->committed(handle, kSbBytes)) return;
    if (cpu.mem->rd32(handle + kSbMagic) != kSbMagicValue) return;  // 幂等：已经回收过
    const std::uint32_t serial = cpu.mem->rd32(handle + kSbSerial);
    cpu.mem->wr32(handle + kSbMagic, 0u);
    const auto it = sbSnapshots().find(serial);
    if (it != sbSnapshots().end()) {
        sbReleaseRefs(cpu, it->second);
        sbSnapshots().erase(it);
    }
    cpu.heap->free(handle);
}

// =====================================================================================
// Draw：顶点装配 → T&L → 裁剪 → 光栅
// =====================================================================================
namespace {

// 把一次 Draw 的顶点喂进光栅；返回 false ⇒ 硬失败（why 已填）
bool submitPrimitives(X86& cpu, std::uint32_t pe, std::uint32_t prim, const std::vector<CVertex>& verts,
                      std::uint32_t prim_count, DrawContext& ctx, std::uint32_t* triangles_out,
                      std::uint32_t* rasterized_out, std::string* why);
bool prepareContext(X86& cpu, const char* what, std::uint32_t dev, std::uint32_t pe, DrawContext* ctx,
                    std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    DeviceInfo dev_info;
    if (!deviceInfoOfState(cpu, dev, &dev_info, why)) return false;
    const std::uint32_t color_arena = plGet(cpu, pe, kPlColorArena);
    GuestSurface* gs = m->surfaces().find(color_arena != 0 ? color_arena : dev_info.arena);
    if (!gs) {
        if (why) *why = color_arena != 0 ? "D3D9 流水线：渲染目标像素不在 arena 里"
                                         : "D3D9 流水线：后缓冲像素不在 arena 里";
        return false;
    }
    const std::uint32_t depth = plGet(cpu, pe, kPlDepthArena);
    GuestSurface* dgs = m->surfaces().find(depth);
    if (!dgs) {
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return false;
    }
    ctx->cpu = &cpu;
    ctx->machine = m;
    ctx->gpu = m->gpuRaster();
    ctx->pe = pe;
    ctx->fvf = plGet(cpu, pe, kPlFvf);
    ctx->color_base = m->surfaces().pixels(*gs);
    ctx->color_fmt = gs->format;
    ctx->color_width = gs->width;
    ctx->color_height = gs->height;
    ctx->color_pitch = gs->pitch;
    ctx->color_surface = plGet(cpu, pe, kPlColorSurface);
    ctx->depth_base = reinterpret_cast<float*>(m->surfaces().pixels(*dgs));
    ctx->depth_pitch_floats = dgs->pitch / 4u;
    ctx->depth_width = dgs->width;
    ctx->depth_height = dgs->height;
    if (!ctx->color_base || !ctx->depth_base) {
        if (why) *why = "D3D9 流水线：后缓冲 / 深度缓冲像素指针为空";
        return false;
    }
    // 视口
    ctx->viewport_x = static_cast<int>(plGet(cpu, pe, kPlViewport + 0u));
    ctx->viewport_y = static_cast<int>(plGet(cpu, pe, kPlViewport + 4u));
    ctx->viewport_w = static_cast<int>(plGet(cpu, pe, kPlViewport + 8u));
    ctx->viewport_h = static_cast<int>(plGet(cpu, pe, kPlViewport + 12u));
    // 裁剪矩形（ScissorTest 关 ⇒ 整个后缓冲）
    int sx0 = 0;
    int sy0 = 0;
    int sx1 = gs->width;
    int sy1 = gs->height;
    if (rsOf(cpu, pe, kRsScissorTestEnable) != 0u) {
        sx0 = static_cast<int>(plGet(cpu, pe, kPlScissor + 0u));
        sy0 = static_cast<int>(plGet(cpu, pe, kPlScissor + 4u));
        sx1 = static_cast<int>(plGet(cpu, pe, kPlScissor + 8u));
        sy1 = static_cast<int>(plGet(cpu, pe, kPlScissor + 12u));
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > gs->width) sx1 = gs->width;
        if (sy1 > gs->height) sy1 = gs->height;
    }
    ctx->scissor_x0 = sx0;
    ctx->scissor_y0 = sy0;
    ctx->scissor_x1 = sx1;
    ctx->scissor_y1 = sy1;
    // 状态快照
    ctx->z_enable = rsOf(cpu, pe, kRsZEnable) != 0u;
    ctx->z_write = rsOf(cpu, pe, kRsZWriteEnable) != 0u;
    ctx->z_func = rsOf(cpu, pe, kRsZFunc);
    ctx->cull = rsOf(cpu, pe, kRsCullMode);
    ctx->shade = rsOf(cpu, pe, kRsShadeMode);
    ctx->alpha_test = rsOf(cpu, pe, kRsAlphaTestEnable) != 0u;
    ctx->alpha_func = rsOf(cpu, pe, kRsAlphaFunc);
    ctx->alpha_ref = rsOf(cpu, pe, kRsAlphaRef);
    ctx->blend = rsOf(cpu, pe, kRsAlphaBlendEnable) != 0u;
    ctx->src_blend = rsOf(cpu, pe, kRsSrcBlend);
    ctx->dst_blend = rsOf(cpu, pe, kRsDestBlend);
    ctx->blend_op = rsOf(cpu, pe, kRsBlendOp);
    ctx->separate_alpha_blend = rsOf(cpu, pe, kRsSeparateAlphaBlendEnable) != 0u;
    ctx->src_blend_a = rsOf(cpu, pe, kRsSrcBlendAlpha);
    ctx->dst_blend_a = rsOf(cpu, pe, kRsDestBlendAlpha);
    ctx->blend_op_a = rsOf(cpu, pe, kRsBlendOpAlpha);
    ctx->color_write = rsOf(cpu, pe, kRsColorWriteEnable);
    ctx->texture_factor = rsOf(cpu, pe, kRsTextureFactor);
    // ★ 派生 mip 的"轮"：本 Draw 里取到的链指针在本轮内不会被 LRU 回收（多 stage 同时持有）。
    beginMipEpoch();
    { static int serial = 0; ctx->pixels_written = 0; ctx->serial = ++serial; }
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        std::string terr;
        const std::uint32_t tex = plGet(cpu, pe, kPlTextures + s * 4u);
        // ★ 纹理槽的**身份** = **(对象 EA, `SetTexture` 时的代际)**，⛔ 不是"按 EA 解一次 `'TEX1'`"：
        //   guest 堆是首次适配 ⇒ 纹理释放后，同尺寸的下一张纹理拿到**同一个对象 EA** 与同一个
        //   `'TEX1'` 状态块 EA ⇒ 只验 magic 的旧口径在这一格**通过**，Draw 会**静默采样新纹理的像素**
        //   （缺陷单 `REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 4）。代际只在宿主表（`ComRegistry::generationOf`），
        //   ⛔ 不写进 guest 可见结构。不符 ⇒ **一次 Draw 整个跳过**（调用方回 `D3DERR_INVALIDCALL`）
        //   + 默认级别可见的告警，⛔ 不拿新占有者的像素顶替（与 `boundVertexBufferOfState` 同形态）。
        const std::uint32_t tex_gen = plGet(cpu, pe, kPlTextureGens + s * 4u);
        if (tex != 0) {
            const char* iface = cpu.com ? cpu.com->ifaceOf(tex) : nullptr;
            const bool same_object = cpu.com != nullptr && iface != nullptr &&
                                     std::strcmp(iface, "IDirect3DTexture9") == 0 && tex_gen != 0 &&
                                     cpu.com->generationOf(tex) == tex_gen;
            if (!same_object) {
                pipeWarn(PipeWarnCategory::Texture, what,
                         "纹理级 %u 的槽里 0x%08X 已不是绑定时那张纹理（%s；绑定时代际 %u，当前 %u）⇒ "
                         "D3DERR_INVALIDCALL（⛔ 不用新占有者的像素）",
                         s, tex, iface ? iface : "不在对象表里", tex_gen,
                         cpu.com ? cpu.com->generationOf(tex) : 0u);
                char buf[192];
                std::snprintf(buf, sizeof(buf), "D3D9 流水线：纹理级 %u 的绑定已被另一个对象复用（身份不符）", s);
                if (why) *why = buf;
                return false;
            }
        }
        if (!resolveTexture(cpu, m, tex, &ctx->textures[s], &terr)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "D3D9 流水线：纹理级 %u 的纹理无效：%s", s, terr.c_str());
            if (why) *why = buf;
            return false;
        }
    }
    // ★ 偏差 #4 的消除点：纹理级 / 采样器状态在这里**读一遍**（guest 在 Draw 期间不执行，
    //   所以这份快照就是整个光栅阶段看到的全部状态）。像素循环只读 ctx->stages / ctx->samplers。
    //   可观察量：kPlStageSnapshots 每次 Draw 只 +1（⛔ 不随三角形/像素增长）。
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        TextureStageSnapshot& ts = ctx->stages[s];
        ts.color_op = texStageOf(cpu, pe, s, kTssColorOp);
        ts.alpha_op = texStageOf(cpu, pe, s, kTssAlphaOp);
        ts.color_arg0 = texStageOf(cpu, pe, s, kTssColorArg0);
        ts.color_arg1 = texStageOf(cpu, pe, s, kTssColorArg1);
        ts.color_arg2 = texStageOf(cpu, pe, s, kTssColorArg2);
        ts.alpha_arg1 = texStageOf(cpu, pe, s, kTssAlphaArg1);
        ts.alpha_arg2 = texStageOf(cpu, pe, s, kTssAlphaArg2);
        ts.constant = texStageOf(cpu, pe, s, kTssConstant);
        ts.tex_coord_index = texStageOf(cpu, pe, s, kTssTexCoordIndex);
        SamplerSnapshot& ss = ctx->samplers[s];
        ss.address_u = samplerOf(cpu, pe, s, kSampAddressU);
        ss.address_v = samplerOf(cpu, pe, s, kSampAddressV);
        ss.mag_filter = samplerOf(cpu, pe, s, kSampMagFilter);
        ss.min_filter = samplerOf(cpu, pe, s, kSampMinFilter);
        // ★ MIPFILTER 必须读：`D3DTEXF_NONE` = 关掉 mipmap（只 level 0），其余才允许按 LOD 选级。
        //   引擎在真机上设的是 `D3DTEXF_LINEAR`（`.tmp/d3d9/art-debug.log:155`）⇒ 不能当成"没设"。
        ss.mip_filter = samplerOf(cpu, pe, s, kSampMipFilter);
    }
    plSet(cpu, pe, kPlStageSnapshots, plGet(cpu, pe, kPlStageSnapshots) + 1u);
    return true;
}

// 顶点变换（XYZ 路：WORLD → VIEW → PROJECTION；XYZRHW 路：已经是屏幕坐标）
void transformVertex(X86& cpu, std::uint32_t pe, const FvfLayout& fvf, const std::uint8_t* base,
                     std::uint32_t vertex_index, const math::Mat4d& world, const math::Mat4d& view,
                     const math::Mat4d& proj, CVertex* out) {
    readVertex(cpu, fvf, base, 0, out, nullptr);
    if (out->pretransformed) return;
    // 光照（先做：需要世界空间位置与法线）
    if (rsOf(cpu, pe, kRsLighting) != 0u && fvf.has_normal) {
        const math::Vec3d local(out->x, out->y, out->z);
        const math::Vec3d wpos = math::transformPoint3D3D(world, local);
        const std::uint8_t* np = base + fvf.normal_offset;
        float nf[3];
        std::memcpy(nf, np, sizeof(nf));
        const math::Vec3d localn(static_cast<double>(nf[0]), static_cast<double>(nf[1]),
                                 static_cast<double>(nf[2]));
        // ⚠ 近似：真机用 world 的逆转置（非等比缩放下会偏）
        const math::Vec3d n = math::transformVector3D3D(world, localn);
        double lit[4];
        applyLighting(cpu, pe, wpos, n, view, lit);
        const std::uint32_t diffuse_src = rsOf(cpu, pe, kRsDiffuseMaterialSource);
        if (diffuse_src == 1u && fvf.has_diffuse) {
            // D3DMCS_COLOR1：顶点色参与
            lit[0] *= out->r / 255.0;
            lit[1] *= out->g / 255.0;
            lit[2] *= out->b / 255.0;
        }
        out->r = lit[0] * 255.0;
        out->g = lit[1] * 255.0;
        out->b = lit[2] * 255.0;
        out->a = lit[3] * 255.0;
    } else if (rsOf(cpu, pe, kRsLighting) != 0u && !fvf.has_diffuse) {
        // 光照开着、既没法线也没漫反射 ⇒ 用材质漫反射（真机的固定功能也是这样）
        MaterialState mat;
        readMaterial(cpu, pe, &mat);
        out->r = mat.diffuse[0] * 255.0;
        out->g = mat.diffuse[1] * 255.0;
        out->b = mat.diffuse[2] * 255.0;
        out->a = mat.diffuse[3] * 255.0;
    }
    // WORLD → VIEW → PROJECTION 走**同一个** multiplyD3D（行向量顺序在 math 里反转）
    const math::Mat4d m = math::multiplyD3D(math::multiplyD3D(world, view), proj);
    const math::Vec4d clip = math::transformVec4D3D(m, math::Vec4d(out->x, out->y, out->z, 1.0));
    out->x = clip.x;
    out->y = clip.y;
    out->z = clip.z;
    out->w = clip.w;
    (void)vertex_index;
}

// GPU 一批的完整描述：状态 + 可选的 stage 0 纹理（准入通过后由 submitPrimitives 直接消费）。
struct GpuBatchPlan {
    GpuRasterState state;
    bool textured = false;
    // stage 0 用了 TFACTOR ⇒ 顶点色整批替换成 D3DRS_TEXTUREFACTOR（常量色，不依赖插值）。
    bool tex_color_from_tfactor = false;
    GpuRasterTexture texture;
};

bool gpuAddressFor(std::uint32_t mode, GpuAddress* out) {
    switch (mode) {
        case kAddressWrap: *out = GpuAddress::Wrap; return true;
        case kAddressClamp: *out = GpuAddress::Clamp; return true;
        case kAddressMirror: *out = GpuAddress::Mirror; return true;
        // BORDER / MIRROR_ONCE：SDL_GPU 的 sampler address mode 没有对应（BORDER 要 border color，
        // MIRROR_ONCE 语义不同）⇒ 回退 CPU 并记账。
        default: return false;
    }
}

bool gpuFilterFor(std::uint32_t f, GpuFilter* out) {
    switch (f) {
        case kTexfPoint: *out = GpuFilter::Point; return true;
        case kTexfLinear: *out = GpuFilter::Linear; return true;
        // ANISOTROPIC：CPU 侧 `samplerUsesLinear` 已按 LINEAR 近似；GPU 这边用同一条近似。
        case kTexfAnisotropic: *out = GpuFilter::Linear; return true;
        default: return false;  // NONE（不可见组合）⇒ 不猜像素
    }
}

// 无纹理时 CPU 的采样结果是白色 (255,255,255,255)：只有"结果恒等于顶点色"的 op 组合能由
// 无纹理 GPU 着色器（输出顶点色）等价表达。⛔ 不把 SELECTARG1(TEXTURE)=white 当成顶点色。
bool untexturedResolvesToDiffuse(std::uint32_t op, std::uint32_t arg1, std::uint32_t arg2) {
    auto diffuse = [](std::uint32_t a) { return a == kTaDiffuse || a == kTaCurrent; };
    auto texture = [](std::uint32_t a) { return a == kTaTexture; };
    switch (op) {
        // ★ CPU 的 applyTextureOp 对 DISABLE 落 default 分支、取 **arg1**；⛔ 不是无条件 DIFFUSE。
        //   无纹理 + DISABLE(arg1=TEXTURE) 时 CPU 得到白色，不能用"顶点色"冒充 ⇒ 必须 false。
        case kTopDisable: return untexturedResolvesToDiffuse(kTopSelectArg1, arg1, arg2);
        case kTopSelectArg1: return diffuse(arg1);
        case kTopSelectArg2: return diffuse(arg2);
        case kTopModulate:
            // MODULATE(TEXTURE(white), DIFFUSE) / MODULATE(DIFFUSE, TEXTURE(white)) = DIFFUSE
            return (texture(arg1) && diffuse(arg2)) || (diffuse(arg1) && texture(arg2));
        default: return false;
    }
}

// =====================================================================================
// 点 / 线的状态准入（`REQ-01M7372JNFAC2RWX2FXVV6YM5C`）
//
// ★ 结构刻意与三角形的**像素循环同构**（`rasterTriangle` 的 stage 循环）：两边必须在同一批
//   状态上得出同一个"取顶点色 / 取纹理色"的结论，否则点 / 线与三角形在相邻几何上分叉。
// ★ 只放行"结果恒等于顶点色"的组合：级活跃（`color_op`/`alpha_op` 不全 DISABLE）时
//   · 绑了纹理 ⇒ 需要采样 ⇒ **拒绝**（⛔ 不画成纯色糊过去）；
//   · 没绑纹理 ⇒ 复用 `untexturedResolvesToDiffuse`（与 GPU 准入同一个判据）。
//   默认 stage0 = MODULATE(TEXTURE, CURRENT) 且无纹理绑定 ⇒ 通过（夹具就是按这个状态构造的）。
//   ⚠ **站点处是否绑着纹理：未观察**。`.lst` 0x48E08A–0x48E1FB 只调 SetTransform / SetFVF(0x42) /
//   SetRenderState(145,0) / SetMaterial / DrawPrimitiveUP，**既不绑也不解绑纹理**、也不设
//   SetTextureStageState ⇒ 若前一笔遗留了 stage0 纹理，这两条线会被本函数**响亮拒绝**（而不是
//   像修前那样假 S_OK + 0 像素）。真要覆盖那种情况 = "带纹理的线"另开单，不在本单范围。
// ★ 拒绝 = `recordUnsupported(bit16)` + 默认级别可见的 `pipeWarn` + 返回 false（Draw 侧变成
//   `D3DERR_INVALIDCALL`）。why 具体到 stage/op/arg —— 引擎虽然丢弃 Draw 的返回值，日志与
//   状态块位图仍然指得出是哪一项。
// =====================================================================================
bool admitLinePointDraw(X86& cpu, std::uint32_t pe, const DrawContext& ctx, std::string* why) {
    for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
        const TextureStageSnapshot& ts = ctx.stages[s];
        if (ts.color_op == kTopDisable && ts.alpha_op == kTopDisable) break;
        if (ctx.textures[s].present) {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "点/线只做无纹理光栅：级 %u 绑定了纹理 ⇒ COLOROP/ALPHAOP 需要采样", s);
            recordUnsupported(cpu, pe, 16u, buf, s);
            pipeWarn(PipeWarnCategory::Other, "点/线光栅", "%s", buf);
            if (why) *why = buf;
            return false;
        }
        if (!untexturedResolvesToDiffuse(ts.color_op, ts.color_arg1, ts.color_arg2)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "点/线只做顶点色：级 %u 的 COLOROP=%u arg=%u/%u 需要纹理采样（无纹理时也不等于顶点色）",
                          s, ts.color_op, ts.color_arg1, ts.color_arg2);
            recordUnsupported(cpu, pe, 16u, buf, s);
            pipeWarn(PipeWarnCategory::Other, "点/线光栅", "%s", buf);
            if (why) *why = buf;
            return false;
        }
        if (!untexturedResolvesToDiffuse(ts.alpha_op, ts.alpha_arg1, ts.alpha_arg2)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "点/线只做顶点色：级 %u 的 ALPHAOP=%u arg=%u/%u 需要纹理采样（无纹理时也不等于顶点色）",
                          s, ts.alpha_op, ts.alpha_arg1, ts.alpha_arg2);
            recordUnsupported(cpu, pe, 16u, buf, s);
            pipeWarn(PipeWarnCategory::Other, "点/线光栅", "%s", buf);
            if (why) *why = buf;
            return false;
        }
    }
    return true;
}

// ★ 覆盖边界**默认级别可见**的一行（只打一次）：`LineCaps` 是 `d3d9.cpp` 的声明，本单不改那个
//   文件 ⇒ 必须在日志里说清"哪些位名副其实"。用 `logAt` 而不是 `pipeWarn`：这不是一次"拒绝"，
//   ⛔ 不进 `pipe-warn` 的分类计数（那份计数被 `tests/d3d9_pipeline.cpp` 的用例盯着）。
void noteLinePointCapsOnce() {
    static bool noted = false;
    if (noted) return;
    noted = true;
    logAt(LogLevel::Warn, LogD3d9, "[d3d9] 点/线: ",
          "LineCaps=0x1F 是 d3d9.cpp 的 fillCaps(kCapsLineCaps) **声明**（本单不许改那个文件）；"
          "引擎实测只读 caps 的 MaxTextureWidth/Height（0x499128/0x49912E）⇒ LineCaps 不是引擎判据。"
          "本线**名副其实**的位只有 ZTEST|BLEND|ALPHACMP(0x0E)：1px 无纹理点/线（顶点色 + z 插值，"
          "深度/alpha test/混合/写掩码与三角形同一份）；TEXTURE(0x01) 的点/线**响亮拒绝**"
          "（D3DERR_INVALIDCALL + recordUnsupported bit16）；FOG(0x10) 全局未做；"
          "点尺寸（POINTSIZE/POINTSCALE/POINTSPRITE）未实现 ⇒ 点按 1px 并记账（bit19）。");
}

// ★ 准入条件精确到"能等价表达"的状态；其余全部**显式回退**（reason 会进 fallback 账本）。
//   覆盖的 draw 类别（TITLE 的全屏渐变四边形就在里面）：
//     32bpp 默认后缓冲、GOURAUD、无 alpha test、完整/部分 color write、
//     任意可在 SDL_GPU 表达的 blend（含 separate alpha，TITLE = color 5/6 + alpha 2/6）、
//     stage 0 单级纹理（32bpp；SELECTARG1/MODULATE 的 TEXTURE/DIFFUSE 组合）、
//     sampler = POINT/LINEAR × WRAP/CLAMP/MIRROR、纹理坐标取 stage 0 的 TEXCOORDINDEX。
bool gpuEligible(const DrawContext& ctx, GpuBatchPlan* plan, std::string* why) {
    if (plan == nullptr) {
        if (why) *why = "GPU 准入：plan 为空";
        return false;
    }
    if (ctx.gpu == nullptr || !ctx.gpu->rasterActive()) {
        if (why) *why = "没有 GPU 光栅化后端（raster=cpu 或 GPU 设备创建失败）";
        return false;
    }
    if (ctx.color_surface != 0) {
        if (why) *why = "GPU 只支持默认后缓冲（当前渲染目标是 IDirect3DSurface9）";
        return false;
    }
    if (ctx.color_fmt.bpp != 32) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "GPU 只支持 32bpp 后缓冲（当前 %u bpp）",
                      static_cast<unsigned>(ctx.color_fmt.bpp));
        if (why) *why = buf;
        return false;
    }
    if (ctx.alpha_test) {
        if (why) *why = "GPU 不支持 alpha test（ALPHATESTENABLE=1）";
        return false;
    }
    if (ctx.shade != kShadeGouraud) {
        if (why) *why = "GPU 只支持 GOURAUD 着色（FLAT 会改变 provoking vertex 语义）";
        return false;
    }
    if ((ctx.color_write & ~0xFu) != 0) {
        char buf[112];
        std::snprintf(buf, sizeof(buf), "GPU 不认识的 color write mask 0x%X", ctx.color_write);
        if (why) *why = buf;
        return false;
    }
    // ---- 纹理级：只接受 stage 0；其余级必须 DISABLE；不允许 stage 1.. 的纹理 ----
    for (std::uint32_t s = 1; s < kMaxTextureStages; ++s) {
        if (ctx.textures[s].present) {
            char buf[112];
            std::snprintf(buf, sizeof(buf), "GPU 只支持 stage 0 单级纹理（stage %u 绑定了纹理）", s);
            if (why) *why = buf;
            return false;
        }
        if (ctx.stages[s].color_op != kTopDisable || ctx.stages[s].alpha_op != kTopDisable) {
            char buf[144];
            std::snprintf(buf, sizeof(buf), "GPU 只支持 stage 0（stage %u 的 color/alpha op 未 DISABLE）", s);
            if (why) *why = buf;
            return false;
        }
    }
    GpuBatchPlan p;
    p.state.viewport_x = ctx.viewport_x;
    p.state.viewport_y = ctx.viewport_y;
    p.state.viewport_w = ctx.viewport_w;
    p.state.viewport_h = ctx.viewport_h;
    p.state.scissor_x0 = ctx.scissor_x0;
    p.state.scissor_y0 = ctx.scissor_y0;
    p.state.scissor_x1 = ctx.scissor_x1;
    p.state.scissor_y1 = ctx.scissor_y1;
    p.state.scissor_enabled = ctx.scissor_x0 != 0 || ctx.scissor_y0 != 0 || ctx.scissor_x1 != ctx.color_width ||
                              ctx.scissor_y1 != ctx.color_height;
    p.state.depth_test = ctx.z_enable;
    p.state.depth_write = ctx.z_write;
    p.state.depth_func = ctx.z_func;
    p.state.color_write = static_cast<std::uint8_t>(ctx.color_write & 0xFu);
    // ---- 混合（color + separate alpha；映射失败 ⇒ 回退，⛔ 不近似）----
    if (!gpuBlendStateFor(ctx.src_blend, ctx.dst_blend, ctx.blend_op, ctx.separate_alpha_blend, ctx.src_blend_a,
                          ctx.dst_blend_a, ctx.blend_op_a, ctx.blend, &p.state, why)) {
        return false;
    }
    // ---- stage 0 ----
    const TextureView& tv = ctx.textures[0];
    const TextureStageSnapshot& ts = ctx.stages[0];
    const SamplerSnapshot& ss = ctx.samplers[0];
    const std::uint32_t coord = ts.tex_coord_index & 0xFFFFu;
    // ★ CPU 的像素循环在 stage0 的 color/alpha 两个 op **都** DISABLE 时直接 break（连纹理都不采样）
    //   ⇒ 结果恒等于顶点色。这一档不能按 arg 判，否则会把"被 DISABLE 忽略的纹理"当成有效输入。
    const bool stage0_ignored = ts.color_op == kTopDisable && ts.alpha_op == kTopDisable;
    if (tv.present && !stage0_ignored) {
        bool from_tfactor = false;
        if (!gpuTextureModeFor(ts.color_op, ts.color_arg1, ts.color_arg2, ts.alpha_op, ts.alpha_arg1,
                               ts.alpha_arg2, true, coord, &p.state.tex_mode, &from_tfactor, why)) {
            return false;
        }
        p.tex_color_from_tfactor = from_tfactor;
        if (tv.format.bpp != 32) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "GPU 只支持 32bpp 纹理（stage 0 是 %u bpp）",
                          static_cast<unsigned>(tv.format.bpp));
            if (why) *why = buf;
            return false;
        }
        if (tv.width <= 0 || tv.height <= 0 || tv.pixels == nullptr) {
            if (why) *why = "GPU 准入：stage 0 纹理的像素/尺寸非法";
            return false;
        }
        if (!gpuAddressFor(ss.address_u, &p.texture.sampler.address_u) ||
            !gpuAddressFor(ss.address_v, &p.texture.sampler.address_v)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "GPU 不支持该采样器寻址模式（U=%u V=%u；只支持 WRAP/CLAMP/MIRROR）",
                          ss.address_u, ss.address_v);
            if (why) *why = buf;
            return false;
        }
        // ★ CPU 参考把两个过滤**合成一条**决策（`samplerUsesLinear`：任一 LINEAR ⇒ 双线性），
        //   而 SDL_GPU 的 sampler 把 MAG / MIN **分开存、分开用** ⇒ `MAG=POINT + MIN=LINEAR`
        //   这种组合两边都"成功"、整批像素却不同，且 `--gpu-strict` 看不见（它不是 fallback）。
        //   按 `docs/00-origin/decisions.md` §8.1（CPU=oracle / GPU=product）：**要么一致，要么
        //   不接受该批** ⇒ 这里只放行 `MAG == MIN`，其余显式回退并记账。
        //   ⛔ 不去改 CPU 的采样决策（那是另一条单的内容）。
        if (ss.mag_filter != ss.min_filter) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "GPU 采样要求 MAG==MIN（CPU 参考按任一 LINEAR 取双线性；当前 MAG=%u MIN=%u）",
                          ss.mag_filter, ss.min_filter);
            if (why) *why = buf;
            return false;
        }
        if (!gpuFilterFor(ss.mag_filter, &p.texture.sampler.mag_filter) ||
            !gpuFilterFor(ss.min_filter, &p.texture.sampler.min_filter)) {
            char buf[144];
            std::snprintf(buf, sizeof(buf), "GPU 不支持该采样过滤（MAG=%u MIN=%u）", ss.mag_filter,
                          ss.min_filter);
            if (why) *why = buf;
            return false;
        }
        p.textured = true;
        p.texture.pixels = tv.pixels;
        p.texture.pitch = tv.pitch;
        p.texture.width = tv.width;
        p.texture.height = tv.height;
        p.texture.format = tv.format;
        p.texture.key = tv.pixels;
        p.texture.generation = tv.version;
        p.texture.serial = tv.serial;
        // ★ 方案 C：把"最近一次写的矩形 + 版本链"交给后端 —— 由它判要不要切段。
        p.texture.content_change_known = tv.write_version != 0 && tv.write_version == tv.version;
        p.texture.content_change_rect = GpuTexelRect{tv.write_rect[0], tv.write_rect[1],
                                                     tv.write_rect[2] - tv.write_rect[0],
                                                     tv.write_rect[3] - tv.write_rect[1]};
        p.texture.content_change_version = tv.write_version;
        p.texture.content_change_prev_version = tv.prev_write_version;
    } else {
        // 无纹理（CPU 采样为白色），或 stage0 两 op 都 DISABLE（CPU 直接 break ⇒ 顶点色）：
        // 只接受"结果恒等于顶点色"的组合（见 untexturedResolvesToDiffuse）。
        if (!stage0_ignored && (!untexturedResolvesToDiffuse(ts.color_op, ts.color_arg1, ts.color_arg2) ||
                                !untexturedResolvesToDiffuse(ts.alpha_op, ts.alpha_arg1, ts.alpha_arg2))) {
            char buf[208];
            std::snprintf(buf,
                          sizeof(buf),
                          "无纹理时 stage 0 的 op/arg 不等于顶点色（color op=%u arg=%u/%u，alpha op=%u arg=%u/%u）",
                          ts.color_op, ts.color_arg1, ts.color_arg2, ts.alpha_op, ts.alpha_arg1,
                          ts.alpha_arg2);
            if (why) *why = buf;
            return false;
        }
    }
    *plan = p;
    return true;
}


bool submitPrimitives(X86& cpu, std::uint32_t pe, std::uint32_t prim, const std::vector<CVertex>& verts,
                      std::uint32_t prim_count, DrawContext& ctx, std::uint32_t* triangles_out,
                      std::uint32_t* rasterized_out, std::string* why) {
    // ★ 诊断（`LIFT_SLOW_DRAW=1`）：只在这一批**慢**时打一行；默认零输出。
    static const bool slow_draw_trace = std::getenv("LIFT_SLOW_DRAW") != nullptr;
    // ★ 诊断（默认关）：GPU 与 CPU 参考在同一批/同一状态上逐像素对照（见下面 `[gpu-verify]`）。
    static const bool gpu_verify = std::getenv("LIFT_GPU_VERIFY") != nullptr;
    static const double slow_draw_ms = [] {
        const char* text = std::getenv("LIFT_SLOW_DRAW_MS");
        return text ? std::atof(text) : 20.0;
    }();
    const auto slow_draw_begin = slow_draw_trace ? std::chrono::steady_clock::now()
                                                 : std::chrono::steady_clock::time_point{};
    // 顶点已经是**变换后**的 CVertex（由调用方喂），这里只做裁剪 + 屏幕映射 + 光栅
    std::uint32_t triangles = 0;
    std::uint32_t rasterized = 0;
    // ★ `--raster cpu`（ctx.gpu == nullptr）是**显式参考后端**：⛔ 不计 fallback、⛔ 不打印
    //   `[gpu] fallback`。只有 GPU 已启用、本批因能力/后端失败而回退 CPU 时才记账。
    const bool have_gpu_backend = ctx.gpu != nullptr && ctx.gpu->rasterActive();
    // ★ 点 / 线**只走 CPU 软光栅**（GPU 后端只覆盖三角形；`gpuEligible` 的判据本身不改）。
    //   ⇒ 显式 `gpu_ok=false` + 一句精确 reason：GPU 已启用时账本会记一次 fallback，
    //   ⛔ 不会把"点/线没在 GPU 上画"藏成"画了 0 个三角形"。
    //   ★ `--gpu-strict` 下这会让真实 boot 在引擎画线时 exit 3（诚实：GPU 确实没有线覆盖）。
    //   ★ 下面的 `!gpu_ok && have_gpu_backend && rasterDirty()` 同步**照常**发生：更早的 GPU
    //     批必须先落回 guest，否则 Present 读回时会盖掉 CPU 刚写出的线像素。
    const bool line_or_point = prim == kPtPointList || prim == kPtLineList || prim == kPtLineStrip;
    GpuBatchPlan plan;
    std::string gpu_why;
    bool gpu_ok = false;
    if (line_or_point) {
        gpu_why = "点/线图元只走 CPU 软光栅（GPU 后端只覆盖三角形）";
    } else if (have_gpu_backend) {
        gpu_ok = gpuEligible(ctx, &plan, &gpu_why);
    }
    if (!gpu_ok && have_gpu_backend && ctx.gpu->rasterDirty()) {
        // 上一批走 GPU、这一批要回 CPU：先把 GPU 像素落回 guest，CPU 才能接着画。
        std::string sync_err;
        ctx.gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
    }
    std::vector<GpuRasterVertex> gpu_vertices;
    bool gpu_invw_ok = true;
    struct SavedTri {
        SVertex a, b, c;
    };
    std::vector<SavedTri> saved_tris;
    g_alpha_agg = AlphaAgg{};  // ★ 诊断：本批的混合 α 聚合（见 `[draw] fa=`）
    GpuRaster* gpu = gpu_ok ? ctx.gpu : nullptr;
    // 纹理坐标按 **stage 0 的 D3DTSS_TEXCOORDINDEX** 取（⛔ 不硬编码 TEXCOORD0）
    const std::uint32_t gpu_tex_coord = plan.textured ? (ctx.stages[0].tex_coord_index & 0xFFFFu) : 0u;
    // TFACTOR（D3DRS_TEXTUREFACTOR）是每批 Draw 的常量色：GPU 顶点色整批替换成它。
    float tf_r = 1.0f, tf_g = 1.0f, tf_b = 1.0f, tf_a = 1.0f;
    if (plan.textured && plan.tex_color_from_tfactor) {
        double ta = 255.0, tr = 255.0, tg = 255.0, tb = 255.0;
        splitColor(ctx.texture_factor, &ta, &tr, &tg, &tb);
        tf_r = static_cast<float>(tr / 255.0);
        tf_g = static_cast<float>(tg / 255.0);
        tf_b = static_cast<float>(tb / 255.0);
        tf_a = static_cast<float>(ta / 255.0);
    }

    auto push_tri = [&](const SVertex& a, const SVertex& b, const SVertex& c) {
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (!finite(area) || std::fabs(area) < 1e-12) return;
        if (area > 0.0) {
            if (ctx.cull == kCullCw) return;
        } else if (ctx.cull == kCullCcw) {
            return;
        }
        if (gpu == nullptr) {
            rasterTriangle(ctx, a, b, c);
            return;
        }
        // SVertex 的颜色/uv 已经乘过 invw；GPU 的顶点着色器输出 w=1 ⇒ 屏幕空间线性插值，
        // 只有 invw==1 才与 CPU 的透视校正插值等价。
        if (std::fabs(a.invw - 1.0) > 1e-9 || std::fabs(b.invw - 1.0) > 1e-9 ||
            std::fabs(c.invw - 1.0) > 1e-9) {
            gpu_invw_ok = false;
        }
        saved_tris.push_back({a, b, c});
        const auto add = [&](const SVertex& v) {
            GpuRasterVertex g;
            g.x = static_cast<float>(v.x);
            g.y = static_cast<float>(v.y);
            g.z = static_cast<float>(v.z);
            if (plan.textured && plan.tex_color_from_tfactor) {
                g.r = tf_r;
                g.g = tf_g;
                g.b = tf_b;
                g.a = tf_a;
            } else {
                g.r = static_cast<float>(v.r / 255.0);
                g.g = static_cast<float>(v.g / 255.0);
                g.b = static_cast<float>(v.b / 255.0);
                g.a = static_cast<float>(v.a / 255.0);
            }
            g.u = static_cast<float>(v.u[gpu_tex_coord]);
            g.v = static_cast<float>(v.v[gpu_tex_coord]);
            gpu_vertices.push_back(g);
        };
        add(a);
        add(b);
        add(c);
    };

    auto emit = [&](const CVertex& a, const CVertex& b, const CVertex& c) {
        ++triangles;
        CVertex poly[3] = {a, b, c};
        CVertex clipped[kMaxClipVerts];
        // ★ 预变换顶点裁屏幕空间（视口矩形），其余裁齐次裁剪空间 6 个面；语义见 clipHomogeneous 上方
        const std::uint32_t n = poly[0].pretransformed ? clipScreen(poly, 3, ctx, clipped)
                                                       : clipHomogeneous(poly, 3, clipped);
        if (n < 3) return;
        SVertex sv[kMaxClipVerts];
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!toScreen(clipped[i], ctx, &sv[i])) return;
        }
        ++rasterized;
        for (std::uint32_t i = 1; i + 1 < n; ++i) push_tri(sv[0], sv[i], sv[i + 1]);
    };
    const std::uint32_t n = static_cast<std::uint32_t>(verts.size());
    switch (prim) {
        case kPtTriangleList:
            for (std::uint32_t i = 0; i + 2 < n; i += 3) emit(verts[i], verts[i + 1], verts[i + 2]);
            break;
        case kPtTriangleStrip:
            for (std::uint32_t i = 0; i + 2 < n; ++i) {
                if ((i & 1u) == 0) emit(verts[i], verts[i + 1], verts[i + 2]);
                else emit(verts[i + 1], verts[i], verts[i + 2]);
            }
            break;
        case kPtTriangleFan:
            for (std::uint32_t i = 1; i + 1 < n; ++i) emit(verts[0], verts[i], verts[i + 1]);
            break;
        case kPtPointList:
        case kPtLineList:
        case kPtLineStrip: {
            // ★ 覆盖边界先声明（默认级别可见、只打一次）：即使这批被拒绝，也要让人看到
            //   "LineCaps 的哪些位本线真的做了"。
            noteLinePointCapsOnce();
            // ★ 再抛状态准入：需要纹理采样的点 / 线**响亮拒绝**（⛔ 不画成纯色糊过去）。
            //   `why` 由 Draw 侧变成 `D3DERR_INVALIDCALL`；`recordUnsupported(bit16)` 已记账。
            std::string lp_why;
            if (!admitLinePointDraw(cpu, pe, ctx, &lp_why)) {
                if (why) *why = lp_why;
                return false;
            }
            if (prim == kPtPointList) {
                // ★ 点尺寸（POINTSIZE / POINTSCALE / POINTSPRITE / FVF PSIZE）**未实现** ⇒ 1px + 记账。
                //   状态值 0 = 从没 Set 过（D3D 默认 POINTSIZE=1.0f）；只要被显式设置过就记账，
                //   ⛔ 不判它"是不是恰好 1.0"（那需要猜 POINTSIZE_MIN/MAX 的语义，本单不猜）。
                const std::uint32_t psize_bits = rsOf(cpu, pe, kRsPointSize);
                const std::uint32_t point_scale = rsOf(cpu, pe, kRsPointScaleEnable);
                const std::uint32_t point_sprite = rsOf(cpu, pe, kRsPointSpriteEnable);
                if (psize_bits != 0u || point_scale != 0u || point_sprite != 0u ||
                    (ctx.fvf & kFvfPSize) != 0u) {
                    float psize = 1.0f;
                    std::memcpy(&psize, &psize_bits, sizeof(psize));
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                                  "点尺寸（POINTSIZE=%g / POINTSCALE=%u / POINTSPRITE=%u / FVF PSIZE=%d）"
                                  "未实现 ⇒ 点按 1px 画",
                                  static_cast<double>(psize), point_scale, point_sprite,
                                  (ctx.fvf & kFvfPSize) != 0u ? 1 : 0);
                    recordUnsupported(cpu, pe, 19u, buf, psize_bits);
                }
                for (std::uint32_t i = 0; i < n; ++i) rasterizePoint(ctx, verts[i]);
            } else {
                // LASTPIXEL=0（真机不画最后一像素）：本线仍画 ⇒ 记账，⛔ 不静默。
                if (rsOf(cpu, pe, kRsLastPixel) == 0u)
                    recordUnsupported(cpu, pe, 20u, "D3DRS_LASTPIXEL=0（本线仍画每段最后一像素）", 0u);
                if (prim == kPtLineList) {
                    if ((n & 1u) != 0u)
                        pipeLog("点/线", "LINELIST 顶点数为奇数（%u）⇒ 丢掉末尾 1 个顶点", n);
                    for (std::uint32_t i = 0; i + 1u < n; i += 2u)
                        rasterizeLineSegment(ctx, verts[i], verts[i + 1u]);
                } else {
                    for (std::uint32_t i = 0; i + 1u < n; ++i)
                        rasterizeLineSegment(ctx, verts[i], verts[i + 1u]);
                }
            }
            // 诊断：点 / 线不计入 `kPlTriangles` / `kPlRasterized`（那两个数就是**三角形**计数）；
            // 像素数走 `ctx.pixels_written`（与三角形同一个账）。
            pipeLog("点/线", "图元=%u：%u 个顶点 ⇒ 写 %llu 个像素", prim, n,
                    static_cast<unsigned long long>(ctx.pixels_written));
            break;
        }
        default:
            if (why) *why = "D3D9 流水线：未知图元类型";
            return false;
    }

    bool gpu_done = false;
    std::string fallback_reason;
    // ★ 本单：`gpu_queued`= 真的把 draw 交进了后端队列（⛔ 不是"准入通过"）；
    //   `gpu_batch_tag`= 这一批在账本里的批号（0 = 没入队）。
    bool gpu_queued = false;
    std::uint64_t gpu_batch_tag = 0;
    // ★ GPU 的 sampler **表达不了** `MIPFILTER = NONE`（`GpuSampler` 只有 address/filter；`max_lod`
    //   是**整张纹理一档**，见 `d3d9_gpu.cpp` 的 `samplerFor`）⇒ 若这一批**真的**有缩小采样，
    //   CPU（关掉 mipmap ⇒ 只 level 0）与 GPU（会采 mip）必然出不同的像素。按
    //   `docs/00-origin/decisions.md` §8.1（CPU=oracle，"要么一致、要么不接受该批"）**显式回退并记账**。
    //   ⛔ 不是"把 GPU 的 max_lod 设回 0"—— 那会让 `MIPFILTER != NONE` 的正常批次在 GPU 上静默走样。
    bool gpu_mip_divergence = false;
    if (gpu != nullptr && plan.textured && !plan.tex_color_from_tfactor && ctx.samplers[0].mip_filter == kTexfNone &&
        ctx.textures[0].present) {
        const std::uint32_t coord =
            (ctx.stages[0].tex_coord_index & 0xFFFFu) < kMaxTexCoords ? (ctx.stages[0].tex_coord_index & 0xFFFFu) : 0u;
        for (const SavedTri& t : saved_tris) {
            if (triangleMipLevel(t.a, t.b, t.c, coord, ctx.textures[0].width, ctx.textures[0].height) >= 1u) {
                gpu_mip_divergence = true;
                break;
            }
        }
    }
    if (gpu != nullptr && (!gpu_invw_ok || gpu_mip_divergence)) {
        // GPU 已启用，但这一批**不能等价**交给 GPU：
        //   · invw != 1：屏幕空间插值会与 CPU 的透视校正产生像素差；
        //   · MIPFILTER=NONE 且有缩小采样：GPU 的 sampler 关不掉 mip（见上）。
        // ⇒ 明确回退（记账 + 首次打印），⛔ 不静默。
        // ★ 合批屏障：这一批要由 CPU 直接写 guest，而**更早**的批可能还在 GPU 队列里 ⇒ 先 flush +
        //   读回，否则它们在 Present 读回时会盖掉这些 CPU 像素。
        if (gpu->rasterDirty()) {
            std::string sync_err;
            gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
        }
        for (const SavedTri& t : saved_tris) rasterTriangle(ctx, t.a, t.b, t.c);
        gpu_vertices.clear();
        fallback_reason = gpu_mip_divergence
                              ? "MIPFILTER=NONE 且这一批有缩小采样（GPU sampler 的 max_lod 是一张纹理一档，"
                                "表达不了\"关掉 mipmap\"）"
                              : "顶点 invw!=1（屏幕空间 GPU 插值不能等价替代透视校正）";
    } else if (gpu != nullptr) {
        std::string gerr;
        // ★ 目标身份必须带 guest 像素基址：同尺寸换 surface 只看尺寸/格式判不出来（见 gpu_raster.h）。
        bool ok = gpu->rasterPrepare(ctx.color_fmt, ctx.color_width, ctx.color_height, &gerr, ctx.color_base);
        if (ok && !gpu->rasterDirty()) {
            ok = gpu->rasterSyncFromGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &gerr);
        }
        // ★ 诊断（`LIFT_GPU_VERIFY=1`）：同一批、同一状态，CPU 参考光栅 vs GPU 读回逐像素对照。
        //   只在 GPU 镜像与 guest 同步干净（!dirty）时做 —— 否则 CPU 起点会比 GPU 少一批像素。
        std::vector<std::uint8_t> ref_color;
        std::vector<float> ref_depth;
        bool ref_valid = false;
        if (gpu_verify && plan.textured && !saved_tris.empty() && ok && !gpu->rasterDirty()) {
            const std::uint32_t bpp = ctx.color_fmt.bytesPerPixel();
            const std::size_t cw = static_cast<std::size_t>(ctx.color_width);
            const std::size_t ch = static_cast<std::size_t>(ctx.color_height);
            ref_color.resize(cw * ch * bpp);
            for (std::size_t y = 0; y < ch; ++y) {
                std::memcpy(ref_color.data() + y * cw * bpp, ctx.color_base + y * ctx.color_pitch, cw * bpp);
            }
            ref_depth.resize(static_cast<std::size_t>(ctx.depth_pitch_floats) * ch);
            for (std::size_t y = 0; y < ch; ++y) {
                std::memcpy(ref_depth.data() + y * ctx.depth_pitch_floats,
                            ctx.depth_base + y * ctx.depth_pitch_floats,
                            static_cast<std::size_t>(ctx.depth_pitch_floats) * sizeof(float));
            }
            DrawContext ref = ctx;
            ref.gpu = nullptr;  // ⛔ CPU 参考不碰 GPU 后端
            ref.color_base = ref_color.data();
            ref.color_pitch = static_cast<std::uint32_t>(cw * bpp);
            ref.depth_base = ref_depth.data();
            ref.pixels_written = 0;
            for (const SavedTri& t : saved_tris) rasterTriangle(ref, t.a, t.b, t.c);
            ref_valid = true;
        }
        if (ok && !gpu_vertices.empty()) {
            // ★ 账本批号（本单）：**入队前**设、入队后立刻清 0 —— 后端在命令缓冲**提交成功之后**
            //   按这个号回报"这条 draw 真的进了 render pass"。⛔ 不许拿 `rasterQueue` 的返回值当
            //   recorded（那正是"入队成功 = 已覆盖"的原缺陷）。
            gpu_batch_tag = ++g_gpu_batch_seq;
            g_gpu_batch_ledger[gpu_batch_tag] = GpuBatchLedgerEntry{};
            gpuRasterSetBatchTag(gpu_batch_tag);
            // ★ 合批：正常路径**只入队**（End/Present/Clear 才 flush）。`LIFT_GPU_VERIFY=1` 的判据
            //   要求"画完立刻能读"，所以诊断模式仍走会立即 flush 的旧接口（⛔ 不改判据）。
            if (gpu_verify) {
                if (plan.textured) {
                    ok = gpu->rasterDrawTextured(gpu_vertices.data(), gpu_vertices.size(), plan.texture, plan.state,
                                                 &gerr);
                } else {
                    ok = gpu->rasterDraw(gpu_vertices.data(), gpu_vertices.size(), plan.state, &gerr);
                }
            } else if (plan.textured) {
                ok = gpu->rasterQueue(gpu_vertices.data(), gpu_vertices.size(), &plan.texture, plan.state, &gerr);
            } else {
                ok = gpu->rasterQueue(gpu_vertices.data(), gpu_vertices.size(), nullptr, plan.state, &gerr);
            }
            gpuRasterSetBatchTag(0);
            gpu_queued = ok;
        }
        if (ok && ref_valid) {
            std::string verr;
            if (!gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &verr)) {
                std::fprintf(stderr, "[gpu-verify] 读回失败：%s\n", verr.c_str());
            } else {
                const std::uint32_t bpp = ctx.color_fmt.bytesPerPixel();
                const bool alpha = ctx.color_fmt.amask != 0;
                std::uint64_t diff_pixels = 0;
                unsigned max_diff = 0;
                for (int y = 0; y < ctx.color_height; ++y) {
                    const std::uint8_t* pa = ref_color.data() + static_cast<std::size_t>(y) * ctx.color_width * bpp;
                    const std::uint8_t* pb = ctx.color_base + static_cast<std::size_t>(y) * ctx.color_pitch;
                    for (int x = 0; x < ctx.color_width; ++x) {
                        const std::uint32_t ra = pixel_format::loadPixel(pa + static_cast<std::size_t>(x) * bpp,
                                                                         ctx.color_fmt.bpp);
                        const std::uint32_t rb = pixel_format::loadPixel(pb + static_cast<std::size_t>(x) * bpp,
                                                                         ctx.color_fmt.bpp);
                        const unsigned va[4] = {pixel_format::expandChannel(ra, ctx.color_fmt.rmask),
                                                pixel_format::expandChannel(ra, ctx.color_fmt.gmask),
                                                pixel_format::expandChannel(ra, ctx.color_fmt.bmask),
                                                pixel_format::expandChannel(ra, ctx.color_fmt.amask)};
                        const unsigned vb[4] = {pixel_format::expandChannel(rb, ctx.color_fmt.rmask),
                                                pixel_format::expandChannel(rb, ctx.color_fmt.gmask),
                                                pixel_format::expandChannel(rb, ctx.color_fmt.bmask),
                                                pixel_format::expandChannel(rb, ctx.color_fmt.amask)};
                        unsigned d = 0;
                        const int channels = alpha ? 4 : 3;
                        for (int c = 0; c < channels; ++c) {
                            const unsigned diff = va[c] > vb[c] ? va[c] - vb[c] : vb[c] - va[c];
                            if (diff > d) d = diff;
                        }
                        if (d != 0) {
                            ++diff_pixels;
                            if (d > max_diff) max_diff = d;
                        }
                    }
                }
                std::fprintf(stderr,
                             "[gpu-verify] textured batch tris=%zu pixels=%dx%d diff_pixels=%llu max_channel_diff=%u%s\n",
                             saved_tris.size(), ctx.color_width, ctx.color_height,
                             (unsigned long long)diff_pixels, max_diff,
                             alpha ? "" : "（X8 后缓冲：只比 RGB）");
                std::fflush(stderr);
            }
        }
        if (ok) {
            gpu_done = true;
            ++g_gpu_draws;
            g_gpu_triangles += saved_tris.size();
            static bool logged = false;
            if (!logged && !saved_tris.empty()) {
                logged = true;
                const SavedTri& t = saved_tris.front();
                pipeLog("GPU",
                        "首个 GPU 光栅 Draw：%zu 三角形（后端 %s）textured=%d mode=%u z=%.3f/%.3f/%.3f "
                        "depth=%d/%d/%u blend=%d src=%u dst=%u sep=%d srcA=%u dstA=%u cw=%X",
                        saved_tris.size(), gpu->rasterName(), plan.textured ? 1 : 0,
                        static_cast<unsigned>(plan.state.tex_mode), t.a.z, t.b.z, t.c.z,
                        static_cast<int>(plan.state.depth_test), static_cast<int>(plan.state.depth_write),
                        plan.state.depth_func, static_cast<int>(plan.state.blend),
                        static_cast<unsigned>(plan.state.src_blend), static_cast<unsigned>(plan.state.dst_blend),
                        static_cast<int>(plan.state.separate_alpha_blend),
                        static_cast<unsigned>(plan.state.src_blend_alpha),
                        static_cast<unsigned>(plan.state.dst_blend_alpha), ctx.color_write);
            }
        }
        if (!ok) {
            ++g_gpu_coverage.gpu_failures;
            // GPU 本批失败：先让 guest 看到已经画出的部分，再用 CPU 重放这一批。
            std::string sync_err;
            gpu->rasterSyncToGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
            for (const SavedTri& t : saved_tris) rasterTriangle(ctx, t.a, t.b, t.c);
            gpu->rasterSyncFromGuest(ctx.color_base, ctx.color_pitch, ctx.color_width, ctx.color_height, &sync_err);
            fallback_reason = "GPU 后端绘制失败：" + gerr;
            static bool logged = false;
            if (!logged) {
                logged = true;
                pipeLog("GPU", "光栅化失败，已回退 CPU：%s", gerr.c_str());
            }
        }
    }
    // GPU 已启用时的账：每一批 Draw* 都算一次；没画到 GPU 的一定有原因。
    // ★ 空批 / 全被裁掉（CPU 一个像素都没写）**不算 fallback**：它们不是"回退"，也没有像素差；
    //   `draw_calls` 照记（可见性），`fallback_pixels` 只在真的由 CPU 写了像素时加。
    if (have_gpu_backend) {
        ++g_gpu_coverage.draw_calls;
        if (gpu_done) {
            ++g_gpu_coverage.gpu_draws;
            // ★ 本单：入队 ≠ 覆盖。`queued` 只数**真的交进后端队列**的批；`fragments` 是交出去的顶点数
            //   （"能拿到的片元/顶点计数"—— 片元数后端拿不到，这里给顶点数）。
            if (gpu_queued) {
                ++g_gpu_coverage.queued;
                g_gpu_coverage.fragments += gpu_vertices.size();
            }
            // 这一批还没有任何 render pass 录过它 ⇒ 先记进 `empty`（后端回报时再挪到 `recorded`）。
            // ★ `gpu_batch_tag == 0` 也记：连队都没入（`gpu_vertices` 全被裁掉 / 准入在 queue 之前就
            //   失败）比"入队成功但没录"更坏，同样属于"账上覆盖、实际零像素"。
            if (gpu_batch_tag != 0) {
                auto it = g_gpu_batch_ledger.find(gpu_batch_tag);
                if (it != g_gpu_batch_ledger.end() && !it->second.recorded && !it->second.counted_empty) {
                    it->second.counted_empty = true;
                    noteCoverageEmpty();
                }
            } else {
                noteCoverageEmpty();
            }
        } else if (ctx.pixels_written > 0) {
            gpuNoteFallback(fallback_reason.empty()
                                ? (gpu_why.empty() ? std::string("GPU 不可用（原因未登记）") : gpu_why)
                                : fallback_reason,
                            ctx.pixels_written);
        }
    }

    (void)prim_count;
    plSet(cpu, pe, kPlTriangles, plGet(cpu, pe, kPlTriangles) + triangles);
    plSet(cpu, pe, kPlRasterized, plGet(cpu, pe, kPlRasterized) + rasterized);
    if (triangles_out) *triangles_out = triangles;
    if (rasterized_out) *rasterized_out = rasterized;
    std::uint64_t pixels = (static_cast<std::uint64_t>(plGet(cpu, pe, kPlPixels + 4u)) << 32) |
                           plGet(cpu, pe, kPlPixels);
    pixels += ctx.pixels_written;
    cpu.mem->wr32(pe + kPlPixels, static_cast<std::uint32_t>(pixels & 0xFFFFFFFFu));
    cpu.mem->wr32(pe + kPlPixels + 4u, static_cast<std::uint32_t>(pixels >> 32));
    // ★ 派生 mip 的账：本批"想用 mip 但链不可用 ⇒ 退回 level 0"记账**一次**（⛔ 不静默走样）。
    if (ctx.mip_level0_fallback) noteMipLevel0Sampled();
    if (slow_draw_trace) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                    slow_draw_begin)
                              .count();
        if (ms >= slow_draw_ms) {
            std::uint32_t tex_present = 0;
            for (std::uint32_t s = 0; s < kMaxTextureStages; ++s) {
                if (ctx.textures[s].present) ++tex_present;
            }
            // ★ `texserial=` 是**纹理身份**（`TextureInfo::serial`，进程级单调、永不复用）⇒ 两次 Draw 的
            //   这个数相同 = 它们采的是同一张纹理；不同 = 换了纹理（⛔ 只看 `texfmt=` 的 w/h/format 分不出来）。
            // ★ `alpha=` 是**本批输入顶点**的 diffuse alpha 区间：渐变层每帧推进的就是这个数 ——
            //   它不变而画面也不变，就不是"混合画错"，而是这一批本来就没有变化。
            double alpha_min = 255.0;
            double alpha_max = 0.0;
            for (const CVertex& v : verts) {
                if (v.a < alpha_min) alpha_min = v.a;
                if (v.a > alpha_max) alpha_max = v.a;
            }
            // ★ `d0=` 是**第一个顶点**的 diffuse（r,g,b,a，0..255）：只打逐帧会推进的那个数，
            //   `alpha=` 是整批 alpha 区间（4 个顶点同值 ⇒ 两者相等）。`uv=` 是本批 UV 的**范围**
            //   （min..max）：同一张纹理覆盖整屏 vs 只取一个小区域（纯色块 / 黑块）靠它分辨。
            static const CVertex kZeroVertex{};
            const CVertex& v0 = verts.empty() ? kZeroVertex : verts.front();
            double u0 = 1e9, u1 = -1e9, vv0 = 1e9, vv1 = -1e9;
            for (const CVertex& v : verts) {
                if (v.u[0] < u0) u0 = v.u[0];
                if (v.u[0] > u1) u1 = v.u[0];
                if (v.v[0] < vv0) vv0 = v.v[0];
                if (v.v[0] > vv1) vv1 = v.v[0];
            }
            if (verts.empty()) {
                u0 = u1 = vv0 = vv1 = 0.0;
            }
            // ★ `texa=` 是**绑定纹理自身**的 α（在 4 个 1/4–3/4 位置采样，给 min..max）。
            //   用途 = 分辨"混合 α=0 是因为纹理 α=0"还是"stage 数学算错"；`n` = 采样点数。
            //   ⚠ 只打 stage 0；纹理不在 arena 里 / 没绑 ⇒ `texa=none`。
            int texa_n = 0;
            unsigned texa_min = 255, texa_max = 0;
            unsigned long long texa_sum = 0;
            if (ctx.textures[0].present && ctx.textures[0].pixels != nullptr) {
                const TextureView& tv = ctx.textures[0];
                // ★ 16×16 网格（不是 4 点）：判断"整张纹理 α 是不是 0"必须覆盖足够多的位置，
                //   4 点采样会在大图上命中空白区，把"局部透明"误读成"整张透明"。
                const int kGrid = 16;
                for (int gy = 0; gy < kGrid; ++gy) {
                    for (int gx = 0; gx < kGrid; ++gx) {
                        const int px = static_cast<int>((static_cast<long long>(tv.width) * (2 * gx + 1)) / (2 * kGrid));
                        const int py = static_cast<int>((static_cast<long long>(tv.height) * (2 * gy + 1)) / (2 * kGrid));
                        const std::uint8_t* p = tv.pixels + static_cast<std::size_t>(py) * tv.pitch +
                                                static_cast<std::size_t>(px) * tv.format.bytesPerPixel();
                        const std::uint32_t raw = pixel_format::loadPixel(p, tv.format.bpp);
                        const unsigned a = pixel_format::expandChannel(raw, tv.format.amask);
                        ++texa_n;
                        texa_sum += a;
                        if (a < texa_min) texa_min = a;
                        if (a > texa_max) texa_max = a;
                    }
                }
            }
            const double texa_mean =
                texa_n != 0 ? static_cast<double>(texa_sum) / static_cast<double>(texa_n) : 0.0;
            // ★ 本单：`rec=` **不许**只打"入队成功"。正常合批路径此刻还没 flush（Present / Clear 才提交）
            //   ⇒ 这一行先挂起，由后端的回报点（命令缓冲提交成功之后）补打成 `rec=1`；
            //   只有"已经确定不会进 render pass"的批（CPU 路 / GPU 回退 / 空批）才当场打 `rec=0`。
            //   ★ `vp` / `tgt` / `sc` / `z` 是**本批入队时**的状态（`A9` 分诊缺的就是这四个读数）：
            //     `vp` 与 `tgt` 不等 ⇒ `toScreen` 的视口映射会被 GPU 的 NDC→viewport 再乘一次。
            char draw_line[2048];
            std::snprintf(
                draw_line, sizeof(draw_line),
                "frame=%u prim=%u verts=%zu tris=%u rast=%u pixels=%llu gpu=%d "
                "vp=%d,%d,%d,%d tgt=%dx%d sc=%d,%d..%d,%d z=%d/%d/%u "
                "why=%s "
                "sep_alpha=%d tex=%u texserial=%u tfactor=%08X alpha=%.1f..%.1f d0=%.0f,%.0f,%.0f,%.0f "
                "uv=%.4f..%.4f,%.4f..%.4f "
                "blend=%d/%u/%u op=%u cw=%X fvf=%X coord=%u "
                "stage0=%u/%u(%u/%u,%u/%u) samp=U%uV%u M%u%u MF%u lvl=%u..%u miprb=%llu bbox=%d,%d..%d,%d "
                "texfmt=%u/%X %dx%d "
                "texa=%d:%u..%u/%.0f fa=%lld:%.0f/%u/%u submit=%.1fms",
                d3d9::frameSeq(), prim, verts.size(), triangles, rasterized,
                (unsigned long long)ctx.pixels_written, gpu_done ? 1 : 0, ctx.viewport_x, ctx.viewport_y,
                ctx.viewport_w, ctx.viewport_h, ctx.color_width, ctx.color_height, ctx.scissor_x0, ctx.scissor_y0,
                ctx.scissor_x1, ctx.scissor_y1, ctx.z_enable ? 1 : 0, ctx.z_write ? 1 : 0, ctx.z_func,
                gpu_why.empty() ? "-" : gpu_why.c_str(), ctx.separate_alpha_blend ? 1 : 0,
                tex_present, ctx.textures[0].serial, ctx.texture_factor, alpha_min, alpha_max, v0.r, v0.g,
                v0.b, v0.a, u0,
                u1, vv0, vv1, ctx.blend ? 1 : 0, ctx.src_blend, ctx.dst_blend, ctx.blend_op,
                ctx.color_write, ctx.fvf,
                ctx.stages[0].tex_coord_index & 0xFFFFu, ctx.stages[0].color_op, ctx.stages[0].alpha_op,
                ctx.stages[0].color_arg1, ctx.stages[0].color_arg2, ctx.stages[0].alpha_arg1,
                ctx.stages[0].alpha_arg2, ctx.samplers[0].address_u, ctx.samplers[0].address_v,
                ctx.samplers[0].mag_filter, ctx.samplers[0].min_filter, ctx.samplers[0].mip_filter,
                ctx.mip_level_min == 0xFFFFFFFFu ? 0u : ctx.mip_level_min, ctx.mip_level_max,
                static_cast<unsigned long long>(ctx.mip_bytes_built),
                ctx.bbox_x0 == 0x7FFFFFFF ? 0 : ctx.bbox_x0, ctx.bbox_y0 == 0x7FFFFFFF ? 0 : ctx.bbox_y0,
                ctx.bbox_x1, ctx.bbox_y1,
                static_cast<unsigned>(ctx.textures[0].format.bpp), ctx.textures[0].format.amask,
                ctx.textures[0].width, ctx.textures[0].height, texa_n, texa_min, texa_max, texa_mean,
                static_cast<long long>(g_alpha_agg.n),
                g_alpha_agg.n != 0 ? static_cast<double>(g_alpha_agg.sum) / static_cast<double>(g_alpha_agg.n)
                                   : 0.0,
                g_alpha_agg.mn, g_alpha_agg.mx, ms);
            // ★ `rec=` 的判据（本单）：后端**真的**把这条 draw 录进过 render pass（命令缓冲提交成功
            //   之后的回报），⛔ 不是 `rasterQueue` 的返回值。正常合批路径此刻还没 flush ⇒ 行挂进
            //   **提交顺序**队列，由回报点按序放出；`tag==0`（没入队）的行永远不会有回报 ⇒ 当场 rec=0。
            //   ★ 入队**失败**时批号已经发出去、但后端队列里没有这条 draw ⇒ 行必须按 tag 0 处理，
            //     否则它会永远等一个不会来的回报、把后面的行一起卡到下一轮（复审 Note 1）。
            g_pending_draw_lines.push_back(
                GpuPendingDrawLine{gpu_queued ? gpu_batch_tag : 0, std::string(draw_line)});
            drainPendingDrawLines(false);
        }
    }
    return true;
}

// =====================================================================================
// 顶点声明（D3DVERTEXELEMENT9）→ 本线的取顶点计划
// =====================================================================================
// ★ 为什么需要：引擎的 2D 图层走 `SetVertexDeclaration` + **两条流**（stream0 = POSITION FLOAT3、
//   stream1 = TEXCOORD0 FLOAT2）+ `DrawIndexedPrimitive`，而本线的光栅只吃"一路交错顶点"。
//   ⇒ 这里按声明把两条流**去交错**成一份临时缓冲（XYZ + UV [+ diffuse]），再交给既有的
//   gather / transform。⛔ 不认识的元素（法线 / 混合权 / 第二套 UV / 非 32 位分量）一律响亮拒绝。
struct DeclElem {
    std::uint32_t stream = 0;
    std::uint32_t offset = 0;
    std::uint32_t type = 0;   // D3DDECLTYPE
    std::uint32_t usage = 0;  // D3DDECLUSAGE
};

struct DeclLayout {
    bool present = false;   // 这一批用的是顶点声明（而不是 SetFVF）
    DeclElem pos;
    DeclElem tex;
    DeclElem color;
    bool has_tex = false;
    bool has_color = false;
    std::uint32_t synth_stride = 0;  // 去交错后的步长（12 / 20 / 24）
    FvfLayout synth;                 // 给既有 gather 用的合成布局
};

// D3DDECLTYPE（只认本线用得到的几种）
constexpr std::uint32_t kDeclFloat1 = 0;
constexpr std::uint32_t kDeclFloat2 = 1;
constexpr std::uint32_t kDeclFloat3 = 2;
constexpr std::uint32_t kDeclFloat4 = 3;
constexpr std::uint32_t kDeclD3DColor = 4;
// D3DDECLUSAGE
constexpr std::uint32_t kDeclUsagePosition = 0;
constexpr std::uint32_t kDeclUsageTexCoord = 5;
constexpr std::uint32_t kDeclUsageColor = 10;

// 元素表（8 B/项：Stream u16 / Offset u16 / Type u8 / Method u8 / Usage u8 / UsageIndex u8）。
bool declLayoutOf(X86& cpu, std::uint32_t decl, DeclLayout* out, std::string* why) {
    out->present = true;
    const std::uint8_t* elems = nullptr;
    std::uint32_t bytes = 0;
    if (!declarationElements(cpu, decl, &elems, &bytes)) {
        if (why) *why = "D3D9 流水线：顶点声明的元素表不可读（不是本模块建的 / 已被回收）";
        return false;
    }
    bool have_pos = false;
    for (std::uint32_t at = 0; at + 8u <= bytes; at += 8u) {
        const std::uint32_t stream = static_cast<std::uint32_t>(elems[at]) | (static_cast<std::uint32_t>(elems[at + 1]) << 8);
        if (stream == 0xFFu) break;  // D3DDECL_END
        const std::uint32_t offset = static_cast<std::uint32_t>(elems[at + 2]) |
                                     (static_cast<std::uint32_t>(elems[at + 3]) << 8);
        const std::uint32_t type = elems[at + 4];
        // ⚠ D3DVERTEXELEMENT9 的字段序：Stream u16 / Offset u16 / Type u8 / Method u8 / Usage u8 /
        //   UsageIndex u8 ⇒ usage 在**第 6 字节**、usageIndex 在第 7 字节（⛔ 不是 5/6）。
        const std::uint32_t usage = elems[at + 6];
        const std::uint32_t usage_index = elems[at + 7];
        DeclElem e;
        e.stream = stream;
        e.offset = offset;
        e.type = type;
        e.usage = usage;
        if (usage == kDeclUsagePosition && usage_index == 0 && type == kDeclFloat3) {
            out->pos = e;
            have_pos = true;
        } else if (usage == kDeclUsageTexCoord && usage_index == 0 && type == kDeclFloat2) {
            out->tex = e;
            out->has_tex = true;
        } else if (usage == kDeclUsageColor && usage_index == 0 && type == kDeclD3DColor) {
            out->color = e;
            out->has_color = true;
        } else {
            if (why) {
                char buf[176];
                std::snprintf(buf, sizeof(buf),
                              "D3D9 流水线：顶点声明的元素 stream=%u offset=%u type=%u usage=%u idx=%u 不在本线支持集"
                              "（只支持 POSITION/FLOAT3、TEXCOORD0/FLOAT2、COLOR0/D3DCOLOR）",
                              stream, offset, type, usage, usage_index);
                *why = buf;
            }
            return false;
        }
        if (stream >= 2u) {
            if (why) *why = "D3D9 流水线：顶点声明用到 stream≥2（本线只接 2 条流）";
            return false;
        }
    }
    if (!have_pos) {
        if (why) *why = "D3D9 流水线：顶点声明里没有 POSITION(FLOAT3)";
        return false;
    }
    FvfLayout& l = out->synth;
    l = FvfLayout{};
    l.valid = true;
    l.xyz = true;
    l.pos_offset = 0;
    l.stride = 12;
    if (out->has_tex) {
        l.tex_count = 1;
        l.tex_size[0] = 2;
        l.tex_offset[0] = l.stride;
        l.stride += 8u;
    }
    if (out->has_color) {
        l.has_diffuse = true;
        l.diffuse_offset = l.stride;
        l.stride += 4u;
    }
    out->synth_stride = l.stride;
    return true;
}

// 按声明把两条流去交错成一份临时缓冲（`first` = 起始顶点序号，`count` = 顶点数）。
bool buildDeclVertices(X86& cpu, std::uint32_t pe, const char* method, const DeclLayout& d, std::uint32_t first,
                       std::uint32_t count, std::vector<std::uint8_t>* out, std::string* why) {
    struct StreamBinding {
        const std::uint8_t* base = nullptr;
        std::uint32_t offset = 0;
        std::uint32_t stride = 0;
        std::uint32_t length = 0;
    } sb[2];
    const DeclElem* used[3] = {&d.pos, d.has_tex ? &d.tex : nullptr, d.has_color ? &d.color : nullptr};
    for (const DeclElem* e : used) {
        if (e == nullptr) continue;
        StreamBinding& s = sb[e->stream];
        if (s.base != nullptr) continue;  // 同一条流只解析一次
        const std::uint32_t slot = kPlStreams + e->stream * 16u;
        const std::uint32_t vb = plGet(cpu, pe, slot);
        const std::uint32_t gen = plGet(cpu, pe, slot + 12u);
        if (vb == 0) {
            if (why) *why = "D3D9 流水线：顶点声明用到的流没有 SetStreamSource";
            return false;
        }
        std::uint32_t data = 0;
        if (!boundVertexBufferOfState(cpu, vb, gen, method, nullptr, &s.length, &data, why)) return false;
        s.base = cpu.mem->host(data);
        if (s.base == nullptr) {
            if (why) *why = "D3D9 流水线：顶点缓冲的宿主映射为空";
            return false;
        }
        s.offset = plGet(cpu, pe, slot + 4u);
        s.stride = plGet(cpu, pe, slot + 8u);
        if (s.stride == 0) {
            if (why) *why = "D3D9 流水线：顶点声明的流步长是 0（SetStreamSource 没给 stride）";
            return false;
        }
    }
    const std::uint32_t elem_bytes[3] = {12u, d.has_tex ? 8u : 0u, d.has_color ? 4u : 0u};
    for (int k = 0; k < 3; ++k) {
        if (used[k] == nullptr) continue;
        const StreamBinding& s = sb[used[k]->stream];
        const std::uint64_t need = static_cast<std::uint64_t>(s.offset) +
                                   static_cast<std::uint64_t>(first + count) * s.stride;
        if (need + elem_bytes[k] > static_cast<std::uint64_t>(s.length) + elem_bytes[k] ||
            need > s.length) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "D3D9 流水线：声明取顶点越过流 %u 的缓冲（需要 %llu B，缓冲 %u B，stride=%u）",
                          used[k]->stream, static_cast<unsigned long long>(need), s.length, s.stride);
            if (why) *why = buf;
            return false;
        }
    }
    out->assign(static_cast<std::size_t>(count) * d.synth_stride, 0);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t idx = first + i;
        std::uint8_t* dst = out->data() + static_cast<std::size_t>(i) * d.synth_stride;
        for (int k = 0; k < 3; ++k) {
            if (used[k] == nullptr) continue;
            const StreamBinding& s = sb[used[k]->stream];
            const std::uint32_t dst_off = k == 0 ? 0u : (k == 1 ? d.synth.tex_offset[0] : d.synth.diffuse_offset);
            std::memcpy(dst + dst_off,
                        s.base + s.offset + static_cast<std::size_t>(idx) * s.stride + used[k]->offset,
                        elem_bytes[k]);
        }
    }
    return true;
}

// Draw 的公共前置检查（真机的失败码）
bool drawPrecheck(X86& cpu, const char* method, std::uint32_t dev, std::uint32_t* pe_out,
                  FvfLayout* fvf_out, DeclLayout* decl_out, std::string* why) {
    std::uint32_t pe = 0;
    if (!pipelineOf(cpu, dev, &pe, why)) return false;
    // ★ 进入 Draw 就算一次调用（含之后的失败）—— 计数器是引擎要求画了几次的可观察量
    plSet(cpu, pe, kPlDrawCalls, plGet(cpu, pe, kPlDrawCalls) + 1u);
    if (plGet(cpu, pe, kPlInScene) == 0) {
        pipeLog(method, "不在 BeginScene/EndScene 之间 ⇒ D3DERR_INVALIDCALL（真机语义）");
        if (why) *why = "D3D9 流水线：Draw 在 BeginScene 之外";
        return false;
    }
    const std::uint32_t decl = plGet(cpu, pe, kPlVertexDecl);
    if (decl != 0) {
        // ★ 声明路：只有调用方要得出"声明布局"时才支持（UP 变体没有流，仍按不支持拒绝）。
        if (decl_out == nullptr) {
            pipeLog(method, "设了顶点声明（0x%08X）但这一条 Draw 不支持声明路 ⇒ D3DERR_INVALIDCALL", decl);
            if (why) *why = "D3D9 流水线：这一条 Draw 不支持顶点声明";
            return false;
        }
        if (!declLayoutOf(cpu, decl, decl_out, why)) {
            pipeLog(method, "顶点声明（0x%08X）不支持：%s ⇒ D3DERR_INVALIDCALL", decl, why ? why->c_str() : "");
            return false;
        }
        *pe_out = pe;
        *fvf_out = decl_out->synth;
        return true;
    }
    const std::uint32_t fvf = plGet(cpu, pe, kPlFvf);
    if (fvf == 0) {
        pipeLog(method, "没有 SetFVF ⇒ D3DERR_INVALIDCALL");
        if (why) *why = "D3D9 流水线：Draw 之前没有 SetFVF";
        return false;
    }
    std::string parse_why;
    const FvfLayout l = parseFvf(fvf, &parse_why);
    if (!l.valid) {
        pipeLog(method, "FVF=0x%08X 不支持：%s ⇒ D3DERR_INVALIDCALL", fvf, parse_why.c_str());
        if (why) *why = parse_why;
        return false;
    }
    *pe_out = pe;
    *fvf_out = l;
    return true;
}

// 读一批顶点（从 guest 内存的 base 起，按 stride 走）
bool gatherVertices(X86& cpu, std::uint32_t pe, const FvfLayout& fvf, const std::uint8_t* base,
                    std::uint32_t stride, std::uint32_t first, std::uint32_t count,
                    const math::Mat4d& world, const math::Mat4d& view, const math::Mat4d& proj,
                    std::vector<CVertex>* out, std::string* why) {
    if (base == nullptr) {
        if (why) *why = "D3D9 流水线：顶点数据指针为空";
        return false;
    }
    if (stride < fvf.stride) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "D3D9 流水线：顶点步长 %u 小于 FVF 需要的最小步长 %u", stride, fvf.stride);
        if (why) *why = buf;
        return false;
    }
    out->clear();
    out->reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t index = first + i;
        const std::uint8_t* vp = base + static_cast<std::size_t>(index) * stride;
        CVertex v;
        transformVertex(cpu, pe, fvf, vp, index, world, view, proj, &v);
        out->push_back(v);
    }
    return true;
}

}  // namespace

// =====================================================================================
// 派生 mip 的 LOD（`REQ-01M73G2724479XAVS176WBZ65Q`）：纯函数，守卫直接打
// =====================================================================================
bool screenUvDerivatives(const double vx[3], const double vy[3], const double invw[3],
                         const double u_over_w[3], const double v_over_w[3], double* du_dx, double* dv_dx,
                         double* du_dy, double* dv_dy) {
    if (du_dx != nullptr) *du_dx = 0.0;
    if (dv_dx != nullptr) *dv_dx = 0.0;
    if (du_dy != nullptr) *du_dy = 0.0;
    if (dv_dy != nullptr) *dv_dy = 0.0;
    if (vx == nullptr || vy == nullptr || invw == nullptr || u_over_w == nullptr || v_over_w == nullptr) return false;
    const double x0 = vx[0], x1 = vx[1], x2 = vx[2];
    const double y0 = vy[0], y1 = vy[1], y2 = vy[2];
    const double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    // 线性函数在三角形上的**屏幕空间梯度**（f_i 为顶点值）：
    //   f_x = ((f1-f0)*(y2-y0) - (f2-f0)*(y1-y0)) / det
    //   f_y = ((x1-x0)*(f2-f0) - (x2-x0)*(f1-f0)) / det
    auto grad = [&](const double f[3], double* gx, double* gy) {
        *gx = ((f[1] - f[0]) * (y2 - y0) - (f[2] - f[0]) * (y1 - y0)) / det;
        *gy = ((x1 - x0) * (f[2] - f[0]) - (x2 - x0) * (f[1] - f[0])) / det;
    };
    double ux = 0.0, uy = 0.0, vx_ = 0.0, vy_ = 0.0, wx = 0.0, wy = 0.0;
    grad(u_over_w, &ux, &uy);
    grad(v_over_w, &vx_, &vy_);
    grad(invw, &wx, &wy);
    // 质心处的 U/W（与采样器同一个式子）：l_i = 1/3。
    const double w_mid = (invw[0] + invw[1] + invw[2]) / 3.0;
    if (!std::isfinite(w_mid) || std::fabs(w_mid) < 1e-12) return false;
    const double u_mid = (u_over_w[0] + u_over_w[1] + u_over_w[2]) / 3.0 / w_mid;
    const double v_mid = (v_over_w[0] + v_over_w[1] + v_over_w[2]) / 3.0 / w_mid;
    // ∂(U/W)/∂x = (U_x − (U/W)·W_x) / W
    if (du_dx != nullptr) *du_dx = (ux - u_mid * wx) / w_mid;
    if (dv_dx != nullptr) *dv_dx = (vx_ - v_mid * wx) / w_mid;
    if (du_dy != nullptr) *du_dy = (uy - u_mid * wy) / w_mid;
    if (dv_dy != nullptr) *dv_dy = (vy_ - v_mid * wy) / w_mid;
    return std::isfinite(*du_dx) && std::isfinite(*dv_dx) && std::isfinite(*du_dy) && std::isfinite(*dv_dy);
}

double lodFromUvDerivatives(double du_dx, double dv_dx, double du_dy, double dv_dy, double tex_width,
                            double tex_height) {
    // ★ 退化 / 非法 ⇒ 返回一个**必然落到 0 级**的 LOD（⛔ 不返回 0，那会把"1:1"与"不知道"混起来）。
    if (!(tex_width > 0.0) || !(tex_height > 0.0)) return -1000.0;
    const double rx = std::sqrt((du_dx * tex_width) * (du_dx * tex_width) +
                                (dv_dx * tex_height) * (dv_dx * tex_height));
    const double ry = std::sqrt((du_dy * tex_width) * (du_dy * tex_width) +
                                (dv_dy * tex_height) * (dv_dy * tex_height));
    const double rho = rx > ry ? rx : ry;
    if (!std::isfinite(rho) || rho <= 0.0) return -1000.0;
    return std::log2(rho);
}

std::uint32_t mipLevelForLod(double lod, std::uint32_t levels) {
    if (levels <= 1u || !std::isfinite(lod)) return 0u;
    // 最近级：floor(lod + 0.5)。放大（lod < 0）/ 1:1（lod == 0）都落到 0 级。
    const double biased = std::floor(lod + 0.5);
    if (!(biased > 0.0)) return 0u;
    if (biased >= static_cast<double>(levels - 1u)) return levels - 1u;
    return static_cast<std::uint32_t>(biased);
}

bool samplerUsesLinear(std::uint32_t mag_filter, std::uint32_t min_filter) {
    // 定义放匿名 namespace 外面：头文件里是公开 API（夹具直接调，证明 MIN 不再被忽略）
    auto linear = [](std::uint32_t f) { return f == kTexfLinear || f == kTexfAnisotropic; };
    return linear(mag_filter) || linear(min_filter);
}

std::uint32_t drawPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t start_vertex,
                            std::uint32_t prim_count, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    DeclLayout decl;
    if (!drawPrecheck(cpu, "DrawPrimitive", dev, &pe, &fvf, &decl, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) {
        pipeLog("DrawPrimitive", "PrimitiveType=%u 非法 ⇒ D3DERR_INVALIDCALL", prim);
        return kInvalidCall;
    }
    if (prim_count == 0) {
        // 真机：0 个图元 ⇒ S_OK 且什么都不画
        return kOk;
    }
    // ★ 槽里是 **(对象 EA, 绑定时代际)**，⛔ 不是状态块 EA —— 只能经身份校验换算成状态块
    //   （缺陷单 `REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 #4：只验 `'VB09'` magic 会让"同 EA 新缓冲"
    //   静默顶替旧缓冲的顶点）。不符 ⇒ `kInvalidCall` + 默认级别可见的告警，⛔ 不画别人的数据。
    // ★ 声明路（`decl.present`）不用流 0 的 vb/stride：那是 POSITION 流的布局，顶点由
    //   `buildDeclVertices` 按声明从各自的流里取（越界检查也在那里逐流做）。
    std::uint32_t length = 0;
    std::uint32_t data = 0;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    if (!decl.present) {
        const std::uint32_t vb = plGet(cpu, pe, kPlStreams);
        const std::uint32_t vb_gen = plGet(cpu, pe, kPlStreams + 12u);
        if (vb == 0) {
            pipeLog("DrawPrimitive", "没有 SetStreamSource(0) ⇒ D3DERR_INVALIDCALL");
            return kInvalidCall;
        }
        if (!boundVertexBufferOfState(cpu, vb, vb_gen, "DrawPrimitive", nullptr, &length, &data, why))
            return kInvalidCall;
        offset = plGet(cpu, pe, kPlStreams + 4u);
        stride = plGet(cpu, pe, kPlStreams + 8u);
        if (stride == 0) stride = fvf.stride;
    }
    std::uint32_t vertex_count = 0;
    switch (prim) {
        case kPtTriangleList: vertex_count = prim_count * 3u; break;
        case kPtTriangleStrip: vertex_count = prim_count + 2u; break;
        case kPtTriangleFan: vertex_count = prim_count + 2u; break;
        case kPtLineList: vertex_count = prim_count * 2u; break;
        case kPtLineStrip: vertex_count = prim_count + 1u; break;
        default: vertex_count = prim_count; break;
    }
    if (!decl.present) {
        const std::uint64_t need = static_cast<std::uint64_t>(offset) +
                                   static_cast<std::uint64_t>(start_vertex + vertex_count) * stride;
        if (need > length) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "D3D9 流水线：DrawPrimitive 越过顶点缓冲（需要 %llu B，缓冲 %u B，stride=%u）",
                          static_cast<unsigned long long>(need), length, stride);
            pipeLog("DrawPrimitive", "%s ⇒ D3DERR_INVALIDCALL", buf);
            if (why) *why = buf;
            return kInvalidCall;
        }
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, "DrawPrimitive", dev, pe, &ctx, why)) return kInvalidCall;
    std::vector<std::uint8_t> decl_buf;
    const std::uint8_t* base = nullptr;
    if (decl.present) {
        if (!buildDeclVertices(cpu, pe, "DrawPrimitive", decl, start_vertex, vertex_count, &decl_buf, why))
            return kInvalidCall;
        base = decl_buf.data();
        offset = 0;
        stride = decl.synth_stride;
    } else {
        // guest 内存里的顶点数据（不走 arena ⇒ 直接指针）
        // ⚠ 这里用 GuestMemory 的宿主映射（与 Lock 返回的 guest EA 是同一块内存）
        base = cpu.mem->host(data);
        if (!base) {
            if (why) *why = "D3D9 流水线：顶点缓冲的宿主映射为空";
            return kInvalidCall;
        }
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    // 顶点偏移按字节：offset 是流偏移，start_vertex 是顶点序号
    std::vector<CVertex> verts;
    if (!gatherVertices(cpu, pe, fvf, base + offset, stride, start_vertex, vertex_count, world, view, proj, &verts,
                        why)) {
        return kInvalidCall;
    }
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawPrimitive",
            "PrimitiveType=%u StartVertex=%u PrimitiveCount=%u ⇒ 三角形 %u 个（光栅化 %u），写 %llu 个像素", prim,
            start_vertex, prim_count, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawIndexedPrimitive(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t base_vertex,
                                   std::uint32_t min_index, std::uint32_t num_vertices, std::uint32_t start_index,
                                   std::uint32_t prim_count, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    DeclLayout decl;
    if (!drawPrecheck(cpu, "DrawIndexedPrimitive", dev, &pe, &fvf, &decl, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    const std::uint32_t vb = plGet(cpu, pe, kPlStreams);
    const std::uint32_t vb_gen = plGet(cpu, pe, kPlStreams + 12u);
    const std::uint32_t ib = plGet(cpu, pe, kPlIndices);
    const std::uint32_t ib_gen = plGet(cpu, pe, kPlIndicesGen);
    if (vb == 0 || ib == 0) {
        pipeLog("DrawIndexedPrimitive", "缺少 SetStreamSource(0) 或 SetIndices ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    std::uint32_t vlength = 0;
    std::uint32_t vdata = 0;
    std::uint32_t ilength = 0;
    std::uint32_t idata = 0;
    std::uint32_t iformat = 0;
    // 顶点 / 索引两个绑定槽都做身份校验（同上：只验 magic 会被"同 EA 新缓冲"骗过）。
    if (!boundVertexBufferOfState(cpu, vb, vb_gen, "DrawIndexedPrimitive", nullptr, &vlength, &vdata, why))
        return kInvalidCall;
    if (!boundIndexBufferOfState(cpu, ib, ib_gen, "DrawIndexedPrimitive", nullptr, &ilength, &idata, &iformat, why))
        return kInvalidCall;
    std::uint32_t stride = plGet(cpu, pe, kPlStreams + 8u);
    if (stride == 0) stride = fvf.stride;
    const std::uint32_t index_size = iformat == 101u ? 2u : 4u;
    std::uint32_t index_count = 0;
    switch (prim) {
        case kPtTriangleList: index_count = prim_count * 3u; break;
        case kPtTriangleStrip: index_count = prim_count + 2u; break;
        case kPtTriangleFan: index_count = prim_count + 2u; break;
        case kPtLineList: index_count = prim_count * 2u; break;
        case kPtLineStrip: index_count = prim_count + 1u; break;
        default: index_count = prim_count; break;
    }
    if (start_index + index_count > ilength / index_size) {
        pipeLog("DrawIndexedPrimitive", "索引越界（start=%u count=%u，缓冲 %u 个索引）⇒ D3DERR_INVALIDCALL",
                start_index, index_count, ilength / index_size);
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, "DrawIndexedPrimitive", dev, pe, &ctx, why)) return kInvalidCall;
    // ★ 声明路：把两条流按声明去交错成一份 `num_vertices` 个顶点的临时缓冲，后面逐索引取顶点
    //   就退化成"从这份交错缓冲里按 `synth_stride` 取"——与 FVF 路完全同一条。
    std::vector<std::uint8_t> decl_buf;
    if (decl.present && !buildDeclVertices(cpu, pe, "DrawIndexedPrimitive", decl, 0u, num_vertices, &decl_buf, why)) {
        return kInvalidCall;
    }
    const std::uint8_t* vbase =
        decl.present ? decl_buf.data() : reinterpret_cast<const std::uint8_t*>(cpu.mem->host(vdata));
    const std::uint8_t* ibase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(idata));
    if (!vbase || !ibase) {
        if (why) *why = "D3D9 流水线：顶点 / 索引缓冲的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    const std::uint32_t voffset = decl.present ? 0u : plGet(cpu, pe, kPlStreams + 4u);
    const std::uint32_t vstride = decl.present ? decl.synth_stride : stride;
    const std::uint32_t vlen_eff = decl.present ? static_cast<std::uint32_t>(decl_buf.size()) : vlength;
    std::vector<CVertex> verts;
    verts.reserve(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        std::uint32_t idx = 0;
        if (index_size == 2u) {
            std::uint16_t v = 0;
            std::memcpy(&v, ibase + static_cast<std::size_t>(start_index + i) * 2u, 2u);
            idx = v;
        } else {
            std::memcpy(&idx, ibase + static_cast<std::size_t>(start_index + i) * 4u, 4u);
        }
        if (idx >= num_vertices) {
            pipeLog("DrawIndexedPrimitive", "索引 %u 超出 NumVertices=%u ⇒ D3DERR_INVALIDCALL", idx, num_vertices);
            return kInvalidCall;
        }
        const std::uint32_t vertex_index = idx + base_vertex;
        if ((static_cast<std::uint64_t>(voffset) +
             static_cast<std::uint64_t>(vertex_index + 1u) * vstride) > vlen_eff) {
            pipeLog("DrawIndexedPrimitive", "顶点 %u 越过顶点缓冲 ⇒ D3DERR_INVALIDCALL", vertex_index);
            return kInvalidCall;
        }
        CVertex v;
        transformVertex(cpu, pe, fvf, vbase + voffset + static_cast<std::size_t>(vertex_index) * vstride,
                        vertex_index, world, view, proj, &v);
        verts.push_back(v);
    }
    (void)min_index;
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawIndexedPrimitive",
            "PrimitiveType=%u BaseVertexIndex=%d PrimitiveCount=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            static_cast<int>(base_vertex), prim_count, tris, rast,
            static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t prim_count,
                              std::uint32_t data_ea, std::uint32_t stride, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawPrimitiveUP", dev, &pe, &fvf, nullptr, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    if (data_ea == 0) {
        pipeLog("DrawPrimitiveUP", "pVertexStreamZeroData=NULL ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (stride == 0) stride = fvf.stride;
    if (stride < fvf.stride) {
        pipeLog("DrawPrimitiveUP", "VertexStreamZeroStride=%u < FVF 需要的最小步长 %u ⇒ D3DERR_INVALIDCALL", stride,
                fvf.stride);
        return kInvalidCall;
    }
    std::uint32_t vertex_count = 0;
    switch (prim) {
        case kPtTriangleList: vertex_count = prim_count * 3u; break;
        case kPtTriangleStrip: vertex_count = prim_count + 2u; break;
        case kPtTriangleFan: vertex_count = prim_count + 2u; break;
        case kPtLineList: vertex_count = prim_count * 2u; break;
        case kPtLineStrip: vertex_count = prim_count + 1u; break;
        default: vertex_count = prim_count; break;
    }
    const std::uint64_t need = static_cast<std::uint64_t>(vertex_count) * stride;
    if (!cpu.mem->committed(data_ea, static_cast<std::size_t>(need))) {
        pipeLog("DrawPrimitiveUP", "顶点数据 0x%08X 起 %llu 字节未提交 ⇒ D3DERR_INVALIDCALL", data_ea,
                static_cast<unsigned long long>(need));
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, "DrawPrimitiveUP", dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(data_ea));
    if (!base) {
        if (why) *why = "D3D9 流水线：UP 顶点数据的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    std::vector<CVertex> verts;
    if (!gatherVertices(cpu, pe, fvf, base, stride, 0u, vertex_count, world, view, proj, &verts, why)) {
        return kInvalidCall;
    }
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawPrimitiveUP",
            "PrimitiveType=%u PrimitiveCount=%u Stride=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            prim_count, stride, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

std::uint32_t drawIndexedPrimitiveUP(X86& cpu, std::uint32_t dev, std::uint32_t prim, std::uint32_t min_index,
                                     std::uint32_t num_vertices, std::uint32_t prim_count,
                                     std::uint32_t index_ea, std::uint32_t index_format, std::uint32_t data_ea,
                                     std::uint32_t stride, std::string* why) {
    FvfLayout fvf;
    std::uint32_t pe = 0;
    if (!drawPrecheck(cpu, "DrawIndexedPrimitiveUP", dev, &pe, &fvf, nullptr, why)) return kInvalidCall;
    if (prim < kPtPointList || prim > kPtTriangleFan) return kInvalidCall;
    if (prim_count == 0) return kOk;
    if (data_ea == 0 || index_ea == 0) {
        pipeLog("DrawIndexedPrimitiveUP", "pVertexStreamZeroData / pIndexData 为 NULL ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (index_format != 101u && index_format != 102u) {
        pipeLog("DrawIndexedPrimitiveUP", "IndexDataFormat=%u（只认 INDEX16=101 / INDEX32=102）", index_format);
        return kInvalidCall;
    }
    if (stride == 0) stride = fvf.stride;
    if (stride < fvf.stride) return kInvalidCall;
    std::uint32_t index_count = 0;
    switch (prim) {
        case kPtTriangleList: index_count = prim_count * 3u; break;
        case kPtTriangleStrip: index_count = prim_count + 2u; break;
        case kPtTriangleFan: index_count = prim_count + 2u; break;
        case kPtLineList: index_count = prim_count * 2u; break;
        case kPtLineStrip: index_count = prim_count + 1u; break;
        default: index_count = prim_count; break;
    }
    const std::uint32_t index_size = index_format == 101u ? 2u : 4u;
    if (!cpu.mem->committed(index_ea, static_cast<std::size_t>(index_count) * index_size)) {
        pipeLog("DrawIndexedPrimitiveUP", "索引数据未提交 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    if (!cpu.mem->committed(data_ea, static_cast<std::size_t>(num_vertices) * stride)) {
        pipeLog("DrawIndexedPrimitiveUP", "顶点数据未提交 ⇒ D3DERR_INVALIDCALL");
        return kInvalidCall;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kInvalidCall;
    DrawContext ctx;
    if (!prepareContext(cpu, "DrawIndexedPrimitiveUP", dev, pe, &ctx, why)) return kInvalidCall;
    const std::uint8_t* ibase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(index_ea));
    const std::uint8_t* vbase = reinterpret_cast<const std::uint8_t*>(cpu.mem->host(data_ea));
    if (!ibase || !vbase) {
        if (why) *why = "D3D9 流水线：UP 顶点 / 索引数据的宿主映射为空";
        return kInvalidCall;
    }
    const math::Mat4d world = readMatrix(cpu, pe + kPlTransforms + kTsWorld * 64u);
    const math::Mat4d view = readMatrix(cpu, pe + kPlTransforms + kTsView * 64u);
    const math::Mat4d proj = readMatrix(cpu, pe + kPlTransforms + kTsProjection * 64u);
    std::vector<CVertex> verts;
    verts.reserve(index_count);
    for (std::uint32_t i = 0; i < index_count; ++i) {
        std::uint32_t idx = 0;
        if (index_size == 2u) {
            std::uint16_t v = 0;
            std::memcpy(&v, ibase + static_cast<std::size_t>(i) * 2u, 2u);
            idx = v;
        } else {
            std::memcpy(&idx, ibase + static_cast<std::size_t>(i) * 4u, 4u);
        }
        if (idx >= num_vertices) return kInvalidCall;
        CVertex v;
        transformVertex(cpu, pe, fvf, vbase + static_cast<std::size_t>(idx) * stride, idx, world, view, proj, &v);
        verts.push_back(v);
    }
    (void)min_index;
    std::uint32_t tris = 0;
    std::uint32_t rast = 0;
    if (!submitPrimitives(cpu, pe, prim, verts, prim_count, ctx, &tris, &rast, why)) return kInvalidCall;
    plSet(cpu, pe, kPlDraws, plGet(cpu, pe, kPlDraws) + 1u);
    pipeLog("DrawIndexedPrimitiveUP",
            "PrimitiveType=%u PrimitiveCount=%u Stride=%u ⇒ 三角形 %u（光栅化 %u），写 %llu 个像素", prim,
            prim_count, stride, tris, rast, static_cast<unsigned long long>(ctx.pixels_written));
    return kOk;
}

// =====================================================================================
// Present 的像素统计
// =====================================================================================
bool clearDepth(X86& cpu, std::uint32_t pipeline_ea, float z, std::string* why) {
    if (pipeline_ea == 0 || !cpu.mem->committed(pipeline_ea, kPlBytes) ||
        cpu.mem->rd32(pipeline_ea + kPlMagic) != kPlMagicValue) {
        if (why) *why = "D3D9 流水线：清深度时没有流水线状态块";
        return false;
    }
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return false;
    GuestSurface* dgs = m->surfaces().find(plGet(cpu, pipeline_ea, kPlDepthArena));
    if (!dgs) {
        if (why) *why = "D3D9 流水线：深度缓冲不在 arena 里";
        return false;
    }
    std::uint8_t* base = m->surfaces().pixels(*dgs);
    if (!base) {
        if (why) *why = "D3D9 流水线：深度缓冲像素指针为空";
        return false;
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &z, sizeof(bits));
    for (int y = 0; y < dgs->height; ++y) {
        for (int x = 0; x < dgs->width; ++x) {
            std::memcpy(base + static_cast<std::size_t>(y) * dgs->pitch + static_cast<std::size_t>(x) * 4u, &bits,
                        4u);
        }
    }
    return true;
}
bool frameStats(const SurfaceFormat& fmt, const std::uint8_t* base, std::uint32_t pitch, int width, int height,
                FrameStats* out) {
    if (!base || width <= 0 || height <= 0) return false;
    const std::uint32_t bpp = fmt.bytesPerPixel();
    if (bpp == 0) return false;
    std::map<std::uint32_t, std::uint32_t> histogram;
    std::uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = base + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* p = row + static_cast<std::size_t>(x) * bpp;
            const std::uint32_t raw = pixel_format::loadPixel(p, fmt.bpp);
            ++histogram[raw];
            hash ^= raw;
            hash *= 1099511628211ull;
        }
    }
    std::uint32_t best = 0;
    std::uint32_t best_count = 0;
    for (const auto& kv : histogram) {
        if (kv.second > best_count) {
            best_count = kv.second;
            best = kv.first;
        }
    }
    out->total_pixels = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
    out->background_color = best;
    out->background_pixels = best_count;
    out->non_background_pixels = out->total_pixels - best_count;
    out->distinct_colors = static_cast<std::uint32_t>(histogram.size());
    out->hash = hash;
    return true;
}

std::uint64_t gpuDrawCount() { return g_gpu_draws; }
std::uint64_t gpuTriangleCount() { return g_gpu_triangles; }

// =====================================================================================
// GPU 覆盖账本
// =====================================================================================
const GpuCoverage& gpuCoverage() { return g_gpu_coverage; }

void gpuCoverageReset() {
    // ★ 本单：上一轮永远没被回报的 `[draw]` 行在这里按提交顺序补打 `rec=0`
    //   （"到这一轮开始都没进过 pass"）。正常情况下（最后有 Present）不会有残留。
    drainPendingDrawLines(true);
    g_pending_draw_lines.clear();
    g_gpu_batch_ledger.clear();  // ★ 批号表按轮清；序号继续往前 ⇒ 上一轮迟到的回报撞不上新 slot
    g_gpu_coverage = GpuCoverage{};
    g_gpu_draws = 0;
    g_gpu_triangles = 0;
}

// ★ 后端回报：一条 draw 真的被录进 render pass、且命令缓冲**提交成功之后**（见 `gpu_raster.h`）。
//   判据就是"后端真的回报了"，⛔ 与 `rasterQueue` 的返回值无关。tag 不在本轮批号表里 ⇒ 忽略。
void gpuLedgerNoteRecorded(std::uint64_t tag) {
    if (tag == 0) return;
    auto it = g_gpu_batch_ledger.find(tag);
    if (it == g_gpu_batch_ledger.end() || it->second.recorded) return;
    it->second.recorded = true;
    ++g_gpu_coverage.recorded;
    if (it->second.counted_empty) --g_gpu_coverage.empty;
    // `[draw]` 行按**提交顺序**放出（队首那批已经 recorded 才轮得到它）。
    drainPendingDrawLines(false);
}

void gpuNoteFallback(const std::string& reason, std::uint64_t pixels) {
    ++g_gpu_coverage.fallback_draws;
    g_gpu_coverage.fallback_pixels += pixels;
    noteFallbackReason(reason);
}

void gpuNoteSpriteResult(bool gpu, std::uint64_t pixels, const std::string& reason) {
    ++g_gpu_coverage.sprite_draws;
    if (gpu) {
        ++g_gpu_coverage.sprite_gpu_draws;
        return;
    }
    ++g_gpu_coverage.sprite_fallback_draws;
    g_gpu_coverage.sprite_fallback_pixels += pixels;
    noteFallbackReason(reason.empty() ? std::string("sprite: 未知原因") : reason);
}

void gpuNoteSpriteSkipped() { ++g_gpu_coverage.sprite_skipped; }

// 默认级别告警的**拒绝计数**（与限流桶同一份：进程级；`dumpRunSummary` 的 `pipe-warn` 行读它）。
const PipeWarnCounts& pipeWarnCounts() { return g_pipe_warn_counts; }

// =====================================================================================
// GPU 准入 / 状态映射（纯函数）
// =====================================================================================
namespace {
bool blendFactorFor(std::uint32_t f, GpuBlendFactor* out) {
    switch (f) {
        case kBlendZero: *out = GpuBlendFactor::Zero; return true;
        case kBlendOne: *out = GpuBlendFactor::One; return true;
        case kBlendSrcColor: *out = GpuBlendFactor::SrcColor; return true;
        case kBlendInvSrcColor: *out = GpuBlendFactor::InvSrcColor; return true;
        case kBlendSrcAlpha: *out = GpuBlendFactor::SrcAlpha; return true;
        case kBlendInvSrcAlpha: *out = GpuBlendFactor::InvSrcAlpha; return true;
        case kBlendDestAlpha: *out = GpuBlendFactor::DstAlpha; return true;
        case kBlendInvDestAlpha: *out = GpuBlendFactor::InvDstAlpha; return true;
        case kBlendDestColor: *out = GpuBlendFactor::DstColor; return true;
        case kBlendInvDestColor: *out = GpuBlendFactor::InvDstColor; return true;
        case kBlendSrcAlphaSat: *out = GpuBlendFactor::SrcAlphaSaturate; return true;
        // D3DBLEND_BOTHSRCALPHA / BOTHINVSRCALPHA 是"同时改源和目标的因子"，
        // SDL_GPU 的固定 blend state 表达不了 ⇒ 明确拒绝（调用方回退 CPU 并记账）。
        default: return false;
    }
}

bool blendOpFor(std::uint32_t op, GpuBlendOp* out) {
    switch (op) {
        case kBlendOpAdd: *out = GpuBlendOp::Add; return true;
        case kBlendOpSubtract: *out = GpuBlendOp::Subtract; return true;
        case kBlendOpRevSubtract: *out = GpuBlendOp::ReverseSubtract; return true;
        case kBlendOpMin: *out = GpuBlendOp::Min; return true;
        case kBlendOpMax: *out = GpuBlendOp::Max; return true;
        default: return false;
    }
}
}  // namespace

bool gpuUntexturedIsIdentity(std::uint32_t op, std::uint32_t arg1, std::uint32_t arg2) {
    // 无纹理时 CPU 采样是白色；只有结果恒等于顶点色的组合才能由无纹理 GPU 着色器表达。
    return untexturedResolvesToDiffuse(op, arg1, arg2);
}

bool gpuBlendStateFor(std::uint32_t src_color, std::uint32_t dst_color, std::uint32_t color_op,
                      bool separate_alpha, std::uint32_t src_alpha, std::uint32_t dst_alpha,
                      std::uint32_t alpha_op, bool blend_enabled, GpuRasterState* out, std::string* why) {
    if (out == nullptr) {
        if (why) *why = "gpuBlendStateFor: out 为空";
        return false;
    }
    if (!blend_enabled) {
        out->blend = false;
        out->separate_alpha_blend = false;
        return true;
    }
    GpuBlendFactor sc = GpuBlendFactor::One;
    GpuBlendFactor dc = GpuBlendFactor::Zero;
    GpuBlendOp cop = GpuBlendOp::Add;
    if (!blendFactorFor(src_color, &sc) || !blendFactorFor(dst_color, &dc) || !blendOpFor(color_op, &cop)) {
        if (why) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "blend 不能映射到 SDL_GPU（color src=%u dst=%u op=%u）", src_color, dst_color, color_op);
            *why = buf;
        }
        return false;
    }
    out->blend = true;
    out->src_blend = sc;
    out->dst_blend = dc;
    out->blend_op = cop;
    out->separate_alpha_blend = separate_alpha;
    if (separate_alpha) {
        GpuBlendFactor sa = GpuBlendFactor::One;
        GpuBlendFactor da = GpuBlendFactor::Zero;
        GpuBlendOp aop = GpuBlendOp::Add;
        if (!blendFactorFor(src_alpha, &sa) || !blendFactorFor(dst_alpha, &da) || !blendOpFor(alpha_op, &aop)) {
            if (why) {
                char buf[224];
                std::snprintf(buf, sizeof(buf),
                              "separate alpha blend 不能映射到 SDL_GPU（alpha src=%u dst=%u op=%u）", src_alpha,
                              dst_alpha, alpha_op);
                *why = buf;
            }
            return false;
        }
        out->src_blend_alpha = sa;
        out->dst_blend_alpha = da;
        out->blend_op_alpha = aop;
    } else {
        // D3D 语义：SEPARATEALPHABLENDENABLE=0 时 alpha 用 color 的因子/op。
        out->src_blend_alpha = sc;
        out->dst_blend_alpha = dc;
        out->blend_op_alpha = cop;
    }
    return true;
}

namespace {
// stage 0 的 CURRENT 就是 DIFFUSE（纹理级链的输入 = 顶点色）；TFACTOR 是整批 Draw 的常量色。
enum class ConstSrc : std::uint8_t { None = 0, Diffuse, TFactor };
bool constArgAtStage0(std::uint32_t arg, ConstSrc* out) {
    if (arg == kTaDiffuse || arg == kTaCurrent) {
        *out = ConstSrc::Diffuse;
        return true;
    }
    if (arg == kTaTFactor) {
        *out = ConstSrc::TFactor;
        return true;
    }
    return false;
}
bool argIsTexture(std::uint32_t arg) { return arg == kTaTexture; }

enum class ExprKind : std::uint8_t { Const = 0, Texture, ModulateTextureConst, Unsupported };
struct StageExpr {
    ExprKind kind = ExprKind::Unsupported;
    ConstSrc ck = ConstSrc::None;  // kind==Const / ModulateTextureConst 时有效
};

StageExpr stageExprFor(std::uint32_t op, std::uint32_t arg1, std::uint32_t arg2) {
    StageExpr e;
    ConstSrc ck = ConstSrc::None;
    switch (op) {
        case kTopDisable:
            // ★ CPU 的 applyTextureOp 对 DISABLE 落到 default 分支：取 **arg1**（不是“恒等于 DIFFUSE”）。
            //   所以这里按 SELECTARG1(arg1) 判 —— 有纹理 + arg1=TEXTURE 时 CPU 取的是纹理。
            return stageExprFor(kTopSelectArg1, arg1, 0u);
        case kTopSelectArg1:
            if (argIsTexture(arg1)) { e.kind = ExprKind::Texture; return e; }
            if (constArgAtStage0(arg1, &ck)) { e.kind = ExprKind::Const; e.ck = ck; return e; }
            return e;
        case kTopSelectArg2:
            if (argIsTexture(arg2)) { e.kind = ExprKind::Texture; return e; }
            if (constArgAtStage0(arg2, &ck)) { e.kind = ExprKind::Const; e.ck = ck; return e; }
            return e;
        case kTopModulate: {
            ConstSrc c1 = ConstSrc::None;
            ConstSrc c2 = ConstSrc::None;
            if (argIsTexture(arg1) && constArgAtStage0(arg2, &c2)) {
                e.kind = ExprKind::ModulateTextureConst;
                e.ck = c2;
                return e;
            }
            if (constArgAtStage0(arg1, &c1) && argIsTexture(arg2)) {
                e.kind = ExprKind::ModulateTextureConst;
                e.ck = c1;
                return e;
            }
            return e;
        }
        default: return e;
    }
}

void noteConst(ConstSrc ck, bool* has_diffuse, bool* has_tfactor) {
    if (ck == ConstSrc::Diffuse) *has_diffuse = true;
    else if (ck == ConstSrc::TFactor) *has_tfactor = true;
}
}  // namespace

bool gpuTextureModeFor(std::uint32_t color_op, std::uint32_t color_arg1, std::uint32_t color_arg2,
                       std::uint32_t alpha_op, std::uint32_t alpha_arg1, std::uint32_t alpha_arg2,
                       bool texture_present, std::uint32_t tex_coord_index, GpuTexMode* mode,
                       bool* color_from_tfactor, std::string* why) {
    if (mode == nullptr) {
        if (why) *why = "gpuTextureModeFor: mode 为空";
        return false;
    }
    if (color_from_tfactor != nullptr) *color_from_tfactor = false;
    if (tex_coord_index >= kMaxTextureStages) {
        if (why) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "D3DTSS_TEXCOORDINDEX=%u 超出本线的纹理坐标上限（0..7）",
                          tex_coord_index);
            *why = buf;
        }
        return false;
    }
    const StageExpr cs = stageExprFor(color_op, color_arg1, color_arg2);
    const StageExpr as = stageExprFor(alpha_op, alpha_arg1, alpha_arg2);
    if (cs.kind == ExprKind::Unsupported || as.kind == ExprKind::Unsupported) {
        if (why) {
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "纹理级 0 的 op/arg 不能等价表达（color op=%u arg=%u/%u，alpha op=%u arg=%u/%u）",
                          color_op, color_arg1, color_arg2, alpha_op, alpha_arg1, alpha_arg2);
            *why = buf;
        }
        return false;
    }
    // 无纹理时 CPU 采样是白色（1,1,1,1）⇒ 纹理参与等于恒等；但 SELECTARG1(TEXTURE) 会得到白色，
    // 不能用一个"顶点色"着色器冒充 ⇒ 明确拒绝（有纹理时才走本函数）。
    (void)texture_present;
    // GPU 片元着色器只有一个"顶点色"输入槽：DIFFUSE 与 TFACTOR 不能同时出现（否则无法区分）。
    bool has_diffuse = false;
    bool has_tfactor = false;
    noteConst(cs.ck, &has_diffuse, &has_tfactor);
    noteConst(as.ck, &has_diffuse, &has_tfactor);
    if (has_diffuse && has_tfactor) {
        if (why) *why = "纹理级 0 同时用了 DIFFUSE 与 TFACTOR（GPU 只有一个顶点色输入槽）";
        return false;
    }
    const bool use_tfactor = has_tfactor;
    // 组合 → 片元置换（Const 槽在 GPU 上由顶点色承载：DIFFUSE 原样，TFACTOR 整批替换）
    auto pair = [](ExprKind c, ExprKind a) { return static_cast<int>(c) * 8 + static_cast<int>(a); };
    const int key = pair(cs.kind, as.kind);
    switch (key) {
        case pair(ExprKind::ModulateTextureConst, ExprKind::ModulateTextureConst):
            *mode = GpuTexMode::ModulateBoth;
            break;
        case pair(ExprKind::Texture, ExprKind::Texture):
            *mode = GpuTexMode::SelectTextureBoth;
            break;
        case pair(ExprKind::ModulateTextureConst, ExprKind::Texture):
            *mode = GpuTexMode::ModulateColorSelectAlpha;
            break;
        case pair(ExprKind::Texture, ExprKind::ModulateTextureConst):
            *mode = GpuTexMode::SelectColorModulateAlpha;
            break;
        case pair(ExprKind::ModulateTextureConst, ExprKind::Const):
            *mode = GpuTexMode::ModulateColorDiffuseAlpha;
            break;
        case pair(ExprKind::Texture, ExprKind::Const):
            *mode = GpuTexMode::SelectColorDiffuseAlpha;
            break;
        default:
            if (why) *why = "纹理级 0 的组合（Const/Const 或 Const 在颜色侧）没有 GPU 片元置换";
            return false;
    }
    if (color_from_tfactor != nullptr) *color_from_tfactor = use_tfactor;
    return true;
}

}  // namespace d3d9

// ★ 覆盖账本的回报入口（声明在 `gpu_raster.h`，由 `d3d9_gpu.cpp` 在提交成功之后调用）。
//   账本本身在 d3d9 侧（批号表与 `GpuCoverage` 都在那边）⇒ 这里只做一层转调。
void gpuLedgerNoteRecordedDraw(std::uint64_t tag) { d3d9::gpuLedgerNoteRecorded(tag); }

}  // namespace lift
