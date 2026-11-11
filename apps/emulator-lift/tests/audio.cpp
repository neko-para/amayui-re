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
//
// ★ 会红：把 `fireCrossings` 的 sort 去掉 ⇒ ① 的回绕 0 会排在 3 之前；
//   把 `generateTo` 的声道/采样率转换去掉 ⇒ ③ 红；把 removeBuffer 的 pending 清理去掉 ⇒ ⑤ 红；
//   把 `stop()` 的 `dropPending` 去掉 ⇒ `audio-device/stop-then-play-without-seek` 红
//   （旧 q1 + 新 q1 ⇒ pending().size()==2）。
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

}  // namespace

int main() {
    testDeviceModel();
    testMonoResample();
    testFeederLifecycle();
    testFeederContext();
    testCrossingSort();
    testRepositionAndReplay();
    testStopThenPlayWithoutSeek();
    testDefaultMutedDelivery();
    testSoundOnDelivery();
    testMutedDeliveryParity();
    return lift_test::finish("audio-device");
}
