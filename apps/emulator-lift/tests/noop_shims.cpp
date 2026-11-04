// emulator-lift · 明确 no-op 的三族（注册表 / 音频 / 视频；pure：guest 侧自造，不碰游戏）
//
// 判据（SCOPE §1，用户 2026-11-04 裁决）：
//   ① 注册表：open/create 成功 + 假句柄；query 恒 ERROR_FILE_NOT_FOUND(2) 并清零 type/size；
//      set 成功；enum 恒 ERROR_NO_MORE_ITEMS(259)；close 成功。⛔ 不是硬 trap。
//   ② 音频：MCI 接受命令并返回成功、返回串为空；mixer 成功 + 假句柄；mmio 假句柄 + 空流 (0 字节)。
//   ③ 视频 / 媒体：MFStartup / MFShutdown 成功；所有 MFCreate* / MFGetService / DXVA2 返回
//      E_FAIL(0x80004005) 并把输出指针置 0（⛔ 不返回假 COM 对象）。
// ★ 走真实 shim 派发（与 codepage / shims-core 同形）。
#include <cstdint>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Fixture;

constexpr std::uint32_t kFakeKey = 0x00020001u;
constexpr std::uint32_t kFakeMixer = 0x00030001u;
constexpr std::uint32_t kFakeMmio = 0x00040001u;
constexpr std::uint32_t kEfail = 0x80004005u;

}  // namespace

int main() {
    using namespace lift_test;

    Fixture f;
    lift::registerKernel32Shims(f.m.shims());
    LIFT_CHECK(f.ok);

    // ① 注册表
    {
        g_current_case = "registry-noop";
        const std::uint32_t open = f.use("advapi32", "RegOpenKeyExA");
        const std::uint32_t query = f.use("advapi32", "RegQueryValueExA");
        const std::uint32_t set = f.use("advapi32", "RegSetValueExA");
        const std::uint32_t create = f.use("advapi32", "RegCreateKeyExA");
        const std::uint32_t enumerate = f.use("advapi32", "RegEnumValueA");
        const std::uint32_t close = f.use("advapi32", "RegCloseKey");
        LIFT_CHECK(f.ok);

        const std::uint32_t key = f.cstr("Software\\AGE");
        const std::uint32_t value = f.cstr("Setting");
        const std::uint32_t out = f.zeroData(4);
        LIFT_CHECK_EQ_U32(f.call(open, {0x80000002u, key, 0u, 0x20019u, out}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), kFakeKey);

        const std::uint32_t type = f.zeroData(4);
        const std::uint32_t size = f.zeroData(4);
        const std::uint32_t data = f.zeroData(16);
        LIFT_CHECK_EQ_U32(f.call(query, {kFakeKey, value, 0u, type, data, size}), 2u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(type), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(size), 0u);

        const std::uint32_t out2 = f.zeroData(4);
        const std::uint32_t disp = f.zeroData(4);
        LIFT_CHECK_EQ_U32(f.call(create, {0x80000002u, key, 0u, 0u, 0u, 0x20019u, 0u, out2, disp}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out2), kFakeKey);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(disp), 1u);  // REG_CREATED_NEW_KEY

        LIFT_CHECK_EQ_U32(f.call(set, {kFakeKey, value, 0u, 1u, data, 4u}), 0u);

        const std::uint32_t name2 = f.zeroData(4);
        const std::uint32_t cap = f.u32data(4u);
        const std::uint32_t type2 = f.zeroData(4);
        const std::uint32_t size2 = f.zeroData(4);
        LIFT_CHECK_EQ_U32(f.call(enumerate, {kFakeKey, 0u, name2, cap, 0u, type2, data, size2}), 259u);
        LIFT_CHECK_EQ_U32(f.call(close, {kFakeKey}), 0u);
    }

    // ② 音频
    {
        g_current_case = "audio-noop";
        const std::uint32_t mci_cmd = f.use("winmm", "mciSendCommandA");
        const std::uint32_t mci_str = f.use("winmm", "mciSendStringA");
        const std::uint32_t mci_err = f.use("winmm", "mciGetErrorStringA");
        const std::uint32_t mix_open = f.use("winmm", "mixerOpen");
        const std::uint32_t mix_close = f.use("winmm", "mixerClose");
        const std::uint32_t mix_set = f.use("winmm", "mixerSetControlDetails");
        const std::uint32_t mmio_open = f.use("winmm", "mmioOpenA");
        const std::uint32_t mmio_read = f.use("winmm", "mmioRead");
        const std::uint32_t mmio_close = f.use("winmm", "mmioClose");
        const std::uint32_t mmio_asc = f.use("winmm", "mmioAscend");
        const std::uint32_t mmio_desc = f.use("winmm", "mmioDescend");
        LIFT_CHECK(f.ok);

        const std::uint32_t cmd = f.cstr("play track1");
        const std::uint32_t ret = f.zeroData(8);
        LIFT_CHECK_EQ_U32(f.call(mci_cmd, {1u, 0x0806u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.call(mci_str, {cmd, ret, 8u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(ret), 0u);
        LIFT_CHECK_EQ_U32(f.call(mci_err, {0u, ret, 8u}), 0u);

        const std::uint32_t hmix = f.zeroData(4);
        LIFT_CHECK_EQ_U32(f.call(mix_open, {hmix, 0u, 0u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(hmix), kFakeMixer);
        LIFT_CHECK_EQ_U32(f.call(mix_close, {kFakeMixer}), 0u);
        LIFT_CHECK_EQ_U32(f.call(mix_set, {kFakeMixer, 0u, 0u}), 0u);

        const std::uint32_t name = f.cstr("track1.wav");
        LIFT_CHECK_EQ_U32(f.call(mmio_open, {name, 0u, 0u}), kFakeMmio);
        const std::uint32_t buf = f.zeroData(16);
        LIFT_CHECK_EQ_U32(f.call(mmio_read, {kFakeMmio, buf, 16u}), 0u);
        LIFT_CHECK_EQ_U32(f.call(mmio_close, {kFakeMmio, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.call(mmio_asc, {kFakeMmio, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.call(mmio_desc, {kFakeMmio, 0u, 0u, 0u}), 0u);
    }

    // ②' 摇杆：永久不做（SCOPE §1）
    {
        g_current_case = "joystick-noop";
        const std::uint32_t num = f.use("winmm", "joyGetNumDevs");
        const std::uint32_t caps = f.use("winmm", "joyGetDevCapsA");
        const std::uint32_t pos = f.use("winmm", "joyGetPosEx");
        LIFT_CHECK(f.ok);
        LIFT_CHECK_EQ_U32(f.call(num, {}), 0u);
        LIFT_CHECK_EQ_U32(f.call(caps, {0u, 0u, 0u}), 6u);   // MMSYSERR_NODRIVER
        LIFT_CHECK_EQ_U32(f.call(pos, {0u, 0u}), 167u);      // JOYERR_UNPLUGGED
    }

    // ③ 视频 / 媒体
    {
        g_current_case = "video-noop";
        const std::uint32_t startup = f.use("mfplat", "MFStartup");
        const std::uint32_t shutdown = f.use("mfplat", "MFShutdown");
        const std::uint32_t attrs = f.use("mfplat", "MFCreateAttributes");
        const std::uint32_t media_type = f.use("mfplat", "MFCreateMediaType");
        const std::uint32_t session = f.use("mf", "MFCreateMediaSession");
        const std::uint32_t resolver = f.use("mf", "MFCreateSourceResolver");
        const std::uint32_t topology = f.use("mf", "MFCreateTopology");
        const std::uint32_t node = f.use("mf", "MFCreateTopologyNode");
        const std::uint32_t service = f.use("mf", "MFGetService");
        const std::uint32_t sample = f.use("evr", "MFCreateVideoSampleFromSurface");
        const std::uint32_t dxva = f.use("dxva2", "DXVA2CreateDirect3DDeviceManager9");
        LIFT_CHECK(f.ok);

        LIFT_CHECK_EQ_U32(f.call(startup, {0x00020070u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.call(shutdown, {}), 0u);

        const std::uint32_t out = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(attrs, {out, 1u}), kEfail);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);
        LIFT_CHECK_EQ_U32(f.call(media_type, {out}), kEfail);
        LIFT_CHECK_EQ_U32(f.call(session, {0u, out}), kEfail);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);
        LIFT_CHECK_EQ_U32(f.call(resolver, {out}), kEfail);
        LIFT_CHECK_EQ_U32(f.call(topology, {out}), kEfail);
        LIFT_CHECK_EQ_U32(f.call(node, {0u, out}), kEfail);
        LIFT_CHECK_EQ_U32(f.call(service, {0u, 0u, 0u, out}), kEfail);
        LIFT_CHECK_EQ_U32(f.call(sample, {0u, out}), kEfail);
        const std::uint32_t token = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(dxva, {token, out}), kEfail);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(token), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(out), 0u);
    }

    return finish("noop-shims");
}
