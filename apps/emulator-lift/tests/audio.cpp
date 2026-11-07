// emulator-lift · 音频设备模型 / 逐缓冲格式 / 通知次序（pure 档）
//
// ★ 判据（方案 D 的 D1/D2 机械部分）：
//   ① 48k 立体声缓冲：每 0.5s 恰好投递一个通知，次序 0→1→2→3→回绕 0；
//   ② 输出 48k/16/2；设备游标 %bytes；dump 的前 0.5s 与 guest 环的斜坡**逐字节**相等；
//   ③ 44.1k **单声道**缓冲：正确重采样到 48k，左右声道相同（引擎真的会建 mono 流）；
//   ④ 喂料线程身份：register/resume/quit/join 的状态机（不跑 guest 线程体）；
//   ⑤ Stop 后游标冻结、SetCurrentPosition 重排、Release 清掉 pending。
//
// ★ 会红：把 `fireCrossings` 的 sort 去掉 ⇒ ① 的回绕 0 会排在 3 之前；
//   把 `generateTo` 的声道/采样率转换去掉 ⇒ ③ 红；把 removeBuffer 的 pending 清理去掉 ⇒ ⑤ 红。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"

namespace {

constexpr std::uint32_t kOutBytesPerSec = 48000u * 4u;

bool initMachine(lift::Machine& m, std::string* err) {
    if (!m.initBare(err)) return false;
    if (!m.prepareStack(err)) return false;
    return true;
}

void testDeviceModel() {
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    const std::string dump = "lift-audio-dump-test.wav";
    std::remove(dump.c_str());
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, &err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    if (!a.begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.mode()), static_cast<std::uint32_t>(lift::AudioMode::Virtual));

    const std::uint32_t data_ea = 0x00440000u;
    const std::uint32_t state_ea = 0x00430000u;
    const std::uint32_t obj_ea = 0x00450000u;
    const std::uint32_t bytes = 384000u;
    std::vector<std::int16_t> pcm(bytes / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>((i % 2000) - 1000);
    std::vector<std::uint8_t> zeros(16, 0);
    LIFT_CHECK(m.placeData(data_ea, pcm.data(), bytes, &err));
    LIFT_CHECK(m.placeData(state_ea, zeros.data(), zeros.size(), &err));

    lift::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.block_align = 4;
    fmt.avg_bytes = 192000;
    a.addBuffer(state_ea, obj_ea, data_ea, bytes, 0x181C8u, fmt);
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};
    a.setNotify(state_ea, 4, offsets);
    a.play(state_ea, true);
    LIFT_CHECK(a.pending().size() == 1);
    if (!a.pending().empty()) {
        LIFT_CHECK_EQ_U32(a.pending().front().obj, obj_ea);
        LIFT_CHECK_EQ_U32(a.pending().front().quarter, 0);
    }

    // 0.5s：offset 96000（= 1/4 圈）被越过。
    m.clock().advance100ns(5'000'000);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.pump()), static_cast<std::uint32_t>(lift::AudioPump::Idle));
    LIFT_CHECK(a.pending().size() == 2);
    if (a.pending().size() == 2) {
        LIFT_CHECK_EQ_U32(a.pending()[0].quarter, 0);
        LIFT_CHECK_EQ_U32(a.pending()[1].quarter, 1);
    }
    a.clearPendingForTest();

    // 1.0s / 1.5s：2、3。
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().size() == 1 && a.pending().front().quarter == 2);
    a.clearPendingForTest();
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().size() == 1 && a.pending().front().quarter == 3);
    a.clearPendingForTest();

    // 2.0s：回绕 0（必须仍排在它该在的位置）。
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().size() == 1 && a.pending().front().quarter == 0);
    LIFT_CHECK_EQ_U32(a.playedCursor(state_ea), 0u);
    a.clearPendingForTest();

    // 2.5s：下一圈 1。
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().size() == 1 && a.pending().front().quarter == 1);
    a.clearPendingForTest();

    // Stop 后游标冻结、不再投递。
    a.stop(state_ea);
    const std::uint32_t frozen = a.playedCursor(state_ea);
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().empty());
    LIFT_CHECK_EQ_U32(a.playedCursor(state_ea), frozen);

    // SetCurrentPosition 后重新从该位置走。
    a.setPosition(state_ea, 96000);
    a.play(state_ea, true);
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK_EQ_U32(a.playedCursor(state_ea), 192000u);

    a.removeBuffer(state_ea);
    LIFT_CHECK(a.pending().empty());
    a.end();

    // dump：前 0.5s（96000 B）逐字节 = guest 环的斜坡。
    std::FILE* f = std::fopen(dump.c_str(), "rb");
    LIFT_CHECK(f != nullptr);
    if (f) {
        std::uint8_t hdr[44] = {};
        LIFT_CHECK(std::fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
        LIFT_CHECK(std::memcmp(hdr, "RIFF", 4) == 0);
        LIFT_CHECK(std::memcmp(hdr + 8, "WAVEfmt ", 8) == 0);
        std::vector<std::uint8_t> got(96000);
        LIFT_CHECK(std::fread(got.data(), 1, got.size(), f) == got.size());
        LIFT_CHECK(std::memcmp(got.data(), pcm.data(), got.size()) == 0);
        std::fclose(f);
    }
    std::remove(dump.c_str());
}

void testMonoResample() {
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    const std::string dump = "lift-audio-mono-test.wav";
    std::remove(dump.c_str());
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, &err) || !m.audio().begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    const std::uint32_t data_ea = 0x00460000u;
    const std::uint32_t state_ea = 0x00461000u;
    const std::uint32_t obj_ea = 0x00462000u;
    const std::uint32_t bytes = 176400u;
    std::vector<std::int16_t> pcm(bytes / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>((i % 1000) + 1);
    std::vector<std::uint8_t> zeros(16, 0);
    LIFT_CHECK(m.placeData(data_ea, pcm.data(), bytes, &err));
    LIFT_CHECK(m.placeData(state_ea, zeros.data(), zeros.size(), &err));

    lift::AudioFormat fmt;
    fmt.sample_rate = 44100;
    fmt.channels = 1;
    fmt.bits = 16;
    fmt.block_align = 2;
    fmt.avg_bytes = 88200;
    a.addBuffer(state_ea, obj_ea, data_ea, bytes, 0x181C8u, fmt);
    a.play(state_ea, true);
    m.clock().advance100ns(5'000'000);
    a.pump();
    a.end();

    std::FILE* f = std::fopen(dump.c_str(), "rb");
    LIFT_CHECK(f != nullptr);
    if (f) {
        std::uint8_t hdr[44] = {};
        LIFT_CHECK(std::fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr));
        // 0.5s @ 48k stereo = 96000 B；第 0 帧的 L/R 必须相同且等于源第 0 个样本。
        std::uint8_t frame[4] = {};
        LIFT_CHECK(std::fread(frame, 1, 4, f) == 4);
        const std::int16_t l = static_cast<std::int16_t>(frame[0] | (frame[1] << 8));
        const std::int16_t r = static_cast<std::int16_t>(frame[2] | (frame[3] << 8));
        LIFT_CHECK(l == r);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<std::uint16_t>(l)),
                          static_cast<std::uint32_t>(static_cast<std::uint16_t>(pcm[0])));
        // 非零相位：第 1000 个输出帧必须等于 44.1k→48k 线性插值的期望值。
        const std::uint64_t step = (44100ull * 65536ull + 24000ull) / 48000ull;
        const std::uint64_t pos = 1000ull * step;
        const std::uint32_t total = bytes / 2u;
        const std::uint32_t idx = static_cast<std::uint32_t>((pos >> 16) % total);
        const std::uint32_t frac = static_cast<std::uint32_t>(pos & 0xFFFFu);
        const std::int32_t s0 = pcm[idx];
        const std::int32_t s1 = pcm[(idx + 1u) % total];
        const std::int32_t want = s0 + static_cast<std::int32_t>((static_cast<std::int64_t>(s1 - s0) * frac) >> 16);
        std::fseek(f, 44 + 1000 * 4, SEEK_SET);
        std::uint8_t f2[4] = {};
        LIFT_CHECK(std::fread(f2, 1, 4, f) == 4);
        const std::int16_t l2 = static_cast<std::int16_t>(f2[0] | (f2[1] << 8));
        const std::int16_t r2 = static_cast<std::int16_t>(f2[2] | (f2[3] << 8));
        LIFT_CHECK(l2 == r2);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<std::uint16_t>(l2)),
                          static_cast<std::uint32_t>(static_cast<std::uint16_t>(want)));
        std::fclose(f);
    }
    std::remove(dump.c_str());
}

void testFeederLifecycle() {
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    if (!m.configureAudio(lift::AudioMode::Virtual, "", &err) || !m.audio().begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    const std::uint32_t sound = 0x00470000u;
    const std::uint32_t stream = 0x00471000u;
    const std::uint32_t obj = 0x00472000u;
    const std::uint32_t slot = 11u;
    const std::uint32_t quit = 0x00001234u;
    std::vector<std::uint8_t> zeros(0x1000, 0);
    LIFT_CHECK(m.placeData(sound, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(stream, zeros.data(), zeros.size(), &err));
    m.mem().wr32(sound + 0x514u, quit);
    m.mem().wr32(sound + 0x654u + 4u * slot, stream);
    m.mem().wr32(stream + 0x40Cu, obj);

    LIFT_CHECK(a.shouldOwnThread(0x004B5AA0u));
    LIFT_CHECK(!a.shouldOwnThread(0x004B5AA1u));
    LIFT_CHECK(a.registerFeeder(0x004B5AA0u, sound, 0x00001008u, 0x000002D0u));
    LIFT_CHECK(!a.shouldOwnThread(0x004B5AA0u));
    LIFT_CHECK(a.isFeederHandle(0x000002D0u));
    LIFT_CHECK(!a.isFeederHandle(0x000002D1u));
    a.resumeFeeder();
    LIFT_CHECK(!a.feederExited());
    a.noteEventSet(quit);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.pump()), static_cast<std::uint32_t>(lift::AudioPump::Idle));
    LIFT_CHECK(a.feederExited());
    a.joinFeeder();
    LIFT_CHECK(a.feederExited());
    a.end();
}

void testFeederContext() {
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    if (!m.configureAudio(lift::AudioMode::Virtual, "", &err) || !m.audio().begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    const std::uint32_t code_ea = 0x004B6C30u;
    const std::uint32_t sound = 0x00460000u;
    const std::uint32_t stream = 0x00461000u;
    const std::uint32_t state = 0x00462000u;
    const std::uint32_t data = 0x00400000u;
    const std::uint32_t obj = 0x00463000u;
    const std::uint32_t bytes = 384000u;
    const std::uint32_t slot = 11u;
    const std::uint32_t quit = 0x00001234u;

    // fake `sub_4B6C30`：改掉全部 GPR / x87 / XMM，然后 `ret 4`。
    const std::uint8_t stub[] = {
        0xB8, 0x11, 0x11, 0x11, 0x11,  // mov eax,11111111h
        0xB9, 0x22, 0x22, 0x22, 0x22,  // mov ecx,22222222h
        0xBA, 0x33, 0x33, 0x33, 0x33,  // mov edx,33333333h
        0xBB, 0x44, 0x44, 0x44, 0x44,  // mov ebx,44444444h
        0xBE, 0x55, 0x55, 0x55, 0x55,  // mov esi,55555555h
        0xBF, 0x66, 0x66, 0x66, 0x66,  // mov edi,66666666h
        0xBD, 0x77, 0x77, 0x77, 0x77,  // mov ebp,77777777h
        0x83, 0xC0, 0x01,              // add eax,1（改 EFLAGS，验证它也被还原）
        0xD9, 0xE8,                    // fld1
        0x66, 0x0F, 0x6E, 0xC0,        // movd xmm0,eax（SSE 整数搬移；语料里支持的形态）
        0xC2, 0x04, 0x00,              // ret 4
    };
    LIFT_CHECK(m.placeCode(code_ea, stub, sizeof(stub), &err));
    std::vector<std::uint8_t> zeros(0x1000, 0);
    LIFT_CHECK(m.placeData(sound, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(stream, zeros.data(), zeros.size(), &err));
    LIFT_CHECK(m.placeData(state, zeros.data(), 16, &err));
    std::vector<std::uint8_t> pcm(bytes, 0);
    LIFT_CHECK(m.placeData(data, pcm.data(), pcm.size(), &err));
    m.mem().wr32(sound + 0x514u, quit);
    m.mem().wr32(sound + 0x654u + 4u * slot, stream);
    m.mem().wr32(stream + 0x40Cu, obj);
    m.mem().wr32(stream + 0x444u, bytes);

    lift::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.block_align = 4;
    fmt.avg_bytes = 192000;
    a.addBuffer(state, obj, data, bytes, 0x181C8u, fmt);
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};
    a.setNotify(state, 4, offsets);
    a.play(state, true);
    LIFT_CHECK(a.registerFeeder(0x004B5AA0u, sound, 0x00001008u, 0x000002D0u));
    a.resumeFeeder();

    // 泵看到 quarter 0 的通知 ⇒ 铺帧并返回 Suspended。
    m.cpu().eip = 0x00401000u;  // snapshot 的 eip（不会被真正执行；max_insns 会先停）
    m.cpu().eax() = 0xAAAA0001u;
    m.cpu().ecx() = 0xAAAA0002u;
    m.cpu().edx() = 0xAAAA0003u;
    m.cpu().ebx() = 0xAAAA0004u;
    m.cpu().esi() = 0xAAAA0005u;
    m.cpu().edi() = 0xAAAA0006u;
    m.cpu().ebp() = 0xAAAA0007u;
    m.cpu().eflags = 0x0000024Bu;
    m.cpu().fpu.control = 0x037Fu;
    m.cpu().fpu.status = 0;
    m.cpu().fpu.top = 3;
    m.cpu().fpu.tag = 0xFFFFu;
    m.cpu().xmm[0][0] = 0x12345678u;
    m.cpu().shim.thread_id = 0x00001004u;
    m.cpu().shim.last_error = 0xDEADBEEFu;
    const std::uint32_t saved_esp = m.cpu().esp();
    const std::uint32_t saved_ret = m.mem().rd32(saved_esp);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.pump()), static_cast<std::uint32_t>(lift::AudioPump::Suspended));
    LIFT_CHECK(a.inFeederFill());
    LIFT_CHECK_EQ_U32(m.cpu().shim.thread_id, 0x00001008u);
    // 铺帧后、guest 还没跑：CS 必须已以 feeder 身份持有，在飞计数 +1。
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x690u + 24u * slot + 0x0Cu), 0x00001008u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x690u + 24u * slot + 0x08u), 1u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x7F8u + 4u * slot), 1u);

    // 跑完 fake callee：11 条指令（7 mov + stc + fld1 + movd + ret）。
    lift::RunOptions opt;
    opt.max_insns = 11;
    const lift::RunResult r = m.run(opt);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.reason), static_cast<std::uint32_t>(lift::StopReason::MaxInsns));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(a.stats().fills), 1);

    // 整份上下文必须原样回来（⛔ 只存 8 个 GPR 会在这里红）。
    LIFT_CHECK_EQ_U32(m.cpu().eax(), 0xAAAA0001u);
    LIFT_CHECK_EQ_U32(m.cpu().ecx(), 0xAAAA0002u);
    LIFT_CHECK_EQ_U32(m.cpu().edx(), 0xAAAA0003u);
    LIFT_CHECK_EQ_U32(m.cpu().ebx(), 0xAAAA0004u);
    LIFT_CHECK_EQ_U32(m.cpu().esi(), 0xAAAA0005u);
    LIFT_CHECK_EQ_U32(m.cpu().edi(), 0xAAAA0006u);
    LIFT_CHECK_EQ_U32(m.cpu().ebp(), 0xAAAA0007u);
    LIFT_CHECK_EQ_U32(m.cpu().eflags, 0x0000024Bu);
    LIFT_CHECK_EQ_U32(m.cpu().fpu.top, 3u);
    LIFT_CHECK_EQ_U32(m.cpu().fpu.tag, 0xFFFFu);
    LIFT_CHECK_EQ_U32(m.cpu().fpu.control, 0x037Fu);
    LIFT_CHECK_EQ_U32(m.cpu().fpu.status, 0u);
    LIFT_CHECK_EQ_U32(m.cpu().xmm[0][0], 0x12345678u);
    LIFT_CHECK_EQ_U32(m.cpu().shim.thread_id, 0x00001004u);
    LIFT_CHECK_EQ_U32(m.cpu().shim.last_error, 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(m.cpu().esp(), saved_esp);
    LIFT_CHECK_EQ_U32(m.cpu().eip, 0x00401000u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(saved_esp), saved_ret);
    // CS / 在飞计数必须成对收尾。
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x690u + 24u * slot + 0x0Cu), 0u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x690u + 24u * slot + 0x08u), 0u);
    LIFT_CHECK_EQ_U32(m.mem().rd32(sound + 0x7F8u + 4u * slot), 0u);
    LIFT_CHECK(!a.inFeederFill());
}

void testCrossingSort() {
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    if (!m.configureAudio(lift::AudioMode::Virtual, "", &err) || !m.audio().begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    const std::uint32_t data_ea = 0x00440000u;
    const std::uint32_t state_ea = 0x00430000u;
    const std::uint32_t obj_ea = 0x00450000u;
    const std::uint32_t bytes = 384000u;
    std::vector<std::uint8_t> pcm(bytes, 0);
    std::vector<std::uint8_t> zeros(16, 0);
    LIFT_CHECK(m.placeData(data_ea, pcm.data(), pcm.size(), &err));
    LIFT_CHECK(m.placeData(state_ea, zeros.data(), zeros.size(), &err));
    lift::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.block_align = 4;
    fmt.avg_bytes = 192000;
    a.addBuffer(state_ea, obj_ea, data_ea, bytes, 0x181C8u, fmt);
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};
    a.setNotify(state_ea, 4, offsets);
    a.play(state_ea, true);
    a.clearPendingForTest();  // 去掉 Play 的 offset 0；只看一次大推进的排序
    // 一次跨 2.5s：1/4、1/2、3/4、回绕 0、下一圈 1/4 必须在**同一个** fireCrossings 里排序。
    m.clock().advance100ns(25'000'000);
    a.pump();
    const auto& p = a.pending();
    LIFT_CHECK(p.size() == 4);
    if (p.size() == 4) {
        LIFT_CHECK_EQ_U32(p[0].quarter, 1u);
        LIFT_CHECK_EQ_U32(p[1].quarter, 2u);
        LIFT_CHECK_EQ_U32(p[2].quarter, 3u);
        LIFT_CHECK_EQ_U32(p[3].quarter, 0u);
    }
    a.end();
}

}  // namespace

int main() {
    testDeviceModel();
    testMonoResample();
    testFeederLifecycle();
    testFeederContext();
    testCrossingSort();
    return lift_test::finish("audio-device");
}
