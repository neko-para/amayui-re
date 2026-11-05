// emulator-lift · PresentSink 契约（pure；不初始化 SDL 视频）
//
// 判据（红了意味着什么）：
//   ① 无 sink 时 `present()` 的 frameCount / lastHash 与今天一致；
//   ② 有 sink 时每帧恰好调用一次，且 sink 看到的 surface 与 arena 里的同一份；
//   ③ sink 返回 false 时 `present()` 仍返回 true（记录成功），只累加 failures，3 次后 latch；
//   ④ headless 路径不初始化 SDL 视频（防止“默认路径被顺手打开窗口”）。
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "check.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/pixel_format.h"
#include "runtime/present.h"
#include "runtime/surface.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <SDL3/SDL.h>

namespace {

class FakeSink final : public lift::PresentSink {
public:
    int calls = 0;
    int pumps = 0;
    bool fail = false;
    bool stop = false;
    std::uint64_t last_frame_hash = 0;
    std::uint32_t last_ea = 0;
    int last_w = 0;
    int last_h = 0;
    std::uint32_t last_pitch = 0;
    std::uint32_t last_first_pixel = 0;

    bool submitFrame(const lift::SurfaceArena& arena, const lift::GuestSurface& surface,
                     std::string* err) override {
        ++calls;
        last_ea = surface.ea;
        last_w = surface.width;
        last_h = surface.height;
        last_pitch = surface.pitch;
        const std::uint8_t* p = arena.pixels(surface);
        if (p != nullptr && surface.pitch >= 4u) std::memcpy(&last_first_pixel, p, 4);
        last_frame_hash = arena.hash(surface);
        if (fail) {
            if (err) *err = "fake sink requested failure";
            return false;
        }
        return true;
    }
    void pumpHostEvents() override { ++pumps; }
    bool stopRequested() const override { return stop; }
    std::string describe() const override { return "fake"; }
};

struct Fixture {
    lift::GuestMemory mem;
    lift::SurfaceArena arena;
    std::uint32_t ea = 0;
    lift::GuestSurface* surface = nullptr;
    bool ok = false;
};

bool makeFixture(Fixture* f, int w = 8, int h = 8, lift::SurfaceFormat fmt = lift::pixel_format::formatForBpp(32)) {
    f->ok = f->mem.reserve() && f->arena.init(&f->mem, 0x10000000u, 0x00100000u);
    if (!f->ok) return false;
    f->ea = f->arena.create(w, h, fmt);
    f->surface = f->arena.find(f->ea);
    return f->ea != 0 && f->surface != nullptr;
}

}  // namespace

int main() {
    Fixture f;
    LIFT_CHECK(makeFixture(&f));
    if (f.surface == nullptr) {
        LIFT_CHECK(false);
        return lift_test::finish("present-sink");
    }
    auto* p = f.arena.pixels(*f.surface);
    LIFT_CHECK(p != nullptr);
    std::memset(p, 0xAB, static_cast<std::size_t>(f.surface->pitch) * f.surface->height);

    lift::HeadlessPresenter presenter;
    std::string err;
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    const std::uint64_t hash_without = presenter.lastHash();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(presenter.frameCount()), 1);
    LIFT_CHECK(!presenter.hasSink());

    auto sink = std::make_unique<FakeSink>();
    FakeSink* raw = sink.get();
    presenter.setSink(std::move(sink));
    LIFT_CHECK(presenter.hasSink());

    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(presenter.frameCount()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(presenter.lastHash()), static_cast<std::int64_t>(hash_without));
    LIFT_CHECK_EQ_U32(raw->calls, 1u);
    LIFT_CHECK_EQ_U32(raw->last_ea, f.ea);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(raw->last_w), static_cast<std::uint32_t>(f.surface->width));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(raw->last_h), static_cast<std::uint32_t>(f.surface->height));
    LIFT_CHECK_EQ_U32(raw->last_pitch, f.surface->pitch);
    LIFT_CHECK_EQ_U32(raw->last_first_pixel, 0xABABABABu);

    presenter.pumpHostEvents();
    LIFT_CHECK_EQ_U32(raw->pumps, 1u);
    raw->stop = true;
    LIFT_CHECK(presenter.stopRequested());
    raw->stop = false;
    LIFT_CHECK(!presenter.stopRequested());

    // ③ sink 失败：present 仍成功，failure 累加；3 次后 latch，不再调用。
    raw->fail = true;
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    LIFT_CHECK_EQ_U32(raw->calls, 2u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.sinkFailures()), 1u);
    LIFT_CHECK(!presenter.lastSinkError().empty());
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    const int calls_at_latch = raw->calls;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.sinkFailures()), 3u);
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    LIFT_CHECK_EQ_U32(raw->calls, static_cast<std::uint32_t>(calls_at_latch));  // latched
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));                  // 仍然成功
    // latch 后事件泵/关窗仍必须工作（否则窗口关不掉）。
    const int pumps_before = raw->pumps;
    presenter.pumpHostEvents();
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(raw->pumps), static_cast<std::uint32_t>(pumps_before + 1));
    raw->stop = true;
    LIFT_CHECK(presenter.stopRequested());
    raw->stop = false;

    presenter.clearSink();
    LIFT_CHECK(!presenter.hasSink());
    LIFT_CHECK(presenter.present(f.arena, *f.surface, "", &err));
    LIFT_CHECK_EQ_U32(presenter.sinkFailures(), 0u);

    // ---- ④ Machine 块边界：sink 的 stopRequested ⇒ StopReason::Stopped（关窗不能落成 None/exit 2）----
    {
        lift::Machine m;
        std::string merr;
        LIFT_CHECK(m.initBare(&merr));
        LIFT_CHECK(m.prepareStack(&merr));
        const std::uint8_t loop[] = {0xEB, 0xFE};  // jmp $
        LIFT_CHECK(m.placeCode(0x00401000u, loop, sizeof(loop), &merr));
        auto stop_sink = std::make_unique<FakeSink>();
        FakeSink* ss = stop_sink.get();
        ss->stop = false;
        m.presenter().setSink(std::move(stop_sink));
        m.cpu().eip = 0x00401000u;
        lift::RunOptions opt;
        opt.max_insns = 100000u;
        // 让 sink 在第一次 pump 时请求停止（模拟用户关窗）。
        ss->stop = true;
        const lift::RunResult r = m.run(opt);
        LIFT_CHECK(r.reason == lift::StopReason::Stopped);
        LIFT_CHECK(r.detail.find("窗口关闭") != std::string::npos);
        LIFT_CHECK(r.ok());
    }

    // ④ headless 路径没有初始化 SDL 视频（本测试进程也从未 init）。
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);

    return lift_test::finish("present-sink");
}
