// emulator-lift · Present 的**按需回读**（`[present-readback]` Phase A）
//
// 判据（红了意味着什么）：`d3d9::present` 的 needs 谓词（"这一帧有没有消费者要 guest 后缓冲的 CPU
//   字节"）接线漏了一个消费者 ⇒ 静默旧帧 / 旧 hash / dump 写旧帧。⛔ 不抽纯函数（R5：风险面是接线），
//   所以这里用 **Mock GpuRaster + 真 D3D9 设备夹具**走 `d3d9::present` 的真分派。
//
//   · `present-readback/gate-no-consumer`：无 sink / 无 dump / 无 frame-hash ⇒ **不下载**
//     （`sync==0 && flush==1`），`lastHash()==0`（显式未算，⛔ 不是旧值）。
//     改动前红：老代码无条件 `rasterSyncToGuest` ⇒ `sync==1`。
//   · `present-readback/gate-dump-fresh`：`cpu.shim.frame_dump` 非空 ⇒ `sync==1`，且 BMP 像素是
//     **本帧**的 sentinel（= 先材质化再写盘）。needs 谓词漏 `dump_path` ⇒ BMP 不存在 / 是旧字节 ⇒ 红。
//   · `present-readback/dump-refuses-stale`：`present(..., materialized=false)` + dump ⇒ `dumpFailures()`
//     计数、**不产生文件**（⛔ 不静默写旧帧）。改动前红：今天会写出旧字节。
//   · `present-readback/last-hash-pending`：跳过回读后 `lastHash()==0`；`captureLastFrameHash` 补算后
//     == sentinel 那一帧的 hash。+ `capture-target-mismatch`：镜像 target-base 不匹配 ⇒ `hashFailures()`
//     +1、`lastHash()==0`、⛔ 不往 arena 写。
//   · `present-readback/readback-failure-no-hash`（复审 W1）：`rasterSyncToGuest` 返回 false ⇒
//     `lastHash()==0`、`hasFrameHash()==false`（⛔ 不拿旧字节算 hash；收口会重试）；要 dump 时也拒写、
//     **不产生 BMP**。修前红：旧字节 hash 被记成 `hash_valid_`。
//   · `present-readback/dump-last-frame-no-recorded`（复审 W4）：退回路径没有 Present 时 recorded hash ⇒
//     `hash_still_matches==false`，`hashFailures()` 每次 +1，但 warn **只一次**。
//   · `present-readback/gate-sink`：`PresentSink::needsGuestPixels()==false` ⇒ sync 0；`true` ⇒ sync 1
//     （renderer 回退路径的接线）。
//
// ★ 为什么在 d3d9 分派层守：`HeadlessPresenter` 自己只能守"拿到的是不是这一帧"；"要不要拿"是
//   `d3d9.cpp` 的接线，漏了不会报错、只有旧帧（`[present-readback]` 的静默错面）。
// ★ 夹具：裸 `GuestMemory` + `SurfaceArena` + 自造 guest 机器码调 COM 槽；不碰 SDL / GPU / 游戏。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>  // _dup / _dup2 / _fileno / _close / _write（stderr 捕获用）
#else
#include <unistd.h>  // dup / dup2 / fileno / close / write
#endif

#include "check.h"
#include "fixture.h"
#include "runtime/d3d9.h"
#include "runtime/gpu_raster.h"
#include "runtime/guest_token.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present.h"
#include "runtime/surface.h"
#include "shims/register.h"

namespace {

constexpr std::uint8_t kSentinelByte = 0xAB;
constexpr std::uint32_t kSentinelWord = 0xABABABABu;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

// ---------------------------------------------------------------------------------------
// Mock 后端：把"GPU 镜像里有本帧绘制"变成可数的 sync / flush；sync 时把 sentinel 写进 guest
// （= 真实后端的 `rasterSyncToGuest` 下载这一帧）。身份（`rasterTargetBase`）由夹具设成后缓冲基址。
// ---------------------------------------------------------------------------------------
class MockRaster final : public lift::GpuRaster {
public:
    bool rasterActive() const override { return true; }
    const char* rasterName() const override { return "mock-present-readback"; }
    bool rasterPrepare(const lift::SurfaceFormat&, int, int, std::string*, const std::uint8_t*) override {
        return true;
    }
    bool rasterSyncFromGuest(const std::uint8_t*, std::uint32_t, int, int, std::string*) override {
        return true;
    }
    bool rasterSyncToGuest(std::uint8_t* pixels, std::uint32_t pitch, int width, int height,
                           std::string*) override {
        ++sync_count;
        // ★ 可配失败（复审 W1 的用例）：读回失败时 guest 必须仍是旧字节。
        if (fail_sync) return false;
        // 32bpp（X8R8G8B8）：每像素 4 字节，逐行按 pitch 铺 sentinel。
        if (pixels != nullptr && width > 0 && height > 0) {
            for (int y = 0; y < height; ++y) {
                std::memset(pixels + static_cast<std::size_t>(y) * pitch, kSentinelByte,
                            static_cast<std::size_t>(width) * 4u);
            }
        }
        dirty = false;
        return true;
    }
    bool rasterDraw(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterState&,
                    std::string*) override {
        return true;
    }
    bool rasterDrawTextured(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterTexture&,
                            const lift::GpuRasterState&, std::string*) override {
        return true;
    }
    bool rasterFlush(std::string*) override {
        ++flush_count;
        return true;
    }
    bool rasterDirty() const override { return dirty; }
    const void* rasterTargetBase() const override { return target_base; }

    int sync_count = 0;
    int flush_count = 0;
    bool dirty = false;
    bool fail_sync = false;
    const void* target_base = nullptr;
};

// 测试替身 sink：`needsGuestPixels` 可配（守的是 `d3d9.cpp` 有没有问它）。
class NeedsSink final : public lift::PresentSink {
public:
    bool needs = false;
    int submitted = 0;
    bool submitFrame(const lift::SurfaceArena&, const lift::GuestSurface&, std::string*) override {
        ++submitted;
        return true;
    }
    bool needsGuestPixels() const override { return needs; }
};

// ---------------------------------------------------------------------------------------
// stderr 捕获（跨平台 dup/dup2）：数"只第一次 warn"（形状照 tests/d3dx9_effect.cpp 的 StderrTee）。
//   ★ 用完把捕获内容**回写**真 stderr ⇒ 断言失败时的现场不会丢。
// ---------------------------------------------------------------------------------------
class StderrCapture {
public:
#if defined(_WIN32)
    using FdT = int;
    static FdT dupFd(int fd) { return _dup(fd); }
    static int dup2Fd(int from, int to) { return _dup2(from, to); }
    static int filenoOf(std::FILE* f) { return _fileno(f); }
    static void closeFd(FdT fd) { _close(fd); }
    static int writeFd(FdT fd, const void* buf, unsigned n) { return _write(fd, buf, n); }
#else
    using FdT = int;
    static FdT dupFd(int fd) { return ::dup(fd); }
    static int dup2Fd(int from, int to) { return ::dup2(from, to); }
    static int filenoOf(std::FILE* f) { return ::fileno(f); }
    static void closeFd(FdT fd) { ::close(fd); }
    static int writeFd(FdT fd, const void* buf, unsigned n) {
        return static_cast<int>(::write(fd, buf, n));
    }
#endif

    bool begin() {
        tmp_ = std::tmpfile();
        if (tmp_ == nullptr) return false;
        std::fflush(stderr);
        saved_ = dupFd(2);
        if (saved_ < 0) {
            std::fclose(tmp_);
            tmp_ = nullptr;
            return false;
        }
        if (dup2Fd(filenoOf(tmp_), 2) < 0) {
            closeFd(saved_);
            saved_ = -1;
            std::fclose(tmp_);
            tmp_ = nullptr;
            return false;
        }
        active_ = true;
        return true;
    }

    std::string finish() {
        std::string log;
        if (!active_) return log;
        std::fflush(stderr);
        std::fseek(tmp_, 0, SEEK_END);
        const long size = std::ftell(tmp_);
        if (size > 0) {
            std::fseek(tmp_, 0, SEEK_SET);
            std::vector<char> chunk(static_cast<std::size_t>(size));
            const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), tmp_);
            log.assign(chunk.data(), got);
            std::size_t off = 0;
            while (off < got) {  // 回写真 stderr（现场不丢）
                const int n = writeFd(saved_, chunk.data() + off, static_cast<unsigned>(got - off));
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
        }
        dup2Fd(saved_, 2);
        closeFd(saved_);
        saved_ = -1;
        active_ = false;
        std::fclose(tmp_);
        tmp_ = nullptr;
        return log;
    }

private:
    std::FILE* tmp_ = nullptr;
    FdT saved_ = -1;
    bool active_ = false;
};

std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------------------
// guest 侧的 COM 调用：`mov eax,[vtbl]; push args…; push obj; call [eax+off]; ret`
// （形状照 `tests/d3dx9_effect.cpp`；⛔ 不直接调宿主函数 —— 测的是 guest 可见的分派）
// ---------------------------------------------------------------------------------------
struct GuestAsm {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void callMemEax(std::uint32_t off) {
        if (off <= 0x7Fu) {
            u8(0xFF);
            u8(0x50);
            u8(static_cast<std::uint8_t>(off));
        } else {
            u8(0xFF);
            u8(0x90);
            u32(off);
        }
    }
    void ret() { u8(0xC3); }
};

std::uint32_t placeAndRun(lift_fixture::Fixture& f, GuestAsm& a) {
    const std::uint32_t code_ea = f.code_cursor;
    f.code_cursor += 0x100u;
    if (!f.m.placeCode(code_ea, a.b.data(), a.b.size(), &f.err)) {
        f.ok = false;
        return 0;
    }
    f.rearmStack();
    f.m.cpu().eip = code_ea;
    lift::RunOptions o;
    o.until_ea = lift_fixture::kSentinel;
    o.max_insns = 200000;
    const lift::RunResult r = f.m.run(o);
    if (r.reason != lift::StopReason::UntilEa) {
        f.ok = false;
        f.err = std::string("意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
    }
    return f.m.cpu().eax();
}

std::uint32_t callMethod(lift_fixture::Fixture& f, std::uint32_t obj, std::uint32_t off,
                         std::initializer_list<std::uint32_t> args) {
    const std::uint32_t vtbl = f.m.mem().rd32(obj);
    GuestAsm a;
    a.movEaxImm(vtbl);
    const std::vector<std::uint32_t> v(args);
    for (auto it = v.rbegin(); it != v.rend(); ++it) a.pushImm(*it);
    a.pushImm(obj);
    a.callMemEax(off);
    a.ret();
    return placeAndRun(f, a);
}

// 64×64 / X8R8G8B8 的 D3D9 设备（Present = 槽 0x44）。
bool createDevice(lift_fixture::Fixture& f, std::uint32_t d3d9, std::uint32_t* out) {
    std::vector<std::uint8_t> pp(lift::d3d9::kPresentParamsBytes, 0);
    auto wr = [&pp](std::uint32_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            pp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu);
    };
    wr(lift::d3d9::kPpBackBufferWidth, 64u);
    wr(lift::d3d9::kPpBackBufferHeight, 64u);
    wr(lift::d3d9::kPpBackBufferFormat, lift::d3d9::kFmtX8R8G8B8);
    wr(lift::d3d9::kPpBackBufferCount, 1u);
    wr(lift::d3d9::kPpSwapEffect, 1u);  // D3DSWAPEFFECT_DISCARD
    wr(lift::d3d9::kPpHDeviceWindow, 0x00001234u);
    wr(lift::d3d9::kPpWindowed, 1u);
    wr(lift::d3d9::kPpEnableAutoDepthStencil, 1u);
    wr(lift::d3d9::kPpAutoDepthStencilFormat, 75u);  // D3DFMT_D24S8
    wr(lift::d3d9::kPpFlags, lift::d3d9::kPresentFlagLockableBackbuffer);
    wr(lift::d3d9::kPpPresentationInterval, 0x80000000u);
    const std::uint32_t pp_ea = f.data(pp.data(), pp.size());
    const std::uint32_t out_ea = f.u32data(0u);
    if (callMethod(f, d3d9, 0x40u, {0u, 1u, 0x00001234u, 0x44u, pp_ea, out_ea}) != lift::d3d9::kOk) return false;
    *out = f.m.mem().rd32(out_ea);
    return *out != 0;
}

// ---------------------------------------------------------------------------------------
// 夹具：机器 + d3d9 桩 + 设备 + mock 后端（dirty = 本帧有 GPU 绘制，target 是后缓冲）
// ---------------------------------------------------------------------------------------
struct Fixture {
    lift_fixture::Fixture f;
    MockRaster mock;
    std::uint32_t d3d9 = 0;
    std::uint32_t device = 0;
    std::uint32_t state = 0;
    lift::d3d9::DeviceInfo info;

    ~Fixture() { f.m.setGpuRaster(nullptr); }

    bool setup() {
        if (!f.ok) return false;
        if (!f.m.heap().init(&f.m.mem(), f.m.layout().heap_base, kFixtureHeapBytes)) return false;
        f.m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerD3D9Shims(f.m.shims());
        const std::uint32_t proc = f.use("d3d9", "Direct3DCreate9");
        if (proc == 0) return false;
        d3d9 = f.call(proc, {lift::d3d9::kSdkVersion});
        if (d3d9 == 0) return false;
        if (!createDevice(f, d3d9, &device)) return false;
        state = f.m.cpu().com->userOf(device);
        if (state == 0) return false;
        if (!lift::d3d9::deviceInfoOfState(f.m.cpu(), state, &info, nullptr)) return false;
        lift::GuestSurface* gs = f.m.surfaces().find(info.arena);
        if (gs == nullptr) return false;
        f.m.setGpuRaster(&mock);
        mock.target_base = f.m.surfaces().pixels(*gs);
        mock.dirty = true;  // 模拟"这一帧在 GPU 上画过"（Present 的 dirty 分支）
        return true;
    }

    lift::GuestSurface* backBuffer() { return f.m.surfaces().find(info.arena); }

    std::uint32_t present() { return callMethod(f, device, 0x44u, {0u, 0u, 0u, 0u}); }
};

std::string readFile(const char* path) {
    std::FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) return {};
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) != 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}

// ---------------------------------------------------------------------------------------
// ① 无消费者 ⇒ 只 flush、不下载
// ---------------------------------------------------------------------------------------
void caseNoConsumer() {
    Fixture x;
    LIFT_CHECK(x.setup());
    if (!x.f.ok) return;
    LIFT_CHECK_EQ_U32(x.present(), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 0u);   // ⛔ 没有整幅回读
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.flush_count), 1u);  // 队列真的落了 target
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.f.m.presenter().frameCount()), 1u);
    LIFT_CHECK(x.f.m.presenter().lastHash() == 0);  // 未算（⛔ 不是旧 hash）
    std::printf("[gate-no-consumer] sync=%d flush=%d lastHash=0x%llX\n", x.mock.sync_count, x.mock.flush_count,
                static_cast<unsigned long long>(x.f.m.presenter().lastHash()));
}

// ---------------------------------------------------------------------------------------
// ② dump 要这一帧 ⇒ 先回读再写盘（BMP 里必须是 sentinel）
// ---------------------------------------------------------------------------------------
void caseDumpFresh() {
    Fixture x;
    LIFT_CHECK(x.setup());
    if (!x.f.ok) return;
    const char* path = "lift-present-readback-fresh.bmp";
    std::remove(path);
    x.f.m.cpu().shim.frame_dump = path;
    LIFT_CHECK_EQ_U32(x.present(), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 1u);
    const std::string bmp = readFile(path);
    LIFT_CHECK(!bmp.empty());
    // BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40) ⇒ 第一行像素从 54 开始；32bpp 写 B,G,R,0。
    if (bmp.size() > 57) {
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[54])), 0xABu);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[55])), 0xABu);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(static_cast<unsigned char>(bmp[56])), 0xABu);
    }
    // 同一帧的 guest 后缓冲也是 sentinel（= 先材质化，再写盘）。
    LIFT_CHECK_EQ_U32(x.f.m.mem().rd32(x.info.arena), kSentinelWord);
    LIFT_CHECK(x.f.m.presenter().lastDump() == path);
    std::remove(path);
    std::printf("[gate-dump-fresh] sync=%d bmp=%zu bytes\n", x.mock.sync_count, bmp.size());
}

// ---------------------------------------------------------------------------------------
// ③ 没回读却要 dump ⇒ 拒写 + 计数（⛔ 不静默写旧帧）
// ---------------------------------------------------------------------------------------
void caseDumpRefusesStale() {
    lift::GuestMemory mem;
    LIFT_CHECK(mem.reserve());
    lift::SurfaceArena arena;
    LIFT_CHECK(arena.init(&mem, 0x28000000u, 0x00400000u));
    std::uint32_t ea = 0;
    lift::GuestSurface* surface = nullptr;
    {
        lift::GuestToken::Guard guard(lift::guestToken());
        ea = arena.create(4, 4, lift::SurfaceFormat{});
        if (ea != 0) surface = arena.find(ea);
    }
    LIFT_CHECK_NE_U32(ea, 0u);
    LIFT_CHECK(surface != nullptr);
    if (surface == nullptr) return;
    const char* path = "lift-present-readback-stale.bmp";
    std::remove(path);
    lift::HeadlessPresenter presenter;
    std::string err;
    LIFT_CHECK(presenter.present(arena, *surface, path, &err, /*guest_pixels_materialized=*/false));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.dumpFailures()), 1u);
    LIFT_CHECK(!err.empty());
    LIFT_CHECK(presenter.lastDump().empty());
    LIFT_CHECK(std::fopen(path, "rb") == nullptr);  // ⛔ 一个字节都没写
    LIFT_CHECK(presenter.lastHash() == 0);
    // 第二帧照旧：失败只计数（只第一次 warn，⛔ 不刷屏）
    LIFT_CHECK(presenter.present(arena, *surface, path, &err, false));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.dumpFailures()), 2u);
    LIFT_CHECK(std::fopen(path, "rb") == nullptr);
    {
        lift::GuestToken::Guard guard(lift::guestToken());
        (void)arena.destroy(ea);
    }
    std::printf("[dump-refuses-stale] dumpFailures=%llu\n",
                static_cast<unsigned long long>(presenter.dumpFailures()));
}

// ---------------------------------------------------------------------------------------
// ④ 惰性 lastHash：跳过回读 ⇒ 0；显式补算 ⇒ sentinel 那一帧的 hash
// ---------------------------------------------------------------------------------------
void caseLastHashPending() {
    Fixture x;
    LIFT_CHECK(x.setup());
    if (!x.f.ok) return;
    LIFT_CHECK_EQ_U32(x.present(), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 0u);
    LIFT_CHECK(x.f.m.presenter().lastHash() == 0);
    LIFT_CHECK(!x.f.m.presenter().hasFrameHash());
    // 显式补算：只在 target-base 等式成立时写回 guest。
    std::string err;
    LIFT_CHECK(x.f.m.presenter().captureLastFrameHash(x.f.m.gpuRaster(), &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 1u);
    const lift::GuestSurface* gs = x.backBuffer();
    LIFT_CHECK(gs != nullptr);
    if (gs == nullptr) return;
    LIFT_CHECK_EQ_U32(x.f.m.mem().rd32(x.info.arena), kSentinelWord);
    LIFT_CHECK(x.f.m.presenter().lastHash() == x.f.m.surfaces().hash(*gs));
    LIFT_CHECK(x.f.m.presenter().lastHash() != 0);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.f.m.presenter().hashFailures()), 0u);
    std::printf("[last-hash-pending] captured=0x%llX hashFailures=%llu\n",
                static_cast<unsigned long long>(x.f.m.presenter().lastHash()),
                static_cast<unsigned long long>(x.f.m.presenter().hashFailures()));

    // ④b target-base 不匹配 ⇒ ⛔ 不写错 arena、hashFailures+1、lastHash 保持 0
    lift_test::g_current_case = "present-readback/capture-target-mismatch";
    Fixture y;
    LIFT_CHECK(y.setup());
    if (!y.f.ok) return;
    LIFT_CHECK_EQ_U32(y.present(), 0u);
    y.mock.target_base = nullptr;  // 镜像身份不再是这块后缓冲
    const std::uint32_t before = y.f.m.mem().rd32(y.info.arena);
    std::string err2;
    LIFT_CHECK(!y.f.m.presenter().captureLastFrameHash(y.f.m.gpuRaster(), &err2));
    LIFT_CHECK(!err2.empty());
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(y.f.m.presenter().hashFailures()), 1u);
    LIFT_CHECK(y.f.m.presenter().lastHash() == 0);
    LIFT_CHECK_EQ_U32(y.f.m.mem().rd32(y.info.arena), before);  // guest 一个字节没被写
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(y.mock.sync_count), 0u);
    std::printf("[capture-target-mismatch] hashFailures=%llu lastHash=0x%llX\n",
                static_cast<unsigned long long>(y.f.m.presenter().hashFailures()),
                static_cast<unsigned long long>(y.f.m.presenter().lastHash()));
}

// ---------------------------------------------------------------------------------------
// ⑤ sink 的 needsGuestPixels 真的被问（renderer 回退路径的接线）
// ---------------------------------------------------------------------------------------
void caseGateSink() {
    {
        Fixture x;
        LIFT_CHECK(x.setup());
        if (!x.f.ok) return;
        auto sink = std::make_unique<NeedsSink>();
        sink->needs = false;
        NeedsSink* raw = sink.get();
        x.f.m.presenter().setSink(std::move(sink));
        LIFT_CHECK_EQ_U32(x.present(), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(raw->submitted), 1u);
        std::printf("[gate-sink] needs=0 sync=%d\n", x.mock.sync_count);
    }
    {
        Fixture x;
        LIFT_CHECK(x.setup());
        if (!x.f.ok) return;
        auto sink = std::make_unique<NeedsSink>();
        sink->needs = true;
        NeedsSink* raw = sink.get();
        x.f.m.presenter().setSink(std::move(sink));
        LIFT_CHECK_EQ_U32(x.present(), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 1u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(raw->submitted), 1u);
        std::printf("[gate-sink] needs=1 sync=%d\n", x.mock.sync_count);
    }
}

// ---------------------------------------------------------------------------------------
// ⑥ 读回**失败** ⇒ 那一帧的 hash 记为"没算"（复审 W1）
//
// 红意味着什么：`compute_hash` 没看 `materialized` ⇒ 拿**旧 guest 字节**算一个 hash、置 `hash_valid_`,
//   于是 `lastHash()` 把它当本帧、`hasFrameHash()` 变真使收口不再重试 ⇒ 摘要里的 hash 与帧对不上，
//   而且没有任何症状（旧 hash 看着完全正常）。
// ---------------------------------------------------------------------------------------
void caseReadbackFailureNoHash() {
    {
        Fixture x;
        LIFT_CHECK(x.setup());
        if (!x.f.ok) return;
        x.f.m.presenter().setFrameHash(true);  // 让 needs 谓词真的去回读（否则 Present 根本不下载）
        x.mock.fail_sync = true;
        LIFT_CHECK_EQ_U32(x.present(), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.sync_count), 1u);   // 真的试过读回
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.mock.flush_count), 0u);  // 读回失败不是"只 flush"
        LIFT_CHECK(x.f.m.presenter().lastHash() == 0);   // ⛔ 不是旧字节的 hash（W1 修前红）
        LIFT_CHECK(!x.f.m.presenter().hasFrameHash());   // ⇒ 收口补算会重试
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.f.m.presenter().dumpFailures()), 0u);  // 本块没 dump
        LIFT_CHECK(std::fopen("lift-present-readback-fail.bmp", "rb") == nullptr);
        // 失败解除之后，收口补算真的会重试（前提就是上面 hasFrameHash()==false）
        x.mock.fail_sync = false;
        std::string err;
        LIFT_CHECK(x.f.m.presenter().captureLastFrameHash(x.f.m.gpuRaster(), &err));
        LIFT_CHECK(x.f.m.presenter().lastHash() != 0);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.f.m.presenter().hashFailures()), 0u);
        std::printf("[readback-failure-no-hash] failed lastHash=0 → retried ok\n");
    }
    {
        // 读回失败 + 要 dump ⇒ 拒写、**不产生 BMP**（同一帧的旧字节不许写盘）
        Fixture x;
        LIFT_CHECK(x.setup());
        if (!x.f.ok) return;
        const char* path = "lift-present-readback-fail.bmp";
        std::remove(path);
        x.f.m.cpu().shim.frame_dump = path;
        x.mock.fail_sync = true;
        LIFT_CHECK_EQ_U32(x.present(), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(x.f.m.presenter().dumpFailures()), 1u);
        LIFT_CHECK(std::fopen(path, "rb") == nullptr);
        LIFT_CHECK(x.f.m.presenter().lastHash() == 0);
        std::printf("[readback-failure-no-hash] dumpFailures=%llu no-bmp\n",
                    static_cast<unsigned long long>(x.f.m.presenter().dumpFailures()));
    }
}

// ---------------------------------------------------------------------------------------
// ⑦ `dumpLastFrame` 的退回路径**没有 Present 时的 recorded hash** ⇒ `hash_still_matches=false`
//    （复审 W4）：⛔ 不拿"现在现算的值"冒充同一帧；计数随每次读 +1，但 warn **只一次**。
// ---------------------------------------------------------------------------------------
void caseDumpNoRecordedHash() {
    lift::GuestMemory mem;
    LIFT_CHECK(mem.reserve());
    lift::SurfaceArena arena;
    LIFT_CHECK(arena.init(&mem, 0x28000000u, 0x00400000u));
    std::uint32_t ea = 0;
    lift::GuestSurface* surface = nullptr;
    {
        lift::GuestToken::Guard guard(lift::guestToken());
        ea = arena.create(4, 4, lift::SurfaceFormat{});
        if (ea != 0) surface = arena.find(ea);
    }
    LIFT_CHECK(surface != nullptr);
    if (surface == nullptr) return;
    lift::HeadlessPresenter presenter;  // ⛔ 不 setFrameHash、不 freeze ⇒ Present 时 recorded = 0
    std::string err;
    LIFT_CHECK(presenter.present(arena, *surface, std::string(), &err));
    const char* path = "lift-present-readback-no-recorded.bmp";
    std::remove(path);
    bool m1 = true;
    bool m2 = true;
    StderrCapture cap;
    LIFT_CHECK(cap.begin());
    LIFT_CHECK(presenter.dumpLastFrame(path, &m1, &err));
    LIFT_CHECK(presenter.dumpLastFrame(path, &m2, &err));
    const std::string log = cap.finish();
    LIFT_CHECK(!m1);  // ⛔ 不拿现算值冒充同一帧
    LIFT_CHECK(!m2);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.hashFailures()), 2u);  // 每次读都计数
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(countOccurrences(log, "最后帧 hash 不可用")), 1u);  // 只 warn 一次
    LIFT_CHECK(std::fopen(path, "rb") != nullptr);  // 帧照旧写（退回现场 = 今天的行为）
    std::remove(path);
    {
        lift::GuestToken::Guard guard(lift::guestToken());
        (void)arena.destroy(ea);
    }
    std::printf("[dump-no-recorded-hash] matches=0/0 hashFailures=%llu warns=%zu\n",
                static_cast<unsigned long long>(presenter.hashFailures()),
                countOccurrences(log, "最后帧 hash 不可用"));
}

}  // namespace

int main() {
    using namespace lift_test;

    g_current_case = "present-readback/gate-no-consumer";
    caseNoConsumer();
    g_current_case = "present-readback/gate-dump-fresh";
    caseDumpFresh();
    g_current_case = "present-readback/dump-refuses-stale";
    caseDumpRefusesStale();
    g_current_case = "present-readback/last-hash-pending";
    caseLastHashPending();
    g_current_case = "present-readback/readback-failure-no-hash";
    caseReadbackFailureNoHash();
    g_current_case = "present-readback/dump-last-frame-no-recorded";
    caseDumpNoRecordedHash();
    g_current_case = "present-readback/gate-sink";
    caseGateSink();

    return finish("present-readback");
}
