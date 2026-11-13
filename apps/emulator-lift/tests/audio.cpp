// emulator-lift · 音频设备模型 / 逐缓冲格式 / 通知次序（pure 档）
//
// ★ 判据（方案 D 的 D1/D2 机械部分）：
//   ① 48k 立体声缓冲：每 0.5s 恰好投递一个通知，次序 0→1→2→3→回绕 0；
//   ② 输出 48k/16/2；设备游标 %bytes；dump 的前 0.5s 与 guest 环的斜坡**逐字节**相等；
//   ③ 44.1k **单声道**缓冲：正确重采样到 48k，左右声道相同（引擎真的会建 mono 流）；
//   ④ 喂料线程身份：register/resume/quit/join 的状态机（不跑 guest 线程体）；
//   ⑤ Stop 后游标冻结（不 seek 直接 Play 从冻结处继续）、SetCurrentPosition 重排、
//      Release 清掉 pending；
//   ⑥ 静音门：缺省 `sound=false` 时 dump 全 0 但 fills / mixed / 通知照跑；`sound=true` 逐字节
//      等于 guest 斜坡（见文件末尾的 runDelivery）。
//   ⑦ 第 3 层补偿的覆盖面：`removeBuffer` / `addBuffer`（同一 state 换 obj）在
//      `filling_obj_` 命中时**有界等**"正在跑的那一刀"（`removeBuffer` 挡在 guest 堆 free 之前；
//      用夹具 seam 伪造在飞 fill，见文件后半的三条用例）。
//   ⑧ underrun 插静音（判据 = 写见证 `[stream+0x2468]`，⛔ 不是内容比对）：环陈旧 ⇒ 该区交付 0
//      + 计数；健康驱动不误判；guest 自己的全 0 静音不算 underrun；一刀真 fill 的见证接线
//      （`fill_witnessed` / `guest_silence_fills`）。见文件末尾的四条用例。
//
// ★ 会红：把 `fireCrossings` 的 sort 去掉 ⇒ ① 的回绕 0 会排在 3 之前；
//   把 `generateTo` 的声道/采样率转换去掉 ⇒ ③ 红；把 removeBuffer 的 pending 清理去掉 ⇒ ⑤ 红；
//   把 `stop()` 的 `dropPending` 去掉 ⇒ `audio-device/stop-then-play-without-seek` 红
//   （旧 q1 + 新 q1 ⇒ pending().size()==2）。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
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
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, /*sound=*/true, &err)) {
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
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, /*sound=*/true, &err) || !m.audio().begin(&err)) {
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
    if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
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
    if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
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
    if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
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

// ---- D1/D3 守卫：Stop 作废旧 fill / 已播再 Play 不重锚 --------------------------------
//
// ★ 判据（红了意味着什么）：
//   **G1**：`stop()` 之后该 obj 的 pending 必须为空。引擎在 `sub_4B6B60`（Stop）之后才做
//   重定位 / 预填 / Play，而那段**不在槽 CS 里**（见 docs/audio-implementation.md §5）；
//   真机上"越过通知点"的那一刀 fill 早就被消费掉了，emulator 的慢 fill 让它跨过 Stop ⇒
//   不排空就会把 Stop **之前**越过的通知当成 Stop **之后**的事件投递。
//   随后按引擎的真实次序（Stop → SetCurrentPosition(0) `0x4B7441` → Play）必须只产生 `q=0`。
//   **G2**：真 DS 对**已在播**的缓冲再 `Play` 返回 DS_OK、继续播、只有最后一次 flags 生效，
//   **不重置游标、不重发 offset 0 通知**（MS 文档 Play）⇒ 第二次 `play()` 不得入队；
//   之后越过 1/2 圈必须仍是 `q2`（重锚了这里会变成 `q0` / `q1`）。
//
// ★ 会红（这是本守卫存在的理由）：
//   - `stop()` 不 `dropPending` ⇒ G1 的 `pending().empty()` 红。
//     ★ 这一条是**内部机制**断言：删掉 `dropPending` 之后**对外次序其实不变**（下一步
//     `setPosition(0)` 也会 drop）⇒ 它只靠这条内部断言变红。**对外行为**由
//     `audio-device/stop-then-play-without-seek`（不 seek 直接 Play ⇒ 只剩 q1）守住。
//   - `play()` 不在 `b->playing` 时 no-op ⇒ G2 的 `pending().empty()` 红（重锚 q0 ⇒ 后面变 q1）。
void testRepositionAndReplay() {
    const std::uint32_t data_ea = 0x00440000u;
    const std::uint32_t state_ea = 0x00430000u;
    const std::uint32_t obj_ea = 0x00450000u;
    const std::uint32_t bytes = 384000u;  // 48k / 16 / 2 ⇒ 2.0 s 一圈，4 个通知点每 0.5 s
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};

    // ---- G1：stop() 作废"已越过通知点"的旧 fill --------------------------------------
    {
        lift_test::g_current_case = "audio-device/stop-drops-stale-fill";
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return;
        }
        if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
            LIFT_CHECK(false);
            return;
        }
        lift::AudioEngine& a = m.audio();
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
        a.setNotify(state_ea, 4, offsets);
        a.play(state_ea, true);
        LIFT_CHECK(a.pending().size() == 1);
        if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 0u);
        a.clearPendingForTest();

        // 0.5 s：设备游标越过 offset 96000 ⇒ 只该有 q1。
        m.clock().advance100ns(5'000'000);
        a.pump();
        LIFT_CHECK(a.pending().size() == 1);
        if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 1u);

        // ★ Stop：这条"已经越过通知点"的旧 fill 必须作废（真机上它早已被消费）。
        a.stop(state_ea);
        LIFT_CHECK(a.pending().empty());
        // Stop 冻结在 0.5 s = 96000 B：**不重定位就 Play** 会从 96000 继续，于是立刻命中
        // offset 96000 ⇒ 那是 `q1`（真 DS 的 Stop→Play 语义），不是本守卫要的 `q0`。
        LIFT_CHECK_EQ_U32(a.playedCursor(state_ea), 96000u);
        // 引擎的真实次序是 Stop → SetCurrentPosition(0)（`0x4B7441`）→ 预填 → Play。
        a.setPosition(state_ea, 0);
        a.play(state_ea, true);
        LIFT_CHECK(a.pending().size() == 1);
        if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 0u);
        a.end();
    }

    // ---- G2：已播再 Play 不重锚（不重置游标、不重发 offset 0）-------------------------
    {
        lift_test::g_current_case = "audio-device/replay-on-playing";
        std::string err;
        lift::Machine m;
        if (!initMachine(m, &err)) {
            LIFT_CHECK(false);
            return;
        }
        if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
            LIFT_CHECK(false);
            return;
        }
        lift::AudioEngine& a = m.audio();
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
        a.setNotify(state_ea, 4, offsets);
        a.play(state_ea, true);
        a.clearPendingForTest();

        // 0.5 s ⇒ q1；再 0.25 s 还没到 1/2 圈 ⇒ 不该有 crossing。
        m.clock().advance100ns(5'000'000);
        a.pump();
        LIFT_CHECK(a.pending().size() == 1);
        if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 1u);
        a.clearPendingForTest();
        m.clock().advance100ns(2'500'000);
        a.pump();
        LIFT_CHECK(a.pending().empty());

        // ★ 第二次 Play（此时 playing 为真）：不得重锚 ⇒ 不得入队 q0。
        a.play(state_ea, true);
        LIFT_CHECK(a.pending().empty());

        // 再 0.5 s（总 1.25 s）越过 offset 192000 ⇒ 必须仍是 q2（重锚了这里会是 q0/q1）。
        m.clock().advance100ns(5'000'000);
        a.pump();
        LIFT_CHECK(a.pending().size() == 1);
        if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 2u);
        a.end();
    }
}

// ---- 对外行为：Stop 之后**不 seek** 直接 Play（`audio-device/stop-then-play-without-seek`）---
//
// ★ 判据（红了意味着什么）：真 DS 的 `Stop` 把播放位置**冻结**在当前游标（这条流是 96000 B），
//   **不** `SetCurrentPosition` 直接 `Play` 会从 96000 继续 ⇒ 立刻命中 offset 96000 ⇒ 队列里
//   只该有一条 `quarter=1`，⛔ **不许出现 `q0`**（出现 `q0` = 游标被当成回到了 0 / 重生成了）。
//
// ★ 会红（这是本守卫存在的理由）：把 `stop()` 里的 `dropPending(obj)` 删掉 ⇒ Stop 之前越过
//   的那条旧 `q1`（旧代次）还留在队列里，随后 `Play` 又按冻结位置入队**新的** `q1`
//   （`enqueue` 的去重比的是"obj + quarter + generation"，代次不同 ⇒ 不合并）
//   ⇒ `pending().size() == 2`（旧 q1 + 新 q1）⇒ 上面那条 `size()==1` 当场红。
//   ★ 这条是**对外行为**的守卫：`audio-device/stop-drops-stale-fill` 的 `pending().empty()`
//   是内部机制断言（那一步之后没人看得见队列），删掉 `dropPending` 时它红、但**对外次序不变**
//   （下一步 `setPosition(0)` 也会 drop）⇒ 真正"Stop 之后不带着旧通知继续播"由这一条守。
void testStopThenPlayWithoutSeek() {
    lift_test::g_current_case = "audio-device/stop-then-play-without-seek";
    const std::uint32_t data_ea = 0x00440000u;
    const std::uint32_t state_ea = 0x00430000u;
    const std::uint32_t obj_ea = 0x00450000u;
    const std::uint32_t bytes = 384000u;  // 48k / 16 / 2 ⇒ 2.0 s 一圈，4 个通知点每 0.5 s
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};

    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, &err) || !m.audio().begin(&err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
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
    a.setNotify(state_ea, 4, offsets);
    a.play(state_ea, true);
    a.clearPendingForTest();  // 只看 stop → play 这一段的队列；Play 自带的 q0 不参与判据

    // 0.5 s ⇒ 设备游标越过 offset 96000 ⇒ 一条 q1。
    m.clock().advance100ns(5'000'000);
    a.pump();
    LIFT_CHECK(a.pending().size() == 1);
    if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 1u);

    // ★ Stop 冻结在 96000 B。**不** Seek：直接 Play 从 96000 继续。
    a.stop(state_ea);
    LIFT_CHECK_EQ_U32(a.playedCursor(state_ea), 96000u);
    a.play(state_ea, true);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.pending().size()), 1u);
    if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().quarter, 1u);
    for (const lift::AudioNotifyEvent& e : a.pending()) LIFT_CHECK_NE_U32(e.quarter, 0u);
    a.end();
}

// ---- 第 3 层补偿的覆盖面：`removeBuffer` / `addBuffer` 也要等"正在跑的那一刀" --------------
//
// ★ 判据（红了意味着什么）：
//   **G3**：`filling_obj_ == 该 obj` 时 `removeBuffer(state)` **必须真的等**它跑完。
//   这条等待挡在 `destroyBufferBlocks`（`shims/dsound.cpp`）的 `heap->free` **之前** ——
//   它先 `removeBuffer` 再 free guest 环与状态块，而设备模型读的正是 `b.data_ea`
//   ⇒ 不等就会让在飞那一刀往已 free 的 guest 堆块里写（UAF / 串块），而不只是"写回旧相位"。
//   **G4**（负数对照）：没有在飞 fill 时 `removeBuffer` **不许恒等**（`fill_waits` 不增），
//   排除"每条 remove 都睡 20 ms"那种恒等待。
//   **G5**：同一 `state_ea` 换成另一条 obj 时，旧 obj 的在飞那一刀也要等（`addBuffer`）；
//   等完新 obj 必须在模型里生效（Play 入队的是新 obj，不是旧 obj）。
//
// ★ 怎么造"在飞 fill"：本测试是 `AudioMode::Virtual` ⇒ `thread_mode_ == false`、
//   `filling_obj_` 恒 0（它只在 `feederEvent` 里置位，而 feeder 只在 `Sdl && realTime` 起）。
//   ⇒ 用夹具 seam `setFillInFlightForTest(obj)` 伪造，**另一条线程** 20 ms 后清 0。
//   ⛔ 它测的是"等待的接线"（真的进了 `waitFill` + 记账），**不**覆盖：
//     ① feeder 自己调进来的自等早退（`isFeederThread()` 在纯档恒 false，走不到那条分支；
//        漏了那句的后果是每刀 3 s、不是 assert，只能靠 `--audio sdl` 读数看 `fill_wait_timeouts`）；
//     ② 真重叠时旧环被写坏（那要真线程 + 真设备）。
//
// ★ 会红：删掉 `removeBuffer` / `addBuffer` 里那句 `waitFill`（或把它挪到 `thread_mode_ &&`
//   后面）⇒ `fill_waits` 保持 0、耗时 < 15 ms ⇒ G3/G5 当场红。
//   ★ 读数只作**存在性**判据（"真的等了"）：`fill_wait_us > 0`；⛔ 不锁具体毫秒数
//   （机器负载会让 20 ms 的睡变成 21 或 35）。
constexpr std::uint32_t kCompDataEa = 0x00440000u;
constexpr std::uint32_t kCompStateEa = 0x00430000u;
constexpr std::uint32_t kCompObj1 = 0x00450000u;
constexpr std::uint32_t kCompObj2 = 0x00451000u;
constexpr std::uint32_t kCompBytes = 384000u;  // 48k / 16 / 2 ⇒ 2.0 s 一圈

lift::AudioFormat stereo48k() {
    lift::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.block_align = 4;
    fmt.avg_bytes = 192000;
    return fmt;
}

bool beginVirtualAudio(lift::Machine& m, std::string* err) {
    if (!initMachine(m, err)) return false;
    if (!m.configureAudio(lift::AudioMode::Virtual, "", /*sound=*/true, err)) return false;
    return m.audio().begin(err);
}

bool placeCompRing(lift::Machine& m, std::string* err) {
    std::vector<std::uint8_t> pcm(kCompBytes, 0);
    std::vector<std::uint8_t> zeros(16, 0);
    if (!m.placeData(kCompDataEa, pcm.data(), pcm.size(), err)) return false;
    return m.placeData(kCompStateEa, zeros.data(), zeros.size(), err);
}

// 起一条"20 ms 后清掉在飞标志"的线程；返回时它已经在跑。
struct FillClearer {
    std::atomic<bool> cleared{false};
    std::thread thread;
    FillClearer(lift::AudioEngine& a, std::chrono::milliseconds delay)
        : thread([&a, this, delay] {
              std::this_thread::sleep_for(delay);
              a.setFillInFlightForTest(0);
              cleared.store(true);
          }) {}
    ~FillClearer() {
        if (thread.joinable()) thread.join();
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
};

void testRemoveWaitsForInFlight() {
    lift_test::g_current_case = "audio-device/remove-waits-inflight";
    std::string err;
    lift::Machine m;
    if (!beginVirtualAudio(m, &err) || !placeCompRing(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    a.addBuffer(kCompStateEa, kCompObj1, kCompDataEa, kCompBytes, 0x181C8u, stereo48k());
    a.setFillInFlightForTest(kCompObj1);
    FillClearer clearer(a, std::chrono::milliseconds(20));
    const auto t0 = std::chrono::steady_clock::now();
    a.removeBuffer(kCompStateEa);
    const auto t1 = std::chrono::steady_clock::now();
    clearer.join();
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    LIFT_CHECK(clearer.cleared.load());  // 先确认夹具真的动了，再谈"等了多久"
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.stats().fill_waits), 1u);
    LIFT_CHECK(waited_ms >= 15);
    LIFT_CHECK(a.stats().fill_wait_us > 0);
    // 线程清 0 之后标志必须回 0（不是"等完还留着"）。
    LIFT_CHECK_EQ_U32(a.fillInFlightForTest(), 0u);
    // 条目真的删了：`play` 找不到 Buffer（`play` 在 `find() == nullptr` 时直接返回）⇒ 不入队。
    a.play(kCompStateEa, true);
    LIFT_CHECK(a.pending().empty());
    a.end();
}

void testRemoveWithoutInFlightDoesNotWait() {
    lift_test::g_current_case = "audio-device/remove-no-wait-without-inflight";
    std::string err;
    lift::Machine m;
    if (!beginVirtualAudio(m, &err) || !placeCompRing(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    a.addBuffer(kCompStateEa, kCompObj1, kCompDataEa, kCompBytes, 0x181C8u, stereo48k());
    // ⛔ 不设 seam：没有在飞 fill ⇒ 不许等。
    LIFT_CHECK_EQ_U32(a.fillInFlightForTest(), 0u);
    const auto t0 = std::chrono::steady_clock::now();
    a.removeBuffer(kCompStateEa);
    const auto t1 = std::chrono::steady_clock::now();
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.stats().fill_waits), 0u);
    LIFT_CHECK(a.stats().fill_wait_us == 0);
    // 真等待是 20 ms 量级 ⇒ 这里必须远小于它（排除"恒等待"）。
    LIFT_CHECK(waited_ms < 15);
    a.end();
}

void testAddBufferSwapWaitsForInFlight() {
    lift_test::g_current_case = "audio-device/addbuffer-swap-waits-inflight";
    std::string err;
    lift::Machine m;
    if (!beginVirtualAudio(m, &err) || !placeCompRing(m, &err)) {
        LIFT_CHECK(false);
        return;
    }
    lift::AudioEngine& a = m.audio();
    a.addBuffer(kCompStateEa, kCompObj1, kCompDataEa, kCompBytes, 0x181C8u, stereo48k());
    a.setFillInFlightForTest(kCompObj1);
    FillClearer clearer(a, std::chrono::milliseconds(20));
    const auto t0 = std::chrono::steady_clock::now();
    a.addBuffer(kCompStateEa, kCompObj2, kCompDataEa, kCompBytes, 0x181C8u, stereo48k());
    const auto t1 = std::chrono::steady_clock::now();
    clearer.join();
    const auto waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    LIFT_CHECK(clearer.cleared.load());
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.stats().fill_waits), 1u);
    LIFT_CHECK(waited_ms >= 15);
    LIFT_CHECK(a.stats().fill_wait_us > 0);
    LIFT_CHECK_EQ_U32(a.fillInFlightForTest(), 0u);
    // 新 obj 必须在模型里生效：Play 只按同一 state 的那条 Buffer 入队 offset 0。
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};
    a.setNotify(kCompStateEa, 4, offsets);
    a.clearPendingForTest();
    a.play(kCompStateEa, true);
    LIFT_CHECK(a.pending().size() == 1);
    if (a.pending().size() == 1) LIFT_CHECK_EQ_U32(a.pending().front().obj, kCompObj2);
    a.end();
}

// ---- 静音门（缺省静音 / `--sound` 才出声）-------------------------------------------
//
// ★ 判据（红了意味着什么）：
//   ① 缺省（`sound=false`）：dump 的 data 区**逐字节全 0**，且长度 == 期望 B；
//   ② `--sound`（`sound=true`）：**不是**全 0，且逐字节 == guest 环的斜坡（同 :139 的 memcmp 口径）
//      ⇒ 排除"全 0 恒真"；
//   ③ 两遍都 `fills == 1`、`mixed_bytes` 相等且 == 期望 B、`notifications >= 1`、交付字节数相等
//      ⇒ 默认静音**没有**跳过喂料 / 混音 / 交付；
//   ④ 缺省 `muted_bytes == mixed_bytes`（有内容被静音），`--sound` 时 `muted_bytes == 0`。
//
// ★ 场景复用 `testFeederContext` 的假 `sub_4B6C30`（11 条指令、`ret 4`）+ 真 feeder 身份，
//   以及 `testDeviceModel` 的 384000 B 斜坡环；0.25 s 虚拟时钟正好混出 48000 B，且不跨任何
//   新通知点（所以 fill 恰好一次）。
// ★ 这条守卫只用 `AudioMode::Virtual` ⇒ 不需要声卡（pure 档）。
struct DeliveryRun {
    bool begun = false;
    bool filled = false;
    std::uint64_t fills = 0;
    std::uint64_t mixed = 0;
    std::uint64_t muted = 0;
    std::uint64_t notifications = 0;
    std::uint32_t data_declared = 0;      // WAV 头里声明的 data 区长度
    std::vector<std::uint8_t> data;       // 实际读到的 data 区
};

constexpr std::uint32_t kDeliveryBytes = 48000u;  // 0.25 s @ 48k / 16 / 2

std::vector<std::int16_t> makeRamp(std::uint32_t bytes) {
    std::vector<std::int16_t> pcm(bytes / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<std::int16_t>((i % 2000) - 1000);
    return pcm;
}

DeliveryRun runDelivery(bool sound, const char* tag) {
    DeliveryRun r;
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) return r;
    const std::string dump = std::string("lift-audio-mute-") + tag + ".wav";
    std::remove(dump.c_str());
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, sound, &err)) return r;
    lift::AudioEngine& a = m.audio();
    if (!a.begin(&err)) return r;
    r.begun = true;

    const std::uint32_t code_ea = 0x004B6C30u;
    const std::uint32_t sound_obj = 0x00460000u;
    const std::uint32_t stream = 0x00461000u;
    const std::uint32_t state = 0x00462000u;
    const std::uint32_t data = 0x00400000u;
    const std::uint32_t obj = 0x00463000u;
    const std::uint32_t bytes = 384000u;
    const std::uint32_t slot = 11u;
    const std::uint32_t quit = 0x00001234u;

    // 假 `sub_4B6C30`：11 条指令后 `ret 4`（与 testFeederContext 同一份夹具）。
    const std::uint8_t stub[] = {
        0xB8, 0x11, 0x11, 0x11, 0x11, 0xB9, 0x22, 0x22, 0x22, 0x22, 0xBA, 0x33, 0x33, 0x33,
        0x33, 0xBB, 0x44, 0x44, 0x44, 0x44, 0xBE, 0x55, 0x55, 0x55, 0x55, 0xBF, 0x66, 0x66,
        0x66, 0x66, 0xBD, 0x77, 0x77, 0x77, 0x77, 0x83, 0xC0, 0x01, 0xD9, 0xE8, 0x66, 0x0F,
        0x6E, 0xC0, 0xC2, 0x04, 0x00,
    };
    if (!m.placeCode(code_ea, stub, sizeof(stub), &err)) return r;
    std::vector<std::uint8_t> zeros(0x1000, 0);
    if (!m.placeData(sound_obj, zeros.data(), zeros.size(), &err)) return r;
    if (!m.placeData(stream, zeros.data(), zeros.size(), &err)) return r;
    if (!m.placeData(state, zeros.data(), 16, &err)) return r;
    const std::vector<std::int16_t> pcm = makeRamp(bytes);
    if (!m.placeData(data, pcm.data(), bytes, &err)) return r;
    m.mem().wr32(sound_obj + 0x514u, quit);
    m.mem().wr32(sound_obj + 0x654u + 4u * slot, stream);
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
    if (!a.registerFeeder(0x004B5AA0u, sound_obj, 0x00001008u, 0x000002D0u)) return r;
    a.resumeFeeder();

    // ★ 顺序很重要：`Machine::run` 结束时会 `audio_.end()`（回填 WAV 头并关闭 dump），
    //   所以先推进虚拟时钟让 pump 混出 48000 B（dump 还开着），再把 quarter 0 铺成一帧 guest。
    //   0.25 s 不跨任何通知点 ⇒ 不会产生第二次 fill。
    m.clock().advance100ns(2'500'000);
    if (a.pump() != lift::AudioPump::Suspended) return r;
    lift::RunOptions opt;
    opt.max_insns = 11;
    const lift::RunResult rr = m.run(opt);  // 结束时 audio_.end() 回填 WAV 头
    if (rr.reason != lift::StopReason::MaxInsns) return r;
    r.filled = true;

    const lift::AudioStats& st = a.stats();  // end() 不清统计
    r.fills = st.fills;
    r.mixed = st.mixed_bytes;
    r.muted = st.muted_bytes;
    r.notifications = st.notifications;

    std::FILE* f = std::fopen(dump.c_str(), "rb");
    if (f) {
        std::uint8_t hdr[44] = {};
        if (std::fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && std::memcmp(hdr, "RIFF", 4) == 0 &&
            std::memcmp(hdr + 8, "WAVEfmt ", 8) == 0 && std::memcmp(hdr + 36, "data", 4) == 0) {
            r.data_declared = static_cast<std::uint32_t>(hdr[40]) |
                              (static_cast<std::uint32_t>(hdr[41]) << 8) |
                              (static_cast<std::uint32_t>(hdr[42]) << 16) |
                              (static_cast<std::uint32_t>(hdr[43]) << 24);
            r.data.resize(r.data_declared);
            if (std::fread(r.data.data(), 1, r.data.size(), f) != r.data.size()) r.data.clear();
        }
        std::fclose(f);
    }
    std::remove(dump.c_str());
    return r;
}

bool allZero(const std::vector<std::uint8_t>& v) {
    for (std::uint8_t b : v)
        if (b != 0) return false;
    return true;
}

void testDefaultMutedDelivery() {
    lift_test::g_current_case = "audio-device/default-muted-delivery";
    const DeliveryRun r = runDelivery(/*sound=*/false, "muted");
    LIFT_CHECK(r.begun);
    LIFT_CHECK(r.filled);
    // ① 长度 == 期望，且 data 区逐字节全 0。
    LIFT_CHECK_EQ_U32(r.data_declared, kDeliveryBytes);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.data.size()), kDeliveryBytes);
    LIFT_CHECK(allZero(r.data));
    // ③④ 喂料 / 混音照跑；静音字节数 == 混音字节数（= 确实有内容被覆盖，而不是没数据）。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.fills), 1u);
    LIFT_CHECK(r.notifications >= 1);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.mixed), kDeliveryBytes);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.muted), static_cast<std::uint32_t>(r.mixed));
    LIFT_CHECK(r.muted > 0 && r.muted == r.mixed);
}

void testSoundOnDelivery() {
    lift_test::g_current_case = "audio-device/sound-on-delivery";
    const DeliveryRun r = runDelivery(/*sound=*/true, "sound");
    LIFT_CHECK(r.begun);
    LIFT_CHECK(r.filled);
    LIFT_CHECK_EQ_U32(r.data_declared, kDeliveryBytes);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.data.size()), kDeliveryBytes);
    // ② 不是全 0，且逐字节 == guest 环的斜坡（照抄 tests/audio.cpp:139 的 memcmp 口径）。
    LIFT_CHECK(!allZero(r.data));
    const std::vector<std::int16_t> pcm = makeRamp(384000u);
    LIFT_CHECK(r.data.size() <= pcm.size() * sizeof(std::int16_t));
    LIFT_CHECK(std::memcmp(r.data.data(), pcm.data(), r.data.size()) == 0);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.fills), 1u);
    LIFT_CHECK(r.notifications >= 1);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.mixed), kDeliveryBytes);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.muted), 0u);
}

// ③ 同一条输入跑两遍、只差 `sound`：喂料 / 混音 / 交付字节数必须相等 ⇒ 静音没走捷径。
void testMutedDeliveryParity() {
    lift_test::g_current_case = "audio-device/muted-delivery-parity";
    const DeliveryRun muted = runDelivery(/*sound=*/false, "parity-muted");
    const DeliveryRun sound = runDelivery(/*sound=*/true, "parity-sound");
    LIFT_CHECK(muted.begun && muted.filled);
    LIFT_CHECK(sound.begun && sound.filled);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(muted.fills), 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sound.fills), 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(muted.mixed), static_cast<std::uint32_t>(sound.mixed));
    LIFT_CHECK(muted.mixed == kDeliveryBytes);
    LIFT_CHECK(muted.notifications >= 1);
    LIFT_CHECK(sound.notifications >= 1);
    LIFT_CHECK_EQ_U32(muted.data_declared, sound.data_declared);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(muted.data.size()),
                      static_cast<std::uint32_t>(sound.data.size()));
    // ④ 再钉一次计数语义（两个方向都钉，防止"恒等成立"）。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(muted.muted), static_cast<std::uint32_t>(muted.mixed));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sound.muted), 0u);
}

// ---- underrun 插静音（判据 = 写见证，⛔ 不是内容比对）：design §1.6 的 pure 档夹具 ----------
//
// ★ 判据（红了意味着什么）：
//   **S1** `underrun-silence-stale`（正控制，**不注册 feeder** ⇒ 天然 0 刀）：第 2 圈没有任何
//     跑完的 fill ⇒ 环陈旧 ⇒ 宿主交付**静音**（⛔ 不是重播第 1 圈的斜坡），并计 episode。
//   **S2** `underrun-silence-not-triggered`（负控制）：夹具 seam `noteFillForTest` 每圈 4 刀
//     ⇒ 每圈都 fresh ⇒ 3 圈逐字节 = 斜坡重复 3 次，零静音字节、零 episode（排除"恒陈旧"）。
//   **S3** `underrun-silence-guest-zeros`：环内容全 0（= 引擎"放完 + memset 0"的产物）+ 每圈
//     4 刀 ⇒ dump 全 0，但 `underrun_silence_*` 都是 0 ⇒ 专门钉"⛔ 不许用内容判陈旧"。
//
// ★ 会红：
//   - 删掉 `generateTo` 里的陈旧分支（恒 fresh）⇒ S1 红（第 2 圈是斜坡旧内容，events=0）；
//   - 把判据改成"内容全 0 ⇒ 陈旧"或"恒陈旧"⇒ S2 / S3 红；
//   - 删掉 fill 收尾的 `witness++`（`noteFillWitness`）⇒ **S1/S2/S3 全绿**（实测）：本夹具的
//     "刀"走 `noteFillForTest` seam、**不经过** `noteFillWitness` ⇒ 它由 `fill-witness-wiring`
//     的"真 fill 前后恰好一个 quarter 的 `witness` +1"钉住（见该用例的 ★）。
//   - 复位表写错（`markRingFresh` 少 `++witness`）⇒ `sound-on-delivery` / S1 等红，**S2 不红**
//     （实测；`noteFillForTest` 每圈仍把 witness 喂到 consumed 之上）。
constexpr std::uint32_t kRingLapBytes = 384000u;  // 48k / 16 / 2 ⇒ 2.0 s 一圈
constexpr std::uint32_t kLapSteps = 40u;          // 0.05 s × 40 = 2.0 s

struct RingRun {
    bool begun = false;
    std::uint64_t declared = 0;  // WAV 头里声明的 data 区长度
    std::vector<std::uint8_t> data;
    lift::AudioStats stats;
};

// `seam_fills` = 每圈开始前调 `noteFillForTest(obj, 0..3)`（模拟健康 feeder 的每圈 4 刀）。
RingRun runRing(const std::vector<std::int16_t>& pcm, std::uint32_t laps, bool seam_fills, const char* tag) {
    RingRun r;
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) return r;
    const std::string dump = std::string("lift-audio-underrun-") + tag + ".wav";
    std::remove(dump.c_str());
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, /*sound=*/true, &err)) return r;
    lift::AudioEngine& a = m.audio();
    if (!a.begin(&err)) return r;
    r.begun = true;

    const std::uint32_t data_ea = 0x00440000u;
    const std::uint32_t state_ea = 0x00430000u;
    const std::uint32_t obj_ea = 0x00450000u;
    std::vector<std::uint8_t> zeros(16, 0);
    if (!m.placeData(data_ea, pcm.data(), kRingLapBytes, &err)) return r;
    if (!m.placeData(state_ea, zeros.data(), zeros.size(), &err)) return r;

    lift::AudioFormat fmt;
    fmt.sample_rate = 48000;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.block_align = 4;
    fmt.avg_bytes = 192000;
    a.addBuffer(state_ea, obj_ea, data_ea, kRingLapBytes, 0x181C8u, fmt);
    const std::uint32_t offsets[4] = {0u, 96000u, 192000u, 288000u};
    a.setNotify(state_ea, 4, offsets);
    a.play(state_ea, true);

    for (std::uint32_t lap = 0; lap < laps; ++lap) {
        if (seam_fills) {
            for (std::uint32_t q = 0; q < 4u; ++q) a.noteFillForTest(obj_ea, q);
        }
        for (std::uint32_t s = 0; s < kLapSteps; ++s) {
            m.clock().advance100ns(500'000);  // 0.05 s
            a.pump();                          // Virtual 档：mixStep 只 generateTo(played_)
        }
    }
    a.end();  // 回填 WAV 头
    r.stats = a.stats();
    std::FILE* f = std::fopen(dump.c_str(), "rb");
    if (f) {
        std::uint8_t hdr[44] = {};
        if (std::fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && std::memcmp(hdr, "RIFF", 4) == 0 &&
            std::memcmp(hdr + 8, "WAVEfmt ", 8) == 0 && std::memcmp(hdr + 36, "data", 4) == 0) {
            r.declared = static_cast<std::uint64_t>(hdr[40]) | (static_cast<std::uint64_t>(hdr[41]) << 8) |
                         (static_cast<std::uint64_t>(hdr[42]) << 16) | (static_cast<std::uint64_t>(hdr[43]) << 24);
            r.data.resize(static_cast<std::size_t>(r.declared));
            if (std::fread(r.data.data(), 1, r.data.size(), f) != r.data.size()) r.data.clear();
        }
        std::fclose(f);
    }
    std::remove(dump.c_str());
    return r;
}

void testUnderrunSilenceStale() {
    lift_test::g_current_case = "audio-device/underrun-silence-stale";
    const std::vector<std::int16_t> ramp = makeRamp(kRingLapBytes);
    const RingRun r = runRing(ramp, /*laps=*/2, /*seam_fills=*/false, "stale");
    LIFT_CHECK(r.begun);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.declared), 2u * kRingLapBytes);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.data.size()), 2u * kRingLapBytes);
    if (r.data.size() == 2u * kRingLapBytes) {
        // 第 1 圈：逐字节 = 斜坡（起播整圈视为已预填 ⇒ 不误判）。
        LIFT_CHECK(std::memcmp(r.data.data(), ramp.data(), kRingLapBytes) == 0);
        // 第 2 圈：**没有**任何跑完的 fill ⇒ 全 0（⛔ 不许是第 1 圈的重播）。
        const std::vector<std::uint8_t> second(r.data.begin() + kRingLapBytes, r.data.end());
        LIFT_CHECK(allZero(second));
    }
    LIFT_CHECK_EQ_U32(r.stats.fills, 0u);
    LIFT_CHECK_EQ_U32(r.stats.fill_witnessed, 0u);
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_events, 4u);
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_bytes, kRingLapBytes);
    // design §1.5：`underruns` 语义扩档 = 设备队列空 ∪ 环陈旧（Virtual 档没有队列，所以相等）。
    LIFT_CHECK_EQ_U32(r.stats.underruns, r.stats.underrun_silence_events);
    LIFT_CHECK(r.stats.underrun_silence_bytes <= r.stats.mixed_bytes);
}

void testUnderrunSilenceNotTriggered() {
    lift_test::g_current_case = "audio-device/underrun-silence-not-triggered";
    const std::vector<std::int16_t> ramp = makeRamp(kRingLapBytes);
    const RingRun r = runRing(ramp, /*laps=*/3, /*seam_fills=*/true, "fresh");
    LIFT_CHECK(r.begun);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.data.size()), 3u * kRingLapBytes);
    if (r.data.size() == 3u * kRingLapBytes) {
        for (std::uint32_t lap = 0; lap < 3u; ++lap) {
            LIFT_CHECK(std::memcmp(r.data.data() + lap * kRingLapBytes, ramp.data(), kRingLapBytes) == 0);
        }
    }
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_bytes, 0u);
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_events, 0u);
    LIFT_CHECK_EQ_U32(r.stats.underruns, 0u);
    // seam 只喂 witness，不碰真 fill 的账。
    LIFT_CHECK_EQ_U32(r.stats.fills, 0u);
    LIFT_CHECK_EQ_U32(r.stats.fill_witnessed, 0u);
}

void testUnderrunSilenceGuestZeros() {
    lift_test::g_current_case = "audio-device/underrun-silence-guest-zeros";
    // 环内容全 0 = 引擎"放完 + memset 0"（`[+0x2458]==2`）的产物；健康 feeder 每圈 4 刀。
    const std::vector<std::int16_t> zeros(kRingLapBytes / 2, 0);
    const RingRun r = runRing(zeros, /*laps=*/3, /*seam_fills=*/true, "guest-zeros");
    LIFT_CHECK(r.begun);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.data.size()), 3u * kRingLapBytes);
    LIFT_CHECK(allZero(r.data));
    // ★ 这一条就是"⛔ 不许用内容判陈旧"的守卫：换成内容判据 ⇒ 这里会被判成满圈 underrun。
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_bytes, 0u);
    LIFT_CHECK_EQ_U32(r.stats.underrun_silence_events, 0u);
    LIFT_CHECK_EQ_U32(r.stats.underruns, 0u);
}

// ---- 写见证链路：一刀**真 fill** → `[+0x2468]` 前后比较 → `fill_witnessed` ------------------
//
// ★ 判据（design §1.6 的 `fill-witness-wiring`）：照 `runDelivery` 跑一刀真 fill，把
//   `[stream+0x2458]` / `[+0x2468]` 预置成三档 ⇒
//     ① 写（`[+0x2468]` 被 inc）：`fill_witnessed==1`、`guest_silence_fills==0`；
//     ② 0 = Stop 早退（不 inc）：`fill_witnessed==0`；
//     ③ 2 = 放完（inc + `[+0x2458]>=2`）：`fill_witnessed==1`、`guest_silence_fills==1`
//        ⇒ 这就是"guest 自己的静音 ≠ underrun"的机械来源。
// ★ 会红：把 `noteFillWitness` 的 `pre != post` 判据去掉（或改成只看 `[+0x2458]`）⇒ ②/③ 红；
//   ★ 删掉 `noteFillWitness` 的 `++witness` ⇒ **本用例**红（①/③ 的"恰好一个 quarter 的
//   `witness` +1"不成立）；`underrun-silence-*` 全绿 —— 它们走 `noteFillForTest` seam。
constexpr std::uint32_t kWitnessStreamBytes = 0x3000u;  // 必须覆盖 [stream+0x2468]

struct WitnessRun {
    bool begun = false;
    bool filled = false;
    std::uint64_t fills = 0;
    std::uint64_t witnessed = 0;
    std::uint64_t guest_silence = 0;
    // ★ 真 fill 前后 `witness[0..3]` 的快照：前 = `play()` 之后（`markRingFresh` 已跑完）、
    //   `registerFeeder()` 之前；后 = `m.run()` 返回之后。下面对差值的断言 ≡
    //   `noteFillWitness` 里那句 `++witness[quarter]`（stats 的 `fill_witnessed` 钉不住它）。
    std::uint64_t witness_before[4] = {};
    std::uint64_t witness_after[4] = {};
};

// 真 fill 前后 `witness[]` 的变化：涨了的 quarter 数 / 增量总和。真写了一刀 ⇒ 恰好 1 / 1。
std::uint32_t witnessDeltaQuarters(const WitnessRun& w) {
    std::uint32_t n = 0;
    for (std::uint32_t q = 0; q < 4u; ++q)
        if (w.witness_after[q] > w.witness_before[q]) ++n;
    return n;
}

std::uint64_t witnessDeltaSum(const WitnessRun& w) {
    std::uint64_t sum = 0;
    for (std::uint32_t q = 0; q < 4u; ++q) sum += w.witness_after[q] - w.witness_before[q];
    return sum;
}

WitnessRun runOneFillWitness(const std::vector<std::uint8_t>& stub, std::uint32_t stub_insns,
                             std::uint32_t f2458, std::uint32_t f2468, const char* tag) {
    WitnessRun r;
    std::string err;
    lift::Machine m;
    if (!initMachine(m, &err)) return r;
    const std::string dump = std::string("lift-audio-witness-") + tag + ".wav";
    std::remove(dump.c_str());
    if (!m.configureAudio(lift::AudioMode::Virtual, dump, /*sound=*/true, &err)) return r;
    lift::AudioEngine& a = m.audio();
    if (!a.begin(&err)) return r;
    r.begun = true;

    const std::uint32_t code_ea = 0x004B6C30u;
    const std::uint32_t sound_obj = 0x00460000u;
    const std::uint32_t stream = 0x00461000u;
    const std::uint32_t state = 0x00462000u;
    const std::uint32_t data = 0x00400000u;
    const std::uint32_t obj = 0x00463000u;
    const std::uint32_t bytes = kRingLapBytes;
    const std::uint32_t slot = 11u;
    const std::uint32_t quit = 0x00001234u;
    if (!m.placeCode(code_ea, stub.data(), stub.size(), &err)) return r;
    std::vector<std::uint8_t> zeros(0x1000, 0);
    std::vector<std::uint8_t> stream_mem(kWitnessStreamBytes, 0);
    if (!m.placeData(sound_obj, zeros.data(), zeros.size(), &err)) return r;
    if (!m.placeData(stream, stream_mem.data(), stream_mem.size(), &err)) return r;
    if (!m.placeData(state, zeros.data(), 16, &err)) return r;
    const std::vector<std::int16_t> pcm = makeRamp(bytes);
    if (!m.placeData(data, pcm.data(), bytes, &err)) return r;
    m.mem().wr32(sound_obj + 0x514u, quit);
    m.mem().wr32(sound_obj + 0x654u + 4u * slot, stream);
    m.mem().wr32(stream + 0x40Cu, obj);
    m.mem().wr32(stream + 0x444u, bytes);
    m.mem().wr32(stream + 0x2458u, f2458);  // "放完"标志（>=2 = 填静音）
    m.mem().wr32(stream + 0x2468u, f2468);  // 已写圈数（fill 前后比较的写见证）

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
    for (std::uint32_t q = 0; q < 4u; ++q) r.witness_before[q] = a.fillWitnessForTest(obj, q);
    if (!a.registerFeeder(0x004B5AA0u, sound_obj, 0x00001008u, 0x000002D0u)) return r;
    a.resumeFeeder();

    m.clock().advance100ns(2'500'000);  // 0.25 s ⇒ 不跨通知点 ⇒ 恰好一刀
    if (a.pump() != lift::AudioPump::Suspended) return r;
    lift::RunOptions opt;
    opt.max_insns = stub_insns;
    const lift::RunResult rr = m.run(opt);
    if (rr.reason != lift::StopReason::MaxInsns) return r;
    r.filled = true;

    const lift::AudioStats& st = a.stats();
    r.fills = st.fills;
    r.witnessed = st.fill_witnessed;
    r.guest_silence = st.guest_silence_fills;
    for (std::uint32_t q = 0; q < 4u; ++q) r.witness_after[q] = a.fillWitnessForTest(obj, q);
    std::remove(dump.c_str());
    return r;
}

void testFillWitnessWiring() {
    lift_test::g_current_case = "audio-device/fill-witness-wiring";
    // `inc dword ptr [ecx+2468h]` + `ret 4`（两条指令）：模拟"真写了环"的 fill。
    const std::vector<std::uint8_t> write_stub = {0xFF, 0x81, 0x68, 0x24, 0x00, 0x00, 0xC2, 0x04, 0x00};
    // 只有 `ret 4`：模拟 `[+0x2458]==0` 的 Stop 早退（不 inc ⇒ 不计见证）。
    const std::vector<std::uint8_t> stop_stub = {0xC2, 0x04, 0x00};

    // ① 写了环；[+0x2458]=1（没放完）⇒ 见证 1、guest 静音 0。
    {
        const WitnessRun w = runOneFillWitness(write_stub, 2, /*f2458=*/1u, /*f2468=*/0u, "write");
        LIFT_CHECK(w.begun && w.filled);
        LIFT_CHECK_EQ_U32(w.fills, 1u);
        LIFT_CHECK_EQ_U32(w.witnessed, 1u);
        LIFT_CHECK_EQ_U32(w.guest_silence, 0u);
        // ★ 本用例的**核心**：`noteFillWitness` 真的把"这一刀写了环"落到 `witness[quarter]` 上
        //   （stats 的 `fill_witnessed` 是另一行 ⇒ 只断言它钉不住 `++witness`）。
        LIFT_CHECK_EQ_U32(witnessDeltaQuarters(w), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(witnessDeltaSum(w)), 1u);
    }
    // ② Stop 早退（不 inc）⇒ fills=1 但见证 0。
    {
        const WitnessRun w = runOneFillWitness(stop_stub, 1, /*f2458=*/0u, /*f2468=*/0u, "stop");
        LIFT_CHECK(w.begun && w.filled);
        LIFT_CHECK_EQ_U32(w.fills, 1u);
        LIFT_CHECK_EQ_U32(w.witnessed, 0u);
        LIFT_CHECK_EQ_U32(w.guest_silence, 0u);
        // 没写环 ⇒ 写见证也不动（反方向；钉住 `pre == post ⇒ return` 与 `++witness` 的因果）。
        LIFT_CHECK_EQ_U32(witnessDeltaQuarters(w), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(witnessDeltaSum(w)), 0u);
    }
    // ③ 放完 + memset 0：**同一条 inc 路径** ⇒ 记见证，但同时记 guest 静音（≠ underrun）。
    {
        const WitnessRun w = runOneFillWitness(write_stub, 2, /*f2458=*/2u, /*f2468=*/0u, "done");
        LIFT_CHECK(w.begun && w.filled);
        LIFT_CHECK_EQ_U32(w.fills, 1u);
        LIFT_CHECK_EQ_U32(w.witnessed, 1u);
        LIFT_CHECK_EQ_U32(w.guest_silence, 1u);
        LIFT_CHECK(w.guest_silence <= w.witnessed && w.witnessed <= w.fills);
        // guest 自己"放完 + memset 0"也经同一条 `++witness` ⇒ 该 quarter 同样不判陈旧。
        LIFT_CHECK_EQ_U32(witnessDeltaQuarters(w), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(witnessDeltaSum(w)), 1u);
    }
}

}  // namespace

int main() {
    testDeviceModel();
    testMonoResample();
    testFeederLifecycle();
    testFeederContext();
    testCrossingSort();
    testRepositionAndReplay();
    testStopThenPlayWithoutSeek();
    testRemoveWaitsForInFlight();
    testRemoveWithoutInFlightDoesNotWait();
    testAddBufferSwapWaitsForInFlight();
    testDefaultMutedDelivery();
    testSoundOnDelivery();
    testMutedDeliveryParity();
    testUnderrunSilenceStale();
    testUnderrunSilenceNotTriggered();
    testUnderrunSilenceGuestZeros();
    testFillWitnessWiring();
    return lift_test::finish("audio-device");
}
