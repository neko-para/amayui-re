#include "runtime/surface.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "runtime/vfs.h"

namespace lift {
namespace {
void write16(std::FILE* f, std::uint16_t v) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>(v >> 8)};
    std::fwrite(b, 1, 2, f);
}
void write32(std::FILE* f, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v & 0xFF), static_cast<std::uint8_t>((v >> 8) & 0xFF),
                               static_cast<std::uint8_t>((v >> 16) & 0xFF), static_cast<std::uint8_t>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}
}  // namespace

bool SurfaceArena::init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size) {
    mem_ = mem;
    surfaces_.clear();
    if (!mem_ || !heap_.init(mem_, begin, size)) {
        std::snprintf(last_error_, sizeof(last_error_), "surface arena 初始化失败：%s", heap_.lastError());
        return false;
    }
    return true;
}

std::uint32_t SurfaceArena::create(std::int32_t width, std::int32_t height, const SurfaceFormat& fmt) {
    if (!ready()) {
        std::snprintf(last_error_, sizeof(last_error_), "surface arena 未初始化");
        return 0;
    }
    if (width <= 0 || height <= 0) {
        std::snprintf(last_error_, sizeof(last_error_), "surface 尺寸非法：%dx%d", width, height);
        return 0;
    }
    const std::uint32_t bpp = fmt.bytesPerPixel();
    if (bpp == 0 || bpp > 4) {
        std::snprintf(last_error_, sizeof(last_error_), "surface bpp 不支持：%u", fmt.bpp);
        return 0;
    }
    const std::uint64_t row = (static_cast<std::uint64_t>(width) * bpp + 3u) & ~static_cast<std::uint64_t>(3u);
    const std::uint64_t bytes = row * static_cast<std::uint64_t>(height);
    if (bytes > 0x7FFFFFFFu) {
        std::snprintf(last_error_, sizeof(last_error_), "surface 太大：%llu 字节", (unsigned long long)bytes);
        return 0;
    }
    const std::uint32_t ea = heap_.alloc(static_cast<std::uint32_t>(bytes), true);
    if (!ea) {
        std::snprintf(last_error_, sizeof(last_error_), "surface 分配失败（%llu 字节）：%s",
                      (unsigned long long)bytes, heap_.lastError());
        return 0;
    }
    GuestSurface s;
    s.ea = ea;
    s.width = width;
    s.height = height;
    s.pitch = static_cast<std::uint32_t>(row);
    s.format = fmt;
    surfaces_[ea] = s;
    return ea;
}

bool SurfaceArena::destroy(std::uint32_t ea) {
    auto it = surfaces_.find(ea);
    if (it == surfaces_.end()) return false;
    surfaces_.erase(it);
    return heap_.free(ea);
}

GuestSurface* SurfaceArena::find(std::uint32_t ea) {
    auto it = surfaces_.find(ea);
    return it == surfaces_.end() ? nullptr : &it->second;
}

const GuestSurface* SurfaceArena::find(std::uint32_t ea) const {
    auto it = surfaces_.find(ea);
    return it == surfaces_.end() ? nullptr : &it->second;
}

std::uint8_t* SurfaceArena::pixels(GuestSurface& s) { return mem_ ? mem_->host(s.ea) : nullptr; }
const std::uint8_t* SurfaceArena::pixels(const GuestSurface& s) const {
    return mem_ ? mem_->host(s.ea) : nullptr;
}

bool SurfaceArena::lock(std::uint32_t ea) {
    GuestSurface* s = find(ea);
    if (!s) return false;
    ++s->lock_count;
    return true;
}

bool SurfaceArena::unlock(std::uint32_t ea) {
    GuestSurface* s = find(ea);
    if (!s || s->lock_count == 0) return false;
    --s->lock_count;
    return true;
}

std::uint64_t SurfaceArena::hash(const GuestSurface& s) const {
    // ★ 全帧像素指纹（Present 的 `hash=` / DDraw Flip / `--dump-frame` 判据都用它）。
    //   判据不变：**确定性 · 内容敏感（改 1 字节就变）· 非 0**；同内容同值、跨机同值。
    //   实现要点（常数优化，不改语义）：
    //     · 按 8 字节字读（小端假设：本线三个目标平台 x64/arm64 都是小端）；
    //     · 长度先混进去（否则"末尾补零"与"不加"会撞成同一个值）；
    //     · 末尾做一次 finalizer（乘法 + 异或右移），让高位也参与 —— 旧实现是逐字节 FNV，
    //       在没有 finalizer 时低位雪崩很好、高位很差。
    //   ⛔ 不要拿它当"顺序无关"的集合哈希：它有位置语义（同一帧、同一字节序才可比）。
    const std::uint8_t* p = pixels(s);
    if (!p) return 0;
    const std::uint64_t n = static_cast<std::uint64_t>(s.pitch) * static_cast<std::uint64_t>(s.height);
    std::uint64_t h = 0xCBF29CE484222325ull ^ (n * 0x9E3779B97F4A7C15ull);
    std::uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + i, sizeof(w));
        h = (h ^ w) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    return h;
}

bool SurfaceArena::dumpBmp(const GuestSurface& s, const std::string& host_path, std::string* err) const {
    const std::uint8_t* p = pixels(s);
    if (!p) {
        if (err) *err = "surface 没有像素";
        return false;
    }
    std::FILE* f = openHostFileUtf8(host_path, "wb");
    if (!f) {
        if (err) *err = "打不开 BMP 输出：" + host_path;
        return false;
    }
    const bool b32 = s.format.bpp == 32;
    const std::uint32_t out_bpp = b32 ? 32u : 24u;
    const std::uint32_t out_row = (static_cast<std::uint32_t>(s.width) * (out_bpp / 8u) + 3u) & ~3u;
    const std::uint32_t image_size = out_row * static_cast<std::uint32_t>(s.height);
    const std::uint32_t file_size = 14u + 40u + image_size;
    // BITMAPFILEHEADER
    std::fputc('B', f);
    std::fputc('M', f);
    write32(f, file_size);
    write16(f, 0);
    write16(f, 0);
    write32(f, 14u + 40u);
    // BITMAPINFOHEADER
    write32(f, 40);
    write32(f, static_cast<std::uint32_t>(s.width));
    write32(f, static_cast<std::uint32_t>(s.height));
    write16(f, 1);
    write16(f, static_cast<std::uint16_t>(out_bpp));
    write32(f, 0);  // BI_RGB
    write32(f, image_size);
    write32(f, 2835);
    write32(f, 2835);
    write32(f, 0);
    write32(f, 0);
    std::vector<std::uint8_t> row(out_row, 0);
    for (std::int32_t y = s.height - 1; y >= 0; --y) {
        const std::uint8_t* src = p + static_cast<std::uint64_t>(y) * s.pitch;
        for (std::int32_t x = 0; x < s.width; ++x) {
            if (b32) {
                const std::uint8_t* px = src + static_cast<std::uint64_t>(x) * 4u;
                row[static_cast<std::size_t>(x) * 4u + 0] = px[0];
                row[static_cast<std::size_t>(x) * 4u + 1] = px[1];
                row[static_cast<std::size_t>(x) * 4u + 2] = px[2];
                row[static_cast<std::size_t>(x) * 4u + 3] = 0;
            } else {
                const std::uint16_t v = static_cast<std::uint16_t>(src[static_cast<std::size_t>(x) * 2u] |
                                                                   (src[static_cast<std::size_t>(x) * 2u + 1] << 8));
                const std::uint8_t r = static_cast<std::uint8_t>(((v >> 11) & 0x1F) * 255 / 31);
                const std::uint8_t g = static_cast<std::uint8_t>(((v >> 5) & 0x3F) * 255 / 63);
                const std::uint8_t b = static_cast<std::uint8_t>((v & 0x1F) * 255 / 31);
                row[static_cast<std::size_t>(x) * 3u + 0] = b;
                row[static_cast<std::size_t>(x) * 3u + 1] = g;
                row[static_cast<std::size_t>(x) * 3u + 2] = r;
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace lift
