// emulator-lift · guest surface arena + headless present（P0 / G4 + 帧哈希；pure）
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"
#include "runtime/present.h"
#include "runtime/surface.h"

int main() {
  try {
    using namespace lift_test;
    lift::Machine m;
    std::string err;
    g_current_case = "setup";
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));

    lift::SurfaceArena arena;
    LIFT_CHECK(arena.init(&m.mem(), 0x28000000u, 0x01000000u));

    g_current_case = "create-32";
    lift::SurfaceFormat f32;
    f32.bpp = 32;
    const std::uint32_t s32 = arena.create(4, 3, f32);
    LIFT_CHECK(s32 != 0);
    lift::GuestSurface* s = arena.find(s32);
    LIFT_CHECK(s != nullptr);
    if (s) {
        LIFT_CHECK_EQ_I64(s->pitch, 16);
        LIFT_CHECK_EQ_I64(s->width, 4);
        LIFT_CHECK_EQ_I64(s->height, 3);
        std::uint8_t* px = arena.pixels(*s);
        LIFT_CHECK(px != nullptr);
        if (px) {
            for (std::uint32_t i = 0; i < s->pitch * 3; ++i) px[i] = static_cast<std::uint8_t>(i);
        }
        const std::uint64_t h1 = arena.hash(*s);
        LIFT_CHECK(h1 != 0);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h1 == arena.hash(*s)), 1u);
        LIFT_CHECK(arena.lock(s32));
        LIFT_CHECK(arena.lock(s32));
        LIFT_CHECK_EQ_U32(s->lock_count, 2u);
        LIFT_CHECK(arena.unlock(s32));
        LIFT_CHECK(arena.unlock(s32));
        LIFT_CHECK(!arena.unlock(s32));

        g_current_case = "bmp";
        namespace fs = std::filesystem;
        const fs::path out = fs::temp_directory_path() / "lift-surface-test.bmp";
        std::string berr;
        LIFT_CHECK(arena.dumpBmp(*s, out.string(), &berr));
        std::ifstream f(out, std::ios::binary);
        std::vector<std::uint8_t> hdr(54);
        f.read(reinterpret_cast<char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
        LIFT_CHECK(hdr.size() == 54 && hdr[0] == 'B' && hdr[1] == 'M');
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hdr[28]), 32);  // biBitCount
        // ★ 必须先关文件再删（2026-11-04 实测）：POSIX 上 unlink 打开中的文件没问题，
        //   **Windows 上是共享冲突** ⇒ `fs::remove` 抛 `filesystem_error` ⇒ 未捕获 ⇒ std::terminate
        //   ⇒ abort（无输出、退出码 3，mt 上曾被当成"卡死"）。同理 `fs::remove` 用 error_code 重载，
        //   让"清理失败"只记一条日志、⛔ 不许把整洁性变成整轮挂掉。
        f.close();
        std::error_code rmec;
        fs::remove(out, rmec);
        if (rmec) std::fprintf(stderr, "note: 临时 BMP 没删掉：%s\n", rmec.message().c_str());
    }

    g_current_case = "present";
    lift::HeadlessPresenter pres;
    std::string perr;
    LIFT_CHECK(pres.present(arena, *s, "", &perr));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(pres.frameCount()), 1u);
    LIFT_CHECK(pres.lastHash() != 0);

    g_current_case = "reuse";
    LIFT_CHECK(arena.destroy(s32));
    LIFT_CHECK(arena.find(s32) == nullptr);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(arena.surfaceCount()), 0);
    lift::SurfaceFormat f16;
    f16.bpp = 16;
    f16.rmask = 0xF800;
    f16.gmask = 0x07E0;
    f16.bmask = 0x001F;
    f16.amask = 0;
    const std::uint32_t s16 = arena.create(3, 3, f16);
    LIFT_CHECK(s16 != 0);
    lift::GuestSurface* s2 = arena.find(s16);
    LIFT_CHECK(s2 != nullptr);
    if (s2) {
        LIFT_CHECK_EQ_I64(s2->pitch, 8);  // 3*2 = 6 → 8
        LIFT_CHECK_EQ_U32(s2->format.bpp, 16u);
    }
    LIFT_CHECK(arena.destroy(s16));

    g_current_case = "bad";
    LIFT_CHECK(arena.create(0, 3, f32) == 0);
    LIFT_CHECK(arena.create(4, 3, lift::SurfaceFormat{}) != 0);  // 默认 32bpp 合法
    LIFT_CHECK(arena.destroy(0x12345678u) == false);

    return finish("surface-present");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "DIAG uncaught non-std exception\n");
    return 1;
  }
}
