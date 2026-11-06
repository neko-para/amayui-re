// emulator-lift · DirectSound（DSOUND.DLL）silent 层（M2 · SCOPE §1「音频 = 延后实现（silent）」）
//
// ★ 入口是**动态导出**（与 ddraw 同一套 G1 转发表，⛔ 没有给 DSOUND 开特例）：
//   `.lst` 0x004B5C64 `LoadLibraryA("DSOUND.DLL")` → 0x004B5C9F
//   `GetProcAddress(h, "DirectSoundCreate")`（函数 sub_4B5C50 = 0x004B5C50，调用方 0x00406CE0）。
//   ★ **引擎只取这一个导出**：`sub_4B5C50` 的 `.lst` 里只有一处 `GetProcAddress`（0x4B5C9F → 0x4B5CA6），
//     名字字符串 `aDirectsoundcre`（.data:0052CEA0）逐字 = "DirectSoundCreate"；
//     `DirectSoundCreate8` / `DirectSoundEnumerateA` / `DirectSoundCaptureCreate` 在整份 `.lst` 里
//     **0 次出现**（grep 过）⇒ 这里**不登记**它们（⛔ 不凭记忆加导出）。
//
// ★ 口径（SCOPE §1 音频 = silent）：**不出声，但也不报错，而且记账**。
//   判据（缺一条都不算做完）：
//     ① `DirectSoundCreate(pcGuid, ppDS, pUnkOuter)` ⇒ `DS_OK(0)` + guest 可见的 COM 对象
//        （`ComRegistry::createObject`，与 DDraw / D3D9 同一条机制）；
//     ② 设备上**引擎真会调的**槽按"静音设备"给真值（`.lst` 取证到的调用点见下面方法表）；
//     ③ 缓冲的 `Lock` 给一块**真的 guest 内存**（引擎往里面写 PCM），`Unlock` 把这批字节
//        记进账本；`Play` / `Stop` / `Set*` 记账并返回 `DS_OK`；
//     ④ 采样率 / 声道 / 位深从 `WAVEFORMATEX` **如实**记录（进日志、进 `GetFormat` 的返回值）；
//     ⑤ 每次记录都留痕：**同一对象的同一方法只打第一次**（重复不打），
//        对象 `Release` 时打一行该对象的最终账本（累计 PCM 字节 / Play 次数 / 非 0 字节数）。
//   ⛔ 不许"返回成功但什么都没做"而不留痕 —— 那就是静默 no-op（SCOPE §3 第 3 条）。
//   ⛔ 未实现的槽 = `comUnsupported` 的 **loud trap**（vtable 里显式占位，见方法表末尾的三条哨兵）。
//
// ★ 为什么 `Lock` 要给真内存而不是 NULL：引擎 0x004B5F83 拿到锁指针后**直接 `memset(p,0,Size)`**
//   （Size = 缓冲字节数 = 176400）⇒ 给 NULL 或越界块就是宿主侧写坏 / guest 页 fault。
//
// ★ 宿主侧**没有全局状态**：所有账本都在 guest 堆上的状态块里（与 `runtime/ddraw_surface.cpp` 同形），
//   生命周期跟对象的 refcount 走 ⇒ 两台机器 / 两个夹具之间互不影响（判据 A4 的前提）。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/exec.h"
#include "runtime/heap.h"
#include "runtime/memory.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---------------------------------------------------------------------------------------
// 返回码
// ★ 只用到这几个；它们来自 **SDK dsound.h**，⛔ 不是从 `.lst` 取证的（引擎的成功路径只看 0 / 非 0）：
//   错误路径在 boot 上不会走到，所以这些值不会成为"结论"，只保证不把错误伪装成成功。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kDsOk = 0u;                  // DS_OK
constexpr std::uint32_t kEInvalidArg = 0x80070057u;  // DSERR_INVALIDPARAM = E_INVALIDARG
constexpr std::uint32_t kENoInterface = 0x80004002u;  // E_NOINTERFACE
constexpr std::uint32_t kDsErrInvalidCall = 0x88780032u;   // DSERR_INVALIDCALL（MAKE_DSHRESULT(50)）
constexpr std::uint32_t kDsErrNoAggregation = 0x80040110u;  // DSERR_NOAGGREGATION = CLASS_E_NOAGGREGATION

// ---------------------------------------------------------------------------------------
// IID（16 字节，内存里的字节序）
// ★ 来源 = SDK dsound.h。引擎**不做 QI**（整份 `.lst` 里没有 IID_IDirectSound* 的符号 / 常量：
//   `grep DirectSound8` 0 命中）⇒ 这几份 GUID 无法用 `.lst` 交叉验证；夹具拿同一份常量做
//   正反两向断言（本 IID 成功 / 别的 E_NOINTERFACE）。
// ---------------------------------------------------------------------------------------
// IID_IDirectSound8 = {C50A7E93-F395-4834-9EF6-7FA99DE50966}
constexpr std::uint8_t kIidDirectSound8[16] = {0x93, 0x7E, 0x0A, 0xC5, 0xF3, 0x95, 0x48, 0x34,
                                               0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66};
// IID_IDirectSound = {279AFA83-4981-11CE-A521-0020AF0BE560}
constexpr std::uint8_t kIidDirectSound[16] = {0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                              0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
// IID_IDirectSoundBuffer8 = {6825A449-7524-4D82-9200-50DC6C495CF8}
constexpr std::uint8_t kIidDirectSoundBuffer8[16] = {0x49, 0xA4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4D,
                                                     0x92, 0x00, 0x50, 0xDC, 0x6C, 0x49, 0x5C, 0xF8};
// IID_IDirectSoundBuffer = {279AFA85-4981-11CE-A521-0020AF0BE560}
constexpr std::uint8_t kIidDirectSoundBuffer[16] = {0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11,
                                                    0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60};
// IID_IDirectSoundNotify = {B0210783-89CD-11D0-AF08-00A0C925CD16}
constexpr std::uint8_t kIidDirectSoundNotify[16] = {0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11,
                                                    0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16};
// IID_IUnknown = {00000000-0000-0000-C000-000000000046}
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

// ---------------------------------------------------------------------------------------
// 状态块（guest 堆；挂在 ComRegistry 对象的 `obj+8` 扩展槽上，生命周期跟 refcount 走）
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kDevMagic = 0x38445344u;  // 'DSD8'
constexpr std::uint32_t kBufMagic = 0x38425344u;  // 'DSB8'
constexpr std::uint32_t kDevBytes = 0x20u;
constexpr std::uint32_t kBufBytes = 0x80u;

// 设备状态块
constexpr std::uint32_t kDevOffMagic = 0x00;
constexpr std::uint32_t kDevOffHwnd = 0x04;
constexpr std::uint32_t kDevOffCoop = 0x08;
constexpr std::uint32_t kDevOffBuffers = 0x0C;   // 建过几个 buffer
constexpr std::uint32_t kDevOffSpeaker = 0x10;   // GetSpeakerConfig 的返回值
constexpr std::uint32_t kDevOffSeen = 0x14;      // 日志"首次调用"位图
constexpr std::uint32_t kDevOffCreates = 0x18;   // DirectSoundCreate 成功次数

// 缓冲状态块
constexpr std::uint32_t kBufOffMagic = 0x00;
constexpr std::uint32_t kBufOffDevice = 0x04;
constexpr std::uint32_t kBufOffBytes = 0x08;      // DSBUFFERDESC.dwBufferBytes（原始值）
constexpr std::uint32_t kBufOffData = 0x0C;       // 宿主给的 guest PCM 块（≥1 字节）
constexpr std::uint32_t kBufOffFlags = 0x10;      // DSBUFFERDESC.dwFlags
constexpr std::uint32_t kBufOffFmtTag = 0x14;     // WAVEFORMATEX.wFormatTag
constexpr std::uint32_t kBufOffChannels = 0x18;   // .nChannels
constexpr std::uint32_t kBufOffSamples = 0x1C;    // .nSamplesPerSec
constexpr std::uint32_t kBufOffAvgBytes = 0x20;   // .nAvgBytesPerSec
constexpr std::uint32_t kBufOffBlockAlign = 0x24;  // .nBlockAlign
constexpr std::uint32_t kBufOffBits = 0x28;       // .wBitsPerSample
constexpr std::uint32_t kBufOffCbSize = 0x2C;     // .cbSize
constexpr std::uint32_t kBufOffHasFmt = 0x30;     // 1 = 这个 buffer 带 WAVEFORMATEX
constexpr std::uint32_t kBufOffPlaying = 0x34;
constexpr std::uint32_t kBufOffLooping = 0x38;
constexpr std::uint32_t kBufOffSeen = 0x3C;
constexpr std::uint32_t kBufOffLockDepth = 0x40;
constexpr std::uint32_t kBufOffLock1 = 0x44;
constexpr std::uint32_t kBufOffLock1Bytes = 0x48;
constexpr std::uint32_t kBufOffLock2 = 0x4C;
constexpr std::uint32_t kBufOffLock2Bytes = 0x50;
constexpr std::uint32_t kBufOffPcmBytes = 0x54;    // 累计"交给引擎写"的字节（Unlock 的窗口和）
constexpr std::uint32_t kBufOffNonZero = 0x58;     // 这些窗口里**非 0** 的字节数（证明引擎真写过）
constexpr std::uint32_t kBufOffPlays = 0x5C;
constexpr std::uint32_t kBufOffStops = 0x60;
constexpr std::uint32_t kBufOffLocks = 0x64;
constexpr std::uint32_t kBufOffVolume = 0x68;      // 1/100 dB（0 = 满音量）
constexpr std::uint32_t kBufOffFrequency = 0x6C;
constexpr std::uint32_t kBufOffPan = 0x70;
constexpr std::uint32_t kBufOffCursor = 0x74;      // 播放游标（字节；静音设备上 = 已送入的字节）
constexpr std::uint32_t kBufOffRestores = 0x78;

// 日志"首次调用"位（同一对象的同一方法只打一次；⛔ 不刷屏）
enum : std::uint32_t {
    kSeenQi = 1u << 0,
    kSeenAddRef = 1u << 1,
    kSeenRelease = 1u << 2,
    kSeenCreateBuffer = 1u << 3,
    kSeenGetCaps = 1u << 4,
    kSeenDuplicate = 1u << 5,
    kSeenSetCoop = 1u << 6,
    kSeenSpeaker = 1u << 7,
    kSeenLock = 1u << 8,
    kSeenUnlock = 1u << 9,
    kSeenPlay = 1u << 10,
    kSeenStop = 1u << 11,
    kSeenSetPos = 1u << 12,
    kSeenGetPos = 1u << 13,
    kSeenSetVolume = 1u << 14,
    kSeenSetFreq = 1u << 15,
    kSeenSetPan = 1u << 16,
    kSeenSetFormat = 1u << 17,
    kSeenGetFormat = 1u << 18,
    kSeenStatus = 1u << 19,
    kSeenRestore = 1u << 20,
};

constexpr std::uint32_t kMaxDetailedBuffers = 32u;  // 超过这个数只记账、不再逐条打格式

void dsLog(const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "[dsound] %s: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// loud trap：状态崩坏 / 参数形态没实现 ⇒ 打印现场并停机器（⛔ 不静默返回 0）。
void dsTrap(X86& cpu, const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== unimplemented DirectSound call ===\n  %s\n  reason: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n  （⛔ 不静默返回 0；SCOPE §1「音频 silent」只覆盖记过账的路径）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented DirectSound call";
    ++cpu.shim.unimplemented_calls;
}

bool sameGuid(X86& cpu, std::uint32_t ea, const std::uint8_t (&want)[16]) {
    if (ea == 0 || !cpu.mem->committed(ea, 16)) return false;
    std::uint8_t got[16];
    cpu.mem->readBytes(ea, got, sizeof(got));
    for (int i = 0; i < 16; ++i)
        if (got[i] != want[i]) return false;
    return true;
}

// ---- 状态块的读 / 写（key 是 guest EA）----
std::uint32_t stateU32(X86& cpu, std::uint32_t st, std::uint32_t off) {
    return (st != 0 && cpu.mem->committed(st + off, 4)) ? cpu.mem->rd32(st + off) : 0u;
}
void setStateU32(X86& cpu, std::uint32_t st, std::uint32_t off, std::uint32_t v) {
    if (st != 0 && cpu.mem->committed(st + off, 4)) cpu.mem->wr32(st + off, v);
}
void bumpState(X86& cpu, std::uint32_t st, std::uint32_t off, std::uint32_t by = 1u) {
    setStateU32(cpu, st, off, stateU32(cpu, st, off) + by);
}

// "同一对象的同一方法只打第一次" ⇒ 返回 true 表示**这是第一次**（调用方据此决定打不打）。
bool firstTime(X86& cpu, std::uint32_t st, std::uint32_t bit, std::uint32_t seen_off) {
    if (st == 0) return true;
    const std::uint32_t seen = stateU32(cpu, st, seen_off);
    if ((seen & bit) != 0) return false;
    setStateU32(cpu, st, seen_off, seen | bit);
    return true;
}

// ---- WAVEFORMATEX ----
struct Wfx {
    std::uint32_t tag = 0;
    std::uint32_t channels = 0;
    std::uint32_t samples = 0;
    std::uint32_t avg_bytes = 0;
    std::uint32_t block_align = 0;
    std::uint32_t bits = 0;
    std::uint32_t cb_size = 0;
    bool present = false;
};

constexpr std::uint32_t kWfxBytes = 18u;

Wfx readWfx(X86& cpu, std::uint32_t ea) {
    Wfx w;
    if (ea == 0 || !cpu.mem->committed(ea, kWfxBytes)) return w;
    w.tag = cpu.mem->rd16(ea + 0);
    w.channels = cpu.mem->rd16(ea + 2);
    w.samples = cpu.mem->rd32(ea + 4);
    w.avg_bytes = cpu.mem->rd32(ea + 8);
    w.block_align = cpu.mem->rd16(ea + 12);
    w.bits = cpu.mem->rd16(ea + 14);
    w.cb_size = cpu.mem->rd16(ea + 16);
    w.present = true;
    return w;
}

// 一行可读的格式文本（⛔ 不把值改写成别的：原值照打）
std::string wfxText(const Wfx& w) {
    if (!w.present) return std::string("无（没给 lpwfxFormat）");
    char buf[192];
    std::snprintf(buf, sizeof(buf), "tag=%u(%s) ch=%u rate=%uHz bits=%u blockAlign=%u avgBytes=%u cbSize=%u",
                  static_cast<unsigned>(w.tag), w.tag == 1 ? "PCM" : "非 PCM",
                  static_cast<unsigned>(w.channels), static_cast<unsigned>(w.samples),
                  static_cast<unsigned>(w.bits), static_cast<unsigned>(w.block_align),
                  static_cast<unsigned>(w.avg_bytes), static_cast<unsigned>(w.cb_size));
    return std::string(buf);
}

void storeWfx(X86& cpu, std::uint32_t st, const Wfx& w) {
    setStateU32(cpu, st, kBufOffFmtTag, w.tag);
    setStateU32(cpu, st, kBufOffChannels, w.channels);
    setStateU32(cpu, st, kBufOffSamples, w.samples);
    setStateU32(cpu, st, kBufOffAvgBytes, w.avg_bytes);
    setStateU32(cpu, st, kBufOffBlockAlign, w.block_align);
    setStateU32(cpu, st, kBufOffBits, w.bits);
    setStateU32(cpu, st, kBufOffCbSize, w.cb_size);
    setStateU32(cpu, st, kBufOffHasFmt, w.present ? 1u : 0u);
}

Wfx loadWfx(X86& cpu, std::uint32_t st) {
    Wfx w;
    w.present = stateU32(cpu, st, kBufOffHasFmt) != 0;
    w.tag = stateU32(cpu, st, kBufOffFmtTag);
    w.channels = stateU32(cpu, st, kBufOffChannels);
    w.samples = stateU32(cpu, st, kBufOffSamples);
    w.avg_bytes = stateU32(cpu, st, kBufOffAvgBytes);
    w.block_align = stateU32(cpu, st, kBufOffBlockAlign);
    w.bits = stateU32(cpu, st, kBufOffBits);
    w.cb_size = stateU32(cpu, st, kBufOffCbSize);
    return w;
}

// DSBUFFERDESC（32 位布局；dwSize = 36 时 guid3DAlgorithm 也在结构体里）
constexpr std::uint32_t kDsBufferDescBytes = 36u;
constexpr std::uint32_t kDbOffSize = 0;
constexpr std::uint32_t kDbOffFlags = 4;
constexpr std::uint32_t kDbOffBytes = 8;
constexpr std::uint32_t kDbOffReserved = 12;
constexpr std::uint32_t kDbOffFormat = 16;

constexpr std::uint32_t kCapsPrimary = 0x00000001u;
constexpr std::uint32_t kCapsCtrlVolume = 0x00000080u;
constexpr std::uint32_t kCapsCtrlPan = 0x00000040u;
constexpr std::uint32_t kCapsCtrlFrequency = 0x00000008u;
constexpr std::uint32_t kCapsCtrlPositionNotify = 0x00000100u;
constexpr std::uint32_t kCapsGlobalFocus = 0x00008000u;
constexpr std::uint32_t kCapsGetCurrentPosition2 = 0x00010000u;

constexpr std::uint32_t kStatusPlaying = 0x00000001u;
constexpr std::uint32_t kStatusLooping = 0x00000002u;
constexpr std::uint32_t kPlayLooping = 0x00000001u;

// ---------------------------------------------------------------------------------------
// 方法表（定义在文件末尾）：CreateSoundBuffer / DuplicateSoundBuffer 要拿它建对象
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& deviceMethods();
const std::vector<ComMethod>& bufferMethods();
const std::vector<ComMethod>& notifyMethods();

// 对象 → 状态块 EA（设备与缓冲都挂 `obj+8`）
std::uint32_t stateOf(X86& cpu, std::uint32_t obj) { return cpu.com ? cpu.com->userOf(obj) : 0u; }

// 缓冲对象释放时回收独立分配的两块 guest 内存（数据块 + 状态块）
void destroyBufferBlocks(X86& cpu, std::uint32_t st) {
    if (st == 0) return;
    const std::uint32_t data = stateU32(cpu, st, kBufOffData);
    if (data != 0 && cpu.heap) cpu.heap->free(data);
    if (cpu.heap) cpu.heap->free(st);
}

// ---------------------------------------------------------------------------------------
// IDirectSoundNotify（`sub_4B71C0` 在 CreateSoundBuffer 之后 QI 它；缺了会让 WAV 加载失败）
// ---------------------------------------------------------------------------------------
void shimNotifyQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4) || riid == 0 || !cpu.mem->committed(riid, 16)) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (!sameGuid(cpu, riid, kIidDirectSoundNotify) && !sameGuid(cpu, riid, kIidUnknown)) {
        call.result = kENoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = kENoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = kDsOk;
}

void shimNotifySetNotificationPositions(X86&, ShimCall& call) {
    // `sub_4B71C0` 传 4 个 DSBPOSITIONNOTIFY；静音设备只记账，不投递事件。
    call.result = kDsOk;
}

// ---------------------------------------------------------------------------------------
// IUnknown（设备 / 缓冲共用）
// ---------------------------------------------------------------------------------------
void shimQueryInterface(X86& cpu, ShimCall& call, bool buffer) {
    const char* method = buffer ? "IDirectSoundBuffer8::QueryInterface" : "IDirectSound8::QueryInterface";
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        dsLog(method, "ppvObj 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM(0x%08X)", kEInvalidArg);
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        dsLog(method, "riid 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM(0x%08X)", kEInvalidArg);
        call.result = kEInvalidArg;
        return;
    }
    // ★ 缓冲要支持 IDirectSoundNotify：`sub_4B71C0` 在 CreateSoundBuffer 之后 QI 它，
    //   非零失败会让 `sub_4B6570` 返回 0 → WAV 加载失败 → Command_ShowMessage_Exception。
    if (buffer && sameGuid(cpu, riid, kIidDirectSoundNotify)) {
        if (!cpu.com) {
            call.result = kENoInterface;
            return;
        }
        const std::uint32_t obj = cpu.com->createObject(cpu, "IDirectSoundNotify", notifyMethods(), 0);
        if (!obj) {
            call.result = 0x8007000Eu;  // E_OUTOFMEMORY
            return;
        }
        cpu.mem->wr32(ppv, obj);
        if (firstTime(cpu, stateOf(cpu, self), kSeenQi, kBufOffSeen)) {
            dsLog(method, "QI IDirectSoundNotify 成功 ⇒ 新对象 0x%08X（SetNotificationPositions 静音）", obj);
        }
        call.result = kDsOk;
        return;
    }

    const bool wanted = buffer
                            ? (sameGuid(cpu, riid, kIidDirectSoundBuffer8) ||
                               sameGuid(cpu, riid, kIidDirectSoundBuffer) || sameGuid(cpu, riid, kIidUnknown))
                            : (sameGuid(cpu, riid, kIidDirectSound8) || sameGuid(cpu, riid, kIidDirectSound) ||
                               sameGuid(cpu, riid, kIidUnknown));
    if (!wanted) {
        std::uint8_t got[16];
        cpu.mem->readBytes(riid, got, sizeof(got));
        dsLog(method,
              "riid={%08X-%04X-%04X-...} 不是本对象的接口 ⇒ E_NOINTERFACE(0x%08X)（静音设备不是别的设备）",
              static_cast<unsigned>(got[0] | (got[1] << 8) | (got[2] << 16) |
                                    (static_cast<unsigned>(got[3]) << 24)),
              static_cast<unsigned>(got[4] | (got[5] << 8)), static_cast<unsigned>(got[6] | (got[7] << 8)),
              kENoInterface);
        call.result = kENoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        dsLog(method, "this=0x%08X 不是 ComRegistry 建出来的对象 ⇒ E_NOINTERFACE", self);
        call.result = kENoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    if (firstTime(cpu, stateOf(cpu, self), kSeenQi, buffer ? kBufOffSeen : kDevOffSeen)) {
        dsLog(method, "QI 成功 ⇒ 同一对象（refcount=%u）", cpu.com->refCount(self));
    }
    call.result = kDsOk;
}

void shimQueryInterfaceDevice(X86& cpu, ShimCall& call) { shimQueryInterface(cpu, call, false); }
void shimQueryInterfaceBuffer(X86& cpu, ShimCall& call) { shimQueryInterface(cpu, call, true); }

void shimAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    if (firstTime(cpu, stateOf(cpu, self), kSeenAddRef,
                  cpu.com->ifaceOf(self) && std::strstr(cpu.com->ifaceOf(self), "Buffer") ? kBufOffSeen
                                                                                        : kDevOffSeen)) {
        dsLog("AddRef", "this=0x%08X ⇒ refcount=%u", self, cpu.com->refCount(self));
    }
    call.result = cpu.com->refCount(self);
}

// Release：到 0 时先打对象自己的账本，再回收 guest 块（数据块 / 状态块）与 COM 对象。
void shimRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        dsLog("Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 返回 0", self);
        call.result = 0;
        return;
    }
    const char* iface = cpu.com->ifaceOf(self);
    const bool buffer = iface && std::strstr(iface, "Buffer") != nullptr;
    const std::uint32_t st = stateOf(cpu, self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (last && buffer && st != 0) {
        // ★ 这个对象的最终账本（"silent 但记账"的可断言落点）
        dsLog("Release",
              "IDirectSoundBuffer8 最终账本：bytes=%u flags=0x%08X %s pcmBytes=%u nonzeroBytes=%u "
              "locks=%u plays=%u stops=%u restores=%u volume=%d freq=%u pan=%d",
              static_cast<unsigned>(stateU32(cpu, st, kBufOffBytes)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffFlags)),
              wfxText(loadWfx(cpu, st)).c_str(),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPcmBytes)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffNonZero)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffLocks)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPlays)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffStops)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffRestores)),
              static_cast<int>(static_cast<std::int32_t>(stateU32(cpu, st, kBufOffVolume))),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffFrequency)),
              static_cast<int>(static_cast<std::int32_t>(stateU32(cpu, st, kBufOffPan))));
    } else if (last && !buffer && st != 0) {
        dsLog("Release",
              "IDirectSound8 最终账本：creates=%u cooperativeLevel=%u hwnd=0x%08X buffers=%u "
              "speakerConfig=0x%08X（silent：不出声、不报错，全部记账）",
              static_cast<unsigned>(stateU32(cpu, st, kDevOffCreates)),
              static_cast<unsigned>(stateU32(cpu, st, kDevOffCoop)),
              static_cast<unsigned>(stateU32(cpu, st, kDevOffHwnd)),
              static_cast<unsigned>(stateU32(cpu, st, kDevOffBuffers)),
              static_cast<unsigned>(stateU32(cpu, st, kDevOffSpeaker)));
    }
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && buffer) destroyBufferBlocks(cpu, st);
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// IDirectSound8
// ---------------------------------------------------------------------------------------
void shimSetCooperativeLevel(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t hwnd = call.u32(1);
    const std::uint32_t level = call.u32(2);
    const std::uint32_t st = stateOf(cpu, self);
    setStateU32(cpu, st, kDevOffHwnd, hwnd);
    setStateU32(cpu, st, kDevOffCoop, level);
    // `.lst` 0x004B5DD7..0x004B5DDD：`(*(vtable+0x18))(pDS, hWnd, v10)`，v10 ∈ {1,2,3}
    // = DSSCL_NORMAL(1) / DSSCL_PRIORITY(2) / DSSCL_EXCLUSIVE(3)。
    if (firstTime(cpu, st, kSeenSetCoop, kDevOffSeen)) {
        dsLog("SetCooperativeLevel",
              "hwnd=0x%08X level=%u ⇒ DS_OK（静音设备：只记账，不抢焦点、不改混音器）", hwnd,
              static_cast<unsigned>(level));
    }
    call.result = kDsOk;
}

void shimGetCapsDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t caps = call.u32(1);
    const std::uint32_t st = stateOf(cpu, self);
    // DSCAPS（32 位布局；dwSize 由调用方先填，真机要求 ≥ 96）
    // ★ 静音设备**不冒充**有硬件加速：只报结构性事实（有 primary / stereo），
    //   dwFreeHw* / dwTotalHw* 全 0（没有硬件缓冲），采样率范围按"什么都收"给宽区间。
    constexpr std::uint32_t kDscapsBytes = 96u;
    if (caps == 0 || !cpu.mem->committed(caps, 4)) {
        dsLog("GetCaps", "lpDSCaps 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM(0x%08X)", kEInvalidArg);
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t size = cpu.mem->rd32(caps);
    if (size < kDscapsBytes) {
        dsLog("GetCaps", "DSCAPS.dwSize=%u < %u ⇒ DSERR_INVALIDPARAM（调用方要先填 dwSize）",
              static_cast<unsigned>(size), static_cast<unsigned>(kDscapsBytes));
        call.result = kEInvalidArg;
        return;
    }
    std::vector<std::uint8_t> buf(kDscapsBytes, 0);
    auto put = [&buf](std::uint32_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            buf[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    put(0, kDscapsBytes);  // dwSize
    put(4, 0x00000002u);   // dwFlags = DSCAPS_PRIMARYSTEREO
    put(8, 8000u);         // dwMinSecondarySampleRate
    put(12, 192000u);      // dwMaxSecondarySampleRate
    put(16, 1u);           // dwPrimaryBuffers
    cpu.mem->writeBytes(caps, buf.data(), kDscapsBytes);
    if (firstTime(cpu, st, kSeenGetCaps, kDevOffSeen)) {
        dsLog("GetCaps",
              "⇒ DS_OK：静音设备（primary=%u，硬件缓冲 0，采样率 8000..192000；DSCAPS dwSize=%u）",
              static_cast<unsigned>(stateU32(cpu, st, kDevOffBuffers)), static_cast<unsigned>(size));
    }
    call.result = kDsOk;
}

void shimGetSpeakerConfig(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t out = call.u32(1);
    const std::uint32_t st = stateOf(cpu, self);
    // DSSPEAKER_STEREO(0) | DSSPEAKER_GEOMETRY_WIDE(1<<3 → 0x8)? 保守给 DSSPEAKER_STEREO = 0
    const std::uint32_t cfg = stateU32(cpu, st, kDevOffSpeaker);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        dsLog("GetSpeakerConfig", "pdwSpeakerConfig 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM");
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, cfg);
    if (firstTime(cpu, st, kSeenSpeaker, kDevOffSeen)) {
        dsLog("GetSpeakerConfig", "⇒ DS_OK：0x%08X（DSSPEAKER_STEREO；静音设备不枚举真实声卡）",
              static_cast<unsigned>(cfg));
    }
    call.result = kDsOk;
}

void shimSetSpeakerConfig(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t cfg = call.u32(1);
    const std::uint32_t st = stateOf(cpu, self);
    setStateU32(cpu, st, kDevOffSpeaker, cfg);
    if (firstTime(cpu, st, kSeenSpeaker, kDevOffSeen)) {
        dsLog("SetSpeakerConfig", "0x%08X ⇒ DS_OK（只记账；静音设备没有真正的扬声器几何）",
              static_cast<unsigned>(cfg));
    }
    call.result = kDsOk;
}

// 建一个缓冲对象：状态块 + PCM 数据块 + COM 对象（`obj+8` = 状态块）。
// 返回对象 EA；0 = 失败（已经打过日志）。
std::uint32_t createBufferObject(X86& cpu, std::uint32_t device_obj, std::uint32_t desc_ea,
                                 const char* method, std::uint32_t* out_state) {
    if (out_state) *out_state = 0;
    if (desc_ea == 0 || !cpu.mem->committed(desc_ea, kDsBufferDescBytes)) {
        dsLog(method, "lpDirectSoundBufferDesc=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM",
              desc_ea);
        return 0;
    }
    const std::uint32_t size = cpu.mem->rd32(desc_ea + kDbOffSize);
    const std::uint32_t flags = cpu.mem->rd32(desc_ea + kDbOffFlags);
    const std::uint32_t bytes = cpu.mem->rd32(desc_ea + kDbOffBytes);
    const std::uint32_t fmt_ea = cpu.mem->rd32(desc_ea + kDbOffFormat);
    if (size != kDsBufferDescBytes) {
        dsLog(method, "DSBUFFERDESC.dwSize=%u（期望 %u）—— Win32 要求调用方填对；仍按 36 字节布局读",
              static_cast<unsigned>(size), static_cast<unsigned>(kDsBufferDescBytes));
    }
    const Wfx wfx = readWfx(cpu, fmt_ea);
    if (fmt_ea != 0 && !wfx.present) {
        dsLog(method, "DSBUFFERDESC.lpwfxFormat=0x%08X 未提交 %u 字节（WAVEFORMATEX）⇒ 不读格式",
              fmt_ea, static_cast<unsigned>(kWfxBytes));
    }
    if (!cpu.heap || !cpu.heap->ready() || !cpu.com) {
        dsTrap(cpu, method, "cpu.heap / cpu.com 没装配（guest 堆或 ComRegistry 缺失）");
        return 0;
    }
    // PCM 数据块：真给 guest 内存（`bytes` 字节）。primary buffer 的 dwBufferBytes 可以是 0
    // （真机：primary 的大小由 SetFormat 决定）⇒ 给 1 字节占位，保证 Lock 回一个**可写**指针。
    const std::uint32_t alloc_bytes = bytes != 0 ? bytes : 1u;
    const std::uint32_t data = cpu.heap->alloc(alloc_bytes, /*zero=*/true);
    if (data == 0) {
        dsLog(method, "guest 堆分配 %u 字节 PCM 块失败：%s ⇒ DSERR_INVALIDPARAM",
              static_cast<unsigned>(alloc_bytes), cpu.heap->lastError());
        return 0;
    }
    const std::uint32_t st = cpu.heap->alloc(kBufBytes, /*zero=*/true);
    if (st == 0) {
        cpu.heap->free(data);
        dsLog(method, "guest 堆分配 %u 字节状态块失败：%s ⇒ DSERR_INVALIDPARAM",
              static_cast<unsigned>(kBufBytes), cpu.heap->lastError());
        return 0;
    }
    setStateU32(cpu, st, kBufOffMagic, kBufMagic);
    setStateU32(cpu, st, kBufOffDevice, device_obj);
    setStateU32(cpu, st, kBufOffBytes, bytes);
    setStateU32(cpu, st, kBufOffData, data);
    setStateU32(cpu, st, kBufOffFlags, flags);
    setStateU32(cpu, st, kBufOffFrequency, wfx.present ? wfx.samples : 44100u);
    storeWfx(cpu, st, wfx);

    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirectSoundBuffer8", bufferMethods(), st);
    if (obj == 0) {
        cpu.heap->free(data);
        cpu.heap->free(st);
        dsTrap(cpu, method, "在 guest 内存里建 IDirectSoundBuffer8 对象失败（见上一行 com 日志）");
        return 0;
    }
    if (out_state) *out_state = st;
    // 设备侧计数
    const std::uint32_t dst = stateOf(cpu, device_obj);
    bumpState(cpu, dst, kDevOffBuffers);
    const std::uint32_t nth = stateU32(cpu, dst, kDevOffBuffers);
    const bool detail = nth <= kMaxDetailedBuffers;
    // ★ 每个 buffer 是**一个事件**（各自一份 WAVEFORMATEX，是判据的一部分）⇒ 逐条登记；
    //   超过 32 个之后只记账（避免 SE 一大把音效时刷屏），并在跨过阈值时明确说一次。
    if (detail) {
        dsLog(method,
              "#%u ⇒ DS_OK：对象 0x%08X bytes=%u flags=0x%08X%s %s（数据块 0x%08X，%u 字节）",
              static_cast<unsigned>(nth), obj, static_cast<unsigned>(bytes),
              static_cast<unsigned>(flags), (flags & kCapsPrimary) != 0 ? " [primary]" : "",
              wfxText(wfx).c_str(), data, static_cast<unsigned>(alloc_bytes));
    } else if (nth == kMaxDetailedBuffers + 1u) {
        dsLog(method, "建过 %u 个 buffer：**后续同类调用不再逐条打**（仍然记账，见 Release 时的账本）",
              static_cast<unsigned>(nth));
    }
    return obj;
}

void shimCreateSoundBuffer(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t desc = call.u32(1);
    const std::uint32_t out = call.u32(2);
    const std::uint32_t outer = call.u32(3);
    if (outer != 0) {
        dsLog("CreateSoundBuffer", "pUnkOuter=0x%08X 非 NULL（聚合）⇒ DSERR_NOAGGREGATION", outer);
        call.result = kDsErrNoAggregation;
        return;
    }
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        dsLog("CreateSoundBuffer", "ppDSBuffer=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM", out);
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, 0);
    std::uint32_t st = 0;
    const std::uint32_t obj = createBufferObject(cpu, self, desc, "CreateSoundBuffer", &st);
    if (obj == 0) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, obj);
    call.result = kDsOk;
}

// DuplicateSoundBuffer：⛔ 当前证据里引擎**没有**调它（`.lst` 音频模块只有 +12 / +24 / +8），
// 按"静音设备"给真语义：新对象 + **独立的一份** PCM 块（内容拷自源缓冲）。
void shimDuplicateSoundBuffer(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t src = call.u32(1);
    const std::uint32_t out = call.u32(2);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        dsLog("DuplicateSoundBuffer", "ppDSBDuplicate=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM", out);
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, 0);
    const std::uint32_t src_st = stateOf(cpu, src);
    if (src_st == 0 || stateU32(cpu, src_st, kBufOffMagic) != kBufMagic) {
        dsLog("DuplicateSoundBuffer", "lpDSBuffer=0x%08X 不是本模拟器建的 buffer ⇒ DSERR_INVALIDPARAM", src);
        call.result = kEInvalidArg;
        return;
    }
    if (!cpu.heap || !cpu.com) {
        dsTrap(cpu, "DuplicateSoundBuffer", "cpu.heap / cpu.com 没装配");
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t src_bytes = stateU32(cpu, src_st, kBufOffBytes);
    const std::uint32_t src_data = stateU32(cpu, src_st, kBufOffData);
    const std::uint32_t flags = stateU32(cpu, src_st, kBufOffFlags);
    const Wfx wfx = loadWfx(cpu, src_st);
    const std::uint32_t alloc_bytes = src_bytes != 0 ? src_bytes : 1u;
    const std::uint32_t data = cpu.heap->alloc(alloc_bytes, /*zero=*/true);
    const std::uint32_t st = data != 0 ? cpu.heap->alloc(kBufBytes, /*zero=*/true) : 0;
    if (data == 0 || st == 0) {
        if (data != 0) cpu.heap->free(data);
        dsLog("DuplicateSoundBuffer", "guest 堆分配失败：%s ⇒ DSERR_INVALIDPARAM", cpu.heap->lastError());
        call.result = kEInvalidArg;
        return;
    }
    if (src_data != 0 && cpu.mem->committed(src_data, alloc_bytes)) {
        std::vector<std::uint8_t> copy(alloc_bytes, 0);
        cpu.mem->readBytes(src_data, copy.data(), alloc_bytes);
        cpu.mem->writeBytes(data, copy.data(), alloc_bytes);
    }
    setStateU32(cpu, st, kBufOffMagic, kBufMagic);
    setStateU32(cpu, st, kBufOffDevice, self);
    setStateU32(cpu, st, kBufOffBytes, src_bytes);
    setStateU32(cpu, st, kBufOffData, data);
    setStateU32(cpu, st, kBufOffFlags, flags);
    setStateU32(cpu, st, kBufOffFrequency, stateU32(cpu, src_st, kBufOffFrequency));
    storeWfx(cpu, st, wfx);
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirectSoundBuffer8", bufferMethods(), st);
    if (obj == 0) {
        cpu.heap->free(data);
        cpu.heap->free(st);
        dsTrap(cpu, "DuplicateSoundBuffer", "建 IDirectSoundBuffer8 对象失败（见上一行 com 日志）");
        call.result = kEInvalidArg;
        return;
    }
    bumpState(cpu, stateOf(cpu, self), kDevOffBuffers);
    if (firstTime(cpu, stateOf(cpu, self), kSeenDuplicate, kDevOffSeen)) {
        dsLog("DuplicateSoundBuffer",
              "源 0x%08X ⇒ 新对象 0x%08X（**独立**的一份 %u 字节 PCM 块 0x%08X，内容已拷贝；%s）",
              src, obj, static_cast<unsigned>(alloc_bytes), data, wfxText(wfx).c_str());
    }
    cpu.mem->wr32(out, obj);
    call.result = kDsOk;
}

// ---------------------------------------------------------------------------------------
// IDirectSoundBuffer8
// ---------------------------------------------------------------------------------------
std::uint32_t bufferState(X86& cpu, ShimCall& call, const char* method) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t st = stateOf(cpu, self);
    if (st == 0 || stateU32(cpu, st, kBufOffMagic) != kBufMagic) {
        dsLog(method, "this=0x%08X 不是本模拟器建的 buffer（状态块 0x%08X）⇒ DSERR_INVALIDPARAM", self, st);
        return 0;
    }
    return st;
}

void shimBufGetCaps(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetCaps");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    // DSBCAPS（5 个 DWORD = 20 字节；dwSize 由调用方填）
    constexpr std::uint32_t kDsbcapsBytes = 20u;
    if (out == 0 || !cpu.mem->committed(out, kDsbcapsBytes)) {
        dsLog("GetCaps", "lpDSBCaps=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM", out);
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out + 0, kDsbcapsBytes);
    cpu.mem->wr32(out + 4, stateU32(cpu, st, kBufOffFlags));
    cpu.mem->wr32(out + 8, stateU32(cpu, st, kBufOffBytes));
    cpu.mem->wr32(out + 12, 0u);  // dwUnlockTransferRate（静音设备：没有 DMA 传输率）
    cpu.mem->wr32(out + 16, 0u);  // dwPlayCpuOverhead
    if (firstTime(cpu, st, kSeenGetCaps, kBufOffSeen)) {
        dsLog("GetCaps", "⇒ DS_OK flags=0x%08X bytes=%u（静音设备：传输率 / CPU 开销都报 0）",
              static_cast<unsigned>(stateU32(cpu, st, kBufOffFlags)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffBytes)));
    }
    call.result = kDsOk;
}

void shimBufGetCurrentPosition(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetCurrentPosition");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t play_out = call.u32(1);
    const std::uint32_t write_out = call.u32(2);
    const std::uint32_t cursor = stateU32(cpu, st, kBufOffCursor);
    if (play_out != 0 && cpu.mem->committed(play_out, 4)) cpu.mem->wr32(play_out, cursor);
    if (write_out != 0 && cpu.mem->committed(write_out, 4)) cpu.mem->wr32(write_out, cursor);
    if (firstTime(cpu, st, kSeenGetPos, kBufOffSeen)) {
        dsLog("GetCurrentPosition",
              "⇒ DS_OK play=%u write=%u（静音设备：游标 = 已 Unlock 交给设备的字节数；"
              "⛔ 不产生 position-notify 事件 —— 没有混音线程）",
              static_cast<unsigned>(cursor), static_cast<unsigned>(cursor));
    }
    call.result = kDsOk;
}

void shimBufGetFormat(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetFormat");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    const std::uint32_t written = call.u32(3);
    const Wfx w = loadWfx(cpu, st);
    if (!w.present) {
        dsLog("GetFormat", "这个 buffer 没有 WAVEFORMATEX（primary buffer 在 SetFormat 之前）⇒ DSERR_INVALIDCALL");
        call.result = kDsErrInvalidCall;
        return;
    }
    if (written != 0 && cpu.mem->committed(written, 4)) cpu.mem->wr32(written, kWfxBytes);
    if (out == 0 || cap < kWfxBytes || !cpu.mem->committed(out, kWfxBytes)) {
        dsLog("GetFormat", "缓冲 0x%08X 容量 %u < %u 字节 ⇒ DSERR_INVALIDPARAM（*pdwSizeWritten=%u）", out,
              static_cast<unsigned>(cap), static_cast<unsigned>(kWfxBytes),
              static_cast<unsigned>(kWfxBytes));
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr16(out + 0, static_cast<std::uint16_t>(w.tag));
    cpu.mem->wr16(out + 2, static_cast<std::uint16_t>(w.channels));
    cpu.mem->wr32(out + 4, w.samples);
    cpu.mem->wr32(out + 8, w.avg_bytes);
    cpu.mem->wr16(out + 12, static_cast<std::uint16_t>(w.block_align));
    cpu.mem->wr16(out + 14, static_cast<std::uint16_t>(w.bits));
    cpu.mem->wr16(out + 16, static_cast<std::uint16_t>(w.cb_size));
    if (firstTime(cpu, st, kSeenGetFormat, kBufOffSeen)) {
        dsLog("GetFormat", "⇒ DS_OK %s（**逐字段**照 CreateSoundBuffer 时记录的原值）", wfxText(w).c_str());
    }
    call.result = kDsOk;
}

void shimBufSetFormat(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "SetFormat");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t fmt_ea = call.u32(1);
    const Wfx w = readWfx(cpu, fmt_ea);
    if (!w.present) {
        dsLog("SetFormat", "lpcfxFormat=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM", fmt_ea);
        call.result = kEInvalidArg;
        return;
    }
    storeWfx(cpu, st, w);
    setStateU32(cpu, st, kBufOffFrequency, w.samples);
    if (firstTime(cpu, st, kSeenSetFormat, kBufOffSeen)) {
        dsLog("SetFormat", "⇒ DS_OK %s（如实记录；不改宿主任何东西）", wfxText(w).c_str());
    }
    call.result = kDsOk;
}

void shimBufGetStatus(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetStatus");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    std::uint32_t status = 0;
    if (stateU32(cpu, st, kBufOffPlaying) != 0) {
        status |= kStatusPlaying;
        if (stateU32(cpu, st, kBufOffLooping) != 0) status |= kStatusLooping;
    }
    if (out != 0 && cpu.mem->committed(out, 4)) cpu.mem->wr32(out, status);
    if (firstTime(cpu, st, kSeenStatus, kBufOffSeen)) {
        dsLog("GetStatus", "⇒ DS_OK 0x%08X（PLAYING=%d LOOPING=%d；plays=%u）", status,
              (status & kStatusPlaying) != 0 ? 1 : 0, (status & kStatusLooping) != 0 ? 1 : 0,
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPlays)));
    }
    call.result = kDsOk;
}

void shimBufGetVolume(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetVolume");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, stateU32(cpu, st, kBufOffVolume));
    call.result = kDsOk;
}

void shimBufSetVolume(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "SetVolume");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::int32_t v = call.i32(1);  // 1/100 dB（0 = 满音量，负 = 衰减）
    setStateU32(cpu, st, kBufOffVolume, static_cast<std::uint32_t>(v));
    if (firstTime(cpu, st, kSeenSetVolume, kBufOffSeen)) {
        dsLog("SetVolume", "%d（1/100 dB）⇒ DS_OK（记账；静音设备不改宿主音量）", static_cast<int>(v));
    }
    call.result = kDsOk;
}

void shimBufGetPan(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetPan");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, stateU32(cpu, st, kBufOffPan));
    call.result = kDsOk;
}

void shimBufSetPan(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "SetPan");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::int32_t v = call.i32(1);
    setStateU32(cpu, st, kBufOffPan, static_cast<std::uint32_t>(v));
    if (firstTime(cpu, st, kSeenSetPan, kBufOffSeen)) {
        dsLog("SetPan", "%d（1/100 dB 的左右偏置）⇒ DS_OK（记账）", static_cast<int>(v));
    }
    call.result = kDsOk;
}

void shimBufGetFrequency(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "GetFrequency");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t out = call.u32(1);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, stateU32(cpu, st, kBufOffFrequency));
    call.result = kDsOk;
}

void shimBufSetFrequency(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "SetFrequency");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t hz = call.u32(1);
    setStateU32(cpu, st, kBufOffFrequency, hz);
    if (firstTime(cpu, st, kSeenSetFreq, kBufOffSeen)) {
        dsLog("SetFrequency", "%u Hz ⇒ DS_OK（记账；⛔ 不做重采样：静音设备不产出采样）",
              static_cast<unsigned>(hz));
    }
    call.result = kDsOk;
}

void shimBufLock(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "Lock");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t offset = call.u32(1);
    const std::uint32_t want = call.u32(2);
    const std::uint32_t p1 = call.u32(3);
    const std::uint32_t b1 = call.u32(4);
    const std::uint32_t p2 = call.u32(5);
    const std::uint32_t b2 = call.u32(6);
    const std::uint32_t flags_arg = call.u32(7);
    const std::uint32_t bytes = stateU32(cpu, st, kBufOffBytes);
    const std::uint32_t data = stateU32(cpu, st, kBufOffData);
    if (offset > bytes) {
        dsLog("Lock", "offset=%u 越过缓冲 %u 字节 ⇒ DSERR_INVALIDPARAM", static_cast<unsigned>(offset),
              static_cast<unsigned>(bytes));
        call.result = kEInvalidArg;
        return;
    }
    if (want == 0xFFFFFFFFu) {  // DSBLOCK_ENTIREBUFFER
        // 整块锁：由我们改写 want
        const std::uint32_t whole = bytes;
        const std::uint32_t n1 = whole;
        const std::uint32_t ptr1 = data + offset;
        if (p1 == 0 || b1 == 0 || !cpu.mem->committed(p1, 4) || !cpu.mem->committed(b1, 4)) {
            dsLog("Lock", "DSBLOCK_ENTIREBUFFER：ppvAudioPtr1 / pdwAudioBytes1 为 NULL / 未提交");
            call.result = kEInvalidArg;
            return;
        }
        cpu.mem->wr32(p1, ptr1);
        cpu.mem->wr32(b1, n1);
        if (p2 != 0 && cpu.mem->committed(p2, 4)) cpu.mem->wr32(p2, 0);
        if (b2 != 0 && cpu.mem->committed(b2, 4)) cpu.mem->wr32(b2, 0);
        setStateU32(cpu, st, kBufOffLock1, ptr1);
        setStateU32(cpu, st, kBufOffLock1Bytes, n1);
        setStateU32(cpu, st, kBufOffLock2, 0);
        setStateU32(cpu, st, kBufOffLock2Bytes, 0);
        bumpState(cpu, st, kBufOffLockDepth);
        bumpState(cpu, st, kBufOffLocks);
        if (firstTime(cpu, st, kSeenLock, kBufOffSeen)) {
            dsLog("Lock", "**整块**（DSBLOCK_ENTIREBUFFER）offset=%u ⇒ DS_OK p1=0x%08X b1=%u p2=0 b2=0"
                          "（真 guest 内存：引擎直接往里写 PCM）",
                  static_cast<unsigned>(offset), ptr1, static_cast<unsigned>(n1));
        }
        call.result = kDsOk;
        return;
    }
    if (p1 == 0 || b1 == 0 || !cpu.mem->committed(p1, 4) || !cpu.mem->committed(b1, 4)) {
        dsLog("Lock", "ppvAudioPtr1=0x%08X / pdwAudioBytes1=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM",
              p1, b1);
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t avail = bytes - offset;
    const std::uint32_t n1 = want < avail ? want : avail;
    const std::uint32_t n2 = want - n1;
    if (n2 != 0 && (p2 == 0 || b2 == 0)) {
        dsLog("Lock", "跨回绕 %u 字节，但 ppvAudioPtr2=0x%08X / pdwAudioBytes2=0x%08X 为 NULL ⇒ "
                      "DSERR_INVALIDPARAM",
              static_cast<unsigned>(n2), p2, b2);
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t ptr1 = data + offset;
    cpu.mem->wr32(p1, ptr1);
    cpu.mem->wr32(b1, n1);
    std::uint32_t ptr2 = 0;
    if (n2 != 0) {
        ptr2 = data;
        cpu.mem->wr32(p2, ptr2);
        cpu.mem->wr32(b2, n2);
    } else {
        if (p2 != 0 && cpu.mem->committed(p2, 4)) cpu.mem->wr32(p2, 0);
        if (b2 != 0 && cpu.mem->committed(b2, 4)) cpu.mem->wr32(b2, 0);
    }
    setStateU32(cpu, st, kBufOffLock1, ptr1);
    setStateU32(cpu, st, kBufOffLock1Bytes, n1);
    setStateU32(cpu, st, kBufOffLock2, ptr2);
    setStateU32(cpu, st, kBufOffLock2Bytes, n2);
    bumpState(cpu, st, kBufOffLockDepth);
    bumpState(cpu, st, kBufOffLocks);
    if (firstTime(cpu, st, kSeenLock, kBufOffSeen)) {
        dsLog("Lock",
              "offset=%u bytes=%u flags=0x%08X ⇒ DS_OK p1=0x%08X b1=%u p2=0x%08X b2=%u"
              "（真 guest 内存：引擎直接往里写 PCM；缓冲共 %u 字节）",
              static_cast<unsigned>(offset), static_cast<unsigned>(want),
              static_cast<unsigned>(flags_arg), ptr1, static_cast<unsigned>(n1), ptr2,
              static_cast<unsigned>(n2), static_cast<unsigned>(bytes));
    }
    call.result = kDsOk;
}

void shimBufUnlock(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "Unlock");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t p1 = call.u32(1);
    const std::uint32_t n1 = call.u32(2);
    const std::uint32_t p2 = call.u32(3);
    const std::uint32_t n2 = call.u32(4);
    if (stateU32(cpu, st, kBufOffLockDepth) == 0) {
        dsLog("Unlock", "没有未完成的 Lock ⇒ DSERR_INVALIDCALL(0x%08X)", kDsErrInvalidCall);
        call.result = kDsErrInvalidCall;
        return;
    }
    if (p1 != stateU32(cpu, st, kBufOffLock1) || n1 != stateU32(cpu, st, kBufOffLock1Bytes)) {
        dsLog("Unlock",
              "p1=0x%08X n1=%u 与 Lock 记下的 p1=0x%08X n1=%u 不一致 ⇒ DSERR_INVALIDPARAM"
              "（静音设备仍会记账，但这是调用方的错）",
              p1, static_cast<unsigned>(n1), static_cast<unsigned>(stateU32(cpu, st, kBufOffLock1)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffLock1Bytes)));
        call.result = kEInvalidArg;
        return;
    }
    // ★ 记账：这段窗口就是"引擎交给设备的 PCM"。顺手数一下里面有多少**非 0** 字节 ——
    //   这是"引擎真的写过这块内存"的唯一可用证据（引擎 0x4B5F83 是 memset(0)，所以这里读到 0）。
    std::uint32_t nonzero = 0;
    auto scan = [&](std::uint32_t ptr, std::uint32_t len) {
        if (ptr == 0 || len == 0 || !cpu.mem->committed(ptr, len)) return;
        for (std::uint32_t i = 0; i < len; ++i)
            if (cpu.mem->rd8(ptr + i) != 0) ++nonzero;
    };
    scan(p1, n1);
    scan(p2, n2);
    const std::uint32_t total = n1 + n2;
    bumpState(cpu, st, kBufOffPcmBytes, total);
    bumpState(cpu, st, kBufOffNonZero, nonzero);
    // ★ 静音设备**没有时钟** ⇒ 游标 = "已交给设备的字节数"（pcmBytes）。真机上它随时钟走，
    //   这里给的是可断言、可复现的账本值（⛔ 不编一个假的时间推进）。
    setStateU32(cpu, st, kBufOffCursor, stateU32(cpu, st, kBufOffPcmBytes));
    setStateU32(cpu, st, kBufOffLockDepth, 0);
    setStateU32(cpu, st, kBufOffLock1, 0);
    setStateU32(cpu, st, kBufOffLock1Bytes, 0);
    setStateU32(cpu, st, kBufOffLock2, 0);
    setStateU32(cpu, st, kBufOffLock2Bytes, 0);
    if (firstTime(cpu, st, kSeenUnlock, kBufOffSeen)) {
        dsLog("Unlock",
              "p1=0x%08X n1=%u p2=0x%08X n2=%u ⇒ DS_OK（本区间非 0 字节 %u；**累计 PCM 字节 %u**，"
              "plays=%u）",
              p1, static_cast<unsigned>(n1), p2, static_cast<unsigned>(n2),
              static_cast<unsigned>(nonzero),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPcmBytes)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPlays)));
    }
    call.result = kDsOk;
}

void shimBufPlay(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "Play");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t reserved = call.u32(1);
    const std::uint32_t priority = call.u32(2);
    const std::uint32_t flags_arg = call.u32(3);
    (void)reserved;
    (void)priority;
    setStateU32(cpu, st, kBufOffPlaying, 1u);
    setStateU32(cpu, st, kBufOffLooping, (flags_arg & kPlayLooping) != 0 ? 1u : 0u);
    bumpState(cpu, st, kBufOffPlays);
    if (firstTime(cpu, st, kSeenPlay, kBufOffSeen)) {
        dsLog("Play", "dwFlags=0x%08X（LOOPING=%d）⇒ DS_OK（**不出声**；这是第 %u 次 Play）",
              static_cast<unsigned>(flags_arg), (flags_arg & kPlayLooping) != 0 ? 1 : 0,
              static_cast<unsigned>(stateU32(cpu, st, kBufOffPlays)));
    }
    call.result = kDsOk;
}

void shimBufStop(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "Stop");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    setStateU32(cpu, st, kBufOffPlaying, 0u);
    setStateU32(cpu, st, kBufOffLooping, 0u);
    bumpState(cpu, st, kBufOffStops);
    if (firstTime(cpu, st, kSeenStop, kBufOffSeen)) {
        dsLog("Stop", "⇒ DS_OK（第 %u 次 Stop；游标停在 %u 字节）",
              static_cast<unsigned>(stateU32(cpu, st, kBufOffStops)),
              static_cast<unsigned>(stateU32(cpu, st, kBufOffCursor)));
    }
    call.result = kDsOk;
}

void shimBufSetCurrentPosition(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "SetCurrentPosition");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t pos = call.u32(1);
    const std::uint32_t bytes = stateU32(cpu, st, kBufOffBytes);
    if (pos > bytes) {
        dsLog("SetCurrentPosition", "dwNewPosition=%u 越过缓冲 %u 字节 ⇒ DSERR_INVALIDPARAM",
              static_cast<unsigned>(pos), static_cast<unsigned>(bytes));
        call.result = kEInvalidArg;
        return;
    }
    setStateU32(cpu, st, kBufOffCursor, pos);
    if (firstTime(cpu, st, kSeenSetPos, kBufOffSeen)) {
        dsLog("SetCurrentPosition", "%u ⇒ DS_OK（游标记账；静音设备没有真正的播放位置）",
              static_cast<unsigned>(pos));
    }
    call.result = kDsOk;
}

void shimBufRestore(X86& cpu, ShimCall& call) {
    const std::uint32_t st = bufferState(cpu, call, "Restore");
    if (st == 0) {
        call.result = kEInvalidArg;
        return;
    }
    bumpState(cpu, st, kBufOffRestores);
    if (firstTime(cpu, st, kSeenRestore, kBufOffSeen)) {
        dsLog("Restore", "⇒ DS_OK（静音设备上缓冲从不丢失；第 %u 次 Restore）",
              static_cast<unsigned>(stateU32(cpu, st, kBufOffRestores)));
    }
    call.result = kDsOk;
}

// ---------------------------------------------------------------------------------------
// 动态导出：DirectSoundCreate
// ---------------------------------------------------------------------------------------
// `.lst` 0x004B5C6E..0x004B5C7A：`push 0; push [this+0x408]; push 0; call [this+0x41C]`
//   ⇒ `DirectSoundCreate(NULL, &pDS, NULL)`（3 个参数）。
void shimDirectSoundCreate(X86& cpu, ShimCall& call) {
    const std::uint32_t guid = call.u32(0);
    const std::uint32_t out = call.u32(1);
    const std::uint32_t outer = call.u32(2);
    if (outer != 0) {
        dsLog("DirectSoundCreate", "pUnkOuter=0x%08X 非 NULL（聚合）⇒ DSERR_NOAGGREGATION", outer);
        call.result = kDsErrNoAggregation;
        return;
    }
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        dsLog("DirectSoundCreate", "ppDS=0x%08X 为 NULL / 未提交 ⇒ DSERR_INVALIDPARAM", out);
        call.result = kEInvalidArg;
        return;
    }
    cpu.mem->wr32(out, 0);
    if (guid != 0) {
        // 真机：pcGuidDevice = NULL 表示"默认设备"；具体 GUID 表示点名设备。
        if (!cpu.mem->committed(guid, 16)) {
            dsLog("DirectSoundCreate", "pcGuidDevice=0x%08X 既不是 NULL 也不是可读的 GUID ⇒ 拒绝", guid);
            call.result = kEInvalidArg;
            return;
        }
        dsLog("DirectSoundCreate",
              "pcGuidDevice=0x%08X 指向一个具体设备 GUID：本模拟器只有一台虚拟（静音）设备 ⇒ 按默认设备处理",
              guid);
    }
    if (!cpu.heap || !cpu.heap->ready() || !cpu.com) {
        dsTrap(cpu, "DirectSoundCreate", "cpu.heap / cpu.com 没装配（guest 堆或 ComRegistry 缺失）");
        call.result = kEInvalidArg;
        return;
    }
    const std::uint32_t st = cpu.heap->alloc(kDevBytes, /*zero=*/true);
    if (st == 0) {
        dsLog("DirectSoundCreate", "guest 堆分配设备状态块失败：%s ⇒ DSERR_INVALIDPARAM", cpu.heap->lastError());
        call.result = kEInvalidArg;
        return;
    }
    setStateU32(cpu, st, kDevOffMagic, kDevMagic);
    setStateU32(cpu, st, kDevOffSpeaker, 0u);
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirectSound8", deviceMethods(), st);
    if (obj == 0) {
        cpu.heap->free(st);
        dsTrap(cpu, "DirectSoundCreate", "建 IDirectSound8 对象失败（见上一行 com 日志）");
        call.result = kEInvalidArg;
        return;
    }
    bumpState(cpu, st, kDevOffCreates);
    cpu.mem->wr32(out, obj);
    dsLog("DirectSoundCreate",
          "pcGuidDevice=0x%08X ⇒ DS_OK(0) + IDirectSound8 对象 0x%08X（状态块 0x%08X）"
          "（SCOPE §1 silent：不出声、也不报错；每个方法首次调用打一行，账本见 Release）",
          guid, obj, st);
    call.result = kDsOk;
}

// ---------------------------------------------------------------------------------------
// 方法表
//   偏移**逐条**对过 SDK dsound.h 的接口声明顺序；`.lst` 里引擎真调到的用 ★ 标注（EA = 调用点）。
//   IDirectSound8（IUnknown 3 + IDirectSound 7 + VerifyCertification）
//     +0   QueryInterface       （引擎**没调**：整份 `.lst` 没有 IID_IDirectSound* 常量）
//     +4   AddRef
//     +8   Release              ★ 0x004B649B / 0x004B64A5（清理路径 sub_4B6440）
//     +12  CreateSoundBuffer    ★ 0x004B5E61（primary，dwSize=36 flags=0xC1）
//                               ★ 0x004B5F19（流式，flags=0x181C8 bytes=176400 + WAVEFORMATEX）
//     +16  GetCaps              （引擎**没调**；按静音设备给真值）
//     +20  DuplicateSoundBuffer （引擎**没调**；给"独立拷贝"的真语义）
//     +24  SetCooperativeLevel  ★ 0x004B5DDD（level ∈ {1,2,3}）
//     +28  GetSpeakerConfig     （引擎**没调**）
//     +32  SetSpeakerConfig     （引擎**没调**）
//     +36  Initialize           → **没实现槽**（loud trap；真机对已初始化的对象返回 DSERR_ALREADYINITIALIZED）
//     +40  VerifyCertification  → **没实现槽**（loud trap；只有 IDirectSound8 有这一槽，占位撑满 vtable）
//   IDirectSoundBuffer8（IUnknown 3 + IDirectSoundBuffer 17 + 3）
//     +0/4/8  IUnknown
//     +12  GetCaps              （引擎**没调**）
//     +16  GetCurrentPosition   （引擎**没调**——它要混音线程推进游标，而线程是硬错误 ⇒ 见交付说明）
//     +20  GetFormat / +24 GetVolume / +28 GetPan / +32 GetFrequency / +36 GetStatus
//     +40  Initialize           → **没实现槽**（loud trap）
//     +44  Lock                 ★ 0x004B5F77（7 个参数；primary 与流式缓冲各一次）
//     +48  Play                 ★ 0x004B5FA3 / 0x004B5FB3（3 个参数，dwFlags=1 = DSBPLAY_LOOPING）
//     +52  SetCurrentPosition   （引擎**没调**）
//     +56  SetFormat / +60 SetVolume / +64 SetPan / +68 SetFrequency
//     +72  Stop                 ★ 0x004B6B7F（清理路径）· +76 Unlock ★ 0x004B5FA3 前一步 0x004B5FA0
//     +80  Restore              （引擎**没调**；`dsbWriteStream` 0x004B6C30 会读它的返回值分支）
//     +84  SetFX / +88 AcquireResources / +92 GetObjectInPath → **没实现槽**（loud trap，撑满 vtable）
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& deviceMethods() {
    static const std::vector<ComMethod> methods = {
        {0u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceDevice},
        {4u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {8u, "Release", Conv::Stdcall, 0, &shimRelease},
        {12u, "CreateSoundBuffer", Conv::Stdcall, 3, &shimCreateSoundBuffer},
        {16u, "GetCaps", Conv::Stdcall, 1, &shimGetCapsDevice},
        {20u, "DuplicateSoundBuffer", Conv::Stdcall, 2, &shimDuplicateSoundBuffer},
        {24u, "SetCooperativeLevel", Conv::Stdcall, 2, &shimSetCooperativeLevel},
        {28u, "GetSpeakerConfig", Conv::Stdcall, 1, &shimGetSpeakerConfig},
        {32u, "SetSpeakerConfig", Conv::Stdcall, 1, &shimSetSpeakerConfig},
        {36u, "Initialize", Conv::Stdcall, 1, nullptr},
        {40u, "VerifyCertification", Conv::Stdcall, 1, nullptr},
    };
    return methods;
}

const std::vector<ComMethod>& notifyMethods() {
    static const std::vector<ComMethod> methods = {
        {0u, "QueryInterface", Conv::Stdcall, 2, &shimNotifyQueryInterface},
        {4u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {8u, "Release", Conv::Stdcall, 0, &shimRelease},
        {12u, "SetNotificationPositions", Conv::Stdcall, 2, &shimNotifySetNotificationPositions},
    };
    return methods;
}

const std::vector<ComMethod>& bufferMethods() {
    static const std::vector<ComMethod> methods = {
        {0u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceBuffer},
        {4u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {8u, "Release", Conv::Stdcall, 0, &shimRelease},
        {12u, "GetCaps", Conv::Stdcall, 1, &shimBufGetCaps},
        {16u, "GetCurrentPosition", Conv::Stdcall, 2, &shimBufGetCurrentPosition},
        {20u, "GetFormat", Conv::Stdcall, 3, &shimBufGetFormat},
        {24u, "GetVolume", Conv::Stdcall, 1, &shimBufGetVolume},
        {28u, "GetPan", Conv::Stdcall, 1, &shimBufGetPan},
        {32u, "GetFrequency", Conv::Stdcall, 1, &shimBufGetFrequency},
        {36u, "GetStatus", Conv::Stdcall, 1, &shimBufGetStatus},
        {40u, "Initialize", Conv::Stdcall, 1, nullptr},
        {44u, "Lock", Conv::Stdcall, 7, &shimBufLock},
        {48u, "Play", Conv::Stdcall, 3, &shimBufPlay},
        {52u, "SetCurrentPosition", Conv::Stdcall, 1, &shimBufSetCurrentPosition},
        {56u, "SetFormat", Conv::Stdcall, 1, &shimBufSetFormat},
        {60u, "SetVolume", Conv::Stdcall, 1, &shimBufSetVolume},
        {64u, "SetPan", Conv::Stdcall, 1, &shimBufSetPan},
        {68u, "SetFrequency", Conv::Stdcall, 1, &shimBufSetFrequency},
        {72u, "Stop", Conv::Stdcall, 0, &shimBufStop},
        {76u, "Unlock", Conv::Stdcall, 4, &shimBufUnlock},
        {80u, "Restore", Conv::Stdcall, 0, &shimBufRestore},
        {84u, "SetFX", Conv::Stdcall, 2, nullptr},
        {88u, "AcquireResources", Conv::Stdcall, 1, nullptr},
        {92u, "GetObjectInPath", Conv::Stdcall, 5, nullptr},
    };
    return methods;
}

}  // namespace

void registerDsoundShims(ShimRegistry& shims) {
    // ★ 动态导出（不是 PE 导入）：`LoadLibraryA("DSOUND.DLL")` → `GetProcAddress("DirectSoundCreate")`
    //   从 shim 注册表查到它（与 ddraw 的 DirectDrawCreate 同一条 G1 转发路径）。
    //   ⛔ 只登记引擎**真的取**的这一个名字（`.lst` 0x004B5C9F）；其余 DirectSound* 导出 0 次出现。
    shims.add("dsound", "DirectSoundCreate", Conv::Stdcall, 3, &shimDirectSoundCreate);
}

}  // namespace lift
