// emulator-lift · winmm 音频 / 多媒体 IO（SCOPE §1：延后实现 ⇒ 现在整体 no-op）
//
// ★ 用户口径（2026-11-04）：音频**后续要真做**，但当前按 no-op 实施；不是硬 trap。
//   判据：
//     · MCI（`mciSendCommandA` / `mciSendStringA` / `mciGetErrorStringA`）→ 接受命令、返回成功，
//       `mciSendStringA` 的返回串写成空串；
//     · mixer（`mixerOpen` / `mixerClose` / `mixerSetControlDetails`）→ 成功 + 非空假句柄；
//     · mmio（`mmioOpenA` / `mmioRead` / `mmioClose` / `mmioAscend` / `mmioDescend`）→ **不是 no-op**：
//       它沿 guest 文件句柄的同一个 `FILE*` 解析 RIFF / fmt / data（`sub_4BBEF0` 的 WAV 头）；
//       没有有效 `lpmmioinfo.adwInfo[0]` 时返回真实失败（NULL / 0 字节 / 265）。
// ★ 每一条都打一行 `[shim]`（SCOPE §3：⛔ 不许静默 no-op），登记为 `addStub`。
// ★ 摇杆（`joy*`）不属于音频，仍留在派生缺口里（M2 输入）。
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "runtime/memory.h"
#include "runtime/x86.h"
#include "shims/kernel32_file.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kFakeMixer = 0x00030001u;
constexpr std::uint32_t kFakeMmio = 0x00040001u;

void shimMciSendCommandA(X86&, ShimCall& call) {
    shimLog("winmm", "mciSendCommandA", "音频 no-op ⇒ 返回成功（SCOPE §1；不打开设备、不播放）");
    call.result = 0;
}

void shimMciSendStringA(X86& cpu, ShimCall& call) {
    // 参数：lpstrCommand / lpstrReturnString / uReturnLength / hwndCallback
    const std::uint32_t ret = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    if (ret && cap) cpu.mem->wr8(ret, 0);
    shimLog("winmm", "mciSendStringA", "音频 no-op ⇒ 返回成功、返回串为空（SCOPE §1）");
    call.result = 0;
}

void shimMciGetErrorStringA(X86&, ShimCall& call) {
    shimLog("winmm", "mciGetErrorStringA", "音频 no-op ⇒ 没有错误串可给（返回 FALSE；SCOPE §1）");
    call.result = 0;
}

void shimMixerOpen(X86& cpu, ShimCall& call) {
    // 参数：phmx / uDeviceID / dwCallback / dwInstance / fdwOpen
    const std::uint32_t out = call.u32(0);
    if (out) cpu.mem->wr32(out, kFakeMixer);
    shimLog("winmm", "mixerOpen", "音频 no-op ⇒ 成功 + 假句柄 0x%08X（SCOPE §1）", kFakeMixer);
    call.result = 0;
}

void shimMixerClose(X86&, ShimCall& call) {
    shimLog("winmm", "mixerClose", "音频 no-op ⇒ 返回成功（SCOPE §1）");
    call.result = 0;
}

void shimMixerSetControlDetails(X86&, ShimCall& call) {
    shimLog("winmm", "mixerSetControlDetails", "音频 no-op ⇒ 接受音量 / 控制设置（SCOPE §1；不作用于宿主）");
    call.result = 0;
}

// ---------------------------------------------------------------------------------------
// mmio：WAV 头解析（`sub_4BBEF0`）用的**真** RIFF 读取
//
// ★ 为什么不能再 no-op：`sub_4BBEF0` 用 `mmioOpenA(0, &pmmioinfo, 0)` 接管
//   `pmmioinfo.adwInfo[0]` 里的 **guest 文件句柄**，然后 `mmioDescend` 找 RIFF/fmt/data、
//   `mmioRead` 读头；原来的 no-op 返回 0 字节 ⇒ 头读取失败 ⇒ `sub_4B4F60` 抛
//   `Command_ShowMessage_Exception`（WAVファイル … 読み込みに失敗）⇒ `RaiseException`。
//   这里沿 guest 句柄的同一个 `FILE*` 读（见 `shims/kernel32_file.h`），不是另开文件。
// ★ `mmioClose` **不关闭** `FILE*`：关闭点是 guest 的 `CloseHandle`。
// ★ 只支持 `MMIO_FINDRIFF` / `MMIO_FINDLIST` / `MMIO_FINDCHUNK` 与父 chunk 内扫描；
//   其余 flags / 压缩流返回 `MMIOERR_CHUNKNOTFOUND`（真实失败，⛔ 不假装成功）。
constexpr std::uint32_t kMmioErrChunkNotFound = 265u;
constexpr std::uint32_t kMmioFindChunk = 0x10u;
constexpr std::uint32_t kMmioFindRiff = 0x20u;
constexpr std::uint32_t kMmioFindList = 0x40u;
constexpr std::uint32_t kFourccRiff = 0x46464952u;  // 'RIFF'
constexpr std::uint32_t kFourccList = 0x5453494Cu;  // 'LIST'

struct MmioState {
    std::FILE* fp = nullptr;
    std::uint32_t guest_handle = 0;
};

std::unordered_map<std::uint32_t, MmioState>& mmioMap() {
    static std::unordered_map<std::uint32_t, MmioState> m;
    return m;
}
std::uint32_t g_next_mmio = 0x00040001u;

bool readU32At(X86& cpu, std::uint32_t ea, std::uint32_t* out) {
    if (!ea || !cpu.mem->committed(ea, 4)) return false;
    *out = cpu.mem->rd32(ea);
    return true;
}
bool writeU32At(X86& cpu, std::uint32_t ea, std::uint32_t v) {
    if (!ea || !cpu.mem->committed(ea, 4)) return false;
    cpu.mem->wr32(ea, v);
    return true;
}

// 读 chunk 头：8 字节；RIFF/LIST 再多 4 字节 form type。
bool readChunkHeader(std::FILE* fp, std::uint32_t* ckid, std::uint32_t* cksize, std::uint32_t* form) {
    unsigned char b[12] = {};
    const std::size_t n = std::fread(b, 1, 8, fp);
    if (n != 8) return false;
    *ckid = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
            (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    *cksize = static_cast<std::uint32_t>(b[4]) | (static_cast<std::uint32_t>(b[5]) << 8) |
              (static_cast<std::uint32_t>(b[6]) << 16) | (static_cast<std::uint32_t>(b[7]) << 24);
    if (form) {
        if (std::fread(b, 1, 4, fp) != 4) return false;
        *form = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
    }
    return true;
}

void shimMmioOpenA(X86& cpu, ShimCall& call) {
    // 参数：szFileName / lpmmioinfo / fdwOpen
    const std::uint32_t info = call.u32(1);
    std::uint32_t guest_handle = 0;
    if (info && cpu.mem->committed(info, 76)) {
        guest_handle = cpu.mem->rd32(info + 48);  // MMIOINFO.adwInfo[0]
    }
    std::FILE* fp = guest_handle ? liftFileStream(cpu, guest_handle) : nullptr;
    if (!fp) {
        shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioOpenA",
                "lpmmioinfo.adwInfo[0]=0x%08X 不是有效 guest 文件句柄 ⇒ 返回 NULL（真实失败）",
                guest_handle);
        call.result = 0;
        return;
    }
    const std::uint32_t h = g_next_mmio;
    g_next_mmio += 4u;
    mmioMap()[h] = MmioState{fp, guest_handle};
    shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioOpenA",
            "guest 文件句柄 0x%08X ⇒ HMMIO 0x%08X（沿同一个 FILE* 读 RIFF；mmioClose 不关宿主文件）",
            guest_handle, h);
    call.result = h;
}

void shimMmioRead(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    const std::uint32_t dst = call.u32(1);
    const std::uint32_t n = call.u32(2);
    const auto it = mmioMap().find(h);
    if (it == mmioMap().end() || !it->second.fp) {
        call.result = 0;
        return;
    }
    if (n == 0) {
        call.result = 0;
        return;
    }
    if (!dst || !cpu.mem->committed(dst, n)) {
        shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioRead", "目标缓冲区 0x%08X/%u 未提交 ⇒ 返回 0", dst, n);
        call.result = 0;
        return;
    }
    std::vector<unsigned char> buf(n);
    const std::size_t got = std::fread(buf.data(), 1, n, it->second.fp);
    if (got) cpu.mem->writeBytes(dst, buf.data(), got);
    shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioRead", "hmmio=0x%08X 请求 %u 字节 ⇒ %u 字节", h, n, static_cast<unsigned>(got));
    call.result = static_cast<std::uint32_t>(got);
}

void shimMmioDescend(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    const std::uint32_t lpck = call.u32(1);
    const std::uint32_t parent = call.u32(2);
    const std::uint32_t flags = call.u32(3);
    const auto it = mmioMap().find(h);
    if (it == mmioMap().end() || !it->second.fp) {
        call.result = kMmioErrChunkNotFound;
        return;
    }
    std::FILE* fp = it->second.fp;
    if (!lpck || !cpu.mem->committed(lpck, 20)) {
        call.result = kMmioErrChunkNotFound;
        return;
    }
    std::uint32_t want_ckid = 0;
    std::uint32_t want_type = 0;
    readU32At(cpu, lpck + 0, &want_ckid);
    readU32At(cpu, lpck + 8, &want_type);

    long pos = std::ftell(fp);
    if (pos < 0) {
        call.result = kMmioErrChunkNotFound;
        return;
    }
    long limit = -1;
    if (parent) {
        std::uint32_t p_data = 0, p_size = 0;
        if (readU32At(cpu, parent + 12, &p_data) && readU32At(cpu, parent + 4, &p_size)) {
            limit = static_cast<long>(p_data) + static_cast<long>(p_size);
        }
    }

    std::uint32_t ckid = 0, cksize = 0, form = 0;
    std::uint32_t data_off = 0, data_size = 0;
    bool found = false;
    if (flags & kMmioFindRiff) {
        if (readChunkHeader(fp, &ckid, &cksize, &form) && ckid == kFourccRiff &&
            (want_type == 0 || form == want_type)) {
            data_off = static_cast<std::uint32_t>(pos) + 12u;
            data_size = cksize >= 4 ? cksize - 4u : 0u;
            found = true;
        }
    } else if (flags & kMmioFindList) {
        if (readChunkHeader(fp, &ckid, &cksize, &form) && ckid == kFourccList &&
            (want_type == 0 || form == want_type)) {
            data_off = static_cast<std::uint32_t>(pos) + 12u;
            data_size = cksize >= 4 ? cksize - 4u : 0u;
            found = true;
        }
    } else if (flags & kMmioFindChunk) {
        while (true) {
            if (limit >= 0 && pos + 8 > limit) break;
            std::fseek(fp, pos, SEEK_SET);
            if (!readChunkHeader(fp, &ckid, &cksize, nullptr)) break;
            if (want_ckid == 0 || ckid == want_ckid) {
                data_off = static_cast<std::uint32_t>(pos) + 8u;
                data_size = cksize;
                found = true;
                break;
            }
            pos += 8 + static_cast<long>(cksize) + static_cast<long>(cksize & 1u);
        }
    }

    if (!found) {
        shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioDescend", "hmmio=0x%08X flags=0x%08X 未找到 chunk（want ckid=0x%08X type=0x%08X）⇒ 265",
                h, flags, want_ckid, want_type);
        call.result = kMmioErrChunkNotFound;
        return;
    }
    shimLogAt(LogLevel::Debug, LogWinmm, "winmm", "mmioDescend", "hmmio=0x%08X flags=0x%08X ⇒ ckid=0x%08X size=%u dataOff=%u",
            h, flags, ckid, data_size, data_off);
    writeU32At(cpu, lpck + 0, ckid);
    writeU32At(cpu, lpck + 4, data_size);
    if (flags & (kMmioFindRiff | kMmioFindList)) writeU32At(cpu, lpck + 8, form);
    writeU32At(cpu, lpck + 12, data_off);
    writeU32At(cpu, lpck + 16, 0);
    std::fseek(fp, static_cast<long>(data_off), SEEK_SET);
    call.result = 0;
}

void shimMmioAscend(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    const std::uint32_t lpck = call.u32(1);
    const auto it = mmioMap().find(h);
    if (it == mmioMap().end() || !it->second.fp || !lpck || !cpu.mem->committed(lpck, 20)) {
        call.result = 0;
        return;
    }
    std::uint32_t data_off = 0, size = 0;
    if (readU32At(cpu, lpck + 12, &data_off) && readU32At(cpu, lpck + 4, &size)) {
        std::fseek(it->second.fp, static_cast<long>(data_off + size + (size & 1u)), SEEK_SET);
    }
    call.result = 0;
}

void shimMmioClose(X86&, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    mmioMap().erase(h);
    call.result = 0;
}

}  // namespace

void registerWinmmAudioShims(ShimRegistry& s) {
    s.addStub("winmm", "mciSendCommandA", Conv::Stdcall, 4, &shimMciSendCommandA,
              "SCOPE §1 音频 no-op：mciSendCommandA 接受命令并返回成功（不打开设备、不播放）");
    s.addStub("winmm", "mciSendStringA", Conv::Stdcall, 4, &shimMciSendStringA,
              "SCOPE §1 音频 no-op：mciSendStringA 接受命令并返回成功、返回串为空");
    s.addStub("winmm", "mciGetErrorStringA", Conv::Stdcall, 3, &shimMciGetErrorStringA,
              "SCOPE §1 音频 no-op：mciGetErrorStringA 返回 FALSE（没有错误串）");
    s.addStub("winmm", "mixerOpen", Conv::Stdcall, 5, &shimMixerOpen,
              "SCOPE §1 音频 no-op：mixerOpen 返回成功 + 非空假句柄");
    s.addStub("winmm", "mixerClose", Conv::Stdcall, 1, &shimMixerClose,
              "SCOPE §1 音频 no-op：mixerClose 返回成功");
    s.addStub("winmm", "mixerSetControlDetails", Conv::Stdcall, 3, &shimMixerSetControlDetails,
              "SCOPE §1 音频 no-op：mixerSetControlDetails 接受音量 / 控制设置但不作用于宿主");
    s.addStub("winmm", "mmioOpenA", Conv::Stdcall, 3, &shimMmioOpenA,
              "SCOPE §1 音频 no-op：mmioOpenA 返回非空假 HMMIO（空流）");
    s.addStub("winmm", "mmioRead", Conv::Stdcall, 3, &shimMmioRead,
              "SCOPE §1 音频 no-op：mmioRead 返回 0 字节（空流）");
    s.addStub("winmm", "mmioClose", Conv::Stdcall, 2, &shimMmioClose,
              "SCOPE §1 音频 no-op：mmioClose 返回成功");
    s.addStub("winmm", "mmioAscend", Conv::Stdcall, 3, &shimMmioAscend,
              "SCOPE §1 音频 no-op：mmioAscend 返回成功");
    s.addStub("winmm", "mmioDescend", Conv::Stdcall, 4, &shimMmioDescend,
              "SCOPE §1 音频 no-op：mmioDescend 返回成功（空流没有 chunk）");
}

}  // namespace lift
