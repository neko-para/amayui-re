// emulator-lift · guest 堆实现（地址有序的块表 + 首次适配 + 合并）
//
// ★ 元数据放宿主侧（`std::map`），guest 只看到指针 —— 但**载荷本身在 guest 内存里**，
//   所以 guest 的 CRT 可以直接读写它（这也是"同一个堆"的意义，README §2.6）。
// ★ 每个块头 8 字节（size + magic），载荷 8 字节对齐。magic 用来在 free 时认出"这不是我的块"。
// ★ 野指针 / double free ⇒ 打印 + abort（⛔ 静默忽略会让内存错误跑到别处才炸）。
#include "runtime/heap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/host_encoding.h"

namespace lift {
namespace {
constexpr std::uint32_t kMagic = 0x4C494654u;  // 'LIFT'
inline std::uint32_t alignUp(std::uint32_t v, std::uint32_t a) { return (v + a - 1u) & ~(a - 1u); }
}  // namespace

bool GuestHeap::init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size) {
    return init(mem, begin, size, Options{});
}

bool GuestHeap::init(GuestMemory* mem, std::uint32_t begin, std::uint32_t size, const Options& opt) {
    if (!mem || size < 0x10000u || begin > 0xFFFFFFFFu - size) {
        std::snprintf(last_error_, sizeof(last_error_), "堆初始化参数不合法（begin=%u size=%u）", begin,
                      size);
        return false;
    }
    mem_ = mem;
    begin_ = begin;
    end_ = begin + size;
    opt_ = opt;
    if (!mem_->commit(begin_, size, RegionKind::Heap, "guest-heap")) {
        std::snprintf(last_error_, sizeof(last_error_), "提交堆区间失败");
        return false;
    }
    blocks_.clear();
    Block b;
    b.off = 0;
    b.size = size;
    b.used = false;
    blocks_.emplace(0, b);
    in_use_ = 0;
    peak_ = 0;
    alloc_count_ = 0;
    return true;
}

void* GuestHeap::payloadOf(std::uint32_t off) const { return mem_->host(begin_ + off + opt_.header); }

bool GuestHeap::findBlock(std::uint32_t ptr, std::uint32_t* off) const {
    if (!ready() || ptr < begin_ + opt_.header || ptr >= end_) return false;
    const std::uint32_t o = ptr - begin_ - opt_.header;
    const auto it = blocks_.find(o);
    if (it == blocks_.end() || !it->second.used) return false;
    // 额外核对块头 magic：能抓住"指针指向块中间"这类错误
    if (mem_->rd32(begin_ + o) != kMagic) return false;
    *off = o;
    return true;
}

std::uint32_t GuestHeap::alloc(std::uint32_t bytes, bool zero) {
    if (!ready()) {
        std::snprintf(last_error_, sizeof(last_error_), "堆未初始化");
        return 0;
    }
    // ★ `bytes + header` 必须用 64 位算：32 位回绕时（例如 0xFFFFFFF8 + 8）`need` 会变成 0，
    //   切出一个 size=0 的块并**返回非空指针**；带 ZERO_MEMORY 时 `b.size - header` 还会回绕成
    //   0xFFFFFFF8 ⇒ 4 GiB memset（先踩到 PROT_NONE 就是一次假 guest fault）。
    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;
    if (need64 > 0xFFFFFFFFull) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求过大（%u 字节 + 块头）", bytes);
        return 0;
    }
    const std::uint32_t need = alignUp(static_cast<std::uint32_t>(need64), opt_.align);
    if (static_cast<std::uint64_t>(need) < need64) {  // alignUp 自己回绕（对齐 > 1 时）
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆请求对齐后溢出（%u 字节）", bytes);
        return 0;
    }
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        Block& b = it->second;
        if (b.used || b.size < need) continue;
        if (b.size - need >= opt_.min_split) {
            Block rest;
            rest.off = b.off + need;
            rest.size = b.size - need;
            rest.used = false;
            b.size = need;
            blocks_.emplace(rest.off, rest);
        }
        b.used = true;
        b.req = bytes;
        mem_->wr32(begin_ + b.off, kMagic);
        mem_->wr32(begin_ + b.off + 4, bytes);
        if (zero) std::memset(payloadOf(b.off), 0, b.size - opt_.header);
        in_use_ += b.size;
        if (in_use_ > peak_) peak_ = in_use_;
        ++alloc_count_;
        return begin_ + b.off + opt_.header;
    }
    std::snprintf(last_error_, sizeof(last_error_), "guest 堆耗尽（请求 %u 字节）", bytes);
    return 0;
}

bool GuestHeap::free(std::uint32_t ptr) {
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) {
        std::fprintf(stderr,
                     "\n=== guest heap: 非法 free ===\n  ptr = 0x%08X（不在堆区间内 / 不是块首 / magic 不对 / 已释放）\n"
                     "  堆区间 = 0x%08X..0x%08X  in_use=%u peak=%u\n",
                     ptr, begin_, end_, in_use_, peak_);
        std::fflush(nullptr);
        restoreConsoleUtf8();  // _Exit / abort 不跑析构：控制台码页必须显式恢复
        std::abort();
    }
    Block& b = blocks_[off];
    b.used = false;
    b.req = 0;
    in_use_ -= b.size;
    mem_->wr32(begin_ + off, 0);
    mem_->wr32(begin_ + off + 4, 0);
    coalesce();
    return true;
}

std::uint32_t GuestHeap::realloc(std::uint32_t ptr, std::uint32_t bytes, bool zero) {
    if (ptr == 0) return alloc(bytes, zero);
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) {
        std::fprintf(stderr, "\n=== guest heap: 非法 realloc ===\n  ptr = 0x%08X\n", ptr);
        std::fflush(nullptr);
        restoreConsoleUtf8();
        std::abort();
    }
    Block& b = blocks_[off];
    // 与 alloc 同一条溢出检查：`bytes + header` 回绕会算出 need=0 ⇒ 误判"现有块够大"，
    // 随后的 ZERO_MEMORY 清零按 `bytes - old_req` 一路写成 4 GiB。
    const std::uint64_t need64 = static_cast<std::uint64_t>(bytes) + opt_.header;
    if (need64 > 0xFFFFFFFFull) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆 realloc 请求过大（%u 字节）", bytes);
        return 0;
    }
    const std::uint32_t need = alignUp(static_cast<std::uint32_t>(need64), opt_.align);
    if (static_cast<std::uint64_t>(need) < need64) {
        std::snprintf(last_error_, sizeof(last_error_), "guest 堆 realloc 对齐后溢出（%u 字节）", bytes);
        return 0;
    }
    if (need <= b.size) {
        const std::uint32_t old_req = b.req;
        if (zero && bytes > old_req) std::memset(mem_->host(begin_ + off + opt_.header + old_req), 0, bytes - old_req);
        b.req = bytes;
        mem_->wr32(begin_ + off + 4, bytes);
        return ptr;
    }
    const std::uint32_t np = alloc(bytes, zero);
    if (!np) return 0;
    std::memcpy(mem_->host(np), mem_->host(ptr), b.req);
    free(ptr);
    return np;
}

std::uint32_t GuestHeap::sizeOf(std::uint32_t ptr) const {
    std::uint32_t off = 0;
    if (!findBlock(ptr, &off)) return 0xFFFFFFFFu;
    return blocks_.at(off).req;
}

bool GuestHeap::owns(std::uint32_t ptr) const {
    std::uint32_t off = 0;
    return findBlock(ptr, &off);
}

void GuestHeap::coalesce() {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        if (it->second.used) {
            ++it;
            continue;
        }
        auto next = std::next(it);
        if (next != blocks_.end() && !next->second.used && it->second.off + it->second.size == next->second.off) {
            it->second.size += next->second.size;
            blocks_.erase(next);
            continue;  // 再试一次：可能还能往后并
        }
        ++it;
    }
}

void GuestHeap::dumpLeak(std::uint32_t ptr, const char* api) const {
    std::fprintf(stderr, "  [heap] %s: ptr=0x%08X 不是活动块（in_use=%u）\n", api, ptr, in_use_);
}

}  // namespace lift
